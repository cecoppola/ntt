/* 16_ntt_tile - the paper's tiled single-transform DIF NTT (PLAN.md B2).
 *
 * The e-paper runs one long forward DIF transform (natural in, bit-reversed
 * out) as passes of STG consecutive radix-2 stages.  Stage s pairs elements
 * at distance h = 2^s.  A b16 pass covering stages [s_lo, s_hi] works on
 * tiles of TILE = 2^STG elements spaced h_min = 2^s_lo apart; a block takes
 * 16 adjacent tiles so every global access is a contiguous 16 x 8 B row, and
 * holds them in LDS as sh[TILE][17] (pad 17: conflict-free columns; 17 416 B
 * at TILE = 128, 3 blocks/CU).  Thread (tt = tid>>4, bb = tid&15) owns column
 * bb and butterflies tt, tt+16, ... of every stage.  Twiddles are computed per
 * thread: w = tab[r * TILE/(2H)] * T_H where tab holds the TILE-th roots and
 * T_H = w_n^(c * n / (2 H h_min)) for the thread's global column c, from a
 * two-level 2 x 4096 table per pass (one modmul) and squared once per stage.  The b1 kernel does
 * the last ten stages (h = 512..1) on contiguous 1024-point blocks and
 * canonicalises the output.  Arithmetic is the paper's FP64 Barrett modmul
 * (bench/15) for the products only; adds and subtracts are integer, because
 * u + v with u, v < 2p ~ 2^52.8 does not fit a double (found the hard way).
 * Values stay lazy in [0,2p) and are folded before every multiply so the
 * modmul only ever sees lazy x canonical.
 *
 * Measured: correctness vs a host DIF at 2^20 for several pass splits;
 * per-pass and whole-transform time at 2^28..2^31 on all four APUs (effective
 * TB/s = 16 B x n per pass); batched throughput for log L = 11, 14, 17, 20 as
 * the paper reports it.
 *
 * Usage: 16_ntt_tile [max log2 n]
 */
#include "common_ntt.h"

#define P0 3923057487904769ULL          /* paper's P[0], g = 3 */
#define G0 3ULL
#define BP 17
#define MAXTILE 128                      /* STG <= 7: sh = 17 416 B, 3 blocks/CU */
#define THREADS 256

__device__ static inline double mm(double a, double b, double p, double pinv)
{
    double hi = a * b, lo = fma(a, b, -hi), q = floor(hi * pinv);
    double r = fma(-q, p, hi) + lo;
    r += (r < 0.0 ? p : 0.0); r += (r < 0.0 ? p : 0.0);
    r -= (r >= p ? p : 0.0); r -= (r >= p ? p : 0.0);
    return r;
}
__device__ static double dpow(double b, uint64_t e, double p, double pinv)
{
    double r = 1.0;
    while (e) { if (e & 1) r = mm(r, b, p, pinv); b = mm(b, b, p, pinv); e >>= 1; }
    return r;
}
/* twiddle base T_H (PLAN.md D4):
 *   0  two-level table, one modmul per thread
 *   1  per-thread exponentiation w_n^(c K)          (the paper's words, literally)
 *   2  one exponentiation per block (thread 0, LDS broadcast) x tab[bb] */
static int g_twmode = 0;

/* one b16 pass: stages s_hi..s_lo (h = 2^s), n-point transforms, `batch` of them
 * back to back in memory.  tabT[k] = w_TILE^k. */
__global__ __launch_bounds__(THREADS)
void k_b16(uint64_t *x, int logn, int s_lo, int s_hi, double p, double pinv,
           const double *tlo, const double *thi, const double *tabT, int canon,
           int twmode, double wn)
{
    __shared__ uint64_t sh[MAXTILE * BP + 1];
    __shared__ double tab[MAXTILE];
    __shared__ double tbase;
    const int stg = s_hi - s_lo + 1, tile = 1 << stg;
    const int tt = threadIdx.x >> 4, bb = threadIdx.x & 15;
    const size_t n = (size_t)1 << logn, hmin = (size_t)1 << s_lo;
    const size_t bpt = n / ((size_t)tile * 16);          /* blocks per transform */
    const size_t b = blockIdx.x % bpt, t = blockIdx.x / bpt;
    const size_t slabs = hmin / 16, blk_hi = b / slabs, slab = b % slabs;
    const size_t base = t * n + blk_hi * (size_t)tile * hmin + slab * 16;
    const size_t c = slab * 16 + bb;                      /* global column */
    const uint64_t pu = (uint64_t)p, p2 = 2 * pu;
    int j, H, k;

    for (k = threadIdx.x; k < tile; k += THREADS) tab[k] = tabT[k];
    for (j = tt; j < tile; j += 16)
        sh[j * BP + bb] = x[base + (size_t)j * hmin + bb];
    /* T_H for H = tile/2 is w_n^(c * n/(tile*hmin)): two-level table, one modmul
     * (per-thread exponentiation, as the paper describes, costs 62 modmuls and
     * doubled the pass time) */
    double TH;
    const uint64_t K = n / ((size_t)tile * hmin);
    if (twmode == 1) TH = dpow(wn, (uint64_t)c * K, p, pinv);
    else if (twmode == 2) {
        if (threadIdx.x == 0) tbase = dpow(wn, (uint64_t)slab * 16 * K, p, pinv);
        __syncthreads();
        TH = mm(tbase, tlo[bb], p, pinv);                 /* tlo[k] = w_n^(k K) */
    } else TH = mm(tlo[c & 4095], thi[c >> 12], p, pinv);
    __syncthreads();
    int lgH = stg - 1, lgstep = 0;
    for (H = tile / 2; H >= 1; H >>= 1, lgH--, lgstep++) {
        for (k = tt; k < tile / 2; k += 16) {
            int r = k & (H - 1), q = k >> lgH, j0 = (q << (lgH + 1)) + r, j1 = j0 + H;
            uint64_t u = sh[j0 * BP + bb], v = sh[j1 * BP + bb];
            uint64_t s = u + v, d = u - v + p2;           /* exact: < 4p < 2^54 */
            double w;
            if (s >= p2) s -= p2;
            if (d >= p2) d -= p2;
            w = mm(tab[r << lgstep], TH, p, pinv);        /* canonical x canonical */
            sh[j0 * BP + bb] = s;
            sh[j1 * BP + bb] = (uint64_t)mm((double)d, w, p, pinv);   /* lazy x canonical */
        }
        TH = mm(TH, TH, p, pinv);
        __syncthreads();
    }
    for (j = tt; j < tile; j += 16) {
        uint64_t v = sh[j * BP + bb];
        if (canon && v >= pu) v -= pu;
        x[base + (size_t)j * hmin + bb] = v;
    }
}

/* the last stages, h = 2^s_hi .. 1, on contiguous 2^(s_hi+1)-point blocks.
 * tab1[k] = w_(2^(s_hi+1))^k, k < 2^s_hi. */
__global__ __launch_bounds__(THREADS)
void k_b1(uint64_t *x, int s_hi, double p, double pinv, const double *tab1, int canon)
{
    __shared__ uint64_t sh[1024];
    __shared__ double tab[512];
    const int L = 2 << s_hi;
    const size_t base = (size_t)blockIdx.x * L;
    const uint64_t pu = (uint64_t)p, p2 = 2 * pu;
    int H, k;
    for (k = threadIdx.x; k < L / 2; k += THREADS) tab[k] = tab1[k];
    for (k = threadIdx.x; k < L; k += THREADS) sh[k] = x[base + k];
    __syncthreads();
    int lgH = s_hi, lgstep = 0;
    for (H = L / 2; H >= 1; H >>= 1, lgH--, lgstep++) {
        for (k = threadIdx.x; k < L / 2; k += THREADS) {
            int r = k & (H - 1), q = k >> lgH, j0 = (q << (lgH + 1)) + r, j1 = j0 + H;
            uint64_t u = sh[j0], v = sh[j1], s = u + v, d = u - v + p2;
            if (s >= p2) s -= p2;
            if (d >= p2) d -= p2;
            sh[j0] = s;
            sh[j1] = (uint64_t)mm((double)d, tab[r << lgstep], p, pinv);
        }
        __syncthreads();
    }
    for (k = threadIdx.x; k < L; k += THREADS) {
        uint64_t v = sh[k];
        if (canon && v >= pu) v -= pu;
        x[base + k] = v;
    }
}

/* ---- D5: compile-time STG, and a block always holds 128 rows x 16 columns:
 * NS = 128/TILE slabs of TILE rows each, so small passes keep 2048 elements
 * per block instead of 16 x TILE.  Thread (tt, bb) does butterflies
 * k = tt + 16 m, m < 4, over the 64 per column; tile of butterfly k is
 * k >> (STG-1), so a thread touches NT = 1 / 2 / 4 tiles for STG = 7 / 6 / <=5
 * and keeps that many twiddle bases in registers. */
template <int STG>
__global__ __launch_bounds__(THREADS)
void k_b16t(uint64_t *x, int logn, int s_lo, double p, double pinv,
            const double *tlo, const double *thi, const double *tabT, int canon)
{
    constexpr int tile = 1 << STG, NS = 128 / tile;
    constexpr int NT = tile == 128 ? 1 : tile == 64 ? 2 : 4;
    __shared__ uint64_t sh[128 * BP + 1];
    __shared__ double tab[tile];
    const int tt = threadIdx.x >> 4, bb = threadIdx.x & 15;
    const size_t n = (size_t)1 << logn, hmin = (size_t)1 << s_lo, slabs = hmin / 16;
    const size_t bpt = n / 2048, b = blockIdx.x % bpt, t = blockIdx.x / bpt;
    const size_t groups = slabs / NS, blk_hi = b / groups, slab0 = (b % groups) * NS;
    const size_t base = t * n + blk_hi * (size_t)tile * hmin + slab0 * 16;
    const uint64_t pu = (uint64_t)p, p2 = 2 * pu;
    double T[NT];
    int j, k, m;

    for (k = threadIdx.x; k < tile; k += THREADS) tab[k] = tabT[k];
#pragma unroll
    for (j = tt; j < 128; j += 16)
        sh[j * BP + bb] = x[base + (size_t)(j >> STG) * 16 + (size_t)(j & (tile - 1)) * hmin + bb];
#pragma unroll
    for (m = 0; m < NT; m++) {
        int ti = NT == 1 ? 0 : NT == 2 ? m : ((tt + 16 * m) >> (STG - 1));
        size_t c = (slab0 + ti) * 16 + bb;
        T[m] = mm(tlo[c & 4095], thi[c >> 12], p, pinv);
    }
    __syncthreads();
#pragma unroll
    for (int lgH = STG - 1; lgH >= 0; lgH--) {
        const int H = 1 << lgH, lgstep = STG - 1 - lgH;
#pragma unroll
        for (m = 0; m < 4; m++) {
            int kk = tt + 16 * m, ti = kk >> (STG - 1), kl = kk & (tile / 2 - 1);
            int r = kl & (H - 1), q = kl >> lgH, j0 = ti * tile + (q << (lgH + 1)) + r, j1 = j0 + H;
            int tm = NT == 1 ? 0 : NT == 2 ? (m >> 1) : m;
            uint64_t u = sh[j0 * BP + bb], v = sh[j1 * BP + bb];
            uint64_t sum = u + v, d = u - v + p2;
            double w;
            if (sum >= p2) sum -= p2;
            if (d >= p2) d -= p2;
            w = mm(tab[r << lgstep], T[tm], p, pinv);
            sh[j0 * BP + bb] = sum;
            sh[j1 * BP + bb] = (uint64_t)mm((double)d, w, p, pinv);
        }
#pragma unroll
        for (m = 0; m < NT; m++) T[m] = mm(T[m], T[m], p, pinv);
        __syncthreads();
    }
#pragma unroll
    for (j = tt; j < 128; j += 16) {
        uint64_t v = sh[j * BP + bb];
        if (canon && v >= pu) v -= pu;
        x[base + (size_t)(j >> STG) * 16 + (size_t)(j & (tile - 1)) * hmin + bb] = v;
    }
}

/* b1 with a compile-time block length 2^LGL (stages LGL-1 .. 0) */
template <int LGL>
__global__ __launch_bounds__(THREADS)
void k_b1t(uint64_t *x, double p, double pinv, const double *tab1, int canon)
{
    constexpr int L = 1 << LGL;
    __shared__ uint64_t sh[L];
    __shared__ double tab[L / 2];
    const size_t base = (size_t)blockIdx.x * L;
    const uint64_t pu = (uint64_t)p, p2 = 2 * pu;
    int k;
    for (k = threadIdx.x; k < L / 2; k += THREADS) tab[k] = tab1[k];
    for (k = threadIdx.x; k < L; k += THREADS) sh[k] = x[base + k];
    __syncthreads();
#pragma unroll
    for (int lgH = LGL - 1; lgH >= 0; lgH--) {
        const int H = 1 << lgH, lgstep = LGL - 1 - lgH;
#pragma unroll
        for (k = threadIdx.x; k < L / 2; k += THREADS) {
            int r = k & (H - 1), q = k >> lgH, j0 = (q << (lgH + 1)) + r, j1 = j0 + H;
            uint64_t u = sh[j0], v = sh[j1], sum = u + v, d = u - v + p2;
            if (sum >= p2) sum -= p2;
            if (d >= p2) d -= p2;
            sh[j0] = sum;
            sh[j1] = (uint64_t)mm((double)d, tab[r << lgstep], p, pinv);
        }
        __syncthreads();
    }
    for (k = threadIdx.x; k < L; k += THREADS) {
        uint64_t v = sh[k];
        if (canon && v >= pu) v -= pu;
        x[base + k] = v;
    }
}
static int g_tmpl = 0;      /* 1: use the template kernels */
static int g_lgl = 10;      /* b1 block length, 10..12 */

/* ------------------------------ host ------------------------------------ */
static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t p)
{ return (uint64_t)((__uint128_t)a * b % p); }
static uint64_t powmod(uint64_t a, uint64_t e, uint64_t p)
{ uint64_t r = 1; a %= p; while (e) { if (e & 1) r = mulmod(r, a, p); a = mulmod(a, a, p); e >>= 1; } return r; }

/* reference: the same DIF, on the host */
static void host_dif(uint64_t *x, int logn, uint64_t p)
{
    size_t n = (size_t)1 << logn, h, i, r;
    uint64_t wn = powmod(G0, (p - 1) / n, p);
    for (h = n / 2; h >= 1; h >>= 1) {
        uint64_t w2h = powmod(wn, n / (2 * h), p), w;
        for (i = 0; i < n; i += 2 * h) {
            w = 1;
            for (r = 0; r < h; r++) {
                uint64_t u = x[i + r], v = x[i + r + h];
                x[i + r] = (u + v) % p;
                x[i + r + h] = mulmod((u + p - v) % p, w, p);
                w = mulmod(w, w2h, p);
            }
        }
    }
}

struct plan { int npass, s_lo[8], s_hi[8]; };
/* split stages [10, logn) into b16 passes of at most `stg` stages, high first */
static void make_plan(struct plan *pl, int logn, int stg)
{
    int hi = logn - 1, lgl = g_lgl < logn ? g_lgl : logn;
    pl->npass = 0;
    while (hi >= lgl) {
        int lo = hi - stg + 1; if (lo < lgl) lo = lgl;
        pl->s_hi[pl->npass] = hi; pl->s_lo[pl->npass] = lo; pl->npass++;
        hi = lo - 1;
    }
}

struct dev { double *tabT[8], *tab1, *tab1l[13], *tlo, *thi, *hlo, *hhi; };   /* tabT[stg]: 2^stg-th roots; tab1: 1024-th */
static void dev_tables(struct dev *dv, uint64_t p)
{
    int stg, k;
    double *h = (double *)malloc(512 * 8);
    for (stg = 1; stg <= 7; stg++) {
        int tile = 1 << stg;
        uint64_t w = powmod(G0, (p - 1) / tile, p), a = 1;
        for (k = 0; k < tile; k++) { h[k] = (double)a; a = mulmod(a, w, p); }
        HIP_CHECK(hipMalloc(&dv->tabT[stg], tile * 8));
        HIP_CHECK(hipMemcpy(dv->tabT[stg], h, tile * 8, hipMemcpyHostToDevice));
    }
    {
        uint64_t w = powmod(G0, (p - 1) / 1024, p), a = 1;
        int lg;
        for (k = 0; k < 512; k++) { h[k] = (double)a; a = mulmod(a, w, p); }
        HIP_CHECK(hipMalloc(&dv->tab1, 512 * 8));
        HIP_CHECK(hipMalloc(&dv->tlo, 4096 * 8)); HIP_CHECK(hipMalloc(&dv->thi, 4096 * 8));
        dv->hlo = (double *)malloc(4096 * 8); dv->hhi = (double *)malloc(4096 * 8);
        HIP_CHECK(hipMemcpy(dv->tab1, h, 512 * 8, hipMemcpyHostToDevice));
        free(h); h = (double *)malloc(2048 * 8);
        for (lg = 10; lg <= 12; lg++) {
            int L = 1 << lg; w = powmod(G0, (p - 1) / L, p); a = 1;
            for (k = 0; k < L / 2; k++) { h[k] = (double)a; a = mulmod(a, w, p); }
            HIP_CHECK(hipMalloc(&dv->tab1l[lg], L / 2 * 8));
            HIP_CHECK(hipMemcpy(dv->tab1l[lg], h, L / 2 * 8, hipMemcpyHostToDevice));
        }
    }
    free(h);
}

/* run the full forward transform: batch transforms of 2^logn each; returns ms
 * per pass in pass_ms[] (npass b16 passes then the b1 pass) */
static float run_fwd(uint64_t *x, int logn, size_t batch, const struct plan *pl,
                     struct dev *dv, uint64_t p, float *pass_ms)
{
    double pd = (double)p, pinv = 1.0 / pd;
    uint64_t wn = powmod(G0, (p - 1) / ((uint64_t)1 << logn), p);
    hipEvent_t e0, e1; float ms, tot = 0;
    int i, k;
    timer_events(&e0, &e1);
    for (i = 0; i < pl->npass; i++) {
        int stg = pl->s_hi[i] - pl->s_lo[i] + 1, tile = 1 << stg;
        size_t n = (size_t)1 << logn, hmin = (size_t)1 << pl->s_lo[i];
        size_t blocks = batch * (n / ((size_t)tile * 16));
        uint64_t K = n / ((size_t)tile * hmin), wK = powmod(wn, K, p), wK4 = powmod(wK, 4096, p), a;
        for (k = 0, a = 1; k < 4096; k++) { dv->hlo[k] = (double)a; a = mulmod(a, wK, p); }
        for (k = 0, a = 1; k < 4096; k++) { dv->hhi[k] = (double)a; a = mulmod(a, wK4, p); }
        HIP_CHECK(hipMemcpy(dv->tlo, dv->hlo, 4096 * 8, hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dv->thi, dv->hhi, 4096 * 8, hipMemcpyHostToDevice));
        HIP_CHECK(hipEventRecord(e0, 0));
        if (g_tmpl) {
            unsigned bl = (unsigned)(batch * (n / 2048));
#define B16T(S) case S: k_b16t<S><<<bl, THREADS>>>(x, logn, pl->s_lo[i], pd, pinv, dv->tlo, dv->thi, dv->tabT[S], 0); break;
            switch (stg) { B16T(1) B16T(2) B16T(3) B16T(4) B16T(5) B16T(6) B16T(7) default: break; }
        } else
        k_b16<<<(unsigned)blocks, THREADS>>>(x, logn, pl->s_lo[i], pl->s_hi[i], pd, pinv, dv->tlo, dv->thi, dv->tabT[stg], 0,
                                             g_twmode, (double)wn);
        HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
        HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); pass_ms[i] = ms; tot += ms;
    }
    {
        int lgl = g_lgl < logn ? g_lgl : logn, s_hi = lgl - 1, L = 1 << lgl;
        size_t blocks = batch * (((size_t)1 << logn) / L);
        HIP_CHECK(hipEventRecord(e0, 0));
        if (g_tmpl && lgl >= 10) {
            if (lgl == 10) k_b1t<10><<<(unsigned)blocks, THREADS>>>(x, pd, pinv, dv->tab1l[10], 1);
            else if (lgl == 11) k_b1t<11><<<(unsigned)blocks, THREADS>>>(x, pd, pinv, dv->tab1l[11], 1);
            else k_b1t<12><<<(unsigned)blocks, THREADS>>>(x, pd, pinv, dv->tab1l[12], 1);
        } else
        k_b1<<<(unsigned)blocks, THREADS>>>(x, s_hi, pd, pinv, dv->tab1, 1);
        HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
        HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); pass_ms[pl->npass] = ms; tot += ms;
    }
    return tot;
}

int main(int argc, char **argv)
{
    int maxlog = argc > 1 ? atoi(argv[1]) : 31;
    int LN = argc > 2 ? atoi(argv[2]) : 20;
    uint64_t p = P0;
    int nd = device_count(), d, i, logn;
    struct dev dv[MAXD];
    double ok = 1;

    printf("== 16_ntt_tile : the paper's tiled DIF NTT, p = P[0], FP64 Barrett ==\n");
    meta("16_ntt_tile");
    for (d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); dev_tables(&dv[d], p); }

    /* ---- correctness at 2^20 vs host DIF, several pass splits, APU0 ------ */
    {
        size_t n = (size_t)1 << LN, k;
        uint64_t *hx = (uint64_t *)malloc(n * 8), *hr = (uint64_t *)malloc(n * 8), *hg = (uint64_t *)malloc(n * 8), *dx;
        int stgs[5] = { 7, 5, 6, 4, 3 };
        float pm[9];
        HIP_CHECK(hipSetDevice(0));
        HIP_CHECK(hipMalloc(&dx, n * 8));
        for (k = 0; k < n; k++) hx[k] = (k * 0x9E3779B97F4A7C15ULL + 11) % p;
        memcpy(hr, hx, n * 8); host_dif(hr, LN, p);
        { uint64_t sum = 0; for (k = 0; k < n; k++) sum = (sum + hx[k]) % p;
          printf("host: sum x mod p = %llu, ref[0] = %llu\n", (unsigned long long)sum, (unsigned long long)hr[0]); }
        printf("\ncorrectness, n = 2^%d, vs host DIF:\n", LN);
        for (i = 0; i < 14; i++) {
            struct plan pl; int j, bad = 0;
            /* runs 0-4: runtime kernel, splits; 5-6: twiddle modes 1, 2;
             * 7-11: template kernel, splits, lgl 10; 12-13: template, lgl 11, 12 */
            g_twmode = (i == 5 || i == 6) ? i - 4 : 0;
            g_tmpl = i >= 7; g_lgl = i < 12 ? 10 : i - 1;
            make_plan(&pl, LN, stgs[i < 5 ? i : i < 7 ? 0 : i < 12 ? i - 7 : 0]);
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            run_fwd(dx, LN, 1, &pl, &dv[0], p, pm);
            HIP_CHECK(hipMemcpy(hg, dx, n * 8, hipMemcpyDeviceToHost));
            for (k = 0; k < n; k++) if (hg[k] != hr[k]) { bad++; if (bad < 3) printf("  mismatch at %zu: %llu vs %llu (diff %lld)\n", k, (unsigned long long)hg[k], (unsigned long long)hr[k], (long long)(hg[k] - hr[k])); }
            printf("  gpu[0] = %llu, %d of %zu wrong\n", (unsigned long long)hg[0], bad, n);
            printf("  %s lgl %d twmode %d: passes", g_tmpl ? "tmpl" : "rt  ", g_lgl, g_twmode);
            for (j = 0; j < pl.npass; j++) printf(" [%d..%d]", pl.s_lo[j], pl.s_hi[j]);
            printf(" [0..9]  -> %s\n", bad ? "FAILED" : "OK");
            if (bad) ok = 0;
        }
        g_twmode = 0; g_tmpl = 0; g_lgl = 10;
        /* batched: 2^9 transforms of 2^11 (one 1-stage b16 pass + b1), and 2^6 of 2^14 */
        for (int LB = 11; LB <= 14; LB += 3) {
            struct plan pl; int j, bad = 0; size_t nb = (size_t)1 << LB, B = n / nb;
            make_plan(&pl, LB, 7);
            for (j = 0; j < (int)B; j++) { memcpy(hr + j * nb, hx + j * nb, nb * 8); host_dif(hr + j * nb, LB, p); }
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            run_fwd(dx, LB, B, &pl, &dv[0], p, pm);
            HIP_CHECK(hipMemcpy(hg, dx, n * 8, hipMemcpyDeviceToHost));
            for (k = 0; k < n; k++) if (hg[k] != hr[k]) bad++;
            printf("  batched %zu x 2^%d: %s\n", B, LB, bad ? "FAILED" : "OK");
            if (bad) ok = 0;
        }
        printf("VERIFY tiled DIF == host DIF: %s\n", ok ? "OK" : "FAILED");
        HIP_CHECK(hipFree(dx)); free(hx); free(hr); free(hg);
        if (!ok) return 1;
    }

    /* ---- single long transform, 2^28 .. 2^maxlog, all APUs concurrently --- */
    for (logn = 28; logn <= maxlog; logn++) {
        size_t n = (size_t)1 << logn;
        struct plan pl; float pm[MAXD][9]; double tot[MAXD], tbs[MAXD], per[9][MAXD];
        int j;
        make_plan(&pl, logn, 7);
        printf("\n-- n = 2^%d (%.1f GiB per APU), passes:", logn, n * 8.0 / 1073741824.0);
        for (j = 0; j < pl.npass; j++) printf(" [%d..%d]", pl.s_lo[j], pl.s_hi[j]);
        printf(" [0..9] --\n");
#pragma omp parallel num_threads(nd)
        {
            int dev = omp_get_thread_num(), rep;
            uint64_t *dx; float t, best = 1e30f, bp[9];
            HIP_CHECK(hipSetDevice(dev));
            HIP_CHECK(hipMalloc(&dx, n * 8));
            HIP_CHECK(hipMemset(dx, 1, n * 8));
            for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                t = run_fwd(dx, logn, 1, &pl, &dv[dev], p, bp);
                if (t < best) { best = t; memcpy(pm[dev], bp, sizeof bp); }
            }
            tot[dev] = best;
            tbs[dev] = 16.0 * n * (pl.npass + 1) / (best * 1e-3) / 1e12;
            HIP_CHECK(hipFree(dx));
        }
        header("pass");
        for (j = 0; j <= pl.npass; j++) {
            char nm[40];
            for (d = 0; d < nd; d++) per[j][d] = 16.0 * n / (pm[d][j] * 1e-3) / 1e12;
            if (j < pl.npass) snprintf(nm, sizeof nm, "2^%d b16[%d..%d] TB/s", logn, pl.s_lo[j], pl.s_hi[j]);
            else snprintf(nm, sizeof nm, "2^%d b1[0..9] TB/s", logn);
            report_max(nm, "TB/s", per[j], nd);
        }
        { char nm[40]; snprintf(nm, sizeof nm, "2^%d transform ms", logn); report_max(nm, "ms", tot, nd);
          snprintf(nm, sizeof nm, "2^%d effective TB/s", logn); report_max(nm, "TB/s", tbs, nd); }
    }

    /* ---- D4: twiddle modes at 2^maxlog ---------------------------------- */
    {
        int mode; size_t n = (size_t)1 << maxlog; struct plan pl; double tot[MAXD];
        static const char *mn[3] = { "two-level table", "per-thread dpow", "block dpow + tab[bb]" };
        make_plan(&pl, maxlog, 7);
        printf("\n-- D4: twiddle base strategy, full 2^%d transform --\n", maxlog);
        header("twiddle mode");
        for (mode = 0; mode < 3; mode++) {
            g_twmode = mode;
#pragma omp parallel num_threads(nd)
            {
                int dev = omp_get_thread_num(), rep; uint64_t *dx; float t, best = 1e30f, bp[9];
                HIP_CHECK(hipSetDevice(dev));
                HIP_CHECK(hipMalloc(&dx, n * 8)); HIP_CHECK(hipMemset(dx, 1, n * 8));
                for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                    t = run_fwd(dx, maxlog, 1, &pl, &dv[dev], p, bp);
                    if (t < best) best = t;
                }
                tot[dev] = best;
                HIP_CHECK(hipFree(dx));
            }
            { char nm[48]; snprintf(nm, sizeof nm, "D4 %s ms", mn[mode]); report_max(nm, "ms", tot, nd); }
        }
        g_twmode = 0;
    }

    /* ---- D5: template kernels x b1 length, at 2^maxlog and batched ------- */
    {
        int lgl, li; int logl[3] = { 14, 17, 20 };
        printf("\n-- D5: compile-time STG + 2048-element blocks, b1 length 2^lgl --\n");
        header("variant");
        for (lgl = 10; lgl <= 12; lgl++) {
            struct plan pl; size_t n = (size_t)1 << maxlog; double tot[MAXD], tbs[MAXD]; char nm[48];
            g_tmpl = 1; g_lgl = lgl;
            make_plan(&pl, maxlog, 7);
#pragma omp parallel num_threads(nd)
            {
                int dev = omp_get_thread_num(), rep; uint64_t *dx; float t, best = 1e30f, bp[9];
                HIP_CHECK(hipSetDevice(dev));
                HIP_CHECK(hipMalloc(&dx, n * 8)); HIP_CHECK(hipMemset(dx, 1, n * 8));
                for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                    t = run_fwd(dx, maxlog, 1, &pl, &dv[dev], p, bp);
                    if (t < best) best = t;
                }
                tot[dev] = best; tbs[dev] = 16.0 * n * (pl.npass + 1) / (best * 1e-3) / 1e12;
                HIP_CHECK(hipFree(dx));
            }
            snprintf(nm, sizeof nm, "D5 2^%d tmpl lgl=%d ms", maxlog, lgl); report_max(nm, "ms", tot, nd);
            snprintf(nm, sizeof nm, "D5 2^%d tmpl lgl=%d eff", maxlog, lgl); report_max(nm, "TB/s", tbs, nd);
            for (li = 0; li < 3; li++) {
                size_t nb = (size_t)1 << 28, B = nb >> logl[li]; double eff[MAXD];
                make_plan(&pl, logl[li], 7);
#pragma omp parallel num_threads(nd)
                {
                    int dev = omp_get_thread_num(), rep; uint64_t *dx; float t, best = 1e30f, bp[9];
                    HIP_CHECK(hipSetDevice(dev));
                    HIP_CHECK(hipMalloc(&dx, nb * 8)); HIP_CHECK(hipMemset(dx, 1, nb * 8));
                    for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                        t = run_fwd(dx, logl[li], B, &pl, &dv[dev], p, bp);
                        if (t < best) best = t;
                    }
                    eff[dev] = 16.0 * nb * (pl.npass + 1) / (best * 1e-3) / 1e9;
                    HIP_CHECK(hipFree(dx));
                }
                snprintf(nm, sizeof nm, "D5 logL=%d tmpl lgl=%d pass-traffic", logl[li], lgl); report_sum(nm, "GB/s", eff, nd);
            }
        }
        g_tmpl = 0; g_lgl = 10;
    }

    /* ---- batched, as the paper reports: log L = 11, 14, 17, 20 over 2^28 -- */
    {
        int logl[4] = { 11, 14, 17, 20 }, li;
        const int LT = 28; size_t n = (size_t)1 << LT;
        printf("\n-- batched sub-transforms over 2^%d points per APU --\n", LT);
        header("log L");
        for (li = 0; li < 4; li++) {
            struct plan pl; size_t B = n >> logl[li]; double gbs[MAXD], eff[MAXD];
            make_plan(&pl, logl[li], 7);
#pragma omp parallel num_threads(nd)
            {
                int dev = omp_get_thread_num(), rep;
                uint64_t *dx; float t, best = 1e30f, bp[9];
                HIP_CHECK(hipSetDevice(dev));
                HIP_CHECK(hipMalloc(&dx, n * 8)); HIP_CHECK(hipMemset(dx, 1, n * 8));
                for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                    t = run_fwd(dx, logl[li], B, &pl, &dv[dev], p, bp);
                    if (t < best) best = t;
                }
                gbs[dev] = 8.0 * n / (best * 1e-3) / 1e9;                    /* one plane per transform */
                eff[dev] = 16.0 * n * (pl.npass + 1) / (best * 1e-3) / 1e9; /* all pass traffic */
                HIP_CHECK(hipFree(dx));
            }
            { char nm[40]; snprintf(nm, sizeof nm, "logL=%d plane GB/s", logl[li]); report_sum(nm, "GB/s", gbs, nd);
              snprintf(nm, sizeof nm, "logL=%d pass-traffic GB/s", logl[li]); report_sum(nm, "GB/s", eff, nd); }
        }
        printf("paper: 103 GB/s @ logL=11, 1146 @ 14, ~1300 @ >=17 (per APU, 'batched NTT bandwidth')\n");
    }
    printf("\npaper: ~1.08 TB/s effective, 4 read+write passes per direction at log n = 31\n");
    return 0;
}

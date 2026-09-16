/* 20_cpu - the CPU side of the paper's pipeline, on the node's 96 Zen4 cores
 * (PLAN.md B6).
 *
 *   crt      crt_carry_par4: Garner reconstruction of 4 x 52-bit residues into
 *            a 206-bit coefficient, shifted-add at limb offset k (b = 64) in
 *            T stripes with spill buffers, then a sequential spill merge.
 *            Verified against GMP at 2^16 coefficients; timed at 2^30.
 *   repack   OpenMP copy of 2^31 limbs host->host (the hstage repack)
 *   stream   triad over 2^31 doubles, 192 threads
 *   seed     P,Q for spans of 512 terms of 1/k! with GMP; spans/s, and the
 *            time for all N/512 spans at d = 4e10 (N = 3.06e9)
 *   busy     crt again while all four APUs run a VALU kernel: interference
 *
 * Usage: 20_cpu [log2 coefficients for crt]
 */
#include "common_ntt.h"
#include <gmp.h>

static const uint64_t PR[4] = {3923057487904769ULL, 3641582511194113ULL,
                               2867526325239809ULL, 2586051348529153ULL};
typedef unsigned __int128 u128;

static uint64_t inv_mod(uint64_t a, uint64_t p)   /* a^(p-2) mod p */
{
    uint64_t r = 1, e = p - 2; a %= p;
    while (e) { if (e & 1) r = (uint64_t)((u128)r * a % p); a = (uint64_t)((u128)a * a % p); e >>= 1; }
    return r;
}
/* x (3 limbs, little endian) mod p, p < 2^52 */
static inline uint64_t mod3(const uint64_t *x, uint64_t p)
{
    u128 t = x[2] % p;
    t = ((t << 64) | x[1]) % p;
    t = ((t << 64) | x[0]) % p;
    return (uint64_t)t;
}
struct garner { uint64_t c1, c2, c3; uint64_t M1[2], M2[3]; };
static void garner_init(struct garner *g)
{
    u128 m1 = (u128)PR[0] * PR[1];
    g->c1 = inv_mod(PR[0] % PR[1], PR[1]);
    g->M1[0] = (uint64_t)m1; g->M1[1] = (uint64_t)(m1 >> 64);
    g->c2 = inv_mod((uint64_t)(m1 % PR[2]), PR[2]);
    /* M2 = M1 * P2, 3 limbs */
    { u128 lo = (u128)g->M1[0] * PR[2], hi = (u128)g->M1[1] * PR[2] + (lo >> 64);
      g->M2[0] = (uint64_t)lo; g->M2[1] = (uint64_t)hi; g->M2[2] = (uint64_t)(hi >> 64); }
    g->c3 = inv_mod(mod3(g->M2, PR[3]), PR[3]);
}
/* out[4] = value with residues r[4] */
static inline void garner4(const struct garner *g, const uint64_t r[4], uint64_t out[4])
{
    uint64_t x[4] = { r[0], 0, 0, 0 };
    u128 t;
    /* x += t1 * P0, t1 = (r1 - x) c1 mod P1 */
    { uint64_t xm = x[0] % PR[1]; uint64_t t1 = (uint64_t)((u128)((r[1] + PR[1] - xm) % PR[1]) * g->c1 % PR[1]);
      t = (u128)t1 * PR[0] + x[0]; x[0] = (uint64_t)t; x[1] = (uint64_t)(t >> 64); }
    /* x += t2 * M1, t2 = (r2 - x mod P2) c2 mod P2 */
    { uint64_t xm = (uint64_t)((((u128)x[1] % PR[2]) << 64 | x[0]) % PR[2]);
      uint64_t t2 = (uint64_t)((u128)((r[2] + PR[2] - xm) % PR[2]) * g->c2 % PR[2]);
      u128 lo = (u128)t2 * g->M1[0] + x[0], hi = (u128)t2 * g->M1[1] + x[1] + (lo >> 64);
      x[0] = (uint64_t)lo; x[1] = (uint64_t)hi; x[2] = (uint64_t)(hi >> 64); }
    /* x += t3 * M2 */
    { uint64_t xm = mod3(x, PR[3]);
      uint64_t t3 = (uint64_t)((u128)((r[3] + PR[3] - xm) % PR[3]) * g->c3 % PR[3]);
      u128 a0 = (u128)t3 * g->M2[0] + x[0];
      u128 a1 = (u128)t3 * g->M2[1] + x[1] + (a0 >> 64);
      u128 a2 = (u128)t3 * g->M2[2] + x[2] + (a1 >> 64);
      out[0] = (uint64_t)a0; out[1] = (uint64_t)a1; out[2] = (uint64_t)a2; out[3] = (uint64_t)(a2 >> 64); }
}

/* ---- D8: the same Garner ladder with FP64-Barrett reductions instead of
 * __int128 % p.  p < 2^52 so a*b < 2^104; q = floor(a*b/p) from doubles is
 * off by at most 2, and the true remainder fits in a signed 64-bit word, so
 * r = (a*b - q*p) mod 2^64 reinterpreted as int64 plus two corrections is
 * exact.  mod64(x) reduces any 64-bit x the same way. */
struct garner_f { uint64_t p[4], c1, c2, c3, M1[2], M2[3], r64_2, r64_3, r128_3; double pinv[4]; };
static inline uint64_t mulmod_f(uint64_t a, uint64_t b, uint64_t p, double pinv)
{
    double q = floor((double)a * (double)b * pinv);
    int64_t r = (int64_t)(a * b - (uint64_t)q * p);
    r += (r < 0) ? (int64_t)p : 0; r += (r < 0) ? (int64_t)p : 0;
    r -= (r >= (int64_t)p) ? (int64_t)p : 0; r -= (r >= (int64_t)p) ? (int64_t)p : 0;
    return (uint64_t)r;
}
static inline uint64_t mod64_f(uint64_t x, uint64_t p, double pinv)
{
    double q = floor((double)x * pinv);
    int64_t r = (int64_t)(x - (uint64_t)q * p);
    r += (r < 0) ? (int64_t)p : 0; r += (r < 0) ? (int64_t)p : 0;
    r -= (r >= (int64_t)p) ? (int64_t)p : 0; r -= (r >= (int64_t)p) ? (int64_t)p : 0;
    return (uint64_t)r;
}
static void garner_f_init(struct garner_f *g, const struct garner *gi)
{
    int i; u128 t;
    for (i = 0; i < 4; i++) { g->p[i] = PR[i]; g->pinv[i] = 1.0 / (double)PR[i]; }
    g->c1 = gi->c1; g->c2 = gi->c2; g->c3 = gi->c3;
    memcpy(g->M1, gi->M1, sizeof g->M1); memcpy(g->M2, gi->M2, sizeof g->M2);
    t = ((u128)1 << 64) % PR[2]; g->r64_2 = (uint64_t)t;
    t = ((u128)1 << 64) % PR[3]; g->r64_3 = (uint64_t)t;
    g->r128_3 = (uint64_t)(((u128)g->r64_3 * g->r64_3) % PR[3]);
}
static inline void garner4_f(const struct garner_f *g, const uint64_t r[4], uint64_t out[4])
{
    const uint64_t P1 = g->p[1], P2 = g->p[2], P3 = g->p[3];
    uint64_t x0 = r[0], x1, x2, xm, t1, t2, t3;
    u128 t;
    xm = mod64_f(x0, P1, g->pinv[1]);
    t1 = mulmod_f(r[1] + P1 - xm, g->c1, P1, g->pinv[1]);          /* r1+P1-xm < 2P1 < 2^53: fine for the double product */
    t = (u128)t1 * g->p[0] + x0; x0 = (uint64_t)t; x1 = (uint64_t)(t >> 64);
    xm = mulmod_f(mod64_f(x1, P2, g->pinv[2]), g->r64_2, P2, g->pinv[2]) + mod64_f(x0, P2, g->pinv[2]);
    if (xm >= P2) xm -= P2;
    t2 = mulmod_f(r[2] + P2 - xm, g->c2, P2, g->pinv[2]);
    { u128 lo = (u128)t2 * g->M1[0] + x0, hi = (u128)t2 * g->M1[1] + x1 + (lo >> 64);
      x0 = (uint64_t)lo; x1 = (uint64_t)hi; x2 = (uint64_t)(hi >> 64); }
    xm = mulmod_f(mod64_f(x2, P3, g->pinv[3]), g->r128_3, P3, g->pinv[3])
       + mulmod_f(mod64_f(x1, P3, g->pinv[3]), g->r64_3, P3, g->pinv[3]);
    if (xm >= P3) xm -= P3;
    xm += mod64_f(x0, P3, g->pinv[3]); if (xm >= P3) xm -= P3;
    t3 = mulmod_f(r[3] + P3 - xm, g->c3, P3, g->pinv[3]);
    { u128 a0 = (u128)t3 * g->M2[0] + x0, a1 = (u128)t3 * g->M2[1] + x1 + (a0 >> 64), a2 = (u128)t3 * g->M2[2] + x2 + (a1 >> 64);
      out[0] = (uint64_t)a0; out[1] = (uint64_t)a1; out[2] = (uint64_t)a2; out[3] = (uint64_t)(a2 >> 64); }
}

/* D8: the same striping with a tight carry window.  A coefficient is < 2^207,
 * so c[3] < 2^15 and a 3-limb pending window plus one carry bit suffices:
 *   out[k] = w0 + c0;  w0 = w1 + c1 + cy;  w1 = w2 + c2 + cy;  w2 = c3 + cy */
static void crt_carry_par_tight(const struct garner *g, uint64_t *const res[4], size_t n,
                                uint64_t *out, int T)
{
    uint64_t (*spill)[8] = (uint64_t (*)[8])calloc(T, sizeof *spill);
    int t;
#pragma omp parallel for num_threads(T) schedule(static)
    for (t = 0; t < T; t++) {
        size_t k0 = n * t / T, k1 = n * (t + 1) / T, k;
        uint64_t w0 = 0, w1 = 0, w2 = 0;
        for (k = k0; k < k1; k++) {
            uint64_t r[4] = { res[0][k], res[1][k], res[2][k], res[3][k] }, c[4];
            u128 s;
            garner4(g, r, c);
            s = (u128)w0 + c[0]; out[k] = (uint64_t)s;
            s = (s >> 64) + w1 + c[1]; w0 = (uint64_t)s;
            s = (s >> 64) + w2 + c[2]; w1 = (uint64_t)s;
            w2 = (uint64_t)(s >> 64) + c[3];
        }
        spill[t][0] = w0; spill[t][1] = w1; spill[t][2] = w2;
    }
    for (t = 0; t < T; t++) {
        size_t k1 = n * (t + 1) / T, i; u128 s = 0;
        for (i = 0; i < 3; i++) { s += (u128)out[k1 + i] + spill[t][i]; out[k1 + i] = (uint64_t)s; s >>= 64; }
        for (i = 3; s && k1 + i < n + 4; i++) { s += out[k1 + i]; out[k1 + i] = (uint64_t)s; s >>= 64; }
    }
    free(spill);
}

/* mode 0: full (int128 %); 1: full, FP64-Barrett Garner; 2: Garner only, no
 * carry (out[k] = xor of the 4 result limbs); 3: memory only (out[k] = xor of
 * the 4 residues) -- the floor; 4: tight carry window */
static int g_crtmode = 0;
static const struct garner_f *g_gf;

/* residues: 4 planes of n; result: n + 4 limbs (zeroed by caller) */
static void crt_carry_par(const struct garner *g, uint64_t *const res[4], size_t n,
                          uint64_t *out, int T)
{
    uint64_t (*spill)[8] = (uint64_t (*)[8])calloc(T, sizeof *spill);
    int t;
#pragma omp parallel for num_threads(T) schedule(static)
    for (t = 0; t < T; t++) {
        size_t k0 = n * t / T, k1 = n * (t + 1) / T, k;
        uint64_t acc[8] = {0};              /* sliding window: acc[i] = limb k+i */
        for (k = k0; k < k1; k++) {
            uint64_t r[4] = { res[0][k], res[1][k], res[2][k], res[3][k] }, c[4];
            u128 s; int i;
            if (g_crtmode == 3) { out[k] = r[0] ^ r[1] ^ r[2] ^ r[3]; continue; }
            if (g_crtmode == 1) garner4_f(g_gf, r, c); else garner4(g, r, c);
            if (g_crtmode == 2) { out[k] = c[0] ^ c[1] ^ c[2] ^ c[3]; continue; }
            s = (u128)acc[0] + c[0]; out[k] = (uint64_t)s; s >>= 64;
            for (i = 1; i < 4; i++) { s += (u128)acc[i] + c[i]; acc[i - 1] = (uint64_t)s; s >>= 64; }
            for (i = 4; i < 8; i++) { s += acc[i]; acc[i - 1] = (uint64_t)s; s >>= 64; }
            acc[7] = 0;
        }
        memcpy(spill[t], acc, sizeof acc);   /* limbs k1 .. k1+7 pending */
    }
    for (t = 0; t < T; t++) {               /* sequential merge with carry ripple */
        size_t k1 = n * (t + 1) / T, i; u128 s = 0;
        for (i = 0; i < 8; i++) { s += (u128)out[k1 + i] + spill[t][i]; out[k1 + i] = (uint64_t)s; s >>= 64; }
        for (i = 8; s && k1 + i < n + 4; i++) { s += out[k1 + i]; out[k1 + i] = (uint64_t)s; s >>= 64; }
    }
    free(spill);
}

__global__ void k_busy(uint64_t p, uint64_t c, uint64_t cp, int iters, uint64_t *sink)
{
    uint64_t x = threadIdx.x + 1; int i;
    for (i = 0; i < iters; i++) { uint64_t q = __umul64hi(cp, x); x = c * x - q * p; }
    if (x == 7) *sink = x;
}

static uint64_t splitmix(uint64_t *s) { uint64_t z = (*s += 0x9E3779B97F4A7C15ULL); z ^= z >> 30; z *= 0xBF58476D1CE4E5B9ULL; z ^= z >> 27; z *= 0x94D049BB133111EBULL; return z ^ (z >> 31); }

int main(int argc, char **argv)
{
    int lg = argc > 1 ? atoi(argv[1]) : 30;
    size_t n = (size_t)1 << lg, k;
    struct garner g;
    uint64_t *res[4], *out;
    double t0, t1, v[1];
    int nd = device_count(), i;

    printf("== 20_cpu : CRT+carry, repack, STREAM, seed spans on %d CPUs ==\n", omp_get_num_procs());
    meta("20_cpu");
    garner_init(&g);
    { static struct garner_f gf; garner_f_init(&gf, &g); g_gf = &gf; }

    /* ---- verify at 2^16 vs GMP ------------------------------------------- */
    {
        size_t m = 1 << 16; uint64_t seed = 42; mpz_t acc, c, pw; int bad = 0;
        for (i = 0; i < 4; i++) res[i] = (uint64_t *)malloc(m * 8);
        out = (uint64_t *)calloc(m + 4, 8);
        mpz_inits(acc, c, pw, NULL);
        for (k = 0; k < m; k++) {
            /* random 206-bit value, take residues */
            uint64_t w[4] = { splitmix(&seed), splitmix(&seed), splitmix(&seed), splitmix(&seed) & 0x3FFF };
            mpz_import(c, 4, -1, 8, 0, 0, w);
            for (i = 0; i < 4; i++) res[i][k] = mpz_fdiv_ui(c, PR[i]);
            mpz_mul_2exp(pw, c, 64 * k); mpz_add(acc, acc, pw);
        }
        crt_carry_par(&g, res, m, out, 96);
        { size_t cnt; uint64_t *ref = (uint64_t *)calloc(m + 4, 8);
          mpz_export(ref, &cnt, -1, 8, 0, 0, acc);
          for (k = 0; k < m + 4; k++) if (ref[k] != out[k]) bad++;
          printf("VERIFY crt_carry_par (96 stripes, 2^16 coefficients) vs GMP: %s\n", bad ? "FAILED" : "OK");
          if (bad) return 1;
          memset(out, 0, (m + 4) * 8); g_crtmode = 1; crt_carry_par(&g, res, m, out, 96); g_crtmode = 0;
          for (k = 0; k < m + 4; k++) if (ref[k] != out[k]) bad++;
          printf("VERIFY FP64-Barrett Garner (D8) vs GMP: %s\n", bad ? "FAILED" : "OK");
          if (bad) return 1;
          memset(out, 0, (m + 4) * 8); crt_carry_par_tight(&g, res, m, out, 96);
          for (k = 0; k < m + 4; k++) if (ref[k] != out[k]) bad++;
          printf("VERIFY tight carry window (D8) vs GMP: %s\n", bad ? "FAILED" : "OK");
          if (bad) return 1;
          free(ref); }
        for (i = 0; i < 4; i++) free(res[i]); free(out);
        mpz_clears(acc, c, pw, NULL);
    }

    /* ---- time at 2^lg ------------------------------------------------------ */
    for (i = 0; i < 4; i++) { res[i] = (uint64_t *)malloc(n * 8); }
    out = (uint64_t *)calloc(n + 4, 8);
#pragma omp parallel for num_threads(192)
    for (k = 0; k < n; k++) { uint64_t s = k * 7 + 1; int j; for (j = 0; j < 4; j++) res[j][k] = splitmix(&s) % PR[j]; }
    {
        int T[3] = { 48, 96, 192 }, ti;
        for (ti = 0; ti < 3; ti++) {
            char nm[40];
            t0 = omp_get_wtime(); crt_carry_par(&g, res, n, out, T[ti]); t1 = omp_get_wtime();
            printf("crt+carry 2^%d coefficients, %3d stripes: %.3f s  (%.2f Gcoef/s)\n", lg, T[ti], t1 - t0, n / (t1 - t0) / 1e9);
            snprintf(nm, sizeof nm, "crt_2^%d_T%d", lg, T[ti]); v[0] = t1 - t0; result(nm, "s", v[0], v, 1);
        }
        /* D8 decomposition at 192 stripes */
        {
            static const char *mn[5] = { "int128 % Garner + carry", "FP64-Barrett Garner + carry", "Garner only (int128 %)", "memory only (floor)", "tight 4-limb carry window" };
            int md; double tm[5]; char nm[40];
            for (md = 0; md < 5; md++) {
                int rep2; double best = 1e300;
                for (rep2 = 0; rep2 < 3; rep2++) {
                    g_crtmode = md < 4 ? md : 0;
                    t0 = omp_get_wtime();
                    if (md == 4) crt_carry_par_tight(&g, res, n, out, 192); else crt_carry_par(&g, res, n, out, 192);
                    t1 = omp_get_wtime();
                    if (t1 - t0 < best) best = t1 - t0;
                }
                tm[md] = best;
                printf("D8 %-32s 2^%d, 192 stripes: %.3f s  (%.1f GB/s of residue+result traffic)\n", mn[md], lg, best, 40.0 * n / best / 1e9);
                snprintf(nm, sizeof nm, "D8_mode%d", md); v[0] = best; result(nm, "s", v[0], v, 1);
            }
            g_crtmode = 0;
            printf("D8: arithmetic share = %.0f%% (mode 2 - mode 3) / mode 0; FP64 Garner speedup %.2fx; tight carry %.2fx\n",
                   100.0 * (tm[2] - tm[3]) / tm[0], tm[0] / tm[1], tm[0] / tm[4]);
        }
    }
    /* ---- repack and stream --------------------------------------------------- */
    {
        uint64_t *dst = (uint64_t *)malloc(n * 8); double *a = (double *)res[2], *b = (double *)res[3], *c = (double *)res[0];
        t0 = omp_get_wtime();
#pragma omp parallel for num_threads(192) schedule(static)
        for (k = 0; k < n; k++) dst[k] = res[1][k];
        t1 = omp_get_wtime();
        printf("repack (copy) 2^%d limbs, 192 threads: %.3f s = %.1f GB/s\n", lg, t1 - t0, 16.0 * n / (t1 - t0) / 1e9);
        v[0] = 16.0 * n / (t1 - t0) / 1e9; result("repack_copy", "GB/s", v[0], v, 1);
        t0 = omp_get_wtime();
#pragma omp parallel for num_threads(192) schedule(static)
        for (k = 0; k < n; k++) a[k] = b[k] + 3.0 * c[k];
        t1 = omp_get_wtime();
        printf("stream triad 2^%d doubles, 192 threads: %.1f GB/s\n", lg, 24.0 * n / (t1 - t0) / 1e9);
        v[0] = 24.0 * n / (t1 - t0) / 1e9; result("stream_triad", "GB/s", v[0], v, 1);
        free(dst);
    }
    /* ---- seed spans ---------------------------------------------------------- */
    {
        const int SPAN = 512, NS = 4096; int s;
        double spans_s; const double Nterms = 3.06e9;
        t0 = omp_get_wtime();
#pragma omp parallel for num_threads(192) schedule(dynamic)
        for (s = 0; s < NS; s++) {
            /* P(a,b), Q(a,b) for [a, a+512): Q = prod (a+1..b), P = sum_{k} Q(k,b)... 2-var recursion */
            mpz_t P, Q; unsigned long a = 1000000000UL + (unsigned long)s * SPAN, kk;
            mpz_init_set_ui(P, 1); mpz_init_set_ui(Q, a + 1);     /* P(a,a+1)=1, Q=a+1 */
            for (kk = a + 2; kk <= a + SPAN; kk++) {               /* merge one term: P = P*kk + 1, Q = Q*kk */
                mpz_mul_ui(P, P, kk); mpz_add_ui(P, P, 1); mpz_mul_ui(Q, Q, kk);
            }
            if (mpz_sizeinbase(Q, 2) == 0) printf("?");
            mpz_clears(P, Q, NULL);
        }
        t1 = omp_get_wtime();
        spans_s = NS / (t1 - t0);
        printf("seed spans (512 terms, GMP, 192 threads): %.0f spans/s -> all %.2e spans at d=4e10: %.1f s\n",
               spans_s, Nterms / SPAN, Nterms / SPAN / spans_s);
        v[0] = Nterms / SPAN / spans_s; result("seed_phase_est", "s", v[0], v, 1);
    }
    /* ---- interference: crt while all APUs run a VALU kernel ------------------ */
    {
        double tb, tq; volatile int stop = 0;
        omp_set_max_active_levels(2);        /* the CRT's own parallel region nests inside this one */
        t0 = omp_get_wtime(); crt_carry_par(&g, res, n, out, 96); tq = omp_get_wtime() - t0;
#pragma omp parallel num_threads(nd + 1)
        {
            int id = omp_get_thread_num();
            if (id < nd) {
                uint64_t *sink; HIP_CHECK(hipSetDevice(id)); HIP_CHECK(hipMalloc(&sink, 8));
                while (!stop) { k_busy<<<228 * 8, 256>>>(PR[0], 12345, 0x10000000000ULL, 20000, sink); HIP_CHECK(hipDeviceSynchronize()); }
                HIP_CHECK(hipFree(sink));
            } else {
                double s0 = omp_get_wtime(); crt_carry_par(&g, res, n, out, 96); tb = omp_get_wtime() - s0; stop = 1;
            }
        }
        printf("crt+carry 96 stripes: quiet %.3f s, with all 4 APUs busy %.3f s (%.1f%% slower)\n", tq, tb, 100 * (tb / tq - 1));
        v[0] = 100 * (tb / tq - 1); result("crt_gpu_interference", "%", v[0], v, 1);
    }
    printf("\npaper: 10dP phase 12.6 s includes CRT of ~2^31 coefficients; PLAN.md B6 pass: <= 3 s.\n");
    return 0;
}

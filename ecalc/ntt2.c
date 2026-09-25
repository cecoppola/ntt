/* ntt2.c - engine 2: the same tiled DIF/DIT kernels with u64 Montgomery arithmetic on two 62-bit primes (derived from ntt.c).
 * See ntt.h.  Kernels are the verified ones from bench/16_ntt_tile (RESULTS.md
 * 25, 33), made two-directional and given the scale / pointwise fusions.
 */
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include "ntt2.h"

#define BP 17                      /* LDS pad: sh[128][17], conflict-free columns */
#define THREADS 256
#define B1_LGL 10                  /* b1 block length 2^10 (rule 3b) */

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)

int ntt2_stg = 7;
int ntt2_pw_fuse = 14;
int ntt2_b16_body = 1;      /* NTT_B16_BODY: 0 tile kernel (paper), 1 register-blocked body for 7-stage passes, 2 = 1 + radix-4 stages */

/* ------------------------------------------------------------------------ */
/* b16 pass: STG stages over tiles of 2^STG elements spaced hmin = 2^s_lo
 * apart; a block holds 16 adjacent columns of 128 rows (2048 elements) in
 * sh[128][17].  Thread (tt = tid>>4, bb = tid&15) owns column bb.
 * Forward (INV = 0): stages s_lo+STG-1 .. s_lo, DIF butterflies, twiddle
 * after the difference.  Inverse (INV = 1): stages ascending, DIT, twiddle
 * before the sum, inverse roots in the tables; scale != 0 multiplies the
 * stored values by it and makes them canonical (the n^-1 fusion).
 * Twiddle of stage H (tile-local), row r, global column c:
 *   w = tabT[r * tile/(2H)] * T_H,  T_H = w_n^(c n / (2 H hmin)),
 * T for the top stage from the two-level table (tlo, thi), lower stages by
 * squaring. */
template <int STG, int INV>
__global__ __launch_bounds__(THREADS)
void k_b16(uint64_t *x, int logn, int s_lo, e2_mod m, const uint64_t *tlo, const uint64_t *thi,
           const uint64_t *tabT, uint64_t scale)
{
    constexpr int tile = 1 << STG, NS = 128 / tile;
    constexpr int NT = tile == 128 ? 1 : tile == 64 ? 2 : 4;
    __shared__ uint64_t sh[128 * BP + 1];
    __shared__ uint64_t tab[tile];
    const int tt = threadIdx.x >> 4, bb = threadIdx.x & 15;
    const size_t n = (size_t)1 << logn, hmin = (size_t)1 << s_lo, slabs = hmin / 16;
    const size_t bpt = n / 2048, b = blockIdx.x % bpt, t = blockIdx.x / bpt;
    const size_t groups = slabs / NS, blk_hi = b / groups, slab0 = (b % groups) * NS;
    const size_t base = t * n + blk_hi * (size_t)tile * hmin + slab0 * 16;
    const uint64_t pu = m.p, p2 = m.p2;
    uint64_t T[NT][INV ? STG : 1];
    int j, k, mm_;

    for (k = threadIdx.x; k < tile; k += THREADS) tab[k] = tabT[k];
#pragma unroll
    for (j = tt; j < 128; j += 16)
        sh[j * BP + bb] = x[base + (size_t)(j >> STG) * 16 + (size_t)(j & (tile - 1)) * hmin + bb];
#pragma unroll
    for (mm_ = 0; mm_ < NT; mm_++) {
        int ti = NT == 1 ? 0 : NT == 2 ? mm_ : ((tt + 16 * mm_) >> (STG - 1));
        size_t c = (slab0 + ti) * 16 + bb;
        T[mm_][INV ? STG - 1 : 0] = e2_mm(tlo[c & 4095], thi[c >> 12], m);
        if (INV) {
#pragma unroll
            for (int l = STG - 1; l > 0; l--) T[mm_][l - 1] = e2_mm(T[mm_][l], T[mm_][l], m);
        }
    }
    __syncthreads();
#pragma unroll
    for (int st = 0; st < STG; st++) {
        const int lgH = INV ? st : STG - 1 - st;
        const int H = 1 << lgH, lgstep = STG - 1 - lgH;
#pragma unroll
        for (mm_ = 0; mm_ < 4; mm_++) {
            int kk = tt + 16 * mm_, ti = kk >> (STG - 1), kl = kk & (tile / 2 - 1);
            int r = kl & (H - 1), q = kl >> lgH, j0 = ti * tile + (q << (lgH + 1)) + r, j1 = j0 + H;
            int tm = NT == 1 ? 0 : NT == 2 ? (mm_ >> 1) : mm_;
            uint64_t u = sh[j0 * BP + bb], v = sh[j1 * BP + bb];
            uint64_t w = e2_mm(tab[r << lgstep], T[tm][INV ? lgH : 0], m);
            if (INV) {
                uint64_t vw = e2_mm(v, w, m);                               /* [0,2p) */
                uint64_t sum = u + vw, d = u + p2 - vw;                     /* < 4p */
                if (sum >= p2) sum -= p2;
                if (d >= p2) d -= p2;
                sh[j0 * BP + bb] = sum;
                sh[j1 * BP + bb] = d;
            } else {
                uint64_t sum = u + v, d = u - v + p2;                       /* < 4p */
                if (sum >= p2) sum -= p2;
                if (d >= p2) d -= p2;
                sh[j0 * BP + bb] = sum;
                sh[j1 * BP + bb] = e2_mm(d, w, m);
            }
        }
        if (!INV) {
#pragma unroll
            for (mm_ = 0; mm_ < NT; mm_++) T[mm_][0] = e2_mm(T[mm_][0], T[mm_][0], m);
        }
        __syncthreads();
    }
#pragma unroll
    for (j = tt; j < 128; j += 16) {
        uint64_t v = sh[j * BP + bb];
        if (scale) v = e2_fold(e2_mm(v, scale, m), pu);            /* x n^-1 (plain) and out of Montgomery form */
        x[base + (size_t)(j >> STG) * 16 + (size_t)(j & (tile - 1)) * hmin + bb] = v;
    }
}


/* Register-blocked body for a 7-stage pass (tile 128, NS = 1): thread
 * (tt = tid>>4, bb = tid&15) keeps 8 rows of column bb in registers and runs
 * the stages in three groups with two LDS exchanges (the tile kernel does an
 * LDS round trip per stage):
 *   group A  h = 64, 32, 16   rows tt + 16 i
 *   group B  h = 8, 4, 2      rows ((tt>>1)<<4) | (i<<1) | (tt&1)
 *   group C  h = 1            rows 8 tt + i
 * Same butterflies, folds and twiddle products as k_b16, so the output is
 * bit-identical.  INV runs the groups in reverse (DIT). */
template <int INV, int R4>
__global__ __launch_bounds__(THREADS)
void k_b16r(uint64_t *x, int logn, int s_lo, e2_mod m, const uint64_t *tlo, const uint64_t *thi,
            const uint64_t *tabT, uint64_t scale)
{
    constexpr int STG = 7, tile = 128;
    __shared__ uint64_t sh[128 * BP + 1];
    __shared__ uint64_t tab[tile];
    const int tt = threadIdx.x >> 4, bb = threadIdx.x & 15;
    const size_t n = (size_t)1 << logn, hmin = (size_t)1 << s_lo, slabs = hmin / 16;
    const size_t bpt = n / 2048, b = blockIdx.x % bpt, t = blockIdx.x / bpt;
    const size_t blk_hi = b / slabs, slab0 = b % slabs;
    const size_t base = t * n + blk_hi * (size_t)tile * hmin + slab0 * 16;
    const uint64_t pu = m.p, p2 = m.p2;
    uint64_t v[8];
    uint64_t T[INV ? STG : 1];
    int i;

    for (int k = threadIdx.x; k < tile; k += THREADS) tab[k] = tabT[k];
    {
        size_t c = slab0 * 16 + bb;
        T[INV ? STG - 1 : 0] = e2_mm(tlo[c & 4095], thi[c >> 12], m);
        if (INV) {
#pragma unroll
            for (int l = STG - 1; l > 0; l--) T[l - 1] = e2_mm(T[l], T[l], m);
        }
    }
    __syncthreads();

#define ROW_A(i) (tt + 16 * (i))
#define ROW_B(i) ((((tt) >> 1) << 4) | ((i) << 1) | ((tt) & 1))
#define ROW_C(i) (8 * (tt) + (i))
#define BFLY(u_, v_, w_) do {                                                         \
        if (INV) { uint64_t vw = e2_mm((v_), (w_), m);                                \
                   uint64_t s_ = (u_) + vw, d_ = (u_) + p2 - vw;                      \
                   if (s_ >= p2) s_ -= p2; if (d_ >= p2) d_ -= p2; (u_) = s_; (v_) = d_; } \
        else { uint64_t s_ = (u_) + (v_), d_ = (u_) - (v_) + p2;                      \
               if (s_ >= p2) s_ -= p2; if (d_ >= p2) d_ -= p2;                        \
               (u_) = s_; (v_) = e2_mm(d_, (w_), m); } } while (0)
    /* one stage on the registers: register bit `rb` pairs (i, i + 2^rb); r(i) = row & (H-1) */
#define STAGE(lgH, rb, ROWF)                                                          \
    do {                                                                              \
        const int H_ = 1 << (lgH), lgstep_ = STG - 1 - (lgH);                         \
        _Pragma("unroll")                                                             \
        for (int i_ = 0; i_ < 8; i_++) if (!(i_ & (1 << (rb)))) {                     \
            int r_ = ROWF(i_) & (H_ - 1);                                             \
            uint64_t w_ = e2_mm(tab[r_ << lgstep_], T[INV ? (lgH) : 0], m);          \
            BFLY(v[i_], v[i_ + (1 << (rb))], w_);                                     \
        }                                                                             \
        if (!INV) T[0] = e2_mm(T[0], T[0], m);                                        \
    } while (0)
    /* radix-4: stages lgH and lgH-1 together (register bits rb and rb-1).  With
     * a = v[i], b = v[i + 2^(rb-1)], c = v[i + 2^rb], d = v[i + 3 2^(rb-1)] and
     * w = w_{2H}^r (r = row & (H-1), which has bit H/2 clear): the stage-H pair
     * (b, d) uses w w_4 and both stage-H/2 pairs use w^2 - the same values the
     * two radix-2 stages form, so the output is bit-identical with 3 twiddle
     * modmuls per 4 elements instead of 4.  Forward: H then H/2; inverse: H/2
     * then H. */
#define STAGE4(lgH, rb, ROWF)                                                         \
    do {                                                                              \
        const int H_ = 1 << (lgH), lgstep_ = STG - 1 - (lgH);                         \
        const uint64_t w4_ = tab[tile / 4];                                             \
        _Pragma("unroll")                                                             \
        for (int i_ = 0; i_ < 8; i_++) if (!(i_ & (3 << ((rb) - 1)))) {               \
            int r_ = ROWF(i_) & (H_ - 1);                                             \
            uint64_t w_ = e2_mm(tab[r_ << lgstep_], T[INV ? (lgH) : 0], m);          \
            uint64_t w2_ = e2_mm(w_, w_, m), wi_ = e2_mm(w_, w4_, m);                 \
            const int ib = i_ + (1 << ((rb) - 1)), ic = i_ + (1 << (rb)), id = ic + (1 << ((rb) - 1)); \
            if (INV) { BFLY(v[i_], v[ib], w2_); BFLY(v[ic], v[id], w2_);              \
                       BFLY(v[i_], v[ic], w_);  BFLY(v[ib], v[id], wi_); }            \
            else     { BFLY(v[i_], v[ic], w_);  BFLY(v[ib], v[id], wi_);              \
                       BFLY(v[i_], v[ib], w2_); BFLY(v[ic], v[id], w2_); }            \
        }                                                                             \
        if (!INV) { T[0] = e2_mm(T[0], T[0], m); T[0] = e2_mm(T[0], T[0], m); }       \
    } while (0)
#define LOAD_G(ROWF)  _Pragma("unroll") for (i = 0; i < 8; i++) v[i] = x[base + (size_t)ROWF(i) * hmin + bb]
#define STORE_G(ROWF) _Pragma("unroll") for (i = 0; i < 8; i++) { uint64_t o = v[i];                        \
                          if (scale) o = e2_fold(e2_mm(o, scale, m), pu);                                   \
                          x[base + (size_t)ROWF(i) * hmin + bb] = o; }
#define TO_SH(ROWF)   _Pragma("unroll") for (i = 0; i < 8; i++) sh[ROWF(i) * BP + bb] = v[i]
#define FROM_SH(ROWF) _Pragma("unroll") for (i = 0; i < 8; i++) v[i] = sh[ROWF(i) * BP + bb]

    if (!INV) {
        LOAD_G(ROW_A);
        if (R4) { STAGE4(6, 2, ROW_A); STAGE(4, 0, ROW_A); } else { STAGE(6, 2, ROW_A); STAGE(5, 1, ROW_A); STAGE(4, 0, ROW_A); }
        TO_SH(ROW_A); __syncthreads(); FROM_SH(ROW_B);
        if (R4) { STAGE4(3, 2, ROW_B); STAGE(1, 0, ROW_B); } else { STAGE(3, 2, ROW_B); STAGE(2, 1, ROW_B); STAGE(1, 0, ROW_B); }
        __syncthreads(); TO_SH(ROW_B); __syncthreads(); FROM_SH(ROW_C);
        STAGE(0, 0, ROW_C);
        STORE_G(ROW_C);
    } else {
        LOAD_G(ROW_C);
        STAGE(0, 0, ROW_C);
        TO_SH(ROW_C); __syncthreads(); FROM_SH(ROW_B);
        if (R4) { STAGE(1, 0, ROW_B); STAGE4(3, 2, ROW_B); } else { STAGE(1, 0, ROW_B); STAGE(2, 1, ROW_B); STAGE(3, 2, ROW_B); }
        __syncthreads(); TO_SH(ROW_B); __syncthreads(); FROM_SH(ROW_A);
        if (R4) { STAGE(4, 0, ROW_A); STAGE4(6, 2, ROW_A); } else { STAGE(4, 0, ROW_A); STAGE(5, 1, ROW_A); STAGE(6, 2, ROW_A); }
        STORE_G(ROW_A);
    }
#undef ROW_A
#undef ROW_B
#undef ROW_C
#undef BFLY
#undef STAGE
#undef STAGE4
#undef LOAD_G
#undef STORE_G
#undef TO_SH
#undef FROM_SH
}

/* b1 pass on contiguous 2^LGL-point blocks.  MODE 0: forward stages LGL-1..0,
 * canonical output.  MODE 1: inverse stages 0..LGL-1.  MODE 2: inverse with
 * the pointwise product x[i] y[i] fused into the load. */
template <int LGL, int MODE>
__global__ __launch_bounds__(THREADS)
void k_b1(uint64_t *x, const uint64_t *y, size_t ymask, e2_mod m, const uint64_t *tab1, uint64_t scale)
{
    constexpr int L = 1 << LGL;
    __shared__ uint64_t sh[L];
    __shared__ uint64_t tab[L / 2];
    const size_t base = (size_t)blockIdx.x * L;
    const uint64_t pu = m.p, p2 = m.p2;
    int k;
    for (k = threadIdx.x; k < L / 2; k += THREADS) tab[k] = tab1[k];
    for (k = threadIdx.x; k < L; k += THREADS)
        sh[k] = MODE == 2 ? e2_mm(x[base + k], y[(base + k) & ymask], m) : x[base + k];
    __syncthreads();
#pragma unroll
    for (int st = 0; st < LGL; st++) {
        const int lgH = MODE ? st : LGL - 1 - st;
        const int H = 1 << lgH, lgstep = LGL - 1 - lgH;
#pragma unroll
        for (k = threadIdx.x; k < L / 2; k += THREADS) {
            int r = k & (H - 1), q = k >> lgH, j0 = (q << (lgH + 1)) + r, j1 = j0 + H;
            uint64_t u = sh[j0], v = sh[j1];
            if (MODE) {
                uint64_t vw = e2_mm(v, tab[r << lgstep], m);
                uint64_t sum = u + vw, d = u + p2 - vw;
                if (sum >= p2) sum -= p2;
                if (d >= p2) d -= p2;
                sh[j0] = sum; sh[j1] = d;
            } else {
                uint64_t sum = u + v, d = u - v + p2;
                if (sum >= p2) sum -= p2;
                if (d >= p2) d -= p2;
                sh[j0] = sum;
                sh[j1] = e2_mm(d, tab[r << lgstep], m);
            }
        }
        __syncthreads();
    }
    for (k = threadIdx.x; k < L; k += THREADS) {
        uint64_t v = sh[k];
        if (MODE == 0) { if (v >= pu) v -= pu; }
        else if (scale) v = e2_fold(e2_mm(v, scale, m), pu);
        x[base + k] = v;
    }
}

__global__ void k_pw(uint64_t *x, const uint64_t *y, size_t ymask, size_t n, e2_mod m)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) x[i] = e2_mm(x[i], y[i & ymask], m);
}
/* 45-bit points from 64-bit limbs, into Montgomery form */
__global__ void k_load(uint64_t *dst, const uint64_t *src, size_t nlimbs, size_t npoints, e2_mod m)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < npoints; i += stride) {
        size_t bit = i * E2_BITS, l = bit >> 6; unsigned o = (unsigned)(bit & 63);
        uint64_t v = 0;
        if (l < nlimbs) {
            v = src[l] >> o;
            if (o > 64 - E2_BITS && l + 1 < nlimbs) v |= src[l + 1] << (64 - o);
            v &= ((uint64_t)1 << E2_BITS) - 1;
        }
        dst[i] = v ? e2_mm(v, m.r2, m) : 0;
    }
}

/* ------------------------------------------------------------------------ */
struct pass_tw { uint64_t *tlo, *thi; };             /* per pass, per direction */
struct plan_tw { int built; struct pass_tw f[NTT_MAXPASS], i[NTT_MAXPASS]; };

struct ntt2_ctx {
    int prime;
    e2_mod m;
    uint64_t *tabT_f[8], *tabT_i[8];               /* 2^stg-th roots (Montgomery), stg 1..7 */
    uint64_t *tab1_f, *tab1_i;
    uint64_t ninv[NTT_LOGN_MAX + 1];               /* plain n^-1: the scale multiply also leaves Montgomery form */
    struct plan_tw plan[NTT_LOGN_MAX + 1];
};

struct plan { int npass, s_lo[NTT_MAXPASS], s_hi[NTT_MAXPASS]; };
static void make_plan(struct plan *pl, int logn)
{
    int hi = logn - 1, stg = ntt2_stg < 3 ? 3 : ntt2_stg > 7 ? 7 : ntt2_stg;
    pl->npass = 0;
    while (hi >= B1_LGL) {
        int lo = hi - stg + 1; if (lo < B1_LGL) lo = B1_LGL;
        if (pl->npass == NTT_MAXPASS) { ec_fatal(EC_RC_FATAL, "ntt: too many passes for logn %d stg %d\n", logn, stg); }
        pl->s_hi[pl->npass] = hi; pl->s_lo[pl->npass] = lo; pl->npass++;
        hi = lo - 1;
    }
}
int ntt2_npass(int logn) { struct plan pl; make_plan(&pl, logn); return pl.npass + 1; }
void ntt2_pass_bounds(int logn, int pass, int *s_lo, int *s_hi)
{
    struct plan pl; make_plan(&pl, logn);
    if (pass < pl.npass) { *s_lo = pl.s_lo[pass]; *s_hi = pl.s_hi[pass]; }
    else { *s_lo = 0; *s_hi = B1_LGL - 1; }
}

static uint64_t *dev_table_pow(uint64_t w, size_t cnt, uint64_t p, const e2_mod m)
{
    uint64_t *h = (uint64_t *)malloc(cnt * sizeof *h), *d;
    uint64_t a = 1;
    for (size_t k = 0; k < cnt; k++) { h[k] = e2_fold(e2_to_mont(a, m), p); a = e2_mulmod_ref(a, w, p); }
    HIP_CHECK(hipMalloc(&d, cnt * sizeof *d));
    HIP_CHECK(hipMemcpy(d, h, cnt * sizeof *d, hipMemcpyHostToDevice));
    free(h);
    return d;
}

ntt2_ctx *ntt2_ctx_create(int prime)
{
    ntt2_ctx *c = (ntt2_ctx *)calloc(1, sizeof *c);
    uint64_t p = e2_P[prime];
    c->prime = prime;
    c->m = e2_mod_get(prime);
    for (int stg = 1; stg <= 7; stg++) {
        c->tabT_f[stg] = dev_table_pow(e2_root(prime, stg), (size_t)1 << stg, p, c->m);
        c->tabT_i[stg] = dev_table_pow(e2_root_inv(prime, stg), (size_t)1 << stg, p, c->m);
    }
    c->tab1_f = dev_table_pow(e2_root(prime, B1_LGL), 1 << (B1_LGL - 1), p, c->m);
    c->tab1_i = dev_table_pow(e2_root_inv(prime, B1_LGL), 1 << (B1_LGL - 1), p, c->m);
    for (int l = 0; l <= NTT_LOGN_MAX; l++) c->ninv[l] = e2_inv(e2_powmod(2, l, p), p);
    return c;
}
void ntt2_ctx_free(ntt2_ctx *c)
{
    if (!c) return;
    for (int stg = 1; stg <= 7; stg++) { HIP_CHECK(hipFree(c->tabT_f[stg])); HIP_CHECK(hipFree(c->tabT_i[stg])); }
    HIP_CHECK(hipFree(c->tab1_f)); HIP_CHECK(hipFree(c->tab1_i));
    for (int l = 0; l <= NTT_LOGN_MAX; l++) if (c->plan[l].built)
        for (int i = 0; i < NTT_MAXPASS; i++) {
            if (c->plan[l].f[i].tlo) { HIP_CHECK(hipFree(c->plan[l].f[i].tlo)); HIP_CHECK(hipFree(c->plan[l].f[i].thi)); }
            if (c->plan[l].i[i].tlo) { HIP_CHECK(hipFree(c->plan[l].i[i].tlo)); HIP_CHECK(hipFree(c->plan[l].i[i].thi)); }
        }
    free(c);
}
int ntt2_ctx_prime(const ntt2_ctx *c) { return c->prime; }

/* pass twiddle base tables: T_top(c) = wK^c, wK = w_n^(n / 2^(s_hi+1)), the
 * primitive 2^(s_hi+1)-th root; c < hmin = 2^s_lo.  tlo[k] = wK^k (4096),
 * thi[k] = wK^(4096 k) (2^(s_lo-12) entries, at least 1). */
static void build_pass_tw(struct pass_tw *t, int prime, int s_lo, int s_hi, int inv)
{
    uint64_t p = e2_P[prime];
    uint64_t wK = inv ? e2_root_inv(prime, s_hi + 1) : e2_root(prime, s_hi + 1);
    size_t nhi = s_lo > 12 ? (size_t)1 << (s_lo - 12) : 1;
    e2_mod m = e2_mod_get(prime);
    t->tlo = dev_table_pow(wK, 4096, p, m);
    t->thi = dev_table_pow(e2_powmod(wK, 4096, p), nhi, p, m);
}
static struct plan_tw *get_plan_tw(ntt2_ctx *c, int logn, const struct plan *pl)
{
    struct plan_tw *pt = &c->plan[logn];
    if (!pt->built) {
        for (int i = 0; i < pl->npass; i++) {
            build_pass_tw(&pt->f[i], c->prime, pl->s_lo[i], pl->s_hi[i], 0);
            build_pass_tw(&pt->i[i], c->prime, pl->s_lo[i], pl->s_hi[i], 1);
        }
        pt->built = 1;
    }
    return pt;
}

static void check_logn(int logn)
{
    if (logn < NTT_LOGN_MIN || logn > NTT_LOGN_MAX) { ec_fatal(EC_RC_FATAL, "ntt: logn %d out of range\n", logn); }
}

#define LAUNCH_B16(S, INV) case S: k_b16<S, INV><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, INV ? c->tabT_i[S] : c->tabT_f[S], scale); break;
static void launch_b16(ntt2_ctx *c, uint64_t *x, int logn, int s_lo, int stg, const struct pass_tw *tw,
                       int inv, uint64_t scale, unsigned blocks, hipStream_t s)
{
    if (stg == 7 && ntt2_b16_body >= 1) {
        const uint64_t *tb = inv ? c->tabT_i[7] : c->tabT_f[7];
        if (ntt2_b16_body == 2) { if (inv) k_b16r<1, 1><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, tb, scale);
                                 else     k_b16r<0, 1><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, tb, scale); }
        else                   { if (inv) k_b16r<1, 0><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, tb, scale);
                                 else     k_b16r<0, 0><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, tb, scale); }
        return;
    }
    if (inv) switch (stg) { LAUNCH_B16(1, 1) LAUNCH_B16(2, 1) LAUNCH_B16(3, 1) LAUNCH_B16(4, 1) LAUNCH_B16(5, 1) LAUNCH_B16(6, 1) LAUNCH_B16(7, 1) default: ec_fatal(EC_RC_FATAL, "ntt: the radix-16 pass of %d stages is not built", stg); }
    else     switch (stg) { LAUNCH_B16(1, 0) LAUNCH_B16(2, 0) LAUNCH_B16(3, 0) LAUNCH_B16(4, 0) LAUNCH_B16(5, 0) LAUNCH_B16(6, 0) LAUNCH_B16(7, 0) default: ec_fatal(EC_RC_FATAL, "ntt: the radix-16 pass of %d stages is not built", stg); }
}

void ntt2_fwd(ntt2_ctx *c, uint64_t *x, int logn, size_t batch, hipStream_t s)
{
    struct plan pl;
    check_logn(logn);
    make_plan(&pl, logn);
    struct plan_tw *pt = get_plan_tw(c, logn, &pl);
    unsigned blocks = logn >= 11 ? (unsigned)(batch << (logn - 11)) : 0;
    for (int i = 0; i < pl.npass; i++)
        launch_b16(c, x, logn, pl.s_lo[i], pl.s_hi[i] - pl.s_lo[i] + 1, &pt->f[i], 0, 0, blocks, s);
    k_b1<B1_LGL, 0><<<(unsigned)(batch << (logn - B1_LGL)), THREADS, 0, s>>>(x, 0, 0, c->m, c->tab1_f, 0);
}

static void inv_common(ntt2_ctx *c, uint64_t *x, const uint64_t *y, size_t ymask, int logn, size_t batch, hipStream_t s)
{
    struct plan pl;
    check_logn(logn);
    make_plan(&pl, logn);
    struct plan_tw *pt = get_plan_tw(c, logn, &pl);
    unsigned blocks = logn >= 11 ? (unsigned)(batch << (logn - 11)) : 0;
    uint64_t sc = c->ninv[logn];
    unsigned b1 = (unsigned)(batch << (logn - B1_LGL));
    if (y) k_b1<B1_LGL, 2><<<b1, THREADS, 0, s>>>(x, y, ymask, c->m, c->tab1_i, pl.npass ? 0 : sc);
    else   k_b1<B1_LGL, 1><<<b1, THREADS, 0, s>>>(x, 0, 0, c->m, c->tab1_i, pl.npass ? 0 : sc);
    for (int i = pl.npass - 1; i >= 0; i--)
        launch_b16(c, x, logn, pl.s_lo[i], pl.s_hi[i] - pl.s_lo[i] + 1, &pt->i[i], 1, i == 0 ? sc : 0, blocks, s);
}
void ntt2_inv(ntt2_ctx *c, uint64_t *x, int logn, size_t batch, hipStream_t s) { inv_common(c, x, 0, 0, logn, batch, s); }
void ntt2_inv_pw(ntt2_ctx *c, uint64_t *x, const uint64_t *y, int logn, size_t batch, hipStream_t s)
{
    if (logn >= ntt2_pw_fuse) inv_common(c, x, y, ~(size_t)0, logn, batch, s);
    else { ntt2_pw(c, x, y, batch << logn, s); inv_common(c, x, 0, 0, logn, batch, s); }
}
void ntt2_inv_pw_bcast(ntt2_ctx *c, uint64_t *x, const uint64_t *y, int logn, size_t batch, hipStream_t s)
{
    size_t mask = ((size_t)1 << logn) - 1;
    if (logn >= ntt2_pw_fuse) inv_common(c, x, y, mask, logn, batch, s);
    else { ntt2_pw_bcast(c, x, y, logn, batch << logn, s); inv_common(c, x, 0, 0, logn, batch, s); }
}
void ntt2_pw(ntt2_ctx *c, uint64_t *x, const uint64_t *y, size_t count, hipStream_t s)
{
    size_t blocks = (count + 255) / 256; if (blocks > 228 * 16) blocks = 228 * 16;
    k_pw<<<(unsigned)blocks, 256, 0, s>>>(x, y, ~(size_t)0, count, c->m);
}
void ntt2_pw_bcast(ntt2_ctx *c, uint64_t *x, const uint64_t *y, int logn, size_t count, hipStream_t s)
{
    size_t blocks = (count + 255) / 256; if (blocks > 228 * 16) blocks = 228 * 16;
    k_pw<<<(unsigned)blocks, 256, 0, s>>>(x, y, ((size_t)1 << logn) - 1, count, c->m);
}
void ntt2_load(ntt2_ctx *c, uint64_t *dst, const uint64_t *src, size_t nlimbs, size_t npoints, hipStream_t s)
{
    size_t blocks = (npoints + 255) / 256; if (blocks > 228 * 16) blocks = 228 * 16;
    k_load<<<(unsigned)blocks, 256, 0, s>>>(dst, src, nlimbs, npoints, c->m);
}

/* ---- host references ------------------------------------------------------ */
void ntt2_host_fwd(uint64_t *x, int logn, int prime)
{
    uint64_t p = e2_P[prime];
    size_t n = (size_t)1 << logn, h, i, r;
    uint64_t wn = e2_root(prime, logn);
    for (h = n / 2; h >= 1; h >>= 1) {
        uint64_t w2h = e2_powmod(wn, n / (2 * h), p);
#pragma omp parallel for schedule(static) private(r)
        for (i = 0; i < n; i += 2 * h) {
            uint64_t w = 1;
            for (r = 0; r < h; r++) {
                uint64_t u = x[i + r], v = x[i + r + h];
                x[i + r] = (u + v) % p;
                x[i + r + h] = e2_mulmod_ref((u + p - v) % p, w, p);
                w = e2_mulmod_ref(w, w2h, p);
            }
        }
    }
}
void ntt2_host_inv(uint64_t *x, int logn, int prime)
{
    uint64_t p = e2_P[prime];
    size_t n = (size_t)1 << logn, h, i, r;
    uint64_t wn = e2_root_inv(prime, logn), ninv = e2_inv(n % p, p);
    for (h = 1; h < n; h <<= 1) {
        uint64_t w2h = e2_powmod(wn, n / (2 * h), p);
#pragma omp parallel for schedule(static) private(r)
        for (i = 0; i < n; i += 2 * h) {
            uint64_t w = 1;
            for (r = 0; r < h; r++) {
                uint64_t u = x[i + r], v = e2_mulmod_ref(x[i + r + h], w, p);
                x[i + r] = (u + v) % p;
                x[i + r + h] = (u + p - v) % p;
                w = e2_mulmod_ref(w, w2h, p);
            }
        }
    }
    for (i = 0; i < n; i++) x[i] = e2_mulmod_ref(x[i], ninv, p);
}

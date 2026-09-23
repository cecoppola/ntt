/* ntt.c - tiled DIF forward / DIT inverse NTT, FP64-Barrett arithmetic.
 * See ntt.h.  Kernels are the verified ones from bench/16_ntt_tile (RESULTS.md
 * 25, 33), made two-directional and given the scale / pointwise fusions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ntt.h"

#define BP 17                      /* LDS pad: sh[128][17], conflict-free columns */
#define THREADS 256
#define B1_LGL 10                  /* b1 block length 2^10 (rule 3b) */

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

int ntt_stg = 7;
int ntt_pw_fuse = 14;
int ntt_b16_body = 1;      /* Phase 9 N-kernel: the register-blocked body (bit-identical, RESULTS 43) is the default now */
int ntt_b1_shoup = 0;      /* NTT_B1_SHOUP: 1 = Shoup integer modmul in the b1 pass (Phase 5 item 1) */      /* NTT_B16_BODY: 0 tile kernel (paper), 1 register-blocked body for 7-stage passes, 2 = 1 + radix-4 stages */
int ntt_b16_xchg = -1;     /* NTT_B16_XCHG: 1 = the register-blocked body's B<->C exchange by ds_swizzle instead of LDS (Phase 9 B4); -1 = from the environment, default 0 */
int ntt_modmul = -1;       /* NTT_MODMUL (Phase 13a K, H3): 0 FP64 Barrett, 1 reduced-correction Barrett (default since Phase 13b: +5-12 %, bit-identical), 2 Shoup; -1 = from the environment */
int ntt_b16_var = -1;      /* NTT_B16_VAR (Phase 13a K, H6): k_b16r variant bits (1 unpadded LDS + global twiddle table, 2 block order); 0 default */
int ntt_mall = -1;         /* NTT_MALL (Phase 13a K, H2): log2 of a MALL-resident chunk (points); 0 = off (default); -1 = from the environment */

static int env_int(const char *nm, int dflt) { const char *e = getenv(nm); return e ? atoi(e) : dflt; }
int ntt_modmul_get(void) { if (ntt_modmul < 0) ntt_modmul = env_int("NTT_MODMUL", 1); return ntt_modmul; }
int ntt_mall_get(void) { if (ntt_mall < 0) ntt_mall = env_int("NTT_MALL", 0); return ntt_mall; }

/* ---- Phase 13a K (H3): the reduced-correction FP64 Barrett.  The quotient is taken from the exact product
 * hi + lo against the two-term reciprocal pinv + pinvl (pinvl = (1 - p pinv) / p, computed per thread):
 *   x = fl(hi pinv + fl(hi pinvl + fl(lo pinv)))  differs from ab/p by at most 0.5 + 2^-50 (ab/p < 2p < 2^53,
 *   so the final rounding is at most half an ulp <= 0.5; the other terms are below 2^-51),
 * hence q = floor(x) is Q - 1, Q or Q + 1, and q = Q - 1 only when frac(ab/p) < 0.5 + eps, q = Q + 1 only when
 * frac(ab/p) >= 0.5 - eps: r = ab - q p lies in (-0.51 p, 1.51 p).  |hi - q p| < 1.51 p + 2^51 < 2^53, so both
 * the fma and the + lo are exact.  One correction gives the lazy [0, 2p) the butterflies accept (mm_lazy),
 * two give canonical (mm_canon, for twiddles).  Same operand rule as ec_mm: a < 2p, b < p. */
__device__ static inline double mm_raw(double a, double b, double p, double pinv, double pinvl)
{
    double hi = a * b, lo = fma(a, b, -hi);
    double q = floor(fma(hi, pinv, fma(hi, pinvl, lo * pinv)));
    return fma(-q, p, hi) + lo;
}
__device__ static inline uint64_t mm_lazy(double a, double b, double p, double pinv, double pinvl)
{
    double r = mm_raw(a, b, p, pinv, pinvl);
    r += (r < 0.0 ? p : 0.0);
    return (uint64_t)r;
}
__device__ static inline double mm_canon(double a, double b, double p, double pinv, double pinvl)
{
    double r = mm_raw(a, b, p, pinv, pinvl);
    r += (r < 0.0 ? p : 0.0);
    r -= (r >= p ? p : 0.0);
    return r;
}
/* ---- Phase 13a K (H3): Shoup's constant floor(w 2^64 / p), exact, for a w < p formed on the device (the
 * per-stage column twiddle T): a double estimate, one Barrett step on the signed 128-bit error, one fix-up. */
__device__ static inline uint64_t shoup_q(uint64_t w, uint64_t pu, double pinv)
{
    double a = (double)w * pinv * 18446744073709551616.0;
    if (a > 1.8446744073709550e19) a = 1.8446744073709550e19;
    uint64_t q = (uint64_t)a, lo = 0 - q * pu, hi = w - __umul64hi(q, pu) - (lo != 0);   /* e = w 2^64 - q p */
    double ed = (double)(int64_t)hi * 18446744073709551616.0 + (double)lo;
    q += (uint64_t)(int64_t)floor(ed * pinv);
    lo = 0 - q * pu; hi = w - __umul64hi(q, pu) - (lo != 0);                              /* e in [-p, 2p) */
    if ((int64_t)hi < 0) q--;
    else if (lo >= pu) q++;
    return q;
}

/* Phase 9 B1: where block `base` of x (a batch of transforms of Lt points) reads its pointwise operand:
 * transform t = base / Lt, point k; y transform is t (FULL), 0 (BCAST) or t >> 1 (PAIR).  Blocks never
 * straddle a transform (Lt is a multiple of the block length). */
__device__ static inline size_t y_base(size_t base, size_t Lt, int ymode)
{
    if (ymode == NTT_Y_FULL) return base;
    size_t t = base / Lt, k = base - t * Lt;
    return (ymode == NTT_Y_BCAST ? 0 : (t >> 1)) * Lt + k;
}

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
void k_b16(uint64_t *x, int logn, int s_lo, ec_mod m, const double *tlo, const double *thi,
           const double *tabT, double scale)
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
    const double p = m.p, pinv = m.pinv;
    const uint64_t pu = m.pu, p2 = 2 * pu;
    double T[NT][INV ? STG : 1];
    int j, k, mm_;

    for (k = threadIdx.x; k < tile; k += THREADS) tab[k] = tabT[k];
#pragma unroll
    for (j = tt; j < 128; j += 16)
        sh[j * BP + bb] = x[base + (size_t)(j >> STG) * 16 + (size_t)(j & (tile - 1)) * hmin + bb];
#pragma unroll
    for (mm_ = 0; mm_ < NT; mm_++) {
        int ti = NT == 1 ? 0 : NT == 2 ? mm_ : ((tt + 16 * mm_) >> (STG - 1));
        size_t c = (slab0 + ti) * 16 + bb;
        T[mm_][INV ? STG - 1 : 0] = ec_mm(tlo[c & 4095], thi[c >> 12], p, pinv);
        if (INV) {
#pragma unroll
            for (int l = STG - 1; l > 0; l--) T[mm_][l - 1] = ec_mm(T[mm_][l], T[mm_][l], p, pinv);
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
            double w = ec_mm(tab[r << lgstep], T[tm][INV ? lgH : 0], p, pinv);
            if (INV) {
                uint64_t vw = (uint64_t)ec_mm((double)v, w, p, pinv);      /* [0,p) */
                uint64_t sum = u + vw, d = u + pu - vw;                     /* < 3p */
                if (sum >= p2) sum -= p2;
                if (d >= p2) d -= p2;
                sh[j0 * BP + bb] = sum;
                sh[j1 * BP + bb] = d;
            } else {
                uint64_t sum = u + v, d = u - v + p2;                       /* < 4p */
                if (sum >= p2) sum -= p2;
                if (d >= p2) d -= p2;
                sh[j0 * BP + bb] = sum;
                sh[j1 * BP + bb] = (uint64_t)ec_mm((double)d, w, p, pinv);
            }
        }
        if (!INV) {
#pragma unroll
            for (mm_ = 0; mm_ < NT; mm_++) T[mm_][0] = ec_mm(T[mm_][0], T[mm_][0], p, pinv);
        }
        __syncthreads();
    }
#pragma unroll
    for (j = tt; j < 128; j += 16) {
        uint64_t v = sh[j * BP + bb];
        if (scale != 0.0) v = (uint64_t)ec_mm((double)v, scale, p, pinv);
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
/* Phase 9 B4: the B <-> C exchange is between threads tt and tt ^ 1, i.e. lanes l and l ^ 16 of one
 * wavefront (lane = 16 tt + bb), and each thread keeps half of its 8 rows: 4 values cross per thread.
 * SW = 1 does it with ds_swizzle (xor 16 within 32 lanes, RESULTS.md 49: 2x ds_bpermute, 1.6x an LDS
 * round trip) and no barrier; the values are the same, so the output stays bit-identical.
 * Forward, o = tt & 1: row 16 j + 2 i + o (B, register i) -> row 16 j + 8 o + c (C, register c):
 *   send[i] = o ? v[i] : v[4 + i];  C[2 i] = o ? recv[i] : v[i];  C[2 i + 1] = o ? v[4 + i] : recv[i]
 * Inverse (C -> B):
 *   send[i] = o ? v[2 i] : v[2 i + 1];  B[i] = o ? recv[i] : v[2 i];  B[4 + i] = o ? v[2 i + 1] : recv[i] */
__device__ static inline uint64_t xchg16(uint64_t v)
{
    uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
    lo = (uint32_t)__builtin_amdgcn_ds_swizzle((int)lo, 0x401F);     /* bitmask mode: and 0x1f, or 0, xor 0x10 */
    hi = (uint32_t)__builtin_amdgcn_ds_swizzle((int)hi, 0x401F);
    return ((uint64_t)hi << 32) | lo;
}
/* Phase 13a K (H3): MM selects the modmul of the butterflies.  0: the FP64 Barrett (ec_mm; the default,
 * unchanged).  1: the reduced-correction Barrett (mm_lazy for the data, mm_canon for the twiddles), data
 * products lazy in [0, 2p).  2: Shoup integer products: v tab[r] (tabS/tabSq: the 128-entry table as integers
 * and Shoup constants) then x T (T formed and squared by ec_mm as before, its Shoup constant by shoup_q once
 * per stage), lazy in [0, 2p); no per-butterfly twiddle product.  For MM != 0 the inverse's difference is
 * u + 2p - vw.  Every variant is exact, so the canonical outputs are bit-identical to MM 0's. */
/* Phase 13a K (H6), VAR (bit mask, 0 = the default): bit 0 (OCC) the LDS tile unpadded (sh[128][16]) and the
 * 128-entry twiddle table read from global memory instead of LDS: 16 KB of LDS per block, so 4 blocks per CU
 * instead of 3 (the default's 18.4 KB caps occupancy at 3 waves/SIMD; VGPRs would allow 8+).  bit 1 (ORD) the
 * blocks ordered tile-row-group fastest (blk_hi = b mod nhi) instead of slab fastest, for the wide-stride passes
 * (s_lo 17 and 24 measured at 0.8x).  Same arithmetic: bit-identical. */
template <int INV, int R4, int SW, int MM, int VAR = 0>
__global__ __launch_bounds__(THREADS)
void k_b16r(uint64_t *x, int logn, int s_lo, ec_mod m, const double *tlo, const double *thi,
            const double *tabT, double scale, const uint64_t *tabS, const uint64_t *tabSq)
{
    constexpr int STG = 7, tile = 128, BPX = (VAR & 1) ? 16 : BP;
    __shared__ uint64_t sh[128 * BPX + ((VAR & 1) ? 0 : 1)];
    __shared__ double tab[(VAR & 1) ? 1 : tile];
    __shared__ uint64_t tabs[MM == 2 ? tile : 1], tabsq[MM == 2 ? tile : 1];
#define TABV(k_) ((VAR & 1) ? tabT[k_] : tab[k_])
    const int tt = threadIdx.x >> 4, bb = threadIdx.x & 15;
    const size_t n = (size_t)1 << logn, hmin = (size_t)1 << s_lo, slabs = hmin / 16;
    const size_t bpt = n / 2048, b = blockIdx.x % bpt, t = blockIdx.x / bpt;
    const size_t nhi = n / ((size_t)tile * hmin);
    const size_t blk_hi = (VAR & 2) ? b % nhi : b / slabs, slab0 = (VAR & 2) ? b / nhi : b % slabs;
    const size_t base = t * n + blk_hi * (size_t)tile * hmin + slab0 * 16;
    const double p = m.p, pinv = m.pinv;
    const double pinvl = MM == 1 ? fma(-p, pinv, 1.0) * pinv : 0.0;
    const uint64_t pu = m.pu, p2 = 2 * pu;
    uint64_t v[8];
    double T[INV ? STG : 1];
    uint64_t Tu[MM == 2 ? (INV ? STG : 1) : 1], Tq[MM == 2 ? (INV ? STG : 1) : 1];
    int i;
#define MMC(a_, b_) (MM == 1 ? mm_canon((a_), (b_), p, pinv, pinvl) : ec_mm((a_), (b_), p, pinv))

    for (int k = threadIdx.x; k < tile; k += THREADS) { if (!(VAR & 1)) tab[k] = tabT[k]; if (MM == 2) { tabs[k] = tabS[k]; tabsq[k] = tabSq[k]; } }
    {
        size_t c = slab0 * 16 + bb;
        T[INV ? STG - 1 : 0] = MMC(tlo[c & 4095], thi[c >> 12]);
        if (INV) {
#pragma unroll
            for (int l = STG - 1; l > 0; l--) T[l - 1] = MMC(T[l], T[l]);
        }
        if (MM == 2) {
#pragma unroll
            for (int l = 0; l < (INV ? STG : 1); l++) { Tu[l] = (uint64_t)T[l]; Tq[l] = shoup_q(Tu[l], pu, pinv); }
        }
    }
    __syncthreads();

#define ROW_A(i) (tt + 16 * (i))
#define ROW_B(i) ((((tt) >> 1) << 4) | ((i) << 1) | ((tt) & 1))
#define ROW_C(i) (8 * (tt) + (i))
#define BFLY(u_, v_, w_) do {                                                         \
        if (INV) { uint64_t vw = (uint64_t)ec_mm((double)(v_), (w_), p, pinv);        \
                   uint64_t s_ = (u_) + vw, d_ = (u_) + pu - vw;                      \
                   if (s_ >= p2) s_ -= p2; if (d_ >= p2) d_ -= p2; (u_) = s_; (v_) = d_; } \
        else { uint64_t s_ = (u_) + (v_), d_ = (u_) - (v_) + p2;                      \
               if (s_ >= p2) s_ -= p2; if (d_ >= p2) d_ -= p2;                        \
               (u_) = s_; (v_) = (uint64_t)ec_mm((double)d_, (w_), p, pinv); } } while (0)
    /* MM != 0: data times the twiddle of table index ix_ and stage slot ti_, lazy [0, 2p) */
#define MULW(val_, ix_, ti_) (MM == 2 ? ec_shoup_lazy(Tu[ti_], Tq[ti_], ec_shoup_lazy(tabs[ix_], tabsq[ix_], (val_), pu), pu)   \
                                      : mm_lazy((double)(val_), mm_canon(TABV(ix_), T[ti_], p, pinv, pinvl), p, pinv, pinvl))
#define BFLYM(u_, v_, ix_, ti_) do {                                                  \
        if (INV) { uint64_t vw = MULW((v_), (ix_), (ti_));                             \
                   uint64_t s_ = (u_) + vw, d_ = (u_) + p2 - vw;                      \
                   if (s_ >= p2) s_ -= p2; if (d_ >= p2) d_ -= p2; (u_) = s_; (v_) = d_; } \
        else { uint64_t s_ = (u_) + (v_), d_ = (u_) - (v_) + p2;                      \
               if (s_ >= p2) s_ -= p2; if (d_ >= p2) d_ -= p2;                        \
               (u_) = s_; (v_) = MULW(d_, (ix_), (ti_)); } } while (0)
    /* one stage on the registers: register bit `rb` pairs (i, i + 2^rb); r(i) = row & (H-1) */
#define STAGE(lgH, rb, ROWF)                                                          \
    do {                                                                              \
        const int H_ = 1 << (lgH), lgstep_ = STG - 1 - (lgH);                         \
        _Pragma("unroll")                                                             \
        for (int i_ = 0; i_ < 8; i_++) if (!(i_ & (1 << (rb)))) {                     \
            int r_ = ROWF(i_) & (H_ - 1);                                             \
            if (MM == 0) {                                                            \
                double w_ = ec_mm(TABV(r_ << lgstep_), T[INV ? (lgH) : 0], p, pinv);   \
                BFLY(v[i_], v[i_ + (1 << (rb))], w_);                                 \
            } else BFLYM(v[i_], v[i_ + (1 << (rb))], r_ << lgstep_, INV ? (lgH) : 0); \
        }                                                                             \
        if (!INV) {                                                                   \
            T[0] = MMC(T[0], T[0]);                                                   \
            if (MM == 2) { Tu[0] = (uint64_t)T[0]; Tq[0] = shoup_q(Tu[0], pu, pinv); } \
        }                                                                             \
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
        const double w4_ = TABV(tile / 4);                                             \
        _Pragma("unroll")                                                             \
        for (int i_ = 0; i_ < 8; i_++) if (!(i_ & (3 << ((rb) - 1)))) {               \
            int r_ = ROWF(i_) & (H_ - 1);                                             \
            double w_ = ec_mm(TABV(r_ << lgstep_), T[INV ? (lgH) : 0], p, pinv);       \
            double w2_ = ec_mm(w_, w_, p, pinv), wi_ = ec_mm(w_, w4_, p, pinv);       \
            const int ib = i_ + (1 << ((rb) - 1)), ic = i_ + (1 << (rb)), id = ic + (1 << ((rb) - 1)); \
            if (INV) { BFLY(v[i_], v[ib], w2_); BFLY(v[ic], v[id], w2_);              \
                       BFLY(v[i_], v[ic], w_);  BFLY(v[ib], v[id], wi_); }            \
            else     { BFLY(v[i_], v[ic], w_);  BFLY(v[ib], v[id], wi_);              \
                       BFLY(v[i_], v[ib], w2_); BFLY(v[ic], v[id], w2_); }            \
        }                                                                             \
        if (!INV) { T[0] = ec_mm(T[0], T[0], p, pinv); T[0] = ec_mm(T[0], T[0], p, pinv); } \
    } while (0)
#define LOAD_G(ROWF)  _Pragma("unroll") for (i = 0; i < 8; i++) v[i] = x[base + (size_t)ROWF(i) * hmin + bb]
#define STORE_G(ROWF) _Pragma("unroll") for (i = 0; i < 8; i++) { uint64_t o = v[i];                        \
                          if (scale != 0.0) o = (uint64_t)ec_mm((double)o, scale, p, pinv);                 \
                          x[base + (size_t)ROWF(i) * hmin + bb] = o; }
#define TO_SH(ROWF)   _Pragma("unroll") for (i = 0; i < 8; i++) sh[ROWF(i) * BPX + bb] = v[i]
#define FROM_SH(ROWF) _Pragma("unroll") for (i = 0; i < 8; i++) v[i] = sh[ROWF(i) * BPX + bb]

    const int odd = tt & 1;
    if (!INV) {
        LOAD_G(ROW_A);
        if (R4) { STAGE4(6, 2, ROW_A); STAGE(4, 0, ROW_A); } else { STAGE(6, 2, ROW_A); STAGE(5, 1, ROW_A); STAGE(4, 0, ROW_A); }
        TO_SH(ROW_A); __syncthreads(); FROM_SH(ROW_B);
        if (R4) { STAGE4(3, 2, ROW_B); STAGE(1, 0, ROW_B); } else { STAGE(3, 2, ROW_B); STAGE(2, 1, ROW_B); STAGE(1, 0, ROW_B); }
        if (SW) {
            uint64_t r[4], nv[8];
#pragma unroll
            for (i = 0; i < 4; i++) r[i] = xchg16(odd ? v[i] : v[4 + i]);
#pragma unroll
            for (i = 0; i < 4; i++) { nv[2 * i] = odd ? r[i] : v[i]; nv[2 * i + 1] = odd ? v[4 + i] : r[i]; }
#pragma unroll
            for (i = 0; i < 8; i++) v[i] = nv[i];
        } else { __syncthreads(); TO_SH(ROW_B); __syncthreads(); FROM_SH(ROW_C); }
        STAGE(0, 0, ROW_C);
        STORE_G(ROW_C);
    } else {
        LOAD_G(ROW_C);
        STAGE(0, 0, ROW_C);
        if (SW) {
            uint64_t r[4], nv[8];
#pragma unroll
            for (i = 0; i < 4; i++) r[i] = xchg16(odd ? v[2 * i] : v[2 * i + 1]);
#pragma unroll
            for (i = 0; i < 4; i++) { nv[i] = odd ? r[i] : v[2 * i]; nv[4 + i] = odd ? v[2 * i + 1] : r[i]; }
#pragma unroll
            for (i = 0; i < 8; i++) v[i] = nv[i];
        } else { TO_SH(ROW_C); __syncthreads(); FROM_SH(ROW_B); }
        if (R4) { STAGE(1, 0, ROW_B); STAGE4(3, 2, ROW_B); } else { STAGE(1, 0, ROW_B); STAGE(2, 1, ROW_B); STAGE(3, 2, ROW_B); }
        if (SW) { TO_SH(ROW_B); __syncthreads(); FROM_SH(ROW_A); }
        else { __syncthreads(); TO_SH(ROW_B); __syncthreads(); FROM_SH(ROW_A); }
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
#undef MULW
#undef BFLYM
#undef MMC
#undef TABV
}

/* b1 pass with Shoup's integer modmul for the twiddle products (Phase 5 item
 * 1): tab1s holds the twiddles, tab1sp their Shoup constants floor(w 2^64/p);
 * shoup(d, w) = d w - floor(w' d / 2^64) p is in [0, 2p) for any 64-bit d.
 * The scale and the fused pointwise product keep the FP64 modmul (variable
 * multiplier). */
template <int LGL, int MODE>
__global__ __launch_bounds__(THREADS)
void k_b1s(uint64_t *x, const uint64_t *y, size_t Lt, int ymode, ec_mod m, const uint64_t *tab1s, const uint64_t *tab1sp, double scale, size_t goff)
{
    constexpr int L = 1 << LGL;
    __shared__ uint64_t sh[L];
    __shared__ uint64_t tab[L / 2], tabp[L / 2];
    const size_t base = (size_t)blockIdx.x * L, ybase = MODE == 2 ? y_base(goff + base, Lt, ymode) : 0;   /* goff: x's offset in the batch (NTT_MALL chunks) */
    const double p = m.p, pinv = m.pinv;
    const uint64_t pu = m.pu, p2 = 2 * pu;
    int k;
    for (k = threadIdx.x; k < L / 2; k += THREADS) { tab[k] = tab1s[k]; tabp[k] = tab1sp[k]; }
    for (k = threadIdx.x; k < L; k += THREADS)
        sh[k] = MODE == 2 ? (uint64_t)ec_mm((double)x[base + k], (double)y[ybase + k], p, pinv) : x[base + k];
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
                uint64_t vw = ec_shoup_lazy(tab[r << lgstep], tabp[r << lgstep], v, pu);   /* [0,2p) */
                uint64_t sum = u + vw, d = u + p2 - vw;
                if (sum >= p2) sum -= p2;
                if (d >= p2) d -= p2;
                sh[j0] = sum; sh[j1] = d;
            } else {
                uint64_t sum = u + v, d = u - v + p2;
                if (sum >= p2) sum -= p2;
                if (d >= p2) d -= p2;
                sh[j0] = sum;
                sh[j1] = ec_shoup_lazy(tab[r << lgstep], tabp[r << lgstep], d, pu);
            }
        }
        __syncthreads();
    }
    for (k = threadIdx.x; k < L; k += THREADS) {
        uint64_t v = sh[k];
        if (MODE == 0) { if (v >= pu) v -= pu; }
        else if (scale != 0.0) v = (uint64_t)ec_mm((double)v, scale, p, pinv);
        x[base + k] = v;
    }
}

/* b1 pass on contiguous 2^LGL-point blocks.  MODE 0: forward stages LGL-1..0,
 * canonical output.  MODE 1: inverse stages 0..LGL-1.  MODE 2: inverse with
 * the pointwise product x[i] y[i] fused into the load. */
template <int LGL, int MODE, int MM>
__global__ __launch_bounds__(THREADS)
void k_b1(uint64_t *x, const uint64_t *y, size_t Lt, int ymode, ec_mod m, const double *tab1, double scale, size_t goff)
{
    constexpr int L = 1 << LGL;
    __shared__ uint64_t sh[L];
    __shared__ double tab[L / 2];
    const size_t base = (size_t)blockIdx.x * L, ybase = MODE == 2 ? y_base(goff + base, Lt, ymode) : 0;
    const double p = m.p, pinv = m.pinv;
    const double pinvl = MM ? fma(-p, pinv, 1.0) * pinv : 0.0;     /* MM 1: the reduced-correction Barrett (H3) */
    const uint64_t pu = m.pu, p2 = 2 * pu;
    int k;
    for (k = threadIdx.x; k < L / 2; k += THREADS) tab[k] = tab1[k];
    for (k = threadIdx.x; k < L; k += THREADS)
        sh[k] = MODE == 2 ? (uint64_t)ec_mm((double)x[base + k], (double)y[ybase + k], p, pinv) : x[base + k];
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
                uint64_t vw = MM ? mm_lazy((double)v, tab[r << lgstep], p, pinv, pinvl) : (uint64_t)ec_mm((double)v, tab[r << lgstep], p, pinv);
                uint64_t sum = u + vw, d = u + (MM ? p2 : pu) - vw;
                if (sum >= p2) sum -= p2;
                if (d >= p2) d -= p2;
                sh[j0] = sum; sh[j1] = d;
            } else {
                uint64_t sum = u + v, d = u - v + p2;
                if (sum >= p2) sum -= p2;
                if (d >= p2) d -= p2;
                sh[j0] = sum;
                sh[j1] = MM ? mm_lazy((double)d, tab[r << lgstep], p, pinv, pinvl) : (uint64_t)ec_mm((double)d, tab[r << lgstep], p, pinv);
            }
        }
        __syncthreads();
    }
    for (k = threadIdx.x; k < L; k += THREADS) {
        uint64_t v = sh[k];
        if (MODE == 0) { if (v >= pu) v -= pu; }
        else if (scale != 0.0) v = (uint64_t)ec_mm((double)v, scale, p, pinv);
        x[base + k] = v;
    }
}

__global__ void k_pw(uint64_t *x, const uint64_t *y, size_t ymask, size_t n, ec_mod m)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) x[i] = (uint64_t)ec_mm((double)x[i], (double)y[i & ymask], m.p, m.pinv);
}
/* B1: the pointwise product with a y layout; transforms of Lt = (r3 ? 3 : 1) << logk points */
__global__ void k_pw_y(uint64_t *x, const uint64_t *y, size_t Lt, int logk, int r3, int ymode, size_t n, ec_mod m)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) {
        size_t t = r3 ? (i >> logk) / 3 : i >> logk, k = i - t * Lt;
        size_t yi = (ymode == NTT_Y_FULL ? t : ymode == NTT_Y_BCAST ? 0 : (t >> 1)) * Lt + k;
        x[i] = (uint64_t)ec_mm((double)x[i], (double)y[yi], m.p, m.pinv);
    }
}
__global__ void k_load(uint64_t *dst, const uint64_t *src, size_t nlimbs, size_t npoints, ec_mod m)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < npoints; i += stride) dst[i] = i < nlimbs ? ec_canon64(src[i], m.pu, m.mu) : 0;
}

/* ------------------------------------------------------------------------ */
struct pass_tw { double *tlo, *thi; };             /* per pass, per direction */
struct plan_tw { int built; struct pass_tw f[NTT_MAXPASS], i[NTT_MAXPASS]; };

struct ntt_ctx {
    int prime;
    ec_mod m;
    double *tabT_f[8], *tabT_i[8];                 /* 2^stg-th roots, stg 1..7 */
    double *tab1_f, *tab1_i;                       /* 1024-th roots, 512 entries */
    uint64_t *tab1s_f, *tab1s_i, *tab1sp_f, *tab1sp_i;   /* the same as integers + Shoup constants */
    uint64_t *tab7s_f, *tab7sq_f, *tab7s_i, *tab7sq_i;   /* H3 (NTT_MODMUL=2): tabT_f/i[7] as integers + Shoup constants */
    double ninv[NTT_LOGN_MAX + 1];
    struct plan_tw plan[NTT_LOGN_MAX + 1];
};

struct plan { int npass, s_lo[NTT_MAXPASS], s_hi[NTT_MAXPASS]; };
static void make_plan(struct plan *pl, int logn)
{
    int hi = logn - 1, stg = ntt_stg < 3 ? 3 : ntt_stg > 7 ? 7 : ntt_stg;
    pl->npass = 0;
    while (hi >= B1_LGL) {
        int lo = hi - stg + 1; if (lo < B1_LGL) lo = B1_LGL;
        if (pl->npass == NTT_MAXPASS) { fprintf(stderr, "ntt: too many passes for logn %d stg %d\n", logn, stg); exit(1); }
        pl->s_hi[pl->npass] = hi; pl->s_lo[pl->npass] = lo; pl->npass++;
        hi = lo - 1;
    }
}
int ntt_npass(int logn) { struct plan pl; make_plan(&pl, logn); return pl.npass + 1; }
void ntt_pass_bounds(int logn, int pass, int *s_lo, int *s_hi)
{
    struct plan pl; make_plan(&pl, logn);
    if (pass < pl.npass) { *s_lo = pl.s_lo[pass]; *s_hi = pl.s_hi[pass]; }
    else { *s_lo = 0; *s_hi = B1_LGL - 1; }
}

/* Phase 11 I11 (agent P): a context's tables (and each plan's pass twiddles) are built in one host buffer and go to the
 * device in one hipMalloc + one hipMemcpy instead of one pair per table (a context was 22 pairs, a plan 4 per pass;
 * 16 contexts x 20 lengths over a run) -- the values are unchanged */
static void host_table_pow(double *h, uint64_t w, size_t cnt, uint64_t p) { uint64_t a = 1; for (size_t k = 0; k < cnt; k++) { h[k] = (double)a; a = ec_mulmod_ref(a, w, p); } }
static void *dev_upload(const void *h, size_t bytes) { void *d; HIP_CHECK(hipMalloc(&d, bytes)); HIP_CHECK(hipMemcpy(d, h, bytes, hipMemcpyHostToDevice)); return d; }

ntt_ctx *ntt_ctx_create(int prime)
{
    ntt_ctx *c = (ntt_ctx *)calloc(1, sizeof *c);
    uint64_t p = ec_P[prime];
    c->prime = prime;
    c->m = ec_mod_get(prime);
    const size_t n1 = (size_t)1 << (B1_LGL - 1);
    size_t words = 0; for (int stg = 1; stg <= 7; stg++) words += (size_t)2 << stg; words += 2 * n1 + 4 * n1 + 4 * 128;   /* tabT x 2, tab1 x 2, tab1s/tab1sp x 2, tab7s/tab7sq x 2 (all 8-byte entries) */
    uint64_t *h = (uint64_t *)malloc(words * 8); size_t off = 0;
    for (int stg = 1; stg <= 7; stg++) {
        host_table_pow((double *)(h + off), ec_root(prime, stg), (size_t)1 << stg, p); off += (size_t)1 << stg;
        host_table_pow((double *)(h + off), ec_root_inv(prime, stg), (size_t)1 << stg, p); off += (size_t)1 << stg;
    }
    size_t o1f = off; host_table_pow((double *)(h + off), ec_root(prime, B1_LGL), n1, p); off += n1;
    size_t o1i = off; host_table_pow((double *)(h + off), ec_root_inv(prime, B1_LGL), n1, p); off += n1;
    size_t os[2], osp[2];
    for (int inv = 0; inv < 2; inv++) {
        uint64_t a = 1, w = inv ? ec_root_inv(prime, B1_LGL) : ec_root(prime, B1_LGL);
        os[inv] = off; osp[inv] = off + n1;
        for (size_t k = 0; k < n1; k++) { h[os[inv] + k] = a; h[osp[inv] + k] = ec_shoup_pre(a, p); a = ec_mulmod_ref(a, w, p); }
        off += 2 * n1;
    }
    size_t o7[2];
    for (int inv = 0; inv < 2; inv++) {
        uint64_t a = 1, w = inv ? ec_root_inv(prime, 7) : ec_root(prime, 7);
        o7[inv] = off;
        for (size_t k = 0; k < 128; k++) { h[off + k] = a; h[off + 128 + k] = ec_shoup_pre(a, p); a = ec_mulmod_ref(a, w, p); }
        off += 256;
    }
    uint64_t *d = (uint64_t *)dev_upload(h, words * 8); free(h); off = 0;
    for (int stg = 1; stg <= 7; stg++) { c->tabT_f[stg] = (double *)(d + off); off += (size_t)1 << stg; c->tabT_i[stg] = (double *)(d + off); off += (size_t)1 << stg; }
    c->tab1_f = (double *)(d + o1f); c->tab1_i = (double *)(d + o1i);
    c->tab7s_f = d + o7[0]; c->tab7sq_f = d + o7[0] + 128; c->tab7s_i = d + o7[1]; c->tab7sq_i = d + o7[1] + 128;
    c->tab1s_f = d + os[0]; c->tab1sp_f = d + osp[0]; c->tab1s_i = d + os[1]; c->tab1sp_i = d + osp[1];
    for (int l = 0; l <= NTT_LOGN_MAX; l++) c->ninv[l] = (double)ec_inv(ec_powmod(2, l, p), p);
    return c;
}
void ntt_ctx_free(ntt_ctx *c)
{
    if (!c) return;
    HIP_CHECK(hipFree(c->tabT_f[1]));                       /* the one buffer of all the tables */
    for (int l = 0; l <= NTT_LOGN_MAX; l++) if (c->plan[l].built)
        for (int i = 0; i < NTT_MAXPASS; i++) {
            if (c->plan[l].f[i].tlo) HIP_CHECK(hipFree(c->plan[l].f[i].tlo));   /* thi lives in the same buffer */
            if (c->plan[l].i[i].tlo) HIP_CHECK(hipFree(c->plan[l].i[i].tlo));
        }
    free(c);
}
int ntt_ctx_prime(const ntt_ctx *c) { return c->prime; }

/* pass twiddle base tables: T_top(c) = wK^c, wK = w_n^(n / 2^(s_hi+1)), the
 * primitive 2^(s_hi+1)-th root; c < hmin = 2^s_lo.  tlo[k] = wK^k (4096),
 * thi[k] = wK^(4096 k) (2^(s_lo-12) entries, at least 1). */
static void build_pass_tw(struct pass_tw *t, int prime, int s_lo, int s_hi, int inv)
{
    uint64_t p = ec_P[prime];
    uint64_t wK = inv ? ec_root_inv(prime, s_hi + 1) : ec_root(prime, s_hi + 1);
    size_t nhi = s_lo > 12 ? (size_t)1 << (s_lo - 12) : 1;
    double *h = (double *)malloc((4096 + nhi) * sizeof *h);
    host_table_pow(h, wK, 4096, p); host_table_pow(h + 4096, ec_powmod(wK, 4096, p), nhi, p);
    t->tlo = (double *)dev_upload(h, (4096 + nhi) * sizeof *h); t->thi = t->tlo + 4096; free(h);   /* I11: one upload per pass */
}
static struct plan_tw *get_plan_tw(ntt_ctx *c, int logn, const struct plan *pl)
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
    if (logn < NTT_LOGN_MIN || logn > NTT_LOGN_MAX) { fprintf(stderr, "ntt: logn %d out of range\n", logn); exit(1); }
}

#define LAUNCH_B16(S, INV) case S: k_b16<S, INV><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, INV ? c->tabT_i[S] : c->tabT_f[S], scale); break;
static void launch_b16(ntt_ctx *c, uint64_t *x, int logn, int s_lo, int stg, const struct pass_tw *tw,
                       int inv, double scale, unsigned blocks, hipStream_t s)
{
    if (stg == 7 && ntt_b16_body >= 1) {
        const double *tb = inv ? c->tabT_i[7] : c->tabT_f[7];
        if (ntt_b16_xchg < 0) ntt_b16_xchg = getenv("NTT_B16_XCHG") ? atoi(getenv("NTT_B16_XCHG")) : 0;
        int mm = ntt_b16_body == 1 ? ntt_modmul_get() : 0;      /* H3: the modmul variants are built for body 1 (LDS exchange) */
        if (ntt_b16_var < 0) ntt_b16_var = env_int("NTT_B16_VAR", 0);
        if (ntt_b16_body == 1 && ntt_b16_var > 0 && mm < 2 && !ntt_b16_xchg) {                  /* H6 variants */
            int v = ntt_b16_var & 3, sel = (inv ? 8 : 0) | (mm ? 4 : 0) | v;
#define LAUNCH_V(INV, MM, V) k_b16r<INV, 0, 0, MM, V><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, tb, scale, 0, 0)
            switch (sel) {
            case 1: LAUNCH_V(0, 0, 1); break; case 2: LAUNCH_V(0, 0, 2); break; case 3: LAUNCH_V(0, 0, 3); break;
            case 5: LAUNCH_V(0, 1, 1); break; case 6: LAUNCH_V(0, 1, 2); break; case 7: LAUNCH_V(0, 1, 3); break;
            case 9: LAUNCH_V(1, 0, 1); break; case 10: LAUNCH_V(1, 0, 2); break; case 11: LAUNCH_V(1, 0, 3); break;
            case 13: LAUNCH_V(1, 1, 1); break; case 14: LAUNCH_V(1, 1, 2); break; default: LAUNCH_V(1, 1, 3); break;
            }
#undef LAUNCH_V
            return;
        }
        if (mm == 1 || mm == 2) {
            const uint64_t *ts = inv ? c->tab7s_i : c->tab7s_f, *tq = inv ? c->tab7sq_i : c->tab7sq_f;
#define LAUNCH_M(INV, MM) k_b16r<INV, 0, 0, MM><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, tb, scale, ts, tq)
            switch ((inv ? 2 : 0) | (mm == 2)) {
            case 0: LAUNCH_M(0, 1); break; case 1: LAUNCH_M(0, 2); break; case 2: LAUNCH_M(1, 1); break; default: LAUNCH_M(1, 2); break;
            }
#undef LAUNCH_M
            return;
        }
#define LAUNCH_R(INV, R4, SW) k_b16r<INV, R4, SW, 0><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, tb, scale, 0, 0)
        int sel = (inv ? 4 : 0) | (ntt_b16_body == 2 ? 2 : 0) | (ntt_b16_xchg ? 1 : 0);
        switch (sel) {
        case 0: LAUNCH_R(0, 0, 0); break; case 1: LAUNCH_R(0, 0, 1); break; case 2: LAUNCH_R(0, 1, 0); break; case 3: LAUNCH_R(0, 1, 1); break;
        case 4: LAUNCH_R(1, 0, 0); break; case 5: LAUNCH_R(1, 0, 1); break; case 6: LAUNCH_R(1, 1, 0); break; default: LAUNCH_R(1, 1, 1); break;
        }
#undef LAUNCH_R
        return;
    }
    if (inv) switch (stg) { LAUNCH_B16(1, 1) LAUNCH_B16(2, 1) LAUNCH_B16(3, 1) LAUNCH_B16(4, 1) LAUNCH_B16(5, 1) LAUNCH_B16(6, 1) LAUNCH_B16(7, 1) default: abort(); }
    else     switch (stg) { LAUNCH_B16(1, 0) LAUNCH_B16(2, 0) LAUNCH_B16(3, 0) LAUNCH_B16(4, 0) LAUNCH_B16(5, 0) LAUNCH_B16(6, 0) LAUNCH_B16(7, 0) default: abort(); }
}
/* the b1 pass: mode 0 forward, 1 inverse, 2 inverse with the pointwise product (y in layout ymode, Lt points per
 * transform; goff = x's offset in the batch, for the NTT_MALL chunks).  NTT_MODMUL 2 (or NTT_B1_SHOUP) uses the
 * Shoup kernel, NTT_MODMUL 1 the reduced-correction Barrett. */
static void launch_b1(ntt_ctx *c, uint64_t *x, const uint64_t *y, size_t Lt, int ymode, int mode, double scale, size_t goff, unsigned nb, hipStream_t s)
{
    int mm = ntt_modmul_get();
    if (mm == 2 || (mm == 0 && ntt_b1_shoup)) {
        if (mode == 0) k_b1s<B1_LGL, 0><<<nb, THREADS, 0, s>>>(x, 0, 0, 0, c->m, c->tab1s_f, c->tab1sp_f, 0.0, 0);
        else if (mode == 1) k_b1s<B1_LGL, 1><<<nb, THREADS, 0, s>>>(x, 0, 0, 0, c->m, c->tab1s_i, c->tab1sp_i, scale, 0);
        else k_b1s<B1_LGL, 2><<<nb, THREADS, 0, s>>>(x, y, Lt, ymode, c->m, c->tab1s_i, c->tab1sp_i, scale, goff);
    } else if (mm == 1) {
        if (mode == 0) k_b1<B1_LGL, 0, 1><<<nb, THREADS, 0, s>>>(x, 0, 0, 0, c->m, c->tab1_f, 0.0, 0);
        else if (mode == 1) k_b1<B1_LGL, 1, 1><<<nb, THREADS, 0, s>>>(x, 0, 0, 0, c->m, c->tab1_i, scale, 0);
        else k_b1<B1_LGL, 2, 1><<<nb, THREADS, 0, s>>>(x, y, Lt, ymode, c->m, c->tab1_i, scale, goff);
    } else {
        if (mode == 0) k_b1<B1_LGL, 0, 0><<<nb, THREADS, 0, s>>>(x, 0, 0, 0, c->m, c->tab1_f, 0.0, 0);
        else if (mode == 1) k_b1<B1_LGL, 1, 0><<<nb, THREADS, 0, s>>>(x, 0, 0, 0, c->m, c->tab1_i, scale, 0);
        else k_b1<B1_LGL, 2, 0><<<nb, THREADS, 0, s>>>(x, y, Lt, ymode, c->m, c->tab1_i, scale, goff);
    }
}

/* Phase 13a K (H2), NTT_MALL = lc: the passes whose butterflies stay inside 2^lc contiguous points (span
 * 2^(s_hi+1) <= 2^lc, or whole transforms) run chunk by chunk, all of them on one chunk before the next, so a
 * chunk that fits the 256 MB MALL stays resident between them; the passes of wider span run over the whole
 * array as before.  Same kernels, same arithmetic, so the output is bit-identical.  Returns the first chunked
 * pass (npass: only the b1 pass), or -1 when off or when the whole batch is one chunk. */
static int mall_split(const struct plan *pl, int logn, size_t batch)
{
    int lc = ntt_mall_get();
    if (lc < 11 || (batch << logn) <= ((size_t)1 << lc)) return -1;
    int i0 = 0;
    while (i0 < pl->npass && pl->s_hi[i0] + 1 > lc) i0++;
    return i0;
}

void ntt_fwd(ntt_ctx *c, uint64_t *x, int logn, size_t batch, hipStream_t s)
{
    struct plan pl;
    check_logn(logn);
    make_plan(&pl, logn);
    struct plan_tw *pt = get_plan_tw(c, logn, &pl);
    unsigned blocks = logn >= 11 ? (unsigned)(batch << (logn - 11)) : 0;
    int i0 = mall_split(&pl, logn, batch);
    if (i0 < 0) {
        for (int i = 0; i < pl.npass; i++)
            launch_b16(c, x, logn, pl.s_lo[i], pl.s_hi[i] - pl.s_lo[i] + 1, &pt->f[i], 0, 0.0, blocks, s);
        launch_b1(c, x, 0, 0, 0, 0, 0.0, 0, (unsigned)(batch << (logn - B1_LGL)), s);
        return;
    }
    for (int i = 0; i < i0; i++)
        launch_b16(c, x, logn, pl.s_lo[i], pl.s_hi[i] - pl.s_lo[i] + 1, &pt->f[i], 0, 0.0, blocks, s);
    int lc = ntt_mall_get(), lg = lc < logn ? lc : logn;
    size_t tot = batch << logn, C = (size_t)1 << lc;
    for (size_t off = 0; off < tot; off += C) {
        size_t len = tot - off < C ? tot - off : C, bc = len >> lg;
        for (int i = i0; i < pl.npass; i++)
            launch_b16(c, x + off, lg, pl.s_lo[i], pl.s_hi[i] - pl.s_lo[i] + 1, &pt->f[i], 0, 0.0, (unsigned)(bc << (lg - 11)), s);
        launch_b1(c, x + off, 0, 0, 0, 0, 0.0, 0, (unsigned)(bc << (lg - B1_LGL)), s);
    }
}

/* the inverse with the pointwise product fused into the b1 pass; Lt = points per transform of the y layout
 * (2^logn, or 3 2^logn for the thirds of a radix-3 transform, where batch counts the thirds) */
static void inv_common(ntt_ctx *c, uint64_t *x, const uint64_t *y, size_t Lt, int ymode, int logn, size_t batch, hipStream_t s)
{
    struct plan pl;
    check_logn(logn);
    make_plan(&pl, logn);
    struct plan_tw *pt = get_plan_tw(c, logn, &pl);
    unsigned blocks = logn >= 11 ? (unsigned)(batch << (logn - 11)) : 0;
    double sc = c->ninv[logn];
    int i0 = mall_split(&pl, logn, batch);
    if (i0 < 0) {
        launch_b1(c, x, y, Lt, ymode, y ? 2 : 1, pl.npass ? 0.0 : sc, 0, (unsigned)(batch << (logn - B1_LGL)), s);
        for (int i = pl.npass - 1; i >= 0; i--)
            launch_b16(c, x, logn, pl.s_lo[i], pl.s_hi[i] - pl.s_lo[i] + 1, &pt->i[i], 1, i == 0 ? sc : 0.0, blocks, s);
        return;
    }
    int lc = ntt_mall_get(), lg = lc < logn ? lc : logn;
    size_t tot = batch << logn, C = (size_t)1 << lc;
    for (size_t off = 0; off < tot; off += C) {
        size_t len = tot - off < C ? tot - off : C, bc = len >> lg;
        launch_b1(c, x + off, y, Lt, ymode, y ? 2 : 1, pl.npass ? 0.0 : sc, off, (unsigned)(bc << (lg - B1_LGL)), s);
        for (int i = pl.npass - 1; i >= i0; i--)
            launch_b16(c, x + off, lg, pl.s_lo[i], pl.s_hi[i] - pl.s_lo[i] + 1, &pt->i[i], 1, i == 0 ? sc : 0.0, (unsigned)(bc << (lg - 11)), s);
    }
    for (int i = i0 - 1; i >= 0; i--)
        launch_b16(c, x, logn, pl.s_lo[i], pl.s_hi[i] - pl.s_lo[i] + 1, &pt->i[i], 1, i == 0 ? sc : 0.0, blocks, s);
}
/* H2 timing: one pass of the forward / inverse alone (pass < npass - 1: b16 pass `pass`; npass - 1: the b1 pass),
 * no scale, no pointwise product */
void ntt_pass(ntt_ctx *c, uint64_t *x, int logn, size_t batch, int inv, int pass, hipStream_t s)
{
    struct plan pl;
    check_logn(logn);
    make_plan(&pl, logn);
    struct plan_tw *pt = get_plan_tw(c, logn, &pl);
    unsigned blocks = logn >= 11 ? (unsigned)(batch << (logn - 11)) : 0;
    if (pass < pl.npass) launch_b16(c, x, logn, pl.s_lo[pass], pl.s_hi[pass] - pl.s_lo[pass] + 1, inv ? &pt->i[pass] : &pt->f[pass], inv, 0.0, blocks, s);
    else launch_b1(c, x, 0, 0, 0, inv ? 1 : 0, 0.0, 0, (unsigned)(batch << (logn - B1_LGL)), s);
}
void ntt_inv(ntt_ctx *c, uint64_t *x, int logn, size_t batch, hipStream_t s) { inv_common(c, x, 0, 0, 0, logn, batch, s); }
void ntt_inv_pw_y(ntt_ctx *c, uint64_t *x, const uint64_t *y, int ymode, int logn, size_t batch, hipStream_t s)
{
    if (logn >= ntt_pw_fuse) inv_common(c, x, y, (size_t)1 << logn, ymode, logn, batch, s);
    else { ntt_pw_y(c, x, y, ymode, 0, logn, batch, s); inv_common(c, x, 0, 0, 0, logn, batch, s); }
}
void ntt_inv_pw(ntt_ctx *c, uint64_t *x, const uint64_t *y, int logn, size_t batch, hipStream_t s) { ntt_inv_pw_y(c, x, y, NTT_Y_FULL, logn, batch, s); }
void ntt_inv_pw_bcast(ntt_ctx *c, uint64_t *x, const uint64_t *y, int logn, size_t batch, hipStream_t s) { ntt_inv_pw_y(c, x, y, NTT_Y_BCAST, logn, batch, s); }
/* B1: the inverse of a radix-3 batch (3 batch thirds of 2^logk points; ntt3.c applies the radix-3 stage after)
 * with the pointwise product against y in the given layout fused into the b1 pass */
void ntt_inv3_core_pw(ntt_ctx *c, uint64_t *x, const uint64_t *y, int ymode, int logk, size_t batch, hipStream_t s)
{
    inv_common(c, x, y, (size_t)3 << logk, ymode, logk, 3 * batch, s);
}
void ntt_pw(ntt_ctx *c, uint64_t *x, const uint64_t *y, size_t count, hipStream_t s)
{
    size_t blocks = (count + 255) / 256; if (blocks > 228 * 16) blocks = 228 * 16;
    k_pw<<<(unsigned)blocks, 256, 0, s>>>(x, y, ~(size_t)0, count, c->m);
}
void ntt_pw_bcast(ntt_ctx *c, uint64_t *x, const uint64_t *y, int logn, size_t count, hipStream_t s)
{
    size_t blocks = (count + 255) / 256; if (blocks > 228 * 16) blocks = 228 * 16;
    k_pw<<<(unsigned)blocks, 256, 0, s>>>(x, y, ((size_t)1 << logn) - 1, count, c->m);
}
void ntt_pw_y(ntt_ctx *c, uint64_t *x, const uint64_t *y, int ymode, int r3, int logk, size_t batch, hipStream_t s)
{
    size_t Lt = (size_t)(r3 ? 3 : 1) << logk, count = batch * Lt;
    if (ymode == NTT_Y_FULL) { ntt_pw(c, x, y, count, s); return; }
    if (ymode == NTT_Y_BCAST && !r3) { ntt_pw_bcast(c, x, y, logk, count, s); return; }
    size_t blocks = (count + 255) / 256; if (blocks > 228 * 16) blocks = 228 * 16;
    k_pw_y<<<(unsigned)blocks, 256, 0, s>>>(x, y, Lt, logk, r3, ymode, count, c->m);
}
void ntt_load(ntt_ctx *c, uint64_t *dst, const uint64_t *src, size_t nlimbs, size_t npoints, hipStream_t s)
{
    size_t blocks = (npoints + 255) / 256; if (blocks > 228 * 16) blocks = 228 * 16;
    k_load<<<(unsigned)blocks, 256, 0, s>>>(dst, src, nlimbs, npoints, c->m);
}

/* ---- host references ------------------------------------------------------ */
void ntt_host_fwd(uint64_t *x, int logn, int prime)
{
    uint64_t p = ec_P[prime];
    size_t n = (size_t)1 << logn, h, i, r;
    uint64_t wn = ec_root(prime, logn);
    for (h = n / 2; h >= 1; h >>= 1) {
        uint64_t w2h = ec_powmod(wn, n / (2 * h), p);
#pragma omp parallel for schedule(static) private(r)
        for (i = 0; i < n; i += 2 * h) {
            uint64_t w = 1;
            for (r = 0; r < h; r++) {
                uint64_t u = x[i + r], v = x[i + r + h];
                x[i + r] = (u + v) % p;
                x[i + r + h] = ec_mulmod_ref((u + p - v) % p, w, p);
                w = ec_mulmod_ref(w, w2h, p);
            }
        }
    }
}
void ntt_host_inv(uint64_t *x, int logn, int prime)
{
    uint64_t p = ec_P[prime];
    size_t n = (size_t)1 << logn, h, i, r;
    uint64_t wn = ec_root_inv(prime, logn), ninv = ec_inv(n % p, p);
    for (h = 1; h < n; h <<= 1) {
        uint64_t w2h = ec_powmod(wn, n / (2 * h), p);
#pragma omp parallel for schedule(static) private(r)
        for (i = 0; i < n; i += 2 * h) {
            uint64_t w = 1;
            for (r = 0; r < h; r++) {
                uint64_t u = x[i + r], v = ec_mulmod_ref(x[i + r + h], w, p);
                x[i + r] = (u + v) % p;
                x[i + r + h] = (u + p - v) % p;
                w = ec_mulmod_ref(w, w2h, p);
            }
        }
    }
    for (i = 0; i < n; i++) x[i] = ec_mulmod_ref(x[i], ninv, p);
}

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
template <int INV, int R4, int SW>
__global__ __launch_bounds__(THREADS)
void k_b16r(uint64_t *x, int logn, int s_lo, ec_mod m, const double *tlo, const double *thi,
            const double *tabT, double scale)
{
    constexpr int STG = 7, tile = 128;
    __shared__ uint64_t sh[128 * BP + 1];
    __shared__ double tab[tile];
    const int tt = threadIdx.x >> 4, bb = threadIdx.x & 15;
    const size_t n = (size_t)1 << logn, hmin = (size_t)1 << s_lo, slabs = hmin / 16;
    const size_t bpt = n / 2048, b = blockIdx.x % bpt, t = blockIdx.x / bpt;
    const size_t blk_hi = b / slabs, slab0 = b % slabs;
    const size_t base = t * n + blk_hi * (size_t)tile * hmin + slab0 * 16;
    const double p = m.p, pinv = m.pinv;
    const uint64_t pu = m.pu, p2 = 2 * pu;
    uint64_t v[8];
    double T[INV ? STG : 1];
    int i;

    for (int k = threadIdx.x; k < tile; k += THREADS) tab[k] = tabT[k];
    {
        size_t c = slab0 * 16 + bb;
        T[INV ? STG - 1 : 0] = ec_mm(tlo[c & 4095], thi[c >> 12], p, pinv);
        if (INV) {
#pragma unroll
            for (int l = STG - 1; l > 0; l--) T[l - 1] = ec_mm(T[l], T[l], p, pinv);
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
    /* one stage on the registers: register bit `rb` pairs (i, i + 2^rb); r(i) = row & (H-1) */
#define STAGE(lgH, rb, ROWF)                                                          \
    do {                                                                              \
        const int H_ = 1 << (lgH), lgstep_ = STG - 1 - (lgH);                         \
        _Pragma("unroll")                                                             \
        for (int i_ = 0; i_ < 8; i_++) if (!(i_ & (1 << (rb)))) {                     \
            int r_ = ROWF(i_) & (H_ - 1);                                             \
            double w_ = ec_mm(tab[r_ << lgstep_], T[INV ? (lgH) : 0], p, pinv);       \
            BFLY(v[i_], v[i_ + (1 << (rb))], w_);                                     \
        }                                                                             \
        if (!INV) T[0] = ec_mm(T[0], T[0], p, pinv);                                  \
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
        const double w4_ = tab[tile / 4];                                             \
        _Pragma("unroll")                                                             \
        for (int i_ = 0; i_ < 8; i_++) if (!(i_ & (3 << ((rb) - 1)))) {               \
            int r_ = ROWF(i_) & (H_ - 1);                                             \
            double w_ = ec_mm(tab[r_ << lgstep_], T[INV ? (lgH) : 0], p, pinv);       \
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
#define TO_SH(ROWF)   _Pragma("unroll") for (i = 0; i < 8; i++) sh[ROWF(i) * BP + bb] = v[i]
#define FROM_SH(ROWF) _Pragma("unroll") for (i = 0; i < 8; i++) v[i] = sh[ROWF(i) * BP + bb]

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
}

/* b1 pass with Shoup's integer modmul for the twiddle products (Phase 5 item
 * 1): tab1s holds the twiddles, tab1sp their Shoup constants floor(w 2^64/p);
 * shoup(d, w) = d w - floor(w' d / 2^64) p is in [0, 2p) for any 64-bit d.
 * The scale and the fused pointwise product keep the FP64 modmul (variable
 * multiplier). */
template <int LGL, int MODE>
__global__ __launch_bounds__(THREADS)
void k_b1s(uint64_t *x, const uint64_t *y, size_t Lt, int ymode, ec_mod m, const uint64_t *tab1s, const uint64_t *tab1sp, double scale)
{
    constexpr int L = 1 << LGL;
    __shared__ uint64_t sh[L];
    __shared__ uint64_t tab[L / 2], tabp[L / 2];
    const size_t base = (size_t)blockIdx.x * L, ybase = MODE == 2 ? y_base(base, Lt, ymode) : 0;
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
template <int LGL, int MODE>
__global__ __launch_bounds__(THREADS)
void k_b1(uint64_t *x, const uint64_t *y, size_t Lt, int ymode, ec_mod m, const double *tab1, double scale)
{
    constexpr int L = 1 << LGL;
    __shared__ uint64_t sh[L];
    __shared__ double tab[L / 2];
    const size_t base = (size_t)blockIdx.x * L, ybase = MODE == 2 ? y_base(base, Lt, ymode) : 0;
    const double p = m.p, pinv = m.pinv;
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
                uint64_t vw = (uint64_t)ec_mm((double)v, tab[r << lgstep], p, pinv);
                uint64_t sum = u + vw, d = u + pu - vw;
                if (sum >= p2) sum -= p2;
                if (d >= p2) d -= p2;
                sh[j0] = sum; sh[j1] = d;
            } else {
                uint64_t sum = u + v, d = u - v + p2;
                if (sum >= p2) sum -= p2;
                if (d >= p2) d -= p2;
                sh[j0] = sum;
                sh[j1] = (uint64_t)ec_mm((double)d, tab[r << lgstep], p, pinv);
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
    size_t words = 0; for (int stg = 1; stg <= 7; stg++) words += (size_t)2 << stg; words += 2 * n1 + 4 * n1;   /* tabT x 2, tab1 x 2, tab1s/tab1sp x 2 (all 8-byte entries) */
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
    uint64_t *d = (uint64_t *)dev_upload(h, words * 8); free(h); off = 0;
    for (int stg = 1; stg <= 7; stg++) { c->tabT_f[stg] = (double *)(d + off); off += (size_t)1 << stg; c->tabT_i[stg] = (double *)(d + off); off += (size_t)1 << stg; }
    c->tab1_f = (double *)(d + o1f); c->tab1_i = (double *)(d + o1i);
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
#define LAUNCH_R(INV, R4, SW) k_b16r<INV, R4, SW><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, tb, scale)
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

void ntt_fwd(ntt_ctx *c, uint64_t *x, int logn, size_t batch, hipStream_t s)
{
    struct plan pl;
    check_logn(logn);
    make_plan(&pl, logn);
    struct plan_tw *pt = get_plan_tw(c, logn, &pl);
    unsigned blocks = logn >= 11 ? (unsigned)(batch << (logn - 11)) : 0;
    for (int i = 0; i < pl.npass; i++)
        launch_b16(c, x, logn, pl.s_lo[i], pl.s_hi[i] - pl.s_lo[i] + 1, &pt->f[i], 0, 0.0, blocks, s);
    if (ntt_b1_shoup) k_b1s<B1_LGL, 0><<<(unsigned)(batch << (logn - B1_LGL)), THREADS, 0, s>>>(x, 0, 0, 0, c->m, c->tab1s_f, c->tab1sp_f, 0.0);
    else k_b1<B1_LGL, 0><<<(unsigned)(batch << (logn - B1_LGL)), THREADS, 0, s>>>(x, 0, 0, 0, c->m, c->tab1_f, 0.0);
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
    unsigned b1 = (unsigned)(batch << (logn - B1_LGL));
    if (ntt_b1_shoup) {
        if (y) k_b1s<B1_LGL, 2><<<b1, THREADS, 0, s>>>(x, y, Lt, ymode, c->m, c->tab1s_i, c->tab1sp_i, pl.npass ? 0.0 : sc);
        else   k_b1s<B1_LGL, 1><<<b1, THREADS, 0, s>>>(x, 0, 0, 0, c->m, c->tab1s_i, c->tab1sp_i, pl.npass ? 0.0 : sc);
    } else if (y) k_b1<B1_LGL, 2><<<b1, THREADS, 0, s>>>(x, y, Lt, ymode, c->m, c->tab1_i, pl.npass ? 0.0 : sc);
    else   k_b1<B1_LGL, 1><<<b1, THREADS, 0, s>>>(x, 0, 0, 0, c->m, c->tab1_i, pl.npass ? 0.0 : sc);
    for (int i = pl.npass - 1; i >= 0; i--)
        launch_b16(c, x, logn, pl.s_lo[i], pl.s_hi[i] - pl.s_lo[i] + 1, &pt->i[i], 1, i == 0 ? sc : 0.0, blocks, s);
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

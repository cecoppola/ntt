/* ntt.c - tiled DIF forward / DIT inverse NTT, FP64-Barrett arithmetic.
 * See ntt.h.  Kernels are the verified ones from bench/16_ntt_tile (RESULTS.md
 * 25, 33), made two-directional and given the scale / pointwise fusions.
 */
#include <stdio.h>
#include "fatal.h"
#include "env_int.h"
#include <stdlib.h>
#include <string.h>
#include "ntt.h"

#define BP 17                      /* LDS pad: sh[128][17], conflict-free columns */
#define THREADS 256
#define B1_LGL 10                  /* b1 block length 2^10 (rule 3b) */
#define B1R_TW 4096                /* the register-blocked b1's per-stage twiddle table (b1 lengths up to 2^12) */

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)

int ntt_stg = 7;
int ntt_pw_fuse = 14;
/* ntt_stg and ntt_pw_fuse are internal (the tests' pass-split and fusion checks set them); the kernel choice is fixed:
 * 7-stage passes in the register-blocked body, the reduced-correction Barrett, the register-blocked b1 pass
 * with 2^3 points per thread, the per-logn pass plan (plan_auto) */

/* ---- the reduced-correction FP64 Barrett.  The quotient is taken from the exact product
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
/* where block `base` of x (a batch of transforms of Lt points) reads its pointwise operand:
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
    const size_t xo = t * n + blk_hi * (size_t)tile * hmin + slab0 * 16;
    const double p = m.p, pinv = m.pinv;
    const uint64_t pu = m.pu, p2 = 2 * pu;
    double T[NT][INV ? STG : 1];
    int j, k, mm_;
#define EOFF(j_) ((size_t)((j_) >> STG) * 16 + (size_t)((j_) & (tile - 1)) * hmin + bb)

    for (k = threadIdx.x; k < tile; k += THREADS) tab[k] = tabT[k];
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
#pragma unroll
    for (j = tt; j < 128; j += 16) sh[j * BP + bb] = x[xo + EOFF(j)];
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
        x[xo + EOFF(j)] = v;
    }
#undef EOFF
}


/* Register-blocked body for a 7-stage pass (tile 128, NS = 1): thread
 * (tt = tid>>4, bb = tid&15) keeps 8 rows of column bb in registers and runs
 * the stages in three groups with two LDS exchanges (the tile kernel does an
 * LDS round trip per stage):
 *   group A  h = 64, 32, 16   rows tt + 16 i
 *   group B  h = 8, 4, 2      rows ((tt>>1)<<4) | (i<<1) | (tt&1)
 *   group C  h = 1            rows 8 tt + i
 * INV runs the groups in reverse (DIT).  the reduced-correction Barrett: data products lazy in [0, 2p) (mm_lazy), the twiddles canonical
 * (mm_canon); the inverse's difference is u + 2p - vw.  Exact, so the canonical outputs equal the tile kernel's. */
template <int INV>
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
    const size_t xo = t * n + blk_hi * (size_t)tile * hmin + slab0 * 16;
    const double p = m.p, pinv = m.pinv;
    const double pinvl = fma(-p, pinv, 1.0) * pinv;
    const uint64_t pu = m.pu, p2 = 2 * pu;
    uint64_t v[8];
    double T[INV ? STG : 1];
    int i;

    for (int k = threadIdx.x; k < tile; k += THREADS) tab[k] = tabT[k];
    {
        size_t c = slab0 * 16 + bb;
        T[INV ? STG - 1 : 0] = mm_canon(tlo[c & 4095], thi[c >> 12], p, pinv, pinvl);
        if (INV) {
#pragma unroll
            for (int l = STG - 1; l > 0; l--) T[l - 1] = mm_canon(T[l], T[l], p, pinv, pinvl);
        }
    }
    __syncthreads();

#define ROW_A(i) (tt + 16 * (i))
#define ROW_B(i) ((((tt) >> 1) << 4) | ((i) << 1) | ((tt) & 1))
#define ROW_C(i) (8 * (tt) + (i))
    /* data times the twiddle of table index ix_ and stage slot ti_, lazy [0, 2p) */
#define MULW(val_, ix_, ti_) mm_lazy((double)(val_), mm_canon(tab[ix_], T[ti_], p, pinv, pinvl), p, pinv, pinvl)
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
            BFLYM(v[i_], v[i_ + (1 << (rb))], r_ << lgstep_, INV ? (lgH) : 0);        \
        }                                                                             \
        if (!INV) T[0] = mm_canon(T[0], T[0], p, pinv, pinvl);                        \
    } while (0)
#define LOAD_G(ROWF)  _Pragma("unroll") for (i = 0; i < 8; i++) v[i] = x[xo + (size_t)ROWF(i) * hmin + bb]
#define STORE_G(ROWF) _Pragma("unroll") for (i = 0; i < 8; i++) { uint64_t o = v[i];                        \
                          if (scale != 0.0) o = (uint64_t)ec_mm((double)o, scale, p, pinv);                 \
                          x[xo + (size_t)ROWF(i) * hmin + bb] = o; }
#define TO_SH(ROWF)   _Pragma("unroll") for (i = 0; i < 8; i++) sh[ROWF(i) * BP + bb] = v[i]
#define FROM_SH(ROWF) _Pragma("unroll") for (i = 0; i < 8; i++) v[i] = sh[ROWF(i) * BP + bb]

    if (!INV) {
        LOAD_G(ROW_A);
        STAGE(6, 2, ROW_A); STAGE(5, 1, ROW_A); STAGE(4, 0, ROW_A);
        TO_SH(ROW_A); __syncthreads(); FROM_SH(ROW_B);
        STAGE(3, 2, ROW_B); STAGE(2, 1, ROW_B); STAGE(1, 0, ROW_B);
        __syncthreads(); TO_SH(ROW_B); __syncthreads(); FROM_SH(ROW_C);
        STAGE(0, 0, ROW_C);
        STORE_G(ROW_C);
    } else {
        LOAD_G(ROW_C);
        STAGE(0, 0, ROW_C);
        TO_SH(ROW_C); __syncthreads(); FROM_SH(ROW_B);
        STAGE(1, 0, ROW_B); STAGE(2, 1, ROW_B); STAGE(3, 2, ROW_B);
        __syncthreads(); TO_SH(ROW_B); __syncthreads(); FROM_SH(ROW_A);
        STAGE(4, 0, ROW_A); STAGE(5, 1, ROW_A); STAGE(6, 2, ROW_A);
        STORE_G(ROW_A);
    }
#undef ROW_A
#undef ROW_B
#undef ROW_C
#undef STAGE
#undef LOAD_G
#undef STORE_G
#undef TO_SH
#undef FROM_SH
#undef MULW
#undef BFLYM
}

/* ---- the register-blocked b1 pass.  Block = one 2^LGL-point chunk (LGL 10..12), 2^(LGL-LGV)
 * threads, each keeping V = 2^LGV points in registers.  The LGL stages run in G = ceil(LGL / LGV) groups; group g
 * butterflies the index bits [Bg, Bg + Cg) (forward: g = 0 is the top bits, Bg = LGL - (g+1) LGV clipped at 0), with the
 * point of thread t, register i at
 *   idx = t[0, Bg) | i_lo << Bg | t[Bg, ..) << (Bg + Cg) | i_hi << (LGL - (LGV - Cg))      (i_lo = i's low Cg bits),
 * so the first group's load (and the last inverse group's store) is coalesced.  Between groups one LDS exchange (a plain kernel
 * would do one LDS round trip per stage, ten per pass).  LDS addresses are XOR-swizzled by a linear map of
 * index bits 4.. into bits 0..3 (b1r_swz), found by search to be conflict-free (16 distinct 8-byte banks per 16 lanes)
 * for every group at LGV 3 and 4, LGL 10..12.  Twiddles: tw[H + r] = w_2H^r per stage (one global table per direction,
 * 4096 entries).  The reduced-correction Barrett (mm_lazy); the final canonicalisation and scale as the plain b1 pass
 * (the outputs are canonical, hence unique).
 * Built with 2^3 points per thread (LGV 3; 4 was slower, K13b). */
__device__ static inline unsigned b1r_swz(unsigned x)
{
    const unsigned h = x >> 4;
    unsigned r = 0;
    r ^= (0u - (h & 1)) & 13u;        r ^= (0u - ((h >> 1) & 1)) & 7u;  r ^= (0u - ((h >> 2) & 1)) & 14u; r ^= (0u - ((h >> 3) & 1)) & 8u;
    r ^= (0u - ((h >> 4) & 1)) & 12u; r ^= (0u - ((h >> 5) & 1)) & 11u; r ^= (0u - ((h >> 6) & 1)) & 12u; r ^= (0u - ((h >> 7) & 1)) & 7u;
    return x ^ r;
}
template <int LGL, int LGV, int g> struct b1r_grp {
    static constexpr int B = LGL - (g + 1) * LGV > 0 ? LGL - (g + 1) * LGV : 0;
    static constexpr int C = LGL - g * LGV - B;
    static constexpr int NH = LGV - C;
    __device__ static inline unsigned tpart(unsigned t) { return (t & ((1u << B) - 1)) | ((t >> B) << (B + C)); }
    __device__ static inline unsigned ipart(int i) { return ((unsigned)(i & ((1 << C) - 1)) << B) | ((unsigned)(i >> C) << (LGL - NH)); }
};
/* the stages of group g on the registers: forward descending (DIF), inverse ascending (DIT) */
template <int LGL, int LGV, int g, int INV>
__device__ static inline void b1r_stages(uint64_t *v, unsigned t, const double *tw, double p, double pinv, double pinvl, uint64_t pu, uint64_t p2)
{
    typedef b1r_grp<LGL, LGV, g> GR;
    constexpr int V = 1 << LGV;
    const unsigned tlo = t & ((1u << GR::B) - 1);
#pragma unroll
    for (int kk = 0; kk < GR::C; kk++) {
        const int k = INV ? kk : GR::C - 1 - kk;
        const unsigned H = 1u << (GR::B + k);
        double w[V / 2];
#pragma unroll
        for (int j = 0; j < (1 << k); j++) w[j] = tw[H + tlo + ((unsigned)j << GR::B)];
#pragma unroll
        for (int i = 0; i < V; i++) if (!(i & (1 << k))) {
            const int j = i & ((1 << k) - 1), i1 = i + (1 << k);
            uint64_t u = v[i], x = v[i1];
            if (INV) {
                uint64_t vw = mm_lazy((double)x, w[j], p, pinv, pinvl);
                uint64_t s_ = u + vw, d_ = u + p2 - vw;
                if (s_ >= p2) s_ -= p2;
                if (d_ >= p2) d_ -= p2;
                v[i] = s_; v[i1] = d_;
            } else {
                uint64_t s_ = u + x, d_ = u - x + p2;
                if (s_ >= p2) s_ -= p2;
                if (d_ >= p2) d_ -= p2;
                v[i] = s_;
                v[i1] = mm_lazy((double)d_, w[j], p, pinv, pinvl);
            }
        }
    }
}
/* LDS exchange from group layout ga to gb (a barrier before the writes guards the previous exchange's reads) */
template <int LGL, int LGV, int ga, int gb>
__device__ static inline void b1r_xchg(uint64_t *v, uint64_t *sh, unsigned t, int first)
{
    constexpr int V = 1 << LGV;
    typedef b1r_grp<LGL, LGV, ga> GA; typedef b1r_grp<LGL, LGV, gb> GB;
    if (!first) __syncthreads();
    const unsigned sa = b1r_swz(GA::tpart(t)), sb = b1r_swz(GB::tpart(t));
#pragma unroll
    for (int i = 0; i < V; i++) sh[sa ^ b1r_swz(GA::ipart(i))] = v[i];
    __syncthreads();
#pragma unroll
    for (int i = 0; i < V; i++) v[i] = sh[sb ^ b1r_swz(GB::ipart(i))];
}
template <int LGL, int LGV, int g, int INV>
__device__ static inline void b1r_run(uint64_t *v, uint64_t *sh, unsigned t, const double *tw, double p, double pinv, double pinvl, uint64_t pu, uint64_t p2)
{
    constexpr int G = (LGL + LGV - 1) / LGV;
    constexpr int gg = INV ? G - 1 - g : g;            /* the group this step runs */
    b1r_stages<LGL, LGV, gg, INV>(v, t, tw, p, pinv, pinvl, pu, p2);
    if constexpr (g + 1 < G) {
        constexpr int gn = INV ? gg - 1 : gg + 1;
        b1r_xchg<LGL, LGV, gg, gn>(v, sh, t, g == 0);
        b1r_run<LGL, LGV, g + 1, INV>(v, sh, t, tw, p, pinv, pinvl, pu, p2);
    }
}
template <int LGL, int LGV, int MODE>
__global__ __launch_bounds__(1 << (LGL - LGV))
void k_b1r(uint64_t *x, const uint64_t *y, size_t Lt, int ymode, ec_mod m, const double *tw, double scale, size_t goff)
{
    constexpr int L = 1 << LGL, V = 1 << LGV, G = (LGL + LGV - 1) / LGV, INV = MODE != 0;
    constexpr int gin = INV ? G - 1 : 0, gout = INV ? 0 : G - 1;
    typedef b1r_grp<LGL, LGV, gin> GI; typedef b1r_grp<LGL, LGV, gout> GO;
    __shared__ uint64_t sh[L];
    const unsigned t = threadIdx.x;
    const size_t base = (size_t)blockIdx.x * L, ybase = MODE == 2 ? y_base(goff + base, Lt, ymode) : 0;
    const double p = m.p, pinv = m.pinv;
    const double pinvl = fma(-p, pinv, 1.0) * pinv;
    const uint64_t pu = m.pu, p2 = 2 * pu;
    uint64_t v[V];
    const unsigned ti = GI::tpart(t), to = GO::tpart(t);
#pragma unroll
    for (int i = 0; i < V; i++) {
        const size_t k = ti | GI::ipart(i);
        v[i] = MODE == 2 ? (uint64_t)ec_mm((double)x[base + k], (double)y[ybase + k], p, pinv) : x[base + k];
    }
    b1r_run<LGL, LGV, 0, INV>(v, sh, t, tw, p, pinv, pinvl, pu, p2);
#pragma unroll
    for (int i = 0; i < V; i++) {
        uint64_t o = v[i];
        if (MODE == 0) { if (o >= pu) o -= pu; }
        else if (scale != 0.0) o = (uint64_t)ec_mm((double)o, scale, p, pinv);
        x[base + (to | GO::ipart(i))] = o;
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
struct plan_tw { int built, key; struct pass_tw f[NTT_MAXPASS], i[NTT_MAXPASS]; };

struct ntt_ctx {
    int prime;
    ec_mod m;
    double *tabT_f[8], *tabT_i[8];                 /* 2^stg-th roots, stg 1..7 */
    double *tw1r_f, *tw1r_i;                       /* the register-blocked b1's per-stage twiddles tw[H + r] = w_2H^r, H = 1 .. 2048 */
    double ninv[NTT_LOGN_MAX + 1];
    struct plan_tw plan[NTT_LOGN_MAX + 1];
};
static void free_plan_tw(struct plan_tw *pt);

struct plan { int npass, lb, key, s_lo[NTT_MAXPASS], s_hi[NTT_MAXPASS]; };
/* the pass boundaries.  Plan 0: b1 on stages lb-1..0 with lb = 10, b16 passes of stg stages from the top down,
 * the partial pass (if any) at the bottom.  10 lb + d (lb = 10, 11, 12; d = 0 top-down as plan 0, d = 1 bottom-up: the full
 * passes from lb upward, the partial one at the top), so the row strides 2^s_lo of the passes can be moved off the slow ones
 * (s_lo 17 and 24, archive:results_merged_2026-10-24/KERNELS.md §K13).  Per logn the plan listed in plan_auto (measured, archive:results_merged_2026-10-24/KERNELS.md §K13b).  Every plan computes the same canonical outputs. */
static int plan_auto(int logn)
{
    /* measured (archive:results_merged_2026-10-24/KERNELS.md §K13b, t_ntt bench whole/pass, NTT_B1R 3/4): a b1 of 2^11 or 2^12 points saves a whole pass
     * where the default plan ends in a b16 pass of 1 or 2 stages ((logn - 10) mod 7 = 1, 2: 1.25-1.40x at 2^25, 2^26);
     * at 2^31 the bottom-up plan with a 2^12 b1 moves the passes off s_lo 24 and 17 (s_lo 26, 19, 12: 1.06x fwd, 1.12x
     * inv); at 2^24 (s_lo 17) no plan beats the default */
    switch (logn) {
    case 11: case 18: case 25: case 32: return 110;
    case 12: case 19: case 26: case 33: return 120;
    case 31: return 121;
    default: return 0;
    }
}
static void make_plan(struct plan *pl, int logn)
{
    int stg = ntt_stg < 3 ? 3 : ntt_stg > 7 ? 7 : ntt_stg, pc = plan_auto(logn);
    int lb = pc ? pc / 10 : B1_LGL, up = pc % 10;
    if (lb > logn) lb = logn;
    pl->npass = 0; pl->lb = lb; pl->key = pc * 8 + stg;
    if (!up) {
        int hi = logn - 1;
        while (hi >= lb) {
            int lo = hi - stg + 1; if (lo < lb) lo = lb;
            if (pl->npass == NTT_MAXPASS) { ec_fatal(EC_RC_FATAL, "ntt: too many passes for logn %d stg %d\n", logn, stg); }
            pl->s_hi[pl->npass] = hi; pl->s_lo[pl->npass] = lo; pl->npass++;
            hi = lo - 1;
        }
    } else {
        int lo = lb, n = 0, slo[NTT_MAXPASS], shi[NTT_MAXPASS];
        while (lo < logn) {
            int hi = lo + stg - 1; if (hi > logn - 1) hi = logn - 1;
            if (n == NTT_MAXPASS) { ec_fatal(EC_RC_FATAL, "ntt: too many passes for logn %d stg %d\n", logn, stg); }
            slo[n] = lo; shi[n] = hi; n++;
            lo = hi + 1;
        }
        for (int i = 0; i < n; i++) { pl->s_lo[i] = slo[n - 1 - i]; pl->s_hi[i] = shi[n - 1 - i]; }   /* forward order: top first */
        pl->npass = n;
    }
}
int ntt_npass(int logn) { struct plan pl; make_plan(&pl, logn); return pl.npass + 1; }

/* a context's tables (and each plan's pass twiddles) are built in one host buffer and go to the
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
    size_t words = 0; for (int stg = 1; stg <= 7; stg++) words += (size_t)2 << stg; words += 2 * B1R_TW;   /* tabT x 2, tw1r x 2 (all 8-byte entries) */
    uint64_t *h = (uint64_t *)malloc(words * 8); size_t off = 0;
    for (int stg = 1; stg <= 7; stg++) {
        host_table_pow((double *)(h + off), ec_root(prime, stg), (size_t)1 << stg, p); off += (size_t)1 << stg;
        host_table_pow((double *)(h + off), ec_root_inv(prime, stg), (size_t)1 << stg, p); off += (size_t)1 << stg;
    }
    size_t otw[2];
    for (int inv = 0; inv < 2; inv++) {
        otw[inv] = off; h[off] = 0;
        for (int lgH = 0; (2 << lgH) <= B1R_TW; lgH++)
            host_table_pow((double *)(h + off + ((size_t)1 << lgH)), inv ? ec_root_inv(prime, lgH + 1) : ec_root(prime, lgH + 1), (size_t)1 << lgH, p);
        off += B1R_TW;
    }
    uint64_t *d = (uint64_t *)dev_upload(h, words * 8); free(h); off = 0;
    for (int stg = 1; stg <= 7; stg++) { c->tabT_f[stg] = (double *)(d + off); off += (size_t)1 << stg; c->tabT_i[stg] = (double *)(d + off); off += (size_t)1 << stg; }
    c->tw1r_f = (double *)(d + otw[0]); c->tw1r_i = (double *)(d + otw[1]);
    for (int l = 0; l <= NTT_LOGN_MAX; l++) c->ninv[l] = (double)ec_inv(ec_powmod(2, l, p), p);
    return c;
}
void ntt_ctx_free(ntt_ctx *c)
{
    if (!c) return;
    HIP_CHECK(hipFree(c->tabT_f[1]));                       /* the one buffer of all the tables */
    for (int l = 0; l <= NTT_LOGN_MAX; l++) if (c->plan[l].built) free_plan_tw(&c->plan[l]);
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
static void free_plan_tw(struct plan_tw *pt)
{
    for (int i = 0; i < NTT_MAXPASS; i++) {
        if (pt->f[i].tlo) HIP_CHECK(hipFree(pt->f[i].tlo));   /* thi lives in the same buffer */
        if (pt->i[i].tlo) HIP_CHECK(hipFree(pt->i[i].tlo));
        pt->f[i].tlo = pt->i[i].tlo = 0;
    }
    pt->built = 0;
}
static struct plan_tw *get_plan_tw(ntt_ctx *c, int logn, const struct plan *pl)
{
    struct plan_tw *pt = &c->plan[logn];
    if (pt->built && pt->key != pl->key) { HIP_CHECK(hipDeviceSynchronize()); free_plan_tw(pt); }   /* another plan (ntt_stg switched by a test) */
    if (!pt->built) {
        for (int i = 0; i < pl->npass; i++) {
            build_pass_tw(&pt->f[i], c->prime, pl->s_lo[i], pl->s_hi[i], 0);
            build_pass_tw(&pt->i[i], c->prime, pl->s_lo[i], pl->s_hi[i], 1);
        }
        pt->built = 1; pt->key = pl->key;
    }
    return pt;
}

static void check_logn(int logn)
{
    if (logn < NTT_LOGN_MIN || logn > NTT_LOGN_MAX) { ec_fatal(EC_RC_FATAL, "ntt: logn %d out of range\n", logn); }
}

#define LAUNCH_B16(S, INV) case S: k_b16<S, INV><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, INV ? c->tabT_i[S] : c->tabT_f[S], scale); break;
/* a b16 pass of stg stages: 7 in the register-blocked body, fewer (a partial pass) in the tile kernel */
static void launch_b16(ntt_ctx *c, uint64_t *x, int logn, int s_lo, int stg, const struct pass_tw *tw,
                       int inv, double scale, unsigned blocks, hipStream_t s)
{
    if (stg == 7) {
        const double *tb = inv ? c->tabT_i[7] : c->tabT_f[7];
        if (inv) k_b16r<1><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, tb, scale);
        else k_b16r<0><<<blocks, THREADS, 0, s>>>(x, logn, s_lo, c->m, tw->tlo, tw->thi, tb, scale);
        return;
    }
    if (inv) switch (stg) { LAUNCH_B16(1, 1) LAUNCH_B16(2, 1) LAUNCH_B16(3, 1) LAUNCH_B16(4, 1) LAUNCH_B16(5, 1) LAUNCH_B16(6, 1) default: ec_fatal(EC_RC_FATAL, "ntt: the radix-16 pass of %d stages is not built", stg); }
    else     switch (stg) { LAUNCH_B16(1, 0) LAUNCH_B16(2, 0) LAUNCH_B16(3, 0) LAUNCH_B16(4, 0) LAUNCH_B16(5, 0) LAUNCH_B16(6, 0) default: ec_fatal(EC_RC_FATAL, "ntt: the radix-16 pass of %d stages is not built", stg); }
}
/* the b1 pass on 2^lb-point blocks (nb of them; lb = 10, 11, 12 as the plan says): mode 0 forward, 1 inverse, 2 inverse with
 * the pointwise product (y in layout ymode, Lt points per transform; goff = x's offset in the batch, always 0); the register-blocked kernel, 2^3 points per thread */
template <int LGL>
static void launch_b1r(ntt_ctx *c, uint64_t *x, const uint64_t *y, size_t Lt, int ymode, int mode, double scale, size_t goff, unsigned nb, hipStream_t s)
{
    const unsigned th = 1u << (LGL - 3);
    const double *tw = mode ? c->tw1r_i : c->tw1r_f;
    switch (mode) {
    case 0: k_b1r<LGL, 3, 0><<<nb, th, 0, s>>>(x, 0, 0, 0, c->m, tw, 0.0, 0); break;
    case 1: k_b1r<LGL, 3, 1><<<nb, th, 0, s>>>(x, 0, 0, 0, c->m, tw, scale, 0); break;
    default: k_b1r<LGL, 3, 2><<<nb, th, 0, s>>>(x, y, Lt, ymode, c->m, tw, scale, goff); break;
    }
}
static void launch_b1(ntt_ctx *c, uint64_t *x, const uint64_t *y, size_t Lt, int ymode, int mode, double scale, size_t goff, int lb, unsigned nb, hipStream_t s)
{
    switch (lb) {
    case 10: launch_b1r<10>(c, x, y, Lt, ymode, mode, scale, goff, nb, s); break;
    case 11: launch_b1r<11>(c, x, y, Lt, ymode, mode, scale, goff, nb, s); break;
    case 12: launch_b1r<12>(c, x, y, Lt, ymode, mode, scale, goff, nb, s); break;
    default: ec_fatal(EC_RC_FATAL, "ntt: b1 length 2^%d not built\n", lb);
    }
}

void ntt_fwd(ntt_ctx *c, uint64_t *x, int logn, size_t batch, hipStream_t s)
{

    struct plan pl;
    check_logn(logn);
    make_plan(&pl, logn);
    struct plan_tw *pt = get_plan_tw(c, logn, &pl);
    unsigned blocks = logn >= 11 ? (unsigned)(batch << (logn - 11)) : 0;
    {
        for (int i = 0; i < pl.npass; i++)
            launch_b16(c, x, logn, pl.s_lo[i], pl.s_hi[i] - pl.s_lo[i] + 1, &pt->f[i], 0, 0.0, blocks, s);
        launch_b1(c, x, 0, 0, 0, 0, 0.0, 0, pl.lb, (unsigned)(batch << (logn - pl.lb)), s);
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
    {
        launch_b1(c, x, y, Lt, ymode, y ? 2 : 1, pl.npass ? 0.0 : sc, 0, pl.lb, (unsigned)(batch << (logn - pl.lb)), s);
        for (int i = pl.npass - 1; i >= 0; i--)
            launch_b16(c, x, logn, pl.s_lo[i], pl.s_hi[i] - pl.s_lo[i] + 1, &pt->i[i], 1, i == 0 ? sc : 0.0, blocks, s);
    }
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

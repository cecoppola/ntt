/* p24_crt.c - (results/PRIMES_STRATEGY.md §P2415): the CRT of a P24 plane (four primes, 24 digits per point) into
 * base-10^18 limbs.  One block per column j of the rank's plane: its points [x0, x0 + rows), x0 = R j + a, in chunks of 256 --
 * thread k does Garner for point x0 + k0 + k (dgarner4: a copy of rns_mul.c's, static there), its four base-10^18 digits and their
 * shift by s(x) = 6 (x mod 3) digits into E[x] = e[0..3] at limbs L(x) .. L(x) + 3 (p24.h); then the chunk's limbs
 * [L(x0 + k0), L(x0 + k0 + n)) sum the E of the (at most three) points that meet them -- the last three points of the previous chunk
 * kept as the tails -- the carry chain is a carry-lookahead over
 * wave ballot masks (as k_crt_batch; K4c, results/KERNELS.md §K4c; the serial chain was removed 2026-10-23, Q87), and the limbs go to the column's out-run.  The
 * contributions past the column's last own limb L(x0 + rows) - 1 form its 4-limb spill at L(x0 + rows) (the next out-run's first
 * limbs), added by the caller like k_crt_batch's.  Bound (P2415.md 1.1): min(Pa, Pb) <= 10^12 points gives c < 10^60, so c 10^s < 10^72
 * (four limbs, e4 = 0) and the spill's top limb holds the final carry; the device flag says otherwise (1: e4 != 0, 2: a spill carry). */
#include <hip/hip_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include "rns_int.h"
#include "p24.h"
#include "fatal.h"

#define P24_T 256
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)

/* ---- the device Garner (dmod128 / dmod192 are shared with rns_mul.c through rns_int.h) -------------------------------------- */
__device__ static inline void p24_dgarner4(const struct gconst *g, const uint64_t r[4], uint64_t out[4])
{
    uint64_t x0 = r[0], x1, x2;
    { uint64_t xm = ec_canon64(x0, g->m[1].pu, g->m[1].mu), df = ec_fold(r[1] + g->m[1].pu - xm, g->m[1].pu);
      uint64_t t1 = (uint64_t)ec_mm((double)df, (double)g->c1, g->m[1].p, g->m[1].pinv);
      uint64_t lo = t1 * g->m[0].pu, hi = __umul64hi(t1, g->m[0].pu);
      uint64_t s = lo + x0; hi += s < lo; x0 = s; x1 = hi; }
    { uint64_t xm = dmod128(x1, x0, g->m[2], g->c64[2]), df = ec_fold(r[2] + g->m[2].pu - xm, g->m[2].pu);
      uint64_t t2 = (uint64_t)ec_mm((double)df, (double)g->c2, g->m[2].p, g->m[2].pinv);
      uint64_t l0 = t2 * g->M1[0], h0 = __umul64hi(t2, g->M1[0]);
      uint64_t l1 = t2 * g->M1[1], h1 = __umul64hi(t2, g->M1[1]);
      uint64_t s0 = l0 + x0, c0 = s0 < l0;
      uint64_t s1 = h0 + l1, c1 = s1 < h0; s1 += x1; c1 += s1 < x1; s1 += c0; c1 += s1 < c0;
      x0 = s0; x1 = s1; x2 = h1 + c1; }
    { uint64_t xm = dmod192(x2, x1, x0, g->m[3], g->c64[3]), df = ec_fold(r[3] + g->m[3].pu - xm, g->m[3].pu);
      uint64_t t3 = (uint64_t)ec_mm((double)df, (double)g->c3, g->m[3].p, g->m[3].pinv);
      uint64_t l0 = t3 * g->M2[0], h0 = __umul64hi(t3, g->M2[0]);
      uint64_t l1 = t3 * g->M2[1], h1 = __umul64hi(t3, g->M2[1]);
      uint64_t l2 = t3 * g->M2[2], h2 = __umul64hi(t3, g->M2[2]);
      uint64_t s0 = l0 + x0, c0 = s0 < l0;
      uint64_t s1 = h0 + l1, c1 = s1 < h0; s1 += x1; c1 += s1 < x1; s1 += c0; c1 += s1 < c0;
      uint64_t s2 = h1 + l2, c2 = s2 < h1; s2 += x2; c2 += s2 < x2; s2 += c1; c2 += s2 < c1;
      out[0] = s0; out[1] = s1; out[2] = s2; out[3] = h2 + c2; }
}

__global__ __launch_bounds__(P24_T)
void k_crt24(const uint64_t *p0, const uint64_t *p1, const uint64_t *p2, const uint64_t *p3, struct gconst g, struct p24_run om,
             uint64_t *out, uint64_t *spill, unsigned *err)
{
    const uint64_t BB = EC_1E18;
    __shared__ uint64_t E[3 + P24_T][4];                  /* E[3 + k]: this chunk's point k; E[0..2]: the previous chunk's last three */
    __shared__ uint64_t V[2 * P24_T], CY[2 * P24_T];      /* a chunk's limbs: at most L(x + 256) - L(x) <= 342 */
    __shared__ uint64_t carry;
    __shared__ uint64_t GM[2 * P24_T / 64], PM[2 * P24_T / 64];   /* K4c: the generate / propagate masks of limbs [64 t, 64 t + 64) */
    if (warpSize != 64) __builtin_trap();                 /* the carry-lookahead needs wave64 masks (the serial chain was removed 2026-10-23, Q87) */
    uint64_t cyr = 0;                                     /* K4c: the carry into the chunk, uniform over the block */
    const size_t j = blockIdx.x, rows = om.r, x0 = om.R * j + om.a, base = j * rows, A0 = p24_L(x0), ob = p24_run_S(&om, j);
    const int tid = threadIdx.x;
    if (tid < 3) { E[tid][0] = E[tid][1] = E[tid][2] = E[tid][3] = 0; }
    if (tid == 0) carry = 0;
    __syncthreads();
    for (size_t k0 = 0; k0 < rows; k0 += P24_T) {
        const size_t n = rows - k0 < P24_T ? rows - k0 : P24_T, xc = x0 + k0;
        uint64_t e[4] = { 0, 0, 0, 0 };
        if ((size_t)tid < n) {
            size_t k = base + k0 + tid; uint64_t r[4] = { p0[k], p1[k], p2[k], p3[k] }, c[4], d[4];
            p24_dgarner4(&g, r, c); ec_words_to_dec4(c, d);
            if (p24_shift_digits(d, p24_s(xc + tid), e)) atomicOr(err, 1u);
        }
        E[3 + tid][0] = e[0]; E[3 + tid][1] = e[1]; E[3 + tid][2] = e[2]; E[3 + tid][3] = e[3];
        __syncthreads();
        const size_t Lc0 = p24_L(xc), nl = p24_L(xc + n) - Lc0;
        const long long xl = (long long)xc - 3, xh = (long long)(xc + n) - 1;       /* the points E holds: E[x - xl] */
        for (size_t li = tid; li < nl; li += P24_T) {
            size_t l = Lc0 + li; uint64_t sm = 0;
            long long xa = (long long)p24_xceil(l >= 3 ? l - 3 : 0), xb = (long long)p24_xfl(l);   /* L(x) in [l - 3, l] */
            if (xa < xl) xa = xl; if (xa < 0) xa = 0; if (xb > xh) xb = xh;
            for (long long x = xa; x <= xb; x++) sm += E[x - xl][l - p24_L((size_t)x)];
            uint64_t q = sm / BB; V[li] = sm - q * BB; CY[li] = q;
        }
        __syncthreads();
        {   /* K4c: the chain as a carry-lookahead over ballot masks (as K4b's k_crt_batch, results/KERNELS.md §K4c).
                       * Serial form: sm_i = V_i + CY_{i-1} + o_{i-1} (CY_{-1} = the chunk's carry-in, o_{-1} = 0), o_i = sm_i >= B.  A limb
                       * sums at most three points' digits (< 3B), so CY <= 2, the carry-in <= 3 and w_i = V_i + CY_{i-1} <= B + 2:
                       * o_i = gen_i | (prop_i & o_{i-1}), gen = w >= B, prop = w == B - 1.  nl <= 342: thread t owns limbs t and t + 256 */
            const int in0 = (size_t)tid < nl, in1 = (size_t)tid + P24_T < nl;
            const uint64_t w0 = in0 ? V[tid] + (tid ? CY[tid - 1] : cyr) : 0, w1 = in1 ? V[tid + P24_T] + CY[tid + P24_T - 1] : 0;
            const uint64_t g0 = __ballot(in0 && w0 >= BB), q0 = __ballot(in0 && w0 == BB - 1);
            const uint64_t g1 = __ballot(in1 && w1 >= BB), q1 = __ballot(in1 && w1 == BB - 1);
            if ((tid & 63) == 0) { GM[tid >> 6] = g0; PM[tid >> 6] = q0; GM[P24_T / 64 + (tid >> 6)] = g1; PM[P24_T / 64 + (tid >> 6)] = q1; }
            __syncthreads();
            /* o_{i-1} = bit i of (A + G) ^ A ^ G over the chunk's limbs (A = G | P): the carry into bit i of the sum A + G */
            const int nw = (int)(nl >> 6) + 1;                                       /* the words up to bit nl (nl < 512) */
            uint64_t ci = 0, m0 = 0, m1 = 0, top = 0;
            for (int t = 0; t < nw; t++) {
                const uint64_t G = GM[t], A = G | PM[t], s1 = A + G, s2 = s1 + ci;
                const uint64_t co = (s1 < A) | (s2 < s1), cv = s2 ^ A ^ G;
                if (t == (tid >> 6)) m0 = (cv >> (tid & 63)) & 1;
                if (t == P24_T / 64 + (tid >> 6)) m1 = (cv >> (tid & 63)) & 1;
                if (t == (int)(nl >> 6)) top = (cv >> (nl & 63)) & 1;                 /* o_{nl-1} */
                ci = co;
            }
            uint64_t r0 = w0 + m0, r1 = w1 + m1; if (r0 >= BB) r0 -= BB; if (r1 >= BB) r1 -= BB;
            if (in0) out[ob + (Lc0 - A0) + tid] = r0;
            if (in1) out[ob + (Lc0 - A0) + tid + P24_T] = r1;
            cyr = CY[nl - 1] + top;                      /* V, CY, GM, PM are rewritten only after the next chunk's first barrier */
        }
        uint64_t t[4] = { 0, 0, 0, 0 };
        if (tid < 3) { t[0] = E[n + tid][0]; t[1] = E[n + tid][1]; t[2] = E[n + tid][2]; t[3] = E[n + tid][3]; }
        __syncthreads();
        if (tid < 3) { E[tid][0] = t[0]; E[tid][1] = t[1]; E[tid][2] = t[2]; E[tid][3] = t[3]; }
        __syncthreads();
    }
    if (tid == 0) carry = cyr;
    if (tid == 0) {                                       /* the spill: limbs L(x1) + t from the last three points (E[0..2] = x1 - 3 ..) and the carry */
        const size_t x1 = x0 + rows, L1 = p24_L(x1); uint64_t cy = carry;
        const long long xl = (long long)x1 - 3, xh = (long long)x1 - 1;
        for (int u = 0; u < 4; u++) {
            size_t l = L1 + (size_t)u; uint64_t sm = cy;
            long long xa = (long long)p24_xceil(l >= 3 ? l - 3 : 0), xb = (long long)p24_xfl(l);
            if (xa < xl) xa = xl; if (xa < 0) xa = 0; if (xb > xh) xb = xh;
            for (long long x = xa; x <= xb; x++) sm += E[x - xl][l - p24_L((size_t)x)];
            cy = sm / BB; spill[4 * j + u] = sm - cy * BB;
        }
        if (cy) atomicOr(err, 2u);
    }
}

__device__ unsigned p24_err_flag;                          /* per device (no allocation: a module variable) */
static unsigned *err_dev(void) { void *p = 0; HIP_CHECK(hipGetSymbolAddress(&p, HIP_SYMBOL(p24_err_flag))); return (unsigned *)p; }
void p24_crt_launch(const uint64_t *p0, const uint64_t *p1, const uint64_t *p2, const uint64_t *p3, const struct gconst *gc,
                    size_t R, size_t C, size_t a, size_t rows, uint64_t *out, uint64_t *spill, hipStream_t s)
{
    if (gc->np != 4) { ec_fatal(EC_RC_FATAL, "p24_crt: %d primes (P24 needs four)\n", gc->np); }
    struct p24_run om = p24_run_make(R, C, a, rows, 0);
    k_crt24<<<(unsigned)C, P24_T, 0, s>>>(p0, p1, p2, p3, *gc, om, out, spill, err_dev());
}
unsigned p24_crt_err(hipStream_t s)
{
    unsigned *e = err_dev(), v = 0;
    HIP_CHECK(hipMemcpyAsync(&v, e, sizeof v, hipMemcpyDeviceToHost, s)); HIP_CHECK(hipStreamSynchronize(s));
    if (v) HIP_CHECK(hipMemsetAsync(e, 0, sizeof v, s));
    HIP_CHECK(hipStreamSynchronize(s));
    return v;
}

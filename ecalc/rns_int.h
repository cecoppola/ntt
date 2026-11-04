/* rns_int.h - internals shared by the product tiers (rns_mul.c, rns_dist.c) */
#ifndef EC_RNS_INT_H
#define EC_RNS_INT_H
#include <stdint.h>
#include <stddef.h>
#include <hip/hip_runtime.h>
#include "modarith.h"
struct bdesc { const uint64_t *a, *b, *x; uint64_t *c; uint32_t na, nb, nx; };
struct gconst { ec_mod m[4]; uint64_t c64[4]; uint64_t c1, c2, c3, M1[2], M2[3]; int np; };   /* np: the prime count (P3: 3 -> planes p0..p2, p3 unread) */
#define CRT_THREADS 256
/* device Garner: FP64-Barrett for the modular steps (one definition for rns_mul.c and p24_crt.c; SH3 follow-up) */
__device__ static inline uint64_t dmod128(uint64_t hi, uint64_t lo, const ec_mod m, uint64_t c64)
{
    uint64_t r = (uint64_t)ec_mm((double)ec_canon64(hi, m.pu, m.mu), (double)c64, m.p, m.pinv) + ec_canon64(lo, m.pu, m.mu);
    return ec_fold(r, m.pu);
}
__device__ static inline uint64_t dmod192(uint64_t x2, uint64_t x1, uint64_t x0, const ec_mod m, uint64_t c64)
{
    uint64_t r = dmod128(x2, x1, m, c64);
    r = (uint64_t)ec_mm((double)r, (double)c64, m.p, m.pinv) + ec_canon64(x0, m.pu, m.mu);
    return ec_fold(r, m.pu);
}
__device__ static inline void dgarner4(const struct gconst *g, const uint64_t r[4], uint64_t out[4])
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
struct gconst rns_gconst(void);
/* the striped CRT (rns_mul.c): stripe g = pi S + s of product P[pi] over coefficients [nc s/S, nc (s+1)/S);
 * planes at pi * Lpts; the 4-limb spill of every stripe goes to stripe_spill + 4 g */
__global__ __launch_bounds__(CRT_THREADS)
void k_crt_batch(const uint64_t *p0, const uint64_t *p1, const uint64_t *p2, const uint64_t *p3,
                 const struct bdesc *P, size_t first_stripe, int S, size_t Lpts, struct gconst g, uint64_t *stripe_spill);
#endif

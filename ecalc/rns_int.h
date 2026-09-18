/* rns_int.h - internals shared by the product tiers (rns_mul.c, rns_dist.c) */
#ifndef EC_RNS_INT_H
#define EC_RNS_INT_H
#include <stdint.h>
#include <stddef.h>
#include <hip/hip_runtime.h>
#include "modarith.h"
struct bdesc { const uint64_t *a, *b, *x; uint64_t *c; uint32_t na, nb, nx; };
struct gconst { ec_mod m[4]; uint64_t c64[4]; uint64_t c1, c2, c3, M1[2], M2[3]; };
#define CRT_THREADS 256
struct gconst rns_gconst(void);
/* the striped CRT (rns_mul.c): stripe g = pi S + s of product P[pi] over coefficients [nc s/S, nc (s+1)/S);
 * planes at pi * Lpts; the 4-limb spill of every stripe goes to stripe_spill + 4 g */
__global__ __launch_bounds__(CRT_THREADS)
void k_crt_batch(const uint64_t *p0, const uint64_t *p1, const uint64_t *p2, const uint64_t *p3,
                 const struct bdesc *P, size_t first_stripe, int S, size_t Lpts, struct gconst g, uint64_t *stripe_spill, int dec);
#endif

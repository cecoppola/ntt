/* crt2.c - engine 2 CRT: c = r0 + P0 ((r1 - r0) P0^-1 mod P1) (< 2^124), then
 * the 45-bit carry into 64-bit limbs.  Stripe t takes coefficients [k0, k1):
 * a 4-limb window at limb base floor(45 k / 64) slides along; finished limbs
 * go to out, the window at the end is the stripe's spill (added by the
 * sequential merge, which also ripples carries). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "crt.h"
#include "modarith2.h"

typedef unsigned __int128 u128;

void crt2_carry(uint64_t *const res[2], size_t n, uint64_t *out, size_t limbs_out, int T)
{
    const uint64_t P0 = e2_P[0], P1 = e2_P[1];
    const uint64_t inv01 = e2_inv(P0 % P1, P1);
    if (T < 1) T = 1;
    if ((size_t)T > n / 64 + 1) T = (int)(n / 64 + 1);
    uint64_t (*spill)[5] = (uint64_t (*)[5])calloc(T, sizeof *spill);
    size_t *sbase = (size_t *)calloc(T, sizeof *sbase);
    memset(out, 0, limbs_out * 8);
    int t;
#pragma omp parallel for num_threads(T) schedule(static)
    for (t = 0; t < T; t++) {
        size_t k0 = n * t / T, k1 = n * (t + 1) / T;
        size_t base = (k0 * E2_BITS) >> 6;                  /* window covers limbs base .. base+3 */
        uint64_t w[4] = {0, 0, 0, 0};
        for (size_t k = k0; k < k1; k++) {
            size_t bit = k * E2_BITS, lb = bit >> 6; unsigned o = (unsigned)(bit & 63);
            /* slide: emit finished limbs below lb; limbs written directly (first stripe) or into out (others: they only add their own contributions - out was zeroed, so plain stores are fine within [base_of_stripe .. ), except the first limbs which the previous stripe's spill will add to) */
            while (base < lb) {
                if (base >= (k0 * E2_BITS) >> 6 && base < limbs_out) out[base] = w[0];
                w[0] = w[1]; w[1] = w[2]; w[2] = w[3]; w[3] = 0; base++;
            }
            uint64_t r0 = res[0][k], r1 = res[1][k];
            uint64_t d = e2_mulmod_ref((r1 + P1 - r0 % P1) % P1, inv01, P1);
            u128 c = (u128)d * P0 + r0;                      /* < 2^124 */
            /* add c << o into the window (o <= 63: c << o < 2^187, 3 limbs) */
            uint64_t c0 = (uint64_t)c, c1 = (uint64_t)(c >> 64);
            uint64_t a0 = c0 << o, a1 = o ? (c1 << o) | (c0 >> (64 - o)) : c1, a2 = o ? c1 >> (64 - o) : 0;
            u128 s = (u128)w[0] + a0; w[0] = (uint64_t)s;
            s = (s >> 64) + w[1] + a1; w[1] = (uint64_t)s;
            s = (s >> 64) + w[2] + a2; w[2] = (uint64_t)s;
            w[3] += (uint64_t)(s >> 64);
        }
        spill[t][0] = w[0]; spill[t][1] = w[1]; spill[t][2] = w[2]; spill[t][3] = w[3]; sbase[t] = base;
    }
    /* merge: stripe t's window (4 limbs at sbase[t]) adds into out with ripple */
    for (t = 0; t < T; t++) {
        size_t b = sbase[t]; u128 s = 0; size_t i;
        for (i = 0; i < 4 && b + i < limbs_out; i++) { s += (u128)out[b + i] + spill[t][i]; out[b + i] = (uint64_t)s; s >>= 64; }
        for (i = 4; s && b + i < limbs_out; i++) { s += out[b + i]; out[b + i] = (uint64_t)s; s >>= 64; }
    }
    free(spill); free(sbase);
}

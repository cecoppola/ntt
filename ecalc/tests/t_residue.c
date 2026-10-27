/* t_residue - B3a: the Shoup Horner step (residue_shoup.h) against the 128-bit % form, bit for bit.  Host only, no node time.
 * Random limbs (< 10^18), edge values (0, 1, q-1, max limb, 10^18 mod q), b = 10^18 mod q and b = 10, the T1 primes (verify.c) and
 * random primes-sized moduli in (2^40, 2^63); also a chain of 10^6 steps per prime (value and the final residue). */
#include <stdio.h>
#include "../residue_shoup.h"
#include "harness.h"

typedef unsigned __int128 u128;
#define NQ 8
static const uint64_t t1_q[NQ] = {4611686018427388039ULL, 4611686018427388073ULL, 4611686018427388081ULL, 4611686018427388091ULL,   /* = verify.c t1_q */
                                  4611686018427388093ULL, 4611686018427388097ULL, 4611686018427388157ULL, 4611686018427388181ULL};
static void check_pair(uint64_t q, uint64_t b, uint64_t v, uint64_t x)
{
    uint64_t w = rs_w(b, q), ref = (uint64_t)(((u128)v * b + x) % q), got = rs_step(v, b, w, q, x);
    VERIFY(got == ref, "q=%llu b=%llu v=%llu x=%llu got=%llu ref=%llu", (unsigned long long)q, (unsigned long long)b, (unsigned long long)v, (unsigned long long)x, (unsigned long long)got, (unsigned long long)ref);
}
static void check_modulus(uint64_t q, rng_t *r)
{
    const uint64_t B = 1000000000000000000ULL, XM = B - 1;
    uint64_t bs[3] = {B % q, 10 % q, q - 1};
    uint64_t vs[6] = {0, 1, q - 1, q / 2, B % q, XM % q}, xs[5] = {0, 1, XM < q ? XM : q - 1, 9 % q, q - 1};
    for (int bi = 0; bi < 3; bi++) {
        uint64_t b = bs[bi];
        for (int i = 0; i < 6; i++) for (int j = 0; j < 5; j++) check_pair(q, b, vs[i], xs[j]);
        for (int it = 0; it < 20000; it++) {
            uint64_t v = rng_next(r) % q, x = rng_next(r) % (XM < q ? B : q);
            check_pair(q, b, v, x);
        }
        uint64_t w = rs_w(b, q), a = 0, c = 0;      /* a Horner chain, both forms */
        for (int it = 0; it < 1000000; it++) { uint64_t x = rng_next(r) % (XM < q ? B : q); a = rs_step(a, b, w, q, x); c = (uint64_t)(((u128)c * b + x) % q); }
        VERIFY(a == c, "chain q=%llu b=%llu", (unsigned long long)q, (unsigned long long)b);
    }
}
int main(void)
{
    rng_t r = {12345};
    for (int j = 0; j < NQ; j++) check_modulus(t1_q[j], &r);
    for (int it = 0; it < 200; it++) {
        uint64_t q = (rng_next(&r) >> 1 | ((uint64_t)1 << 40)) | 1;   /* in [2^40, 2^63), odd: the formula needs only b < q < 2^63 */
        check_modulus(q, &r);
    }
    return verify_done("t_residue");
}

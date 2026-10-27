/* residue_shoup.h - the digit residues' Horner step v = (v b + x) mod q by a Shoup precomputed-quotient multiply (B3a, ECALC_RESIDUE=shoup).
 * For a constant b < q < 2^63: w = floor(b 2^64 / q); for any v < 2^64 the value r = v b - hi64(v w) q (mod 2^64) is in [0, 2q), so one
 * conditional subtract gives v b mod q; x < q is added with one more.  Bit-identical to ((u128) v b + x) % q for v < q, x < q: no 128-bit
 * division per step (the % of the default form runs ~13 ns on the target CPUs).  The default (ECALC_RESIDUE unset / anything else) keeps the % form. */
#ifndef EC_RESIDUE_SHOUP_H
#define EC_RESIDUE_SHOUP_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static inline uint64_t rs_w(uint64_t b, uint64_t q) { return (uint64_t)(((unsigned __int128)b << 64) / q); }
static inline uint64_t rs_step(uint64_t v, uint64_t b, uint64_t w, uint64_t q, uint64_t x)
{
    uint64_t t = (uint64_t)(((unsigned __int128)v * w) >> 64);
    uint64_t r = v * b - t * q; if (r >= q) r -= q;
    r += x; if (r >= q) r -= q;
    return r;
}
static inline int ec_residue_shoup(void) { static int v = -1; if (v < 0) { const char *e = getenv("ECALC_RESIDUE"); v = (e && !strcmp(e, "shoup")) ? 1 : 0; } return v; }
#endif

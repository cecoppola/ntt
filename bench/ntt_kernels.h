/* Shared NTT primitives: the two 62-bit primes, the Shoup butterfly, the
 * register-blocked stage macros, the host-side table builders and the hash61
 * verifier.  Extracted from bench/13 and bench/14 (identical copies existed in
 * 05-08 and 12, which keep their own until they are next touched).
 *
 * Kernels using FB/GB/EXW/EXR must define, in scope:
 *   uint64_t x[RPT], p, p2 = 2*p;  const uint64_t *w, *wp;  __shared__ uint64_t s[];
 *   int k;   and RPT before including or using EXW/EXR. */
#ifndef NTT_KERNELS_H
#define NTT_KERNELS_H

#include "common_ntt.h"

/* 62-bit primes with 2^40 * 3 * 5 * 7 | p-1, primitive root 11 */
#define P1 0x3FFEDF0000000001ULL         /* 39943 * 105 * 2^40 + 1 */
#define P2 0x3FF6420000000001ULL         /* 39922 * 105 * 2^40 + 1 */
#define G  11ULL
#define PRIME P1
#define PROOT G

/* XOR swizzle restoring 16 bank classes on every exchange phase (bench/06) */
#define SWZ(e) ((e) ^ (((e) >> 4) & 15))

/* Harvey Alg. 4: w*y mod p with precomputed wp = floor(w * 2^64 / p); [0,2p) */
__device__ static inline uint64_t smul(uint64_t w, uint64_t wp, uint64_t y, uint64_t p)
{
    uint64_t q = __umul64hi(wp, y);
    return w * y - q * p;
}

/* forward (DIT) butterfly on x[a], x[b] with twiddle index wi, lazy [0,4p) */
#define FB(a, b, wi)                                                          \
    do { uint64_t U = x[a], V; int wi_ = (wi);                                \
         if (U >= p2) U -= p2;                                                \
         V = smul(w[wi_], wp[wi_], x[b], p);                                  \
         x[a] = U + V; x[b] = U - V + p2; } while (0)
/* three register-resident forward stages over 8 points */
#define FWD3(b0, b1, b2)                                                      \
    do { FB(0,4,(b0)); FB(1,5,(b0)); FB(2,6,(b0)); FB(3,7,(b0));              \
         FB(0,2,(b1)+0); FB(1,3,(b1)+0); FB(4,6,(b1)+1); FB(5,7,(b1)+1);      \
         FB(0,1,(b2)+0); FB(2,3,(b2)+1); FB(4,5,(b2)+2); FB(6,7,(b2)+3); } while (0)
/* inverse (Gentleman-Sande) butterfly */
#define GB(a, b, wi)                                                          \
    do { uint64_t U = x[a], V = x[b], X_ = U + V; int wi_ = (wi);             \
         if (X_ >= p2) X_ -= p2;                                              \
         x[b] = smul(w[wi_], wp[wi_], U - V + p2, p);                         \
         x[a] = X_; } while (0)
#define GINV3(b0, b1, b2)                                                     \
    do { GB(0,1,(b0)+0); GB(2,3,(b0)+1); GB(4,5,(b0)+2); GB(6,7,(b0)+3);      \
         GB(0,2,(b1)+0); GB(1,3,(b1)+0); GB(4,6,(b1)+1); GB(5,7,(b1)+1);      \
         GB(0,4,(b2)); GB(1,5,(b2)); GB(2,6,(b2)); GB(3,7,(b2)); } while (0)
/* LDS exchange: write all 8 registers at idx(k), barrier; read back, barrier */
#define EXW(idx) do { _Pragma("unroll")                                       \
        for (k = 0; k < RPT; k++) s[SWZ(idx)] = x[k]; __syncthreads(); } while (0)
#define EXR(idx) do { _Pragma("unroll")                                       \
        for (k = 0; k < RPT; k++) x[k] = s[SWZ(idx)]; __syncthreads(); } while (0)

/* ------------------------------ host ------------------------------------ */
static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t p)
{ return (uint64_t)((__uint128_t)a * b % p); }
static uint64_t powmod(uint64_t a, uint64_t e, uint64_t p)
{ uint64_t r = 1; a %= p;
  while (e) { if (e & 1) r = mulmod(r, a, p); a = mulmod(a, a, p); e >>= 1; } return r; }
static uint64_t shoup_pre(uint64_t w, uint64_t p)
{ return (uint64_t)(((__uint128_t)w << 64) / p); }
/* -p^-1 mod 2^64 for Montgomery REDC */
static uint64_t mont_j(uint64_t p)
{ uint64_t x = 1; int i; for (i = 0; i < 6; i++) x *= 2 - p * x; return x; }
static int brv(int i, int bits)
{ int r = 0, k; for (k = 0; k < bits; k++) if (i & (1 << k)) r |= 1 << (bits - 1 - k); return r; }
/* bit-reversed twiddle table: w[m+i] = root^(n/(2m) * brv(i)), m = 1,2,4,..,n/2 */
static void build_table(uint64_t *w, uint64_t *wp, int n, uint64_t root, uint64_t p)
{
    int m, i;
    for (m = 1; m < n; m <<= 1) {
        int lgm = 0;
        while ((1 << lgm) < m) lgm++;
        for (i = 0; i < m; i++) {
            uint64_t e = (uint64_t)(n / (2 * m)) * (uint64_t)brv(i, lgm);
            w[m + i] = powmod(root, e, p); wp[m + i] = shoup_pre(w[m + i], p);
        }
    }
    w[0] = 1; wp[0] = shoup_pre(1, p);
}

/* Homomorphic hash over 2^61-1: hash61(a)*hash61(b) = hash61(a*b) mod M61,
 * for little-endian base-2^64 limb arrays.  The full-size multiply check. */
#define M61 0x1FFFFFFFFFFFFFFFULL
static uint64_t hash61(const uint64_t *a, size_t n)
{
    __uint128_t h = 0; size_t i; uint64_t sh = 1;
    for (i = 0; i < n; i++) {
        h += (__uint128_t)(a[i] % M61) * sh;
        h = (h >> 61) + (h & M61);
        sh = (uint64_t)(((__uint128_t)sh * (((__uint128_t)1 << 64) % M61)) % M61);
    }
    h = (h >> 61) + (h & M61);
    return (uint64_t)(h % M61);
}

#endif /* NTT_KERNELS_H */

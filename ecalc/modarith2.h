/* modarith2.h - engine 2: two 62-bit primes (~/ntt's P1, P2: 2^40 | p-1, root 11),
 * 45-bit points (2 * 45 + 33 = 123 < log2 P1 P2 = 123.6), Montgomery
 * arithmetic (R = 2^64) so twiddles can be formed on the fly as in the
 * paper's kernels.  Values are kept lazy in [0, 2p) (2p < 2^63): REDC(a b)
 * for a, b < 2p is < 2p, so no fold is ever needed before a multiply.
 */
#ifndef EC_MODARITH2_H
#define EC_MODARITH2_H
#include <stdint.h>
#if defined(__HIPCC__) || defined(__HIP__)
#include <hip/hip_runtime.h>
#define E2_HD __host__ __device__
#else
#define E2_HD
#endif

#define E2_NP 2
#define E2_BITS 45                          /* bits per point */
#define E2_LOGN_MAX 33
static const uint64_t e2_P[E2_NP] = {0x3FFEDF0000000001ULL, 0x3FF6420000000001ULL};
static const uint64_t e2_G[E2_NP] = {11, 11};

typedef struct { uint64_t p, p2, pinv_neg, r2, one; int idx; } e2_mod;   /* pinv_neg = -p^-1 mod 2^64, r2 = R^2 mod p, one = R mod p */

E2_HD static inline uint64_t e2_umulhi(uint64_t a, uint64_t b)
{
#if defined(__HIP_DEVICE_COMPILE__)
    return __umul64hi(a, b);
#else
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
#endif
}
/* Montgomery product: a, b < 2p -> [0, 2p) */
E2_HD static inline uint64_t e2_mm(uint64_t a, uint64_t b, const e2_mod m)
{
    uint64_t lo = a * b, hi = e2_umulhi(a, b);
    uint64_t q = lo * m.pinv_neg;
    uint64_t t = e2_umulhi(q, m.p);
    /* (a b + q p) / 2^64 = hi + t + carry(lo + q p_lo) ; lo + (q p mod 2^64) == 0 mod 2^64 by construction, carry = (lo != 0) */
    return hi + t + (lo != 0);
}
E2_HD static inline uint64_t e2_fold(uint64_t x, uint64_t p) { return x >= p ? x - p : x; }

/* host helpers */
static inline uint64_t e2_mulmod_ref(uint64_t a, uint64_t b, uint64_t p) { return (uint64_t)(((unsigned __int128)a * b) % p); }
static inline uint64_t e2_powmod(uint64_t a, uint64_t e, uint64_t p)
{ uint64_t r = 1; a %= p; while (e) { if (e & 1) r = e2_mulmod_ref(r, a, p); a = e2_mulmod_ref(a, a, p); e >>= 1; } return r; }
static inline uint64_t e2_inv(uint64_t a, uint64_t p) { return e2_powmod(a, p - 2, p); }
static inline e2_mod e2_mod_get(int i)
{
    e2_mod m; m.idx = i; m.p = e2_P[i]; m.p2 = 2 * m.p;
    uint64_t inv = 1;                                   /* p^-1 mod 2^64 by Newton */
    for (int k = 0; k < 6; k++) inv *= 2 - m.p * inv;
    m.pinv_neg = (uint64_t)0 - inv;
    unsigned __int128 R = (unsigned __int128)1 << 64;
    m.one = (uint64_t)(R % m.p);
    m.r2 = (uint64_t)((R % m.p) * (R % m.p) % m.p);
    return m;
}
static inline uint64_t e2_to_mont(uint64_t x, const e2_mod m) { return e2_mm(x % m.p, m.r2, m); }   /* x R mod p (lazy) */
static inline uint64_t e2_from_mont(uint64_t x, const e2_mod m) { return e2_fold(e2_mm(x, 1, m), m.p); }
/* primitive 2^logn-th root (plain), logn <= 40 */
static inline uint64_t e2_root(int i, int logn) { return e2_powmod(e2_G[i], (e2_P[i] - 1) >> logn, e2_P[i]); }
static inline uint64_t e2_root_inv(int i, int logn) { return e2_inv(e2_root(i, logn), e2_P[i]); }
#endif

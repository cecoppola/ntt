/* modarith.h - the paper's arithmetic engine (PLAN.md 8, step 1).
 *
 * Four primes p < 2^52 with 2^33 | p-1 (verified by tests/t_params, results/
 * 0_params.txt), the eight-line FP64-Barrett modmul with a Dekker split and
 * two corrections each way, canon64 (Barrett with mu115 = floor(2^115/p) for
 * a 64-bit limb), and an integer Shoup modmul behind the same names as the
 * alternative engine for Phase 5.  Everything here is host + device.
 *
 * Binding rules (PLAN.md 8, "Rules learned in Phase 1"):
 *   1. ec_mm takes at most ONE lazy operand in [0,2p); the other must be
 *      canonical in [0,p).  Both lazy, or either in [0,4p), is wrong in
 *      0.4-21 % of cases (RESULTS.md 19, 30).  Fold with ec_fold first.
 *   2. Adds and subtracts of NTT values are 64-bit integer, never double:
 *      u + v with u, v < 2p ~ 2^52.8 does not round-trip through FP64.
 */
#ifndef EC_MODARITH_H
#define EC_MODARITH_H
#include <stdint.h>
#include <math.h>

#if defined(__HIPCC__) || defined(__HIP__)
#include <hip/hip_runtime.h>
#define EC_HD __host__ __device__
#else
#define EC_HD
#endif

#define EC_NP 4
#define EC_LOGN_MAX 33                      /* 2^33 | p-1 for all four primes */

static const uint64_t ec_P[EC_NP] = {3923057487904769ULL, 3641582511194113ULL,
                                     2867526325239809ULL, 2586051348529153ULL};
static const uint64_t ec_G[EC_NP] = {3, 5, 3, 10};
/* w33 = g^((p-1)/2^33): exact order 2^33 (results/0_params.txt) */
static const uint64_t ec_W33[EC_NP] = {678007507195576ULL, 3536920527846901ULL,
                                       1501474000275416ULL, 2276121144993249ULL};
/* mu115 = floor(2^115 / p), 64 bits each */
static const uint64_t ec_MU115[EC_NP] = {10588265656658394641ULL, 11406682325772086755ULL,
                                         14485786757268845297ULL, 16062471030168855059ULL};
/* LEAF kernel: floor(2^123 / 10^18) */
#define EC_MU_1E18 10633823966279326983ULL

/* per-prime constants, passed by value to kernels */
typedef struct { double p, pinv; uint64_t pu, mu; int idx; } ec_mod;

static inline ec_mod ec_mod_get(int i)
{
    ec_mod m;
    m.pu = ec_P[i]; m.mu = ec_MU115[i]; m.idx = i;
    m.p = (double)m.pu; m.pinv = 1.0 / m.p;
    return m;
}

/* ---- the paper's modmul: a * b mod p, exact for a in [0,2p), b in [0,p) --- */
EC_HD static inline double ec_mm(double a, double b, double p, double pinv)
{
    double hi = a * b;                  /* rounded product */
    double lo = fma(a, b, -hi);         /* Dekker: exact low part */
    double q  = floor(hi * pinv);
    double r  = fma(-q, p, hi) + lo;
    r += (r < 0.0 ? p : 0.0);
    r += (r < 0.0 ? p : 0.0);
    r -= (r >= p ? p : 0.0);
    r -= (r >= p ? p : 0.0);
    return r;
}
/* integer wrapper: x in [0,2p), w in [0,p) -> [0,p) */
EC_HD static inline uint64_t ec_mmu(uint64_t x, uint64_t w, const ec_mod m)
{
    return (uint64_t)ec_mm((double)x, (double)w, m.p, m.pinv);
}
/* lazy [0,2p) -> canonical [0,p) */
EC_HD static inline uint64_t ec_fold(uint64_t x, uint64_t p) { return x >= p ? x - p : x; }
/* lazy add / sub, both inputs in [0,p), outputs in [0,2p) (rule 2: integer) */
EC_HD static inline uint64_t ec_add(uint64_t a, uint64_t b) { return a + b; }
EC_HD static inline uint64_t ec_sub(uint64_t a, uint64_t b, uint64_t p) { return a + p - b; }

EC_HD static inline uint64_t ec_umulhi(uint64_t a, uint64_t b)
{
#if defined(__HIP_DEVICE_COMPILE__)
    return __umul64hi(a, b);
#else
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
#endif
}

/* ---- canon64: any 64-bit limb -> [0,p).  q = floor(x mu / 2^115) is at
 * most 1 below floor(x/p) (x < 2^64, mu < 2^115/p), so one subtract suffices;
 * the second is kept for safety and counted by t_modarith. ------------------ */
EC_HD static inline uint64_t ec_canon64(uint64_t x, uint64_t p, uint64_t mu)
{
    uint64_t q = ec_umulhi(x, mu) >> 51;
    uint64_t r = x - q * p;
    r -= (r >= p) ? p : 0;
    r -= (r >= p) ? p : 0;
    return r;
}

/* ---- Shoup integer modmul (alternative engine).  wp = floor(w 2^64 / p);
 * result in [0,2p) for w < p, any y < 2^64; fold if canonical is needed. ---- */
EC_HD static inline uint64_t ec_shoup_lazy(uint64_t w, uint64_t wp, uint64_t y, uint64_t p)
{
    uint64_t q = ec_umulhi(wp, y);
    return w * y - q * p;
}
EC_HD static inline uint64_t ec_shoup(uint64_t w, uint64_t wp, uint64_t y, uint64_t p)
{
    return ec_fold(ec_shoup_lazy(w, wp, y, p), p);
}

/* ---- host-side exact helpers --------------------------------------------- */
static inline uint64_t ec_mulmod_ref(uint64_t a, uint64_t b, uint64_t p)
{
    return (uint64_t)(((unsigned __int128)a * b) % p);
}
static inline uint64_t ec_shoup_pre(uint64_t w, uint64_t p)
{
    return (uint64_t)((((unsigned __int128)w) << 64) / p);
}
static inline uint64_t ec_powmod(uint64_t a, uint64_t e, uint64_t p)
{
    uint64_t r = 1;
    a %= p;
    while (e) { if (e & 1) r = ec_mulmod_ref(r, a, p); a = ec_mulmod_ref(a, a, p); e >>= 1; }
    return r;
}
static inline uint64_t ec_inv(uint64_t a, uint64_t p) { return ec_powmod(a, p - 2, p); }
/* primitive 2^logn-th root of unity for prime i, 0 <= logn <= 33 */
static inline uint64_t ec_root(int i, int logn)
{
    return ec_powmod(ec_W33[i], 1ULL << (EC_LOGN_MAX - logn), ec_P[i]);
}
static inline uint64_t ec_root_inv(int i, int logn) { return ec_inv(ec_root(i, logn), ec_P[i]); }

#endif

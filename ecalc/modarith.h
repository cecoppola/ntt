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
#include "fatal.h"                          /* ec_fatal: ec_root / ec_root3 beyond the prime's 2-adic order (Phase 15 P) */

#if defined(__HIPCC__) || defined(__HIP__)
#include <hip/hip_runtime.h>
#define EC_HD __host__ __device__
#else
#define EC_HD
#endif

#define EC_NP 4                             /* the size of the prime set: array bounds, and one prime per device in the mdev / striped tiers */
#define EC_LOGN_MAX 33                      /* the order of the stored root tables ec_W33 / ec_W3X33 (2^33 | p-1 for all four primes).
                                               Not the transform limit: ec_root derives any 2^logn up to v2(p-1) (Phase 15 P) */

/* Phase 13a P3 (PLAN 29 E1/E3): the number of primes a product uses, ec_np = 3 or 4 -- ECALC_NP (default 4: every
 * default unchanged).  A product uses the first ec_np primes of ec_P.  Three suffice for base-10^18 limbs: a convolution
 * coefficient is at most nterms (B-1)^2 and p0 p1 p2 = 2^155.36 against (B-1)^2 = 2^119.59, so nterms < 2^35.76
 * (tests/t_primes: the margin is 27x at 2^31 terms, 6.8x at 2^33, the largest 2^k length the primes allow).  {c = 240,
 * 216, 207} is the subset with the largest product (dropping 147, t_primes prints all four).  Four stay required for
 * binary 2^64 limbs (nterms 2^128 < p0 p1 p2 only below 2^27 terms): ec_np_check refuses that loudly. */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
extern int ec_np;                           /* 3 or 4 (crt.c); read by ec_np_init */
int ec_np_init(void);                       /* ECALC_NP once (called by crt_init and rns_init); returns ec_np */
extern size_t ec_np3_max_terms;             /* floor((p0 p1 p2 - 1) / (10^18 - 1)^2): the largest term count three primes reconstruct */
/* abort with a message when ec_np == 3 and the limbs are binary, or a product of nterms terms (min(na, nb), or any
 * upper bound of it) could exceed p0 p1 p2 */
void ec_np_check(size_t nterms, int decimal, const char *where);
/* Phase 15 NP (PLAN 37 row 1, P15 option b): ECALC_NP=auto -- the prime count per product of the distributed tiers.
 * ec_np stays 3 (every one-node tier, the host CRT and ec_np_check as with ECALC_NP=3); mn_core / dist_core (and the B form)
 * take ec_np_for(nc): four primes when nc = pa + pb > ec_np_auto_terms, else three.  ec_np_auto_terms = ec_np3_max_terms
 * unless ECALC_NP_AUTO_TERMS=<n> lowers it (a test knob: both counts at small sizes; never above the bound).  Binary limbs:
 * auto = 4 everywhere (ec_np = 4, ec_np_auto = 0).  Plane pool 0 is sized for ec_np_planes(pool_log, g) planes at init. */
extern int ec_np_auto;                      /* 1: ECALC_NP=auto (decimal limbs) */
extern size_t ec_np_auto_terms;             /* auto: the largest nc a three-prime product of the distributed tiers takes */
int ec_np_for(size_t nterms);               /* the product's prime count: ec_np, or under auto 3 / 4 by nterms (no check) */
int ec_np_prod(size_t nterms, int decimal, const char *where);   /* ec_np_for + the check of that count (ec_np_check at 3) */
/* Phase 15 MPB: ECALC_NP_AUTO_MIN=1 (under auto; off by default) -- the switch-over on a product's real term count.  A coefficient of
 * the product of operands of na and nb limbs (each < 10^18) is the sum of the pairs a_i b_j with i + j = k, 0 <= i < na, 0 <= j < nb:
 * at most min(na, nb) pairs, each <= (10^18 - 1)^2.  mn_core / dist_core form the whole product (every transform length n >= na + nb,
 * so the cyclic convolution does not wrap; the added operand X enters after the CRT, the low / band cuts skip whole pieces), so
 * three primes reconstruct it exactly when min(na, nb) <= ec_np3_max_terms (min (10^18 - 1)^2 < p0 p1 p2).  ec_np_terms(nc, na, nb)
 * = min(na, nb) with the switch, nc (= na + nb, the old conservative form) without; the result is passed to ec_np_for / ec_np_prod. */
extern int ec_np_auto_min;                  /* 1: ECALC_NP_AUTO_MIN=1 with ECALC_NP=auto (decimal limbs) */
size_t ec_np_terms(size_t nc, size_t na, size_t nb);
int ec_np_planes(int pool_log, int g);      /* pool 0's planes: ec_np; auto: 4 when the largest plane of g nodes (2^(min(31, pool_log) + floor(log2 g)) points) exceeds ec_np_auto_terms */
#ifdef __cplusplus
}
#endif

#ifndef EC_PRIMES
#define EC_PRIMES 1                         /* 1: the WP8 set (3 * 2^44 | p-1: 3*2^k lengths); 0: the Phase 3-7 set */
#endif
#if EC_PRIMES
/* c * 2^44 + 1 with 3 | c, c = 240, 216, 207, 147 (RESULTS.md 57); two are the old p1, p3 */
static const uint64_t ec_P[EC_NP] = {4222124650659841ULL, 3799912185593857ULL,
                                     3641582511194113ULL, 2586051348529153ULL};
static const uint64_t ec_G[EC_NP] = {19, 5, 5, 10};
/* w33 = g^((p-1)/2^33): exact order 2^33 */
static const uint64_t ec_W33[EC_NP] = {1664894315601502ULL, 1343624525396189ULL,
                                       3536920527846901ULL, 2276121144993249ULL};
/* w3x33 = g^((p-1)/(3 2^33)): exact order 3 2^33 (w3x33^3 = w33) */
static const uint64_t ec_W3X33[EC_NP] = {2693318777493321ULL, 1846280463207194ULL,
                                         633252398701382ULL, 250170307597761ULL};
/* mu115 = floor(2^115 / p), 64 bits each */
static const uint64_t ec_MU115[EC_NP] = {9838263505978425198ULL, 10931403895531583266ULL,
                                         11406682325772086755ULL, 16062471030168855059ULL};
#else
static const uint64_t ec_P[EC_NP] = {3923057487904769ULL, 3641582511194113ULL,
                                     2867526325239809ULL, 2586051348529153ULL};
static const uint64_t ec_G[EC_NP] = {3, 5, 3, 10};
/* w33 = g^((p-1)/2^33): exact order 2^33 (results/0_params.txt) */
static const uint64_t ec_W33[EC_NP] = {678007507195576ULL, 3536920527846901ULL,
                                       1501474000275416ULL, 2276121144993249ULL};
static const uint64_t ec_W3X33[EC_NP] = {0, 0, 0, 0};   /* p0, p2 have no factor 3 in p-1: no 3 2^k lengths */
/* mu115 = floor(2^115 / p), 64 bits each */
static const uint64_t ec_MU115[EC_NP] = {10588265656658394641ULL, 11406682325772086755ULL,
                                         14485786757268845297ULL, 16062471030168855059ULL};
#endif
/* LEAF kernel: floor(2^123 / 10^18) */
#define EC_MU_1E18 10633823966279326983ULL
#define EC_1E18 1000000000000000000ULL
/* q = floor(x / 10^18), r = x mod 10^18 for x = hi 2^64 + lo < 2^124 (hi < 10^18):
 * Barrett with mu = floor(2^123 / 10^18), q_est <= q, at most 3 corrections */
EC_HD static inline void ec_div1e18(uint64_t hi, uint64_t lo, uint64_t *q, uint64_t *r)
{
    unsigned __int128 x = ((unsigned __int128)hi << 64) | lo;
    unsigned __int128 m_lo = (unsigned __int128)lo * EC_MU_1E18;
    unsigned __int128 m_hi = (unsigned __int128)hi * EC_MU_1E18 + (m_lo >> 64);
    uint64_t qe = (uint64_t)(m_hi >> 59);
    unsigned __int128 rem = x - (unsigned __int128)qe * EC_1E18;
    while (rem >= EC_1E18) { rem -= EC_1E18; qe++; }
    *q = qe; *r = (uint64_t)rem;
}
/* a 4-word (little-endian, < 2^206) value into 4 base-10^18 digits */
EC_HD static inline void ec_words_to_dec4(const uint64_t c[4], uint64_t d[4])
{
    uint64_t w[4] = { c[0], c[1], c[2], c[3] };
    for (int k = 0; k < 4; k++) {
        uint64_t rem = 0;
        for (int i = 3; i >= 0; i--) { uint64_t q; ec_div1e18(rem, w[i], &q, &rem); w[i] = q; }
        d[k] = rem;
    }
}

/* P3: a 3-word value < 2^156 (three primes' CRT) into 3 base-10^18 digits (value < p0 p1 p2 < 10^54 = B^3): three
 * divisions instead of dec4's sixteen.  c[2] < 2^28 < B, so (c[2], c[1]) divides directly; its quotient q1 < 2^33 < B,
 * and c / B = q1 2^64 + q0 < 2^97 has quotient < 2^37 = the top digit. */
EC_HD static inline void ec_words_to_dec3(const uint64_t c[3], uint64_t d[3])
{
    uint64_t q1, q0, r;
    ec_div1e18(c[2], c[1], &q1, &r);
    ec_div1e18(r, c[0], &q0, &r);
    d[0] = r;
    ec_div1e18(q1, q0, &d[2], &r);
    d[1] = r;
}

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
/* ---- the roots of unity (Phase 15 P, PLAN 36: roots above 2^33) ----------------------------------------------------------
 * Before: ec_root(i, logn) = w33^(2^(33 - logn)), a negative shift (undefined; the "root" came out 1) for logn > 33, which the
 * mn tier reaches from 8 nodes on (mn_logn_cap = 31 + floor(log2 g): 2^40 at 576).  Now:
 *   logn <= EC_LOGN_MAX: the table formula as before (bit-identical by construction, the same cost);
 *   EC_LOGN_MAX < logn <= v2(p_i - 1): g^((p_i - 1) / 2^logn) from the generator ec_G.  Both are g^((p-1)/2^logn) (w33 =
 *   g^((p-1)/2^33)); tests/t_roots checks the two agree for logn 1..33 on every prime, and the order (w^(2^logn) = 1,
 *   w^(2^(logn-1)) = p - 1) and the chain root(k+1)^2 = root(k) up to v2(p - 1);
 *   logn > v2(p_i - 1) or < 0: ec_fatal (no such root in this prime).
 * v2(p - 1) = 48, 47, 44, 44 for the WP8 set (c 2^44 + 1: 15 2^48, 27 2^47, 207 2^44, 147 2^44): 2^44 for any subset.
 * The same for the order 3 2^logk (w3x33 = g^((p-1)/(3 2^33))). */
static inline int ec_v2(int i) { return __builtin_ctzll(ec_P[i] - 1); }        /* v2(p_i - 1): the largest 2^k root prime i has */
/* the largest logn every one of the first np primes supports (the transform-length limit of a product on np primes) */
static inline int ec_logn_limit(int np)
{
    int v = 64; for (int i = 0; i < np && i < EC_NP; i++) if (ec_v2(i) < v) v = ec_v2(i);
    return v;
}
/* primitive 2^logn-th root of unity for prime i, 0 <= logn <= v2(p_i - 1) */
static inline uint64_t ec_root(int i, int logn)
{
    if (logn <= EC_LOGN_MAX && logn >= 0) return ec_powmod(ec_W33[i], 1ULL << (EC_LOGN_MAX - logn), ec_P[i]);
    if (logn < 0 || logn > ec_v2(i))
        ec_fatal(EC_RC_FATAL, "ec_root: no root of order 2^%d for prime %d (p = %llu: 2^%d | p - 1 at most)\n", logn, i, (unsigned long long)ec_P[i], ec_v2(i));
    return ec_powmod(ec_G[i], (ec_P[i] - 1) >> logn, ec_P[i]);
}
static inline uint64_t ec_root_inv(int i, int logn) { return ec_inv(ec_root(i, logn), ec_P[i]); }
/* primitive 3 2^logk-th root of unity for prime i (0 when the prime set has none), 0 <= logk <= v2(p_i - 1) */
static inline uint64_t ec_root3(int i, int logk)
{
    if (!ec_W3X33[i]) return 0;
    if (logk <= EC_LOGN_MAX && logk >= 0) return ec_powmod(ec_W3X33[i], 1ULL << (EC_LOGN_MAX - logk), ec_P[i]);
    if (logk < 0 || logk > ec_v2(i) || ((ec_P[i] - 1) >> logk) % 3)
        ec_fatal(EC_RC_FATAL, "ec_root3: no root of order 3 2^%d for prime %d (p = %llu: 2^%d | p - 1 at most)\n", logk, i, (unsigned long long)ec_P[i], ec_v2(i));
    return ec_powmod(ec_G[i], ((ec_P[i] - 1) >> logk) / 3, ec_P[i]);
}
static inline uint64_t ec_root3_inv(int i, int logk) { return ec_inv(ec_root3(i, logk), ec_P[i]); }
static inline int ec_has_radix3(void) { return ec_W3X33[0] != 0; }

#endif

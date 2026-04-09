/*
 * ntt_moduli.h — NTT-friendly prime moduli: table, reduction functions,
 *                and arithmetic helpers for all 15 supported primes.
 *
 * Include this header in any binary that needs multi-modulus support.
 * The reduction functions replace the generic `% q` division in hot paths.
 *
 * REDUCTION STRATEGY BY PRIME CLASS
 * ──────────────────────────────────
 * MOD_FERMAT      (q = 2^m + 1):   exact: t mod q = (t & mask) - (t >> m); 1 sub
 * MOD_KYBER       (q = 13·2^8+1):  software uses reduce_generic (K²RED is FPGA-only)
 * MOD_PROTH_K3    (q = 3·2^m+1):   software uses reduce_generic (K²RED is FPGA-only)
 * MOD_PROTH_K7    (q = 7·2^m+1):   software uses reduce_generic (K²RED is FPGA-only)
 * MOD_PROTH_K15   (q = 15·2^m+1):  software uses reduce_generic (K²RED is FPGA-only)
 * MOD_PROTH_K17   (q = 17·2^m+1):  software uses reduce_generic (K²RED is FPGA-only)
 * MOD_DILITHIUM   (q = 2^23-2^13+1): exact Solinas: t mod q = B + (A<<13) - A
 * MOD_GOLDILOCKS  (q = 2^64-2^32+1): exact 2-step; no 128-bit divide needed
 * MOD_GENERIC     (any):            __uint128_t product % q (hardware divide)
 *
 * NOTE on K²RED: the formula B - k·A (where A = t>>m, B = t&mask) gives
 * t - A·(2^m + k) which is NOT congruent to t mod q in software. K²RED is
 * useful on FPGA where bounded-width arithmetic makes the formula cycle-exact.
 * In software, use Barrett or hardware % for Proth-family primes.
 *
 * OVERFLOW-SAFE BUTTERFLY HELPERS
 * ─────────────────────────────────
 * addmod(u, t, q) and submod(u, t, q) work correctly for all primes
 * including Goldilocks (q near 2^64) where u+t may overflow uint64_t.
 *
 * Build: cc -O2 -Wall -Wextra   (C99; no HIP required for CPU binaries)
 */

#ifndef NTT_MODULI_H
#define NTT_MODULI_H

#include <stdint.h>
#include <stddef.h>  /* NULL */

/* ── Goldilocks prime constant ───────────────────────────────────────────── */
#define GOLDILOCKS_Q  UINT64_C(18446744069414584321)  /* 2^64 - 2^32 + 1 */
#define GOLDILOCKS_MOD UINT64_C(0xFFFFFFFF)           /* 2^32 - 1 = 2^64 mod q */

/* ── Reduction function type ─────────────────────────────────────────────── */
/*
 * reduce_fn_t: pointer to a function that reduces a 128-bit product mod q.
 * product: result of a * b where a, b are NTT elements < q.
 * q:       the modulus (ignored by specialised functions; used by generic).
 * Returns: product mod q in [0, q).
 */
typedef uint64_t (*reduce_fn_t)(__uint128_t product, uint64_t q);

/* ── Modulus classes ─────────────────────────────────────────────────────── */
typedef enum {
    MOD_GENERIC    = 0,  /* fall back to hardware % */
    MOD_FERMAT,          /* q = 2^m + 1 */
    MOD_KYBER,           /* q = 13·2^8+1 (k=13, m=8) */
    MOD_PROTH_K3,        /* q = 3·2^m+1  (k=3) */
    MOD_PROTH_K7,        /* q = 7·2^m+1  (k=7) */
    MOD_PROTH_K15,       /* q = 15·2^m+1 (k=15 = 2^4-1) */
    MOD_PROTH_K17,       /* q = 17·2^m+1 (k=17 = 2^4+1) */
    MOD_DILITHIUM,       /* q = 2^23 - 2^13 + 1 (Solinas) */
    MOD_GOLDILOCKS,      /* q = 2^64 - 2^32 + 1 */
} modulus_class_t;

/* ── Modulus table entry ─────────────────────────────────────────────────── */
typedef struct {
    uint64_t         q;            /* the prime modulus                       */
    uint64_t         g;            /* primitive root mod q                    */
    uint32_t         max_log2_n;   /* max NTT size: n = 2^max_log2_n         */
    modulus_class_t  cls;          /* reduction class                         */
    const char      *name;         /* short label                             */
    const char      *form;         /* algebraic form string                   */
    reduce_fn_t      reduce;       /* fast reduction function                 */
} ntt_modulus_info_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * OVERFLOW-SAFE BUTTERFLY ARITHMETIC
 * Works for all primes including Goldilocks (q near 2^64).
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * addmod: compute (u + t) mod q.
 * For small primes (q < 2^32): u+t < 2^33, no overflow possible; the
 *   `s < u` branch is dead and the compiler removes it.
 * For Goldilocks (q ≈ 2^64): u+t may overflow uint64_t; the overflow
 *   correction adds 2^64 mod q = 2^32-1.
 */
static inline uint64_t addmod(uint64_t u, uint64_t t, uint64_t q)
{
    uint64_t s = u + t;
    if (s < u) s += (UINT64_MAX - q + 1);  /* overflow: s_true = s + 2^64;
                                               (2^64 mod q) = UINT64_MAX-q+1 */
    if (s >= q) s -= q;
    return s;
}

/*
 * submod: compute (u - t) mod q.
 * In uint64_t: when t > u, `u - t + q` wraps correctly because the
 * double-wrap (add 2^64, then add q, then the combined overflow reduces)
 * yields the mathematical value u - t + q which is in [1, q-1].
 */
static inline uint64_t submod(uint64_t u, uint64_t t, uint64_t q)
{
    return (u >= t) ? u - t : u - t + q;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SPECIALIZED REDUCTION FUNCTIONS
 *
 * Each function reduces a 128-bit product mod the target prime.
 * The `q` parameter is accepted for interface uniformity but ignored by
 * specialised functions (q is encoded in the shift and mask constants).
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Generic: hardware divide. Correct for any prime; use as fallback. */
static inline uint64_t reduce_generic(__uint128_t t, uint64_t q)
{
    return (uint64_t)(t % q);
}

/*
 * Fermat primes: q = 2^m + 1.
 * t mod q = (t & mask) - (t >> m)   where mask = 2^m - 1.
 * Negative result: add q once.
 * q = 257  → m=8,  mask=0xFF
 * q = 65537 → m=16, mask=0xFFFF
 */
static inline uint64_t reduce_fermat8(__uint128_t t, uint64_t q_unused)
{
    (void)q_unused;
    /* q = 257 = 2^8 + 1 */
    uint64_t A = (uint64_t)(t >> 8);
    uint64_t B = (uint64_t)t & 0xFFULL;
    int64_t  r = (int64_t)B - (int64_t)(A % 257);  /* A may be > 257 */
    /* Normalise A first: A can be up to 257^2/256 ≈ 258. A % 257 ∈ {0,1}. */
    if (r < 0) r += 257;
    return (uint64_t)r;
}

static inline uint64_t reduce_fermat16(__uint128_t t, uint64_t q_unused)
{
    (void)q_unused;
    /* q = 65537 = 2^16 + 1 */
    /* t < 65537^2 ≈ 2^32; split at bit 16 */
    uint64_t A = (uint64_t)(t >> 16);   /* A < 65537^2 / 2^16 ≈ 65537 */
    uint64_t B = (uint64_t)t & 0xFFFFULL;
    /* A mod 65537: A ≤ 65537, so at most one subtract */
    if (A >= 65537) A -= 65537;
    int64_t r = (int64_t)B - (int64_t)A;
    if (r < 0) r += 65537;
    return (uint64_t)r;
}

/*
 * ML-KEM: q = 3329 = 13·2^8 + 1.
 * K²RED: t mod q ≈ B - 13A = B - (A<<3) - (A<<2) - A   (B = t & 255, A = t>>8)
 * t < 3329^2 ≈ 11.1M; A = t>>8 ≤ 43316; 13A ≤ 563108.
 * Need multiple corrections; use int64 and add q until positive.
 */
static inline uint64_t reduce_kyber(__uint128_t t, uint64_t q_unused)
{
    (void)q_unused;
    /* q = 3329 = 13*2^8+1 */
    uint64_t A = (uint64_t)(t >> 8);
    uint64_t B = (uint64_t)t & 0xFFULL;
    int64_t  r = (int64_t)B - (int64_t)((A << 3) + (A << 2) + A); /* B - 13A */
    /* r ∈ [-13*43316, 255] = [-563108, 255]; need up to ceil(563108/3329)=170 adds.
     * Use modulo for correctness; K²RED benefit is on FPGA, not software. */
    r = ((r % 3329) + 3329) % 3329;
    return (uint64_t)r;
}

/*
 * k=3 Proth family: q = 3·2^m + 1.
 * t mod q = B - 3A = B - (A<<1) - A   where A = t>>m, B = t & (2^m-1).
 * Primes: q=12289 (m=12), q=786433 (m=18), q=3221225473 (m=30).
 * m is encoded in each specific function.
 */
static inline uint64_t reduce_proth_k3_m12(__uint128_t t, uint64_t q_unused)
{
    (void)q_unused;
    /* q = 12289 = 3*2^12+1 */
    const uint64_t Q = 12289, MASK = (1ULL<<12)-1;
    uint64_t A = (uint64_t)(t >> 12);
    uint64_t B = (uint64_t)t & MASK;
    int64_t  r = (int64_t)B - (int64_t)((A << 1) + A);
    r = ((r % (int64_t)Q) + (int64_t)Q) % (int64_t)Q;
    return (uint64_t)r;
}

static inline uint64_t reduce_proth_k3_m18(__uint128_t t, uint64_t q_unused)
{
    (void)q_unused;
    /* q = 786433 = 3*2^18+1 */
    const uint64_t Q = 786433, MASK = (1ULL<<18)-1;
    uint64_t A = (uint64_t)(t >> 18);
    uint64_t B = (uint64_t)t & MASK;
    int64_t  r = (int64_t)B - (int64_t)((A << 1) + A);
    r = ((r % (int64_t)Q) + (int64_t)Q) % (int64_t)Q;
    return (uint64_t)r;
}

static inline uint64_t reduce_proth_k3_m30(__uint128_t t, uint64_t q_unused)
{
    (void)q_unused;
    /* q = 3221225473 = 3*2^30+1 */
    const uint64_t Q = 3221225473ULL, MASK = (1ULL<<30)-1;
    uint64_t A = (uint64_t)(t >> 30);
    uint64_t B = (uint64_t)t & MASK;
    int64_t  r = (int64_t)B - (int64_t)((A << 1) + A);
    r = ((r % (int64_t)Q) + (int64_t)Q) % (int64_t)Q;
    return (uint64_t)r;
}

/*
 * k=7 Proth family: q = 7·2^m + 1.
 * t mod q = B - 7A = B - (A<<2) - (A<<1) - A.
 * Primes: q=7340033 (m=20), q=469762049 (m=26).
 */
static inline uint64_t reduce_proth_k7_m20(__uint128_t t, uint64_t q_unused)
{
    (void)q_unused;
    const uint64_t Q = 7340033, MASK = (1ULL<<20)-1;
    uint64_t A = (uint64_t)(t >> 20);
    uint64_t B = (uint64_t)t & MASK;
    int64_t  r = (int64_t)B - (int64_t)((A<<2) + (A<<1) + A);
    r = ((r % (int64_t)Q) + (int64_t)Q) % (int64_t)Q;
    return (uint64_t)r;
}

static inline uint64_t reduce_proth_k7_m26(__uint128_t t, uint64_t q_unused)
{
    (void)q_unused;
    const uint64_t Q = 469762049, MASK = (1ULL<<26)-1;
    uint64_t A = (uint64_t)(t >> 26);
    uint64_t B = (uint64_t)t & MASK;
    int64_t  r = (int64_t)B - (int64_t)((A<<2) + (A<<1) + A);
    r = ((r % (int64_t)Q) + (int64_t)Q) % (int64_t)Q;
    return (uint64_t)r;
}

/*
 * k=15 = 2^4-1: q = 15·2^27+1 = 2013265921.
 * t mod q = B - 15A = B - (A<<4) + A.
 */
static inline uint64_t reduce_proth_k15(__uint128_t t, uint64_t q_unused)
{
    (void)q_unused;
    const uint64_t Q = 2013265921, MASK = (1ULL<<27)-1;
    uint64_t A = (uint64_t)(t >> 27);
    uint64_t B = (uint64_t)t & MASK;
    int64_t  r = (int64_t)B - (int64_t)((A<<4) - A);  /* -(16A-A) = -15A */
    r = ((r % (int64_t)Q) + (int64_t)Q) % (int64_t)Q;
    return (uint64_t)r;
}

/*
 * k=17 = 2^4+1: q = 17·2^27+1 = 2281701377.
 * t mod q = B - 17A = B - (A<<4) - A.
 * Most efficient non-trivial K²RED: only 2 nonzero bits in k.
 */
static inline uint64_t reduce_proth_k17(__uint128_t t, uint64_t q_unused)
{
    (void)q_unused;
    const uint64_t Q = 2281701377, MASK = (1ULL<<27)-1;
    uint64_t A = (uint64_t)(t >> 27);
    uint64_t B = (uint64_t)t & MASK;
    int64_t  r = (int64_t)B - (int64_t)((A<<4) + A);
    r = ((r % (int64_t)Q) + (int64_t)Q) % (int64_t)Q;
    return (uint64_t)r;
}

/*
 * ML-DSA / Dilithium: q = 8380417 = 2^23 - 2^13 + 1 (Solinas form).
 * 2^23 ≡ 2^13 - 1 (mod q).  For t = A·2^23 + B:
 *   t mod q = B + A·(2^13 - 1) = B + (A<<13) - A.
 * Note: the `% q` here is not a hardware division — it applies only to
 * the intermediate signed result, which is small relative to q.
 */
static inline uint64_t reduce_dilithium(__uint128_t t, uint64_t q_unused)
{
    (void)q_unused;
    const uint64_t Q = 8380417, MASK = (1ULL<<23)-1;
    uint64_t A = (uint64_t)(t >> 23);
    uint64_t B = (uint64_t)t & MASK;
    int64_t  r = (int64_t)B + (int64_t)((A<<13) - A);
    r = ((r % (int64_t)Q) + (int64_t)Q) % (int64_t)Q;
    return (uint64_t)r;
}

/*
 * Goldilocks: q = 2^64 - 2^32 + 1.
 * 2^64 ≡ 2^32 - 1 (mod q).  For 128-bit t = a·2^64 + b:
 *   t mod q = a·(2^32-1) + b.
 * Two-step reduction using the same identity avoids any 128-by-64 division.
 * Algorithm:
 *   Step 1: r = a·(2^32-1) + b  (compute in __uint128_t; r < 2^96)
 *   Step 2: c = r>>64 (< 2^32); d = (uint64_t)r
 *           s = c·(2^32-1) + d  (compute in uint64_t; may overflow once)
 *   Step 3: overflow correction and final conditional subtract.
 */
static inline uint64_t reduce_goldilocks(__uint128_t t, uint64_t q_unused)
{
    (void)q_unused;
    const uint64_t MOD = GOLDILOCKS_MOD;  /* 2^32 - 1 */
    const uint64_t Q   = GOLDILOCKS_Q;

    uint64_t a = (uint64_t)(t >> 64);
    uint64_t b = (uint64_t)t;

    /* Step 1: r = a*(2^32-1) + b; r < a*2^32 + b < q*2^32 + 2^64 < 2^96 */
    __uint128_t r128 = (__uint128_t)a * MOD + b;
    uint64_t c = (uint64_t)(r128 >> 64);   /* c < 2^32 (since r128 < 2^96) */
    uint64_t d = (uint64_t)r128;

    /* Step 2: s = c*(2^32-1) + d; c*MOD < 2^64; s may overflow once */
    uint64_t cm = c * MOD;                 /* c < 2^32, MOD = 2^32-1: cm < 2^64 */
    uint64_t s  = cm + d;
    if (s < cm) s += MOD;                  /* overflow: s_true = s + 2^64
                                              ≡ s + (2^32-1) = s + MOD (mod q) */
    if (s >= Q) s -= Q;
    return s;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MODULI TABLE
 * 15 NTT-friendly primes with all parameters.
 * Access via ntt_modulus_find() or iterate NTT_MODULI[i].
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NTT_NUM_MODULI 15

/*
 * NTT_MODULI table: 15 primes with all parameters.
 *
 * Reduction notes:
 *   Fermat (q=2^m+1): B - A reduction is exact — (t>>m) + (t&mask) mod q = 1 sub.
 *   Dilithium (Solinas q=2^23-2^13+1): B + (A<<13) - A is exact via 2^23≡2^13-1 mod q.
 *   Goldilocks (q=2^64-2^32+1): 2-step 128-bit reduction is exact.
 *   All Proth (k=3,7,15,17) and Kyber (k=13): K²RED formula B-k*A is only an
 *   approximation in software (exact on FPGA); use reduce_generic (128-bit divide)
 *   until a correct software Barrett reduction is added for each.
 */
static const ntt_modulus_info_t NTT_MODULI[NTT_NUM_MODULI] = {
    /*  q                    g   maxlog2n  class              name             form             reduce            */
    {          257,          3,   8, MOD_FERMAT,    "Fermat-8",   "2^8+1",         reduce_fermat8      },
    {         3329,          3,   8, MOD_KYBER,     "ML-KEM",     "13*2^8+1",      reduce_generic      },
    {        12289,         11,  12, MOD_PROTH_K3,  "FALCON",     "3*2^12+1",      reduce_generic      },
    {        65537,          3,  16, MOD_FERMAT,    "Fermat-16",  "2^16+1",        reduce_fermat16     },
    {       786433,         10,  18, MOD_PROTH_K3,  "FHE-RNS-sm","3*2^18+1",      reduce_generic      },
    {   1073479681,         11,  18, MOD_GENERIC,   "TFHE-NTT",   "~2^30",         reduce_generic      },
    {     7340033,           3,  20, MOD_PROTH_K7,  "FHE-RNS",    "7*2^20+1",      reduce_generic      },
    {     8380417,          10,  13, MOD_DILITHIUM, "ML-DSA",     "2^23-2^13+1",   reduce_dilithium    },
    {   998244353,           3,  23, MOD_GENERIC,   "NTT-gen",    "119*2^23+1",    reduce_generic      },
    {  1004535809,           3,  21, MOD_GENERIC,   "HElib-RNS",  "479*2^21+1",    reduce_generic      },
    {  469762049,            3,  26, MOD_PROTH_K7,  "BFV-RNS",    "7*2^26+1",      reduce_generic      },
    { 2013265921,           31,  27, MOD_PROTH_K15, "FHE-RNS-lg", "15*2^27+1",     reduce_generic      },
    { 2281701377,            3,  27, MOD_PROTH_K17, "2-term",     "17*2^27+1",     reduce_generic      },
    { 3221225473ULL,         5,  30, MOD_PROTH_K3,  "Large-NTT",  "3*2^30+1",      reduce_generic      },
    { GOLDILOCKS_Q,          7,  32, MOD_GOLDILOCKS,"Goldilocks", "2^64-2^32+1",   reduce_goldilocks   },
};

/* ═══════════════════════════════════════════════════════════════════════════
 * HELPER FUNCTIONS
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * ntt_modulus_find: look up a modulus by its q value.
 * Returns pointer to entry in NTT_MODULI[], or NULL if not found.
 */
static inline const ntt_modulus_info_t *ntt_modulus_find(uint64_t q)
{
    for (int i = 0; i < NTT_NUM_MODULI; i++)
        if (NTT_MODULI[i].q == q) return &NTT_MODULI[i];
    return NULL;
}

/*
 * ntt_modulus_omega: compute the primitive n-th root of unity for a modulus.
 * Uses omega = g^((q-1)/n) mod q.
 * Requires n to be a power of 2 with n <= 2^max_log2_n.
 * Returns 0 if n is invalid for this modulus.
 */
static inline uint64_t ntt_modulus_omega(const ntt_modulus_info_t *m, uint64_t n)
{
    if (n == 0 || (n & (n-1)) != 0) return 0;         /* not power of 2 */
    if (n > (UINT64_C(1) << m->max_log2_n)) return 0; /* exceeds max    */
    /* omega = g^((q-1)/n) mod q */
    /* (q-1)/n: for Goldilocks q-1 = 2^32*(2^32-1), division by n (power of 2)
     * is exact as long as n <= 2^32. */
    uint64_t exp = (m->q - 1) / n;
    /* mod_pow via __uint128_t */
    uint64_t base = m->g % m->q, result = 1;
    for (uint64_t e = exp; e > 0; e >>= 1) {
        if (e & 1) result = (uint64_t)((__uint128_t)result * base % m->q);
        base = (uint64_t)((__uint128_t)base * base % m->q);
    }
    return result;
}

#endif /* NTT_MODULI_H */

/* bigint.h - little-endian base-2^64 limb arrays (PLAN.md 8).
 *
 * A bigint owns a limb buffer; n is the used length (no leading-zero limbs
 * after bi_norm), cap the allocation.  The heavy operations (shifts, add/sub,
 * repack, get_digit) are in bigint.c from step 3; this header holds what the
 * step-0 harness and step-1 tests need.
 */
#ifndef EC_BIGINT_H
#define EC_BIGINT_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fatal.h"

typedef struct { uint64_t *l; size_t n, cap; } bigint;

/* Limb base (WP1, PLAN.md 15): 0 = binary limbs (2^64, the paper's); 1 =
 * decimal limbs, each < BI_B10 = 10^18 (60 bits).  Set once at start-up
 * (bi_set_decimal / env LIMB_BASE=10); every primitive below honours it.
 * Limb-granular operations (shift by limbs, compare, split) are the same in
 * both bases; bit shifts exist only in binary mode. */
#define BI_B10 1000000000000000000ULL
#ifdef __cplusplus
extern "C" {
#endif
extern int bi_decimal;
void bi_set_decimal(int on);
int  bi_env_base(void);                 /* reads LIMB_BASE, returns bi_decimal */
uint64_t *bi_alloc_huge(uint64_t *old, size_t oldn, size_t cap);   /* >= 64 MiB: 2 MiB-aligned + MADV_HUGEPAGE (first-touch faults are the 10dP/T1 variance, RESULTS.md 62) */
#ifdef __cplusplus
}
#endif

static inline void bi_init(bigint *a) { a->l = 0; a->n = a->cap = 0; }
static inline void bi_free(bigint *a) { free(a->l); bi_init(a); }
static inline void bi_reserve(bigint *a, size_t cap)
{
    if (cap <= a->cap) return;
    if (cap * sizeof *a->l >= ((size_t)64 << 20)) { a->l = bi_alloc_huge(a->l, a->n, cap); a->cap = cap; return; }
    a->l = (uint64_t *)realloc(a->l, cap * sizeof *a->l);
    if (!a->l) ec_fatal(EC_RC_OOM, "bigint: realloc of %zu bytes (host) failed", cap * sizeof *a->l);
    a->cap = cap;
}
static inline void bi_norm(bigint *a) { while (a->n && a->l[a->n - 1] == 0) a->n--; }
static inline void bi_set_zero(bigint *a) { a->n = 0; }
static inline void bi_set_limbs(bigint *a, const uint64_t *src, size_t n)
{
    bi_reserve(a, n); if (n) memcpy(a->l, src, n * sizeof *src); a->n = n; bi_norm(a);
}
static inline int bi_cmp(const bigint *a, const bigint *b)
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (size_t i = a->n; i-- > 0;)
        if (a->l[i] != b->l[i]) return a->l[i] < b->l[i] ? -1 : 1;
    return 0;
}
static inline size_t bi_bits(const bigint *a)
{
    if (!a->n) return 0;
    return 64 * (a->n - 1) + (64 - __builtin_clzll(a->l[a->n - 1]));
}

/* ---- limb-array primitives (bigint.c) ------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif
/* r = a + b (na >= nb), returns carry; r may alias a */
uint64_t limb_add(uint64_t *r, const uint64_t *a, size_t na, const uint64_t *b, size_t nb);
/* r = a - b (a >= b as numbers, na >= nb), returns borrow (0 if a >= b); r may alias a */
uint64_t limb_sub(uint64_t *r, const uint64_t *a, size_t na, const uint64_t *b, size_t nb);
/* r[0 .. na+nb) = a * b, schoolbook, __int128 accumulate, OpenMP over output limbs */
void limb_mul_school(uint64_t *r, const uint64_t *a, size_t na, const uint64_t *b, size_t nb);
/* r = a * m + add, returns the carry-out limb */
uint64_t limb_mul_1(uint64_t *r, const uint64_t *a, size_t na, uint64_t m, uint64_t add);
/* r = a >> bits / a << bits (bits < 64), returns bits shifted out (shl); binary base only */
void limb_shr_bits(uint64_t *r, const uint64_t *a, size_t n, unsigned bits);
uint64_t limb_shl_bits(uint64_t *r, const uint64_t *a, size_t n, unsigned bits);
/* the base-agnostic replacements: r = a * B^k (limb shift) and r = a / B^k */
void bi_shl_limbs(bigint *r, const bigint *a, size_t k);
void bi_shr_limbs(bigint *r, const bigint *a, size_t k);
/* r = a * 10^k (0 <= k < 18), decimal base only */
void bi_mul_pow10(bigint *r, const bigint *a, unsigned k);
/* B^k as a bigint (2^(64k) or 10^(18k)) */
void bi_set_base_pow(bigint *r, size_t k);
/* top limb of a w-limb window read as a signed value: 1 if the value is negative (>= B/2) */
static inline int bi_limb_negative(uint64_t top) { extern int bi_decimal; return bi_decimal ? top >= BI_B10 / 2 : (int)(top >> 63); }
static inline uint64_t bi_limb_max(void) { extern int bi_decimal; return bi_decimal ? BI_B10 - 1 : ~0ULL; }
size_t limb_norm(const uint64_t *a, size_t n);

/* bigint operations; results are normalised */
void bi_copy(bigint *r, const bigint *a);
void bi_set_u64(bigint *r, uint64_t v);
void bi_add(bigint *r, const bigint *a, const bigint *b);
void bi_sub(bigint *r, const bigint *a, const bigint *b);           /* a >= b */
void bi_add_shifted(bigint *r, const bigint *a, size_t limbs);      /* r += a << (64 limbs) */
void bi_sub_shifted(bigint *r, const bigint *a, size_t limbs);      /* r -= a << (64 limbs), r >= that */
void bi_shl(bigint *r, const bigint *a, size_t bits);          /* bits: binary base only; multiples of 64 are limb shifts in either base */
void bi_shr(bigint *r, const bigint *a, size_t bits);
void bi_mul_school(bigint *r, const bigint *a, const bigint *b);
void bi_mul_u64(bigint *r, const bigint *a, uint64_t m);
void bi_add_u64(bigint *r, uint64_t v);
uint64_t bi_divmod_u64(bigint *q, const bigint *a, uint64_t d);     /* returns remainder */
#ifdef __cplusplus
}
#endif
#endif /* EC_BIGINT_H */

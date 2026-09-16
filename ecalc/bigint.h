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

typedef struct { uint64_t *l; size_t n, cap; } bigint;

static inline void bi_init(bigint *a) { a->l = 0; a->n = a->cap = 0; }
static inline void bi_free(bigint *a) { free(a->l); bi_init(a); }
static inline void bi_reserve(bigint *a, size_t cap)
{
    if (cap <= a->cap) return;
    if (a->l && a->cap && a->n == 0 && cap > a->cap) { /* fresh view or pool slot without room */ }
    a->l = (uint64_t *)realloc(a->l, cap * sizeof *a->l);
    if (!a->l) { abort(); }
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
#endif

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
/* r = a >> bits / a << bits (bits < 64), returns bits shifted out (shl) */
void limb_shr_bits(uint64_t *r, const uint64_t *a, size_t n, unsigned bits);
uint64_t limb_shl_bits(uint64_t *r, const uint64_t *a, size_t n, unsigned bits);
size_t limb_norm(const uint64_t *a, size_t n);

/* bigint operations; results are normalised */
void bi_copy(bigint *r, const bigint *a);
void bi_set_u64(bigint *r, uint64_t v);
void bi_add(bigint *r, const bigint *a, const bigint *b);
void bi_sub(bigint *r, const bigint *a, const bigint *b);           /* a >= b */
void bi_add_shifted(bigint *r, const bigint *a, size_t limbs);      /* r += a << (64 limbs) */
void bi_sub_shifted(bigint *r, const bigint *a, size_t limbs);      /* r -= a << (64 limbs), r >= that */
void bi_shl(bigint *r, const bigint *a, size_t bits);
void bi_shr(bigint *r, const bigint *a, size_t bits);
void bi_mul_school(bigint *r, const bigint *a, const bigint *b);
void bi_mul_u64(bigint *r, const bigint *a, uint64_t m);
void bi_add_u64(bigint *r, uint64_t v);
uint64_t bi_divmod_u64(bigint *q, const bigint *a, uint64_t d);     /* returns remainder */
#ifdef __cplusplus
}
#endif

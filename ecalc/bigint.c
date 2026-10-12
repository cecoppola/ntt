/* bigint.c - see bigint.h */
#include <stdio.h>
#include "fatal.h"
#include <omp.h>
#include "bigint.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
/* host numbers: realloc (the 2 MiB-aligned THP form, BI_HUGE, was removed 2026-10-11: measured harmful) */
uint64_t *bi_alloc_huge(uint64_t *old, size_t oldn, size_t cap)
{
    uint64_t *p = (uint64_t *)realloc(old, cap * 8); if (!p) ec_fatal(EC_RC_OOM, "bigint: realloc of %zu bytes (host) failed", cap * 8);   /* BI_HUGE (THP) removed 2026-10-11: measured harmful (defrag stalls) */
    return p;
}

typedef unsigned __int128 u128;
#define B10 BI_B10
/* archive:PLAN §28.5: the limbs are decimal (base 10^18) only; the binary 2^64 pipeline (LIMB_BASE=2, todec) is archived under
 * archive:binary_pipeline/ (tag pre-28.5-binary).  A LIMB_BASE other than 10 is refused here, never silently ignored. */
void bi_check_base(void)
{
    const char *e = getenv("LIMB_BASE");
    if (e && *e && atoi(e) != 10)
        ec_fatal(EC_RC_FATAL, "LIMB_BASE=%s: only decimal limbs (10) exist since PLAN 28.5; the binary 2^64 pipeline is archived (archive:binary_pipeline/, git tag pre-28.5-binary)\n", e);
}

static uint64_t add_serial(uint64_t *r, const uint64_t *a, size_t na, const uint64_t *b, size_t nb, uint64_t cin)
{
    size_t i; uint64_t c = cin;
    for (i = 0; i < nb; i++) { uint64_t s = a[i] + b[i] + c; c = s >= B10; r[i] = c ? s - B10 : s; }
    for (; i < na; i++) { uint64_t s = a[i] + c; c = s >= B10; r[i] = c ? s - B10 : s; }
    return c;
}
static uint64_t sub_serial(uint64_t *r, const uint64_t *a, size_t na, const uint64_t *b, size_t nb, uint64_t bin)
{
    uint64_t br = bin; size_t i;
    for (i = 0; i < nb; i++) { uint64_t ai = a[i], bi = b[i] + br; br = ai < bi; r[i] = br ? ai + B10 - bi : ai - bi; }
    for (; i < na; i++) { uint64_t ai = a[i]; uint64_t d = br; br = ai < d; r[i] = br ? ai + B10 - d : ai - d; }
    return br;
}
#define PAR_MIN (1u << 22)
#define PAR_CHUNK (1u << 20)
/* parallel: chunks added independently, then carries rippled chunk by chunk */
uint64_t limb_add(uint64_t *r, const uint64_t *a, size_t na, const uint64_t *b, size_t nb)
{
    if (na < PAR_MIN) return add_serial(r, a, na, b, nb, 0);
    size_t nch = (na + PAR_CHUNK - 1) / PAR_CHUNK, t;
    uint64_t *cout = (uint64_t *)malloc(nch * 8);
#pragma omp parallel for schedule(static)
    for (t = 0; t < nch; t++) {
        size_t lo = t * PAR_CHUNK, hi = lo + PAR_CHUNK < na ? lo + PAR_CHUNK : na;
        size_t nbb = nb > hi ? PAR_CHUNK : nb > lo ? nb - lo : 0;
        cout[t] = add_serial(r + lo, a + lo, hi - lo, b + lo, nbb, 0);
    }
    uint64_t c = 0;
    for (t = 0; t < nch; t++) {
        size_t lo = t * PAR_CHUNK, hi = lo + PAR_CHUNK < na ? lo + PAR_CHUNK : na, i = lo;
        while (c && i < hi) { r[i] += 1; c = r[i] == B10; if (c) r[i] = 0; i++; }
        c |= cout[t];
    }
    free(cout);
    return c;
}
uint64_t limb_sub(uint64_t *r, const uint64_t *a, size_t na, const uint64_t *b, size_t nb)
{
    if (na < PAR_MIN) return sub_serial(r, a, na, b, nb, 0);
    size_t nch = (na + PAR_CHUNK - 1) / PAR_CHUNK, t;
    uint64_t *bout = (uint64_t *)malloc(nch * 8);
#pragma omp parallel for schedule(static)
    for (t = 0; t < nch; t++) {
        size_t lo = t * PAR_CHUNK, hi = lo + PAR_CHUNK < na ? lo + PAR_CHUNK : na;
        size_t nbb = nb > hi ? PAR_CHUNK : nb > lo ? nb - lo : 0;
        bout[t] = sub_serial(r + lo, a + lo, hi - lo, b + lo, nbb, 0);
    }
    uint64_t c = 0;
    for (t = 0; t < nch; t++) {
        size_t lo = t * PAR_CHUNK, hi = lo + PAR_CHUNK < na ? lo + PAR_CHUNK : na, i = lo;
        while (c && i < hi) { c = r[i] == 0; r[i] = c ? bi_limb_max() : r[i] - 1; i++; }
        c |= bout[t];
    }
    free(bout);
    return c;
}
void limb_mul_school(uint64_t *r, const uint64_t *a, size_t na, const uint64_t *b, size_t nb)
{
    size_t n = na + nb, k;
    if (!na || !nb) return;
    if (na < nb) { const uint64_t *t = a; a = b; b = t; size_t tn = na; na = nb; nb = tn; }
    {   /* columns as 3-word sums (a b < 10^36 < 2^120: up to 2^8 products per u128 before
         * spilling), reduced mod B once per column with the carry rippled: two 128-bit
         * divisions per column instead of one per product */
        uint64_t *w1 = (uint64_t *)malloc(2 * n * sizeof *w1), *w2 = w1 + n;
#pragma omp parallel for schedule(dynamic, 64) if (!omp_in_parallel() && n > 4096)
        for (k = 0; k < n; k++) {
            size_t i0 = k >= nb - 1 ? k - (nb - 1) : 0, i1 = k < na ? k : na - 1, i;
            u128 acc = 0; uint64_t hi = 0;
            for (i = i0; i <= i1; i++) {
                u128 p = (u128)a[i] * b[k - i];
                acc += p; if (acc < p) hi++;
            }
            r[k] = (uint64_t)acc; w1[k] = (uint64_t)(acc >> 64); w2[k] = hi;
        }
        u128 carry = 0;                              /* < n B / B ... < 2^70 */
        for (k = 0; k < n; k++) {
            /* V = w2 2^128 + w1 2^64 + r[k] + carry; H = w2 2^64 + w1 (+ carry's high part) */
            u128 lo = (u128)r[k] + carry; uint64_t l1 = (uint64_t)(lo >> 64), l0 = (uint64_t)lo;
            u128 H = ((u128)w2[k] << 64) + w1[k] + l1;
            u128 qH = H / B10, rH = H % B10;
            u128 L = (rH << 64) + l0;                /* < B 2^64 */
            u128 q2 = L / B10; r[k] = (uint64_t)(L % B10);
            carry = (qH << 64) + q2;
        }
        free(w1);
    }
}
/* Phase 15 S1: the decimal mul_1 by a precomputed reciprocal of 10^18, for any m < B and add <= B, limbs
 * canonical (< B).  mh = floor(m 2^64 / B) (< 2^64 because m < B).  For x = a[i] < 2^64 the true quotient Q = floor(x m / B)
 * satisfies qe = floor(x mh / 2^64) in {Q - 1, Q}: x mh <= x m 2^64 / B, and x mh > x m 2^64 / B - x > (x m / B - 1) 2^64.
 * So s = x m - qe B + c (mod 2^64; the true value is (x m mod B) + (Q - qe) B + c, in [0, 3B) < 2^64 while c <= B) needs at
 * most two corrections, each a compare against a constant (branch-free: clang made the one-at-a-time form branchy, 2x slower
 * on Zen 2).  The quotient does not depend on the incoming carry: the loop-carried chain is only s, c = qe + f + g
 * (<= m <= B - 1 since x < B).  The limbs are those of the exact product, hence identical to the serial form's. */
/* floor(m 2^64 / 10^18) for m < 10^18 (a 64-bit value: kept out of line so that the loops see a 64-bit multiplier) */
__attribute__((noinline)) static uint64_t recip_b10(uint64_t m) { return (uint64_t)(((u128)m << 64) / B10); }
static inline uint64_t mul1_dec_fast(uint64_t *r, const uint64_t *a, size_t na, uint64_t m, uint64_t c)
{
    const uint64_t mh = recip_b10(m);
    for (size_t i = 0; i < na; i++) {
        uint64_t x = a[i];
        uint64_t qe = (uint64_t)(((u128)x * mh) >> 64);
        uint64_t s = x * m - qe * B10 + c;           /* true value: (x m - Q B) + (Q - qe) B + c in [0, 3B) */
        uint64_t f = s >= B10, g = s >= 2 * B10;
        r[i] = s - (B10 & (0 - f)) - (B10 & (0 - g)); c = qe + f + g;
    }
    return c;
}
/* P += Q; Q *= k in one pass over the limbs (the seed span's Horner step; decimal, k < B, P and Q canonical; else the two
 * calls).  Q's limb is read once for both; the multiply is mul1_dec_fast's */
void bi_span_step(bigint *P, bigint *Q, uint64_t k)
{
    size_t n = Q->n, pn = P->n, i;
    if (k >= B10) { bi_add(P, P, Q); bi_mul_u64(Q, Q, k); return; }
    bi_reserve(P, (pn > n ? pn : n) + 1); bi_reserve(Q, n + 1);
    uint64_t *p = P->l, *q = Q->l, ca = 0, c = 0;
    const uint64_t mh = recip_b10(k);
    size_t lo = pn < n ? pn : n;
#define SPAN_MUL(x) do { uint64_t qe = (uint64_t)(((u128)(x) * mh) >> 64), s = (x) * k - qe * B10 + c; \
                         uint64_t f = s >= B10, g = s >= 2 * B10; \
                         q[i] = s - (B10 & (0 - f)) - (B10 & (0 - g)); c = qe + f + g; } while (0)
    for (i = 0; i < lo; i++) {
        uint64_t x = q[i], t = p[i] + x + ca; ca = t >= B10; p[i] = t - (B10 & (0 - ca));
        SPAN_MUL(x);
    }
    for (; i < n; i++) {                               /* Q longer than P (the usual case: by one limb at most) */
        uint64_t x = q[i], t = x + ca; ca = t >= B10; p[i] = t - (B10 & (0 - ca));
        SPAN_MUL(x);
    }
#undef SPAN_MUL
    for (; i < pn; i++) { uint64_t t = p[i] + ca; ca = t >= B10; p[i] = t - (B10 & (0 - ca)); }
    P->n = i; if (ca) P->l[P->n++] = ca;
    if (!k || !n) { Q->n = 0; return; }
    Q->n = n; if (c) Q->l[Q->n++] = c;                 /* c <= k < B: one limb */
}
static uint64_t mul1_serial(uint64_t *r, const uint64_t *a, size_t na, uint64_t m, uint64_t add)
{
    u128 c = add;
    if (m < B10 && add <= B10) return mul1_dec_fast(r, a, na, m, add);
    {                                              /* m < B: a[i] m + c < B^2 + B */
        if (m < ((uint64_t)1 << 33)) {
            /* x = a[i] m + c < 2^93: q = floor(x / 10^18) by a 64-bit Barrett step -- q_est = ((x >> 30) mu) >> 64
             * with mu = floor(2^94 / 10^18) < 2^35, then at most two corrections (WP4, RESULTS.md 58) */
            const uint64_t MU = 19807040628ULL;      /* floor(2^94 / 10^18) */
            for (size_t i = 0; i < na; i++) {
                c += (u128)a[i] * m;
                uint64_t q = (uint64_t)(((u128)(uint64_t)(c >> 30) * MU) >> 64);
                uint64_t rem = (uint64_t)(c - (u128)q * B10);
                while (rem >= B10) { rem -= B10; q++; }
                r[i] = rem; c = q;
            }
            return (uint64_t)c;
        }
        for (size_t i = 0; i < na; i++) { c += (u128)a[i] * m; r[i] = (uint64_t)(c % B10); c /= B10; }
        return (uint64_t)c;
    }
}
/* parallel: chunks multiplied independently, each chunk's carry-out (< m) added into the next chunk with a ripple */
uint64_t limb_mul_1(uint64_t *r, const uint64_t *a, size_t na, uint64_t m, uint64_t add)
{
    if (na < PAR_MIN) return mul1_serial(r, a, na, m, add);
    size_t nch = (na + PAR_CHUNK - 1) / PAR_CHUNK, t;
    uint64_t *cout = (uint64_t *)malloc(nch * 8);
#pragma omp parallel for schedule(static)
    for (t = 0; t < nch; t++) {
        size_t lo = t * PAR_CHUNK, hi = lo + PAR_CHUNK < na ? lo + PAR_CHUNK : na;
        cout[t] = mul1_serial(r + lo, a + lo, hi - lo, m, t == 0 ? add : 0);
    }
    uint64_t c = 0;
    for (t = 0; t < nch; t++) {
        size_t lo = t * PAR_CHUNK, hi = lo + PAR_CHUNK < na ? lo + PAR_CHUNK : na, i = lo;
        if (c) {                                      /* add the previous chunk's carry with ripple */
            while (c && i < hi) { uint64_t s = r[i] + c; c = s >= B10; r[i] = c ? s - B10 : s; i++; }
        }
        c += cout[t];
    }
    free(cout);
    return c;
}
/* parallel chunked copy that tolerates overlap in the memmove sense (chunks
 * processed in the safe order when r and a overlap) */
static void limb_move(uint64_t *r, const uint64_t *a, size_t n)
{
    if (r == a || !n) return;
    if (n < PAR_MIN) { memmove(r, a, n * 8); return; }
    size_t nch = (n + PAR_CHUNK - 1) / PAR_CHUNK;
    size_t dist = r < a ? (size_t)(a - r) : (size_t)(r - a);
    if (dist < n) { memmove(r, a, n * 8); return; }       /* overlapping: serial (parallel chunks would race) */
#pragma omp parallel for schedule(static)
    for (size_t t = 0; t < nch; t++) {
        size_t lo = t * PAR_CHUNK, hi = lo + PAR_CHUNK < n ? lo + PAR_CHUNK : n;
        memcpy(r + lo, a + lo, (hi - lo) * 8);
    }
}
size_t limb_norm(const uint64_t *a, size_t n) { while (n && a[n - 1] == 0) n--; return n; }

void bi_copy(bigint *r, const bigint *a) { if (r == a) return; bi_reserve(r, a->n ? a->n : 1); limb_move(r->l, a->l, a->n); r->n = a->n; }
void bi_set_u64(bigint *r, uint64_t v) { bi_reserve(r, 1); r->l[0] = v; r->n = v ? 1 : 0; }
void bi_add(bigint *r, const bigint *a, const bigint *b)
{
    if (a->n < b->n) { const bigint *t = a; a = b; b = t; }
    bi_reserve(r, a->n + 1);
    uint64_t c = limb_add(r->l, a->l, a->n, b->l, b->n);
    r->n = a->n; if (c) r->l[r->n++] = c;
}
void bi_sub(bigint *r, const bigint *a, const bigint *b)
{
    if (bi_cmp(a, b) < 0) { ec_fatal(EC_RC_FATAL, "bi_sub: a < b\n"); }
    bi_reserve(r, a->n);
    limb_sub(r->l, a->l, a->n, b->l, b->n);
    r->n = a->n; bi_norm(r);
}
void bi_add_shifted(bigint *r, const bigint *a, size_t k)
{
    size_t need = (a->n + k > r->n ? a->n + k : r->n) + 1;
    bi_reserve(r, need);
    { size_t i;
#pragma omp parallel for schedule(static) if (need - r->n > (1u << 22))
      for (i = r->n; i < need; i++) r->l[i] = 0; }
    uint64_t c = limb_add(r->l + k, r->l + k, need - k - 1, a->l, a->n);
    r->l[need - 1] = c;
    r->n = need; bi_norm(r);
}
void bi_sub_shifted(bigint *r, const bigint *a, size_t k)
{
    if (r->n < a->n + k) { ec_fatal(EC_RC_FATAL, "bi_sub_shifted: r too small\n"); }
    uint64_t br = limb_sub(r->l + k, r->l + k, r->n - k, a->l, a->n);
    if (br) { ec_fatal(EC_RC_FATAL, "bi_sub_shifted: negative\n"); }
    bi_norm(r);
}
/* r = a B^k and r = floor(a / B^k): whole-limb shifts (limb_move handles r == a and overlap) */
void bi_shl_limbs(bigint *r, const bigint *a, size_t k)
{
    if (!a->n) { r->n = 0; return; }
    bi_reserve(r, a->n + k);
    limb_move(r->l + k, a->l, a->n);
    for (size_t i = 0; i < k; i++) r->l[i] = 0;
    r->n = a->n + k; bi_norm(r);
}
void bi_shr_limbs(bigint *r, const bigint *a, size_t k)
{
    if (a->n <= k) { r->n = 0; return; }
    bi_reserve(r, a->n - k);
    limb_move(r->l, a->l + k, a->n - k);
    r->n = a->n - k; bi_norm(r);
}
void bi_mul_school(bigint *r, const bigint *a, const bigint *b)
{
    if (!a->n || !b->n) { r->n = 0; return; }
    if (r == a || r == b) { bigint t; bi_init(&t); bi_mul_school(&t, a, b); bi_free(r); *r = t; return; }
    bi_reserve(r, a->n + b->n);
    limb_mul_school(r->l, a->l, a->n, b->l, b->n);
    r->n = a->n + b->n; bi_norm(r);
}
void bi_mul_u64(bigint *r, const bigint *a, uint64_t m)
{
    if (!a->n || !m) { r->n = 0; return; }
    bi_reserve(r, a->n + 1);
    uint64_t c = limb_mul_1(r->l, a->l, a->n, m, 0);
    r->n = a->n; if (c) r->l[r->n++] = c;
}
void bi_add_u64(bigint *r, uint64_t v)
{
    bi_reserve(r, r->n + 2);
    u128 s = v;
    for (size_t i = 0; i < r->n && s; i++) { s += r->l[i]; r->l[i] = (uint64_t)(s % B10); s /= B10; }
    while (s) { r->l[r->n++] = (uint64_t)(s % B10); s /= B10; }
}
uint64_t bi_divmod_u64(bigint *q, const bigint *a, uint64_t d)
{
    u128 rem = 0;
    bi_reserve(q, a->n ? a->n : 1);
    for (size_t i = a->n; i-- > 0;) { rem = rem * B10 + a->l[i]; q->l[i] = (uint64_t)(rem / d); rem %= d; }
    q->n = a->n; bi_norm(q);
    return (uint64_t)rem;
}
void bi_mul_pow10(bigint *r, const bigint *a, unsigned k)
{
    static const uint64_t p10[19] = {1ULL,10ULL,100ULL,1000ULL,10000ULL,100000ULL,1000000ULL,10000000ULL,100000000ULL,1000000000ULL,10000000000ULL,100000000000ULL,1000000000000ULL,10000000000000ULL,100000000000000ULL,1000000000000000ULL,10000000000000000ULL,100000000000000000ULL,1000000000000000000ULL};
    bi_mul_u64(r, a, p10[k]);
}
void bi_set_base_pow(bigint *r, size_t k)
{
    bi_reserve(r, k + 1); memset(r->l, 0, k * 8); r->l[k] = 1; r->n = k + 1;
}

/* V3 (Phase 14) throwaway prototype: the cost of the level-0 seed spans (binsplit.c span()) in the decimal base, and of two
 * candidate replacements for TASKS 2.2 (the decimal mul_1).  Not part of the pipeline.
 *
 *   gcc -O2 -fopenmp -I../ecalc -DEC_FATAL_NO_HIP v3_span_bench.c ../ecalc/bigint.c ../ecalc/fatal.c -o v3_span_bench
 *   ./v3_span_bench <k0> <nspans> [S=256]      spans [k0 + i S, k0 + (i+1) S), i < nspans, all threads
 *
 * mode A = the pipeline's span(): bi_add(P, P, Q); bi_mul_u64(Q, Q, k) -- mul1_serial: Barrett for m < 2^33, else u128 % and /
 * mode B = fused P += Q; Q *= k in one pass, the quotient by a double estimate with an exact integer correction (any m < 2^50),
 *          carry kept off the multiply's dependency chain (q_{i-1} added to r_i with a rare ripple)
 * mode C = B's multiply only, P += Q as the pipeline does (separate add pass)
 * Every mode's P, Q are checked limb for limb against mode A's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <omp.h>
#include "bigint.h"

typedef unsigned __int128 u128;
#define B BI_B10

static void span_A(bigint *P, bigint *Q, unsigned long a, unsigned long b)
{
    bi_set_u64(P, 1); bi_set_u64(Q, b - 1);
    for (unsigned long k = b - 1; k-- > a;) { bi_add(P, P, Q); bi_mul_u64(Q, Q, k); }
}

/* r[0..n) = q[0..n) * m (decimal limbs), returns the carry-out; the per-limb quotient does not feed the next limb's multiply */
static inline uint64_t mul1_B(uint64_t *r, const uint64_t *q, size_t n, uint64_t m)
{
    const double md = (double)m, inv = 1e-18;
    uint64_t carry = 0;                          /* q_{i-1}: the high part of the previous limb's product */
    for (size_t i = 0; i < n; i++) {
        u128 x = (u128)q[i] * m;                 /* < 10^18 * 2^50 */
        int64_t qe = (int64_t)((double)(int64_t)q[i] * md * inv);   /* |qe - x / 10^18| <= 1 */
        int64_t rem = (int64_t)((uint64_t)x - (uint64_t)qe * B);    /* the true remainder + (q - qe) 10^18: in (-10^18, 2 10^18) */
        if (rem < 0) { rem += B; qe--; } else if (rem >= (int64_t)B) { rem -= B; qe++; }
        uint64_t s = (uint64_t)rem + carry;      /* < 10^18 + 2^50 */
        uint64_t c = s >= B; r[i] = s - (c ? B : 0);
        carry = (uint64_t)qe + c;
    }
    return carry;
}
static void span_B(bigint *P, bigint *Q, unsigned long a, unsigned long b, int fused)
{
    bi_set_u64(P, 1); bi_set_u64(Q, b - 1);
    for (unsigned long k = b - 1; k-- > a;) {
        size_t n = Q->n;
        bi_reserve(P, n + 2); bi_reserve(Q, n + 2);
        if (fused) {                              /* P += Q (P->n <= Q->n always here: P < Q's length + 1) */
            uint64_t c = 0; size_t i, pn = P->n;
            for (i = 0; i < n; i++) { uint64_t s = (i < pn ? P->l[i] : 0) + Q->l[i] + c; c = s >= B; P->l[i] = c ? s - B : s; }
            for (; i < pn; i++) { uint64_t s = P->l[i] + c; c = s >= B; P->l[i] = c ? s - B : s; }
            P->n = i; if (c) P->l[P->n++] = c;
        } else bi_add(P, P, Q);
        uint64_t cy = mul1_B(Q->l, Q->l, n, k);
        while (cy) { Q->l[Q->n++] = cy % B; cy /= B; }
    }
}

/* mode D = the pipeline's structure with mul1_serial's Barrett step widened: q_est = ((x >> 40) MU40) >> 64, MU40 = floor(2^104 / 10^18),
 * exact for x < 2^104 (m < 2^44) with the same <= 2 corrections -- TASKS 2.2's minimal form (the constants only) */
static uint64_t mul1_D(uint64_t *r, const uint64_t *a, size_t na, uint64_t m)
{
    u128 c = 0;
    if (m < ((uint64_t)1 << 33)) {                /* the pipeline's own path */
        const uint64_t MU = 19807040628ULL;
        for (size_t i = 0; i < na; i++) { c += (u128)a[i] * m; uint64_t q = (uint64_t)(((u128)(uint64_t)(c >> 30) * MU) >> 64);
            uint64_t rem = (uint64_t)(c - (u128)q * B); while (rem >= B) { rem -= B; q++; } r[i] = rem; c = q; }
        return (uint64_t)c;
    }
    const uint64_t MU40 = (uint64_t)(((u128)1 << 104) / B);   /* < 2^45 */
    for (size_t i = 0; i < na; i++) { c += (u128)a[i] * m; uint64_t q = (uint64_t)(((u128)(uint64_t)(c >> 40) * MU40) >> 64);
        uint64_t rem = (uint64_t)(c - (u128)q * B); while (rem >= B) { rem -= B; q++; } r[i] = rem; c = q; }
    return (uint64_t)c;
}
static void span_D(bigint *P, bigint *Q, unsigned long a, unsigned long b)
{
    bi_set_u64(P, 1); bi_set_u64(Q, b - 1);
    for (unsigned long k = b - 1; k-- > a;) {
        bi_add(P, P, Q); bi_reserve(Q, Q->n + 2);
        uint64_t cy = mul1_D(Q->l, Q->l, Q->n, k);
        while (cy) { Q->l[Q->n++] = cy % B; cy /= B; }
    }
}

int main(int argc, char **argv)
{
    bi_set_decimal(1);
    unsigned long k0 = argc > 1 ? strtoul(argv[1], 0, 10) : 1000000000UL;
    long ns = argc > 2 ? atol(argv[2]) : 100000;
    unsigned long S = argc > 3 ? strtoul(argv[3], 0, 10) : 256;
    int nt = omp_get_max_threads();
    double t[4] = {0, 0, 0, 0}; long bad[4] = {0, 0, 0, 0}; double limbs = 0;
    for (int mode = 0; mode < 4; mode++) {
        double t0 = omp_get_wtime(), lsum = 0; long nb = 0;
#pragma omp parallel reduction(+:lsum, nb)
        {
            bigint p, q, pr, qr; bi_init(&p); bi_init(&q); bi_init(&pr); bi_init(&qr);
#pragma omp for schedule(dynamic, 16)
            for (long i = 0; i < ns; i++) {
                unsigned long a = k0 + i * S, b = a + S;
                if (mode == 0) span_A(&p, &q, a, b); else if (mode == 3) span_D(&p, &q, a, b); else span_B(&p, &q, a, b, mode == 1);
                lsum += q.n;
                if (mode && (i % 97) == 0) { span_A(&pr, &qr, a, b); if (bi_cmp(&p, &pr) || bi_cmp(&q, &qr)) nb++; }
            }
            bi_free(&p); bi_free(&q); bi_free(&pr); bi_free(&qr);
        }
        t[mode] = omp_get_wtime() - t0; bad[mode] = nb; if (!mode) limbs = lsum / ns;
    }
    /* the checked spans are recomputed by A inside B's and C's timing: correct for it (1/97 of the spans) */
    double ta = t[0], tb = t[1] - ta / 97, tc = t[2] - ta / 97, td = t[3] - ta / 97;
    double terms = (double)ns * S;
    printf("k0 %.3e (log2 %.2f)  spans %ld of %lu  threads %d  final Q %.1f limbs\n", (double)k0, __builtin_log2((double)k0), ns, S, nt, limbs);
    printf("  A pipeline span(): %.3f s = %.2f ns/term (all threads) | B fused+fast: %.3f s (x%.2f) mismatches %ld | C fast mul only: %.3f s (x%.2f) mismatches %ld\n",
           ta, 1e9 * ta / terms, tb, ta / tb, bad[1], tc, ta / tc, bad[2]);
    printf("  D Barrett widened (shift 40): %.3f s = %.2f ns/term (x%.2f) mismatches %ld;  per limb-step x threads (A, D): %.3f, %.3f ns\n",
           td, 1e9 * td / terms, ta / td, bad[3], 1e9 * ta * nt / (terms * limbs / 2), 1e9 * td * nt / (terms * limbs / 2));
    return bad[1] || bad[2] || bad[3];
}

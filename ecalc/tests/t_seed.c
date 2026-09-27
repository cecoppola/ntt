/* t_seed - Phase 15 S1: the fast decimal mul_1 and the fused seed span (BI_MUL1_FAST=1) against the serial path (0).
 *
 *   ./tests/t_seed [spans per range = 400] [threads for the timing = all]
 *
 * A. limb_mul_1 (decimal): every multiplier range -- m < 2^33 (the serial path's Barrett step), 2^33 <= m < 2^44 and up to
 *    10^18 - 1 (the serial path's u128 % and /), edge values; operand kinds uniform / all B-1 / sparse; lengths 1 .. 1000 and
 *    one above the parallel threshold (2^22 + 5 limbs); add in {0, 1, random, B - 1, B}.  Fast vs serial limb for limb, and
 *    against GMP on a subset.
 * B. bi_span_step against bi_add + bi_mul_u64 with P longer than Q, equal, shorter (and k = 0).
 * C. the pipeline's span (binsplit.c, through binsplit_span) with the switch off and on, for spans [a, a + S) with S in
 *    {1, 2, 64, 168, 236, 256, 512} and a drawn from every k range: small, just below 2^33, across 2^33, 10^10 (the 10^11
 *    run's top), 3.5 x 10^12 (the 576-node target's top node), 4 x 10^12, up to 2^44; plus a GMP recurrence on a subset.
 * D. timing (measured): ns per term, switch off / on, all threads, per k range (S = 256).
 * Exit 0 only if every comparison matched.
 */
#include "harness.h"
#include <omp.h>
#ifdef __cplusplus
extern "C"
#endif
void binsplit_span(bigint *P, bigint *Q, unsigned long a, unsigned long b);   /* binsplit.c (Phase 15 S1) */

#define B BI_B10
static long mism = 0;

static uint64_t mul1(int fast, uint64_t *r, const uint64_t *a, size_t n, uint64_t m, uint64_t add)
{
    bi_mul1_fast = fast; uint64_t c = limb_mul_1(r, a, n, m, add); bi_mul1_fast = 0; return c;
}
static uint64_t rnd_range(rng_t *r, uint64_t lo, uint64_t hi) { return lo + rng_next(r) % (hi - lo); }

static void test_mul1(rng_t *rg)
{
    const uint64_t R[][2] = { {1, 1ULL << 20}, {1ULL << 20, 1ULL << 33}, {1ULL << 33, 1ULL << 44}, {1ULL << 44, 1ULL << 50}, {1ULL << 50, B} };
    const uint64_t edge[] = { 0, 1, 2, 10, (1ULL << 33) - 1, 1ULL << 33, (1ULL << 33) + 1, 3500000000000ULL, 4000000000000ULL, (1ULL << 44) - 1,
                              1ULL << 44, 100000000000000000ULL, B / 2, B - 2, B - 1 };
    const size_t lens[] = { 1, 2, 3, 17, 64, 256, 287, 300, 1000 };
    size_t cap = 1001; uint64_t *a = (uint64_t *)malloc(cap * 8), *r0 = (uint64_t *)malloc(cap * 8), *r1 = (uint64_t *)malloc(cap * 8);
    long checks = 0, gmp_checks = 0; mpz_t za, zr, zb;
    mpz_init(za); mpz_init(zr); mpz_init(zb);
    int nr = sizeof R / sizeof *R, ne = sizeof edge / sizeof *edge;
    for (int ri = 0; ri < nr + ne; ri++)
        for (int it = 0; it < (ri < nr ? 60 : 6); it++)
            for (size_t li = 0; li < sizeof lens / sizeof *lens; li++)
                for (int kind = 0; kind < 3; kind++) {
                    int gk = kind == 0 ? GEN_UNIFORM : kind == 1 ? GEN_ONES : GEN_SPARSE;
                    size_t n = lens[li]; gen_limbs(a, n, gk, rg);
                    uint64_t m = ri < nr ? rnd_range(rg, R[ri][0], R[ri][1]) : edge[ri - nr];
                    uint64_t adds[5] = { 0, 1, rng_next(rg) % B, B - 1, B }, add = adds[rng_next(rg) % 5];
                    uint64_t c0 = mul1(0, r0, a, n, m, add), c1 = mul1(1, r1, a, n, m, add);
                    checks++;
                    if (c0 != c1 || memcmp(r0, r1, n * 8)) { mism++; if (mism < 10) printf("  mul_1 MISMATCH n %zu m %llu add %llu\n", n, (unsigned long long)m, (unsigned long long)add); }
                    if ((it & 7) == 0) {                                     /* GMP: a m + add */
                        mpz_from_limbs(za, a, n); mpz_mul_ui(zr, za, m); mpz_add_ui(zr, zr, add);
                        r1[n] = c1; size_t nn = n + 1;
                        mpz_from_limbs(zb, r1, nn); gmp_checks++;
                        if (mpz_cmp(zb, zr)) { mism++; if (mism < 10) printf("  mul_1 GMP MISMATCH n %zu m %llu\n", n, (unsigned long long)m); }
                    }
                }
    free(a); free(r0); free(r1);
    /* the parallel path (chunks of 2^20 limbs above 2^22): one per range */
    size_t N = ((size_t)1 << 22) + 5; a = (uint64_t *)malloc(N * 8); r0 = (uint64_t *)malloc(N * 8); r1 = (uint64_t *)malloc(N * 8);
    gen_limbs(a, N, GEN_UNIFORM, rg);
    for (int ri = 0; ri < nr; ri++) {
        uint64_t m = rnd_range(rg, R[ri][0], R[ri][1]);
        uint64_t c0 = mul1(0, r0, a, N, m, 7), c1 = mul1(1, r1, a, N, m, 7); checks++;
        if (c0 != c1 || memcmp(r0, r1, N * 8)) { mism++; printf("  mul_1 MISMATCH (parallel) m %llu\n", (unsigned long long)m); }
    }
    for (size_t i = 0; i < N; i++) a[i] = B - 1;
    { uint64_t c0 = mul1(0, r0, a, N, B - 1, B), c1 = mul1(1, r1, a, N, B - 1, B); checks++;
      if (c0 != c1 || memcmp(r0, r1, N * 8)) { mism++; printf("  mul_1 MISMATCH (parallel, all B-1)\n"); } }
    free(a); free(r0); free(r1); mpz_clear(za); mpz_clear(zr); mpz_clear(zb);
    printf("A. limb_mul_1: %ld comparisons fast vs serial (+ %ld against GMP), mismatches so far %ld\n", checks, gmp_checks, mism);
    VERIFY(mism == 0, "mul_1 mismatches %ld", mism);
}

static void test_step(rng_t *rg)
{
    long checks = 0, m0 = mism;
    bigint P0, Q0, P1, Q1; bi_init(&P0); bi_init(&Q0); bi_init(&P1); bi_init(&Q1);
    for (int it = 0; it < 3000; it++) {
        size_t qn = 1 + rng_next(rg) % 300, pn;
        int shape = it % 4; pn = shape == 0 ? qn : shape == 1 ? (qn > 1 ? qn - 1 - rng_next(rg) % (qn - 1) : 1) : shape == 2 ? qn + 1 + rng_next(rg) % 5 : 1;
        uint64_t k = it % 50 == 0 ? 0 : it % 3 == 0 ? rnd_range(rg, 1, 1ULL << 33) : it % 3 == 1 ? rnd_range(rg, 1ULL << 33, 1ULL << 44) : rnd_range(rg, 1ULL << 44, B);
        bi_random(&P0, pn, it & 1 ? GEN_UNIFORM : GEN_ONES, rg); bi_random(&Q0, qn, it & 2 ? GEN_UNIFORM : GEN_ONES, rg);
        bi_copy(&P1, &P0); bi_copy(&Q1, &Q0);
        bi_mul1_fast = 0; bi_add(&P0, &P0, &Q0); bi_mul_u64(&Q0, &Q0, k);
        bi_mul1_fast = 1; bi_span_step(&P1, &Q1, k); bi_mul1_fast = 0;
        checks++;
        if (bi_cmp(&P0, &P1) || bi_cmp(&Q0, &Q1)) { mism++; if (mism < 10) printf("  span_step MISMATCH pn %zu qn %zu k %llu\n", pn, qn, (unsigned long long)k); }
    }
    bi_free(&P0); bi_free(&Q0); bi_free(&P1); bi_free(&Q1);
    printf("B. bi_span_step: %ld comparisons against bi_add + bi_mul_u64, mismatches %ld\n", checks, mism - m0);
    VERIFY(mism == m0, "span_step mismatches %ld", mism - m0);
}

static const struct { const char *name; unsigned long lo, hi; } KR[] = {
    { "a < 10^4 (a = 0, 1 included)", 0, 10000 },
    { "4 x 10^9 (< 2^33: the serial Barrett)", 3900000000UL, 4100000000UL },
    { "across 2^33", (1UL << 33) - 600, (1UL << 33) + 100 },
    { "10^10 (> 2^33: the 10^11 run's top)", 9000000000UL, 11000000000UL },
    { "3.5 x 10^12 (the target's top node)", 3400000000000UL, 3600000000000UL },
    { "4 x 10^12", 3900000000000UL, 4100000000000UL },
    { "up to 2^44", (1UL << 44) - 100000000UL, (1UL << 44) - 600 },
};
#define NKR (int)(sizeof KR / sizeof *KR)

static void test_span(rng_t *rg, long per)
{
    const unsigned long Ss[] = { 1, 2, 64, 168, 236, 256, 512 };
    long m0 = mism, checks = 0, gmp_checks = 0;
    for (int ki = 0; ki < NKR; ki++) {
        long bad = 0;
        uint64_t seed = rng_next(rg);
        /* the switch is a process-wide flag: every span computed off and stored, then on and compared (two passes) */
        long ns = per * (long)(sizeof Ss / sizeof *Ss);
        unsigned long *as = (unsigned long *)malloc(ns * sizeof *as), *bs = (unsigned long *)malloc(ns * sizeof *bs);
        bigint *P = (bigint *)calloc(ns, sizeof *P), *Q = (bigint *)calloc(ns, sizeof *Q);
        rng_t r = { seed };
        for (long i = 0; i < ns; i++) {
            unsigned long S = Ss[i % (sizeof Ss / sizeof *Ss)];
            as[i] = ki == 0 && i < 14 ? (unsigned long)(i % 2) : KR[ki].lo + rng_next(&r) % (KR[ki].hi - KR[ki].lo); bs[i] = as[i] + S;
        }
        bi_mul1_fast = 0;
#pragma omp parallel for schedule(dynamic, 4)
        for (long i = 0; i < ns; i++) { bi_init(&P[i]); bi_init(&Q[i]); binsplit_span(&P[i], &Q[i], as[i], bs[i]); }
        bi_mul1_fast = 1;
#pragma omp parallel reduction(+:bad)
        {
            bigint p, q; bi_init(&p); bi_init(&q);
#pragma omp for schedule(dynamic, 4)
            for (long i = 0; i < ns; i++) { binsplit_span(&p, &q, as[i], bs[i]); if (bi_cmp(&p, &P[i]) || bi_cmp(&q, &Q[i])) bad++; }
            bi_free(&p); bi_free(&q);
        }
        bi_mul1_fast = 0;
        /* GMP: the recurrence P = 1, Q = b - 1; P += Q, Q *= k, on every 50th span */
        long gbad = 0, gn = 0;
        mpz_t zp, zq; mpz_init(zp); mpz_init(zq);
        for (long i = 0; i < ns; i += 50) {
            mpz_set_ui(zp, 1); mpz_set_ui(zq, bs[i] - 1);
            for (unsigned long k = bs[i] - 1; k-- > as[i];) { mpz_add(zp, zp, zq); mpz_mul_ui(zq, zq, k); }
            gn++; if (!bi_eq_mpz(&P[i], zp) || !bi_eq_mpz(&Q[i], zq)) gbad++;
        }
        mpz_clear(zp); mpz_clear(zq);
        for (long i = 0; i < ns; i++) { bi_free(&P[i]); bi_free(&Q[i]); }
        free(P); free(Q); free(as); free(bs);
        printf("C. span, k %-40s %6ld spans off vs on: %ld mismatches; %ld against GMP: %ld mismatches\n", KR[ki].name, ns, bad, gn, gbad);
        mism += bad + gbad; checks += ns; gmp_checks += gn;
    }
    printf("C. spans: %ld comparisons (+ %ld against GMP), mismatches %ld\n", checks, gmp_checks, mism - m0);
    VERIFY(mism == m0, "span mismatches %ld", mism - m0);
}

static void bench(long nspan)
{
    printf("D. timing (measured, %d threads, S = 256, %ld spans per range): ns per term, all threads together\n", omp_get_max_threads(), nspan);
    const unsigned long S = 256;
    for (int ki = 1; ki < NKR; ki++) {
        double t[2];
        for (int f = 0; f < 2; f++) {
            bi_mul1_fast = f;
            double t0 = now();
#pragma omp parallel
            {
                bigint p, q; bi_init(&p); bi_init(&q);
#pragma omp for schedule(dynamic, 16)
                for (long i = 0; i < nspan; i++) { unsigned long a = KR[ki].lo + (unsigned long)i * S % (KR[ki].hi - KR[ki].lo - S); binsplit_span(&p, &q, a, a + S); }
                bi_free(&p); bi_free(&q);
            }
            t[f] = now() - t0;
        }
        bi_mul1_fast = 0;
        printf("   k %-40s off %.3f ns/term  on %.3f ns/term  x%.2f\n", KR[ki].name, 1e9 * t[0] / (nspan * S), 1e9 * t[1] / (nspan * S), t[0] / t[1]);
    }
}

int main(int argc, char **argv)
{
    long per = argc > 1 ? atol(argv[1]) : 400;
    long nb = argc > 2 ? atol(argv[2]) : 100000;
    bi_set_decimal(1); bi_mul1_fast = 0;
    harness_meta("t_seed");
    rng_t rg = { 20260926 };
    double t0 = now();
    test_mul1(&rg);
    test_step(&rg);
    test_span(&rg, per);
    printf("tests: %.1f s\n", now() - t0);
    if (nb > 0) bench(nb);
    return verify_done("t_seed");
}

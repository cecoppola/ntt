/* t_ntt2 - engine 2 (two 62-bit primes, Montgomery, 45-bit points) against
 * an O(n^2) DFT, round trips to 2^LOGMAX, the 45-bit load, an integer
 * product with 45-bit points vs mpz_mul, and rates.
 * Usage: t_ntt2 [LOGMAX (31)] */
#include "harness.h"
#include "../ntt2.h"
#include <omp.h>
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
static int brv(size_t i, int bits) { size_t r = 0; for (int b = 0; b < bits; b++) r = (r << 1) | ((i >> b) & 1); return (int)r; }
static size_t count_diff(const uint64_t *a, const uint64_t *b, size_t n, size_t *first)
{ size_t bad = 0; *first = n; for (size_t i = 0; i < n; i++) if (a[i] != b[i]) { if (!bad) *first = i; bad++; } return bad; }

int main(int argc, char **argv)
{
    int LOGMAX = argc > 1 ? atoi(argv[1]) : 31;
    int nd = 0, pr, logn;
    HIP_CHECK(hipGetDeviceCount(&nd));
    printf("== t_ntt2: LOGMAX %d, body %d ==\n", LOGMAX, ntt2_b16_body);
    harness_meta("t_ntt2");
    rng_t rng = {0x7E572ULL};
    HIP_CHECK(hipSetDevice(0));
    ntt2_ctx *ctx[E2_NP]; e2_mod md[E2_NP];
    for (pr = 0; pr < E2_NP; pr++) { ctx[pr] = ntt2_ctx_create(pr); md[pr] = e2_mod_get(pr); }
    size_t nmax = (size_t)1 << LOGMAX;
    uint64_t *hx = (uint64_t *)malloc(nmax * 8), *hy = (uint64_t *)malloc(nmax * 8), *hz = (uint64_t *)malloc(nmax * 8), *dx, *dy;
    HIP_CHECK(hipMalloc(&dx, nmax * 8)); HIP_CHECK(hipMalloc(&dy, nmax * 8));

    /* 0. arithmetic: Montgomery product vs 128-bit on random lazy operands */
    printf("-- 0. e2_mm\n");
    for (pr = 0; pr < E2_NP; pr++) {
        size_t bad = 0, nl = 0; uint64_t p = e2_P[pr];
        for (int it = 0; it < 2000000; it++) {
            uint64_t a = rng_next(&rng) % (2 * p), b = rng_next(&rng) % (2 * p);
            uint64_t r = e2_mm(a, b, md[pr]);
            nl += r >= 2 * p;
            /* r = a b R^-1 mod p */
            uint64_t want = e2_mulmod_ref(e2_mulmod_ref(a % p, b % p, p), e2_inv(md[pr].one, p), p);
            bad += e2_fold(r, p) != want;
        }
        VERIFY(bad == 0 && nl == 0, "P%d e2_mm: %zu wrong, %zu not lazy", pr, bad, nl);
        VERIFY(e2_from_mont(e2_to_mont(12345, md[pr]), md[pr]) == 12345, "mont round trip");
    }
    /* 1. DFT: values in Montgomery form in, out compared after conversion */
    printf("-- 1. fwd vs O(n^2) DFT\n");
    for (pr = 0; pr < E2_NP; pr++) {
        static const int ls[] = {10, 11, 13};
        for (int li = 0; li < 3; li++) {
            logn = ls[li]; size_t n = (size_t)1 << logn; uint64_t p = e2_P[pr], wn = e2_root(pr, logn);
            for (size_t i = 0; i < n; i++) hx[i] = rng_next(&rng) % p;                 /* plain */
            for (size_t i = 0; i < n; i++) hz[i] = e2_fold(e2_to_mont(hx[i], md[pr]), p);
            HIP_CHECK(hipMemcpy(dx, hz, n * 8, hipMemcpyHostToDevice));
            ntt2_fwd(ctx[pr], dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            size_t bad = 0;
#pragma omp parallel for reduction(+:bad)
            for (size_t k = 0; k < n; k++) {
                uint64_t wk = e2_powmod(wn, k, p), acc = 0, wj = 1;
                for (size_t j = 0; j < n; j++) { acc = (acc + e2_mulmod_ref(hx[j], wj, p)) % p; wj = e2_mulmod_ref(wj, wk, p); }
                bad += e2_from_mont(hy[brv(k, logn)], md[pr]) != acc;
            }
            VERIFY(bad == 0, "P%d 2^%d: %zu mismatches vs DFT", pr, logn, bad);
        }
    }
    /* 2. round trips (lazy Montgomery values in, canonical plain out via the scale fusion: compare plain) */
    printf("-- 2. round trips\n");
    for (pr = 0; pr < E2_NP; pr++) for (logn = 10; logn <= LOGMAX; logn++) {
        size_t n = (size_t)1 << logn; uint64_t p = e2_P[pr];
        gen_limbs(hx, n, logn <= 20 ? (int)(logn % GEN_KINDS) : GEN_UNIFORM, &rng);
#pragma omp parallel for
        for (size_t i = 0; i < n; i++) { hx[i] %= p; hz[i] = e2_fold(e2_to_mont(hx[i], md[pr]), p); }
        HIP_CHECK(hipMemcpy(dx, hz, n * 8, hipMemcpyHostToDevice));
        ntt2_fwd(ctx[pr], dx, logn, 1, 0); ntt2_inv(ctx[pr], dx, logn, 1, 0);
        HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
        size_t first, bad = count_diff(hx, hy, n, &first);
        VERIFY(bad == 0, "P%d 2^%d round trip: %zu mismatches (first %zu: %llu vs %llu)", pr, logn, bad, first,
               first < n ? (unsigned long long)hy[first] : 0ULL, first < n ? (unsigned long long)hx[first] : 0ULL);
    }
    /* 3. integer product with 45-bit points: ntt2_load from limbs, both primes, CRT on the host, vs mpz_mul */
    printf("-- 3. product via 45-bit points and 2-prime CRT vs mpz_mul\n");
    {
        mpz_t A, B, C, D, t, m1, m2; mpz_inits(A, B, C, D, t, m1, m2, NULL);
        for (logn = 14; logn <= 22; logn += 4) {
            size_t n = (size_t)1 << logn, half_pts = n / 2, nl = half_pts * E2_BITS / 64 - 1;   /* limbs per operand so points fit n/2 */
            bigint a, b; bi_init(&a); bi_init(&b);
            bi_random(&a, nl, GEN_UNIFORM, &rng); bi_random(&b, nl, GEN_ONES, &rng);
            uint64_t *da_, *db_; HIP_CHECK(hipMalloc(&da_, n * 8)); HIP_CHECK(hipMalloc(&db_, n * 8));
            uint64_t *res[2] = { (uint64_t *)malloc(n * 8), (uint64_t *)malloc(n * 8) };
            uint64_t *ha, *hb;
            HIP_CHECK(hipHostMalloc(&ha, nl * 8, 0)); HIP_CHECK(hipHostMalloc(&hb, nl * 8, 0));
            memcpy(ha, a.l, nl * 8); memcpy(hb, b.l, nl * 8);
            for (pr = 0; pr < E2_NP; pr++) {
                ntt2_load(ctx[pr], da_, ha, nl, n, 0); ntt2_load(ctx[pr], db_, hb, nl, n, 0);
                ntt2_fwd(ctx[pr], da_, logn, 1, 0); ntt2_fwd(ctx[pr], db_, logn, 1, 0);
                ntt2_inv_pw(ctx[pr], da_, db_, logn, 1, 0);
                HIP_CHECK(hipMemcpy(res[pr], da_, n * 8, hipMemcpyDeviceToHost));
            }
            /* CRT: c = r0 + P0 ((r1 - r0) P0^-1 mod P1); value = sum c_i 2^(45 i) */
            uint64_t inv01 = e2_inv(e2_P[0] % e2_P[1], e2_P[1]);
            mpz_set_ui(C, 0); mpz_set_ui(m1, e2_P[0]);
            for (size_t i = n; i-- > 0;) {
                uint64_t r0 = res[0][i], r1 = res[1][i];
                uint64_t d = e2_mulmod_ref((r1 + e2_P[1] - r0 % e2_P[1]) % e2_P[1], inv01, e2_P[1]);
                mpz_set_ui(t, d); mpz_mul(t, t, m1); mpz_add_ui(t, t, r0);
                mpz_mul_2exp(C, C, E2_BITS); mpz_add(C, C, t);
            }
            bi_to_mpz(A, &a); bi_to_mpz(B, &b); mpz_mul(D, A, B);
            VERIFY(mpz_cmp(C, D) == 0, "2^%d points: product != mpz_mul", logn);
            printf("   2^%d points, %zu-limb operands: %s\n", logn, nl, mpz_cmp(C, D) == 0 ? "ok" : "MISMATCH");
            HIP_CHECK(hipFree(da_)); HIP_CHECK(hipFree(db_)); HIP_CHECK(hipHostFree(ha)); HIP_CHECK(hipHostFree(hb)); free(res[0]); free(res[1]); bi_free(&a); bi_free(&b);
        }
        mpz_clears(A, B, C, D, t, m1, m2, NULL);
    }
    /* 4. rates at 2^LOGMAX, APU0, both bodies */
    printf("-- 4. rates (P0)\n");
    for (int body = 0; body < 2; body++) {
        ntt2_b16_body = body;
        ntt2_ctx *c = ntt2_ctx_create(0);
        HIP_CHECK(hipMemset(dx, 0, nmax * 8));
        hipEvent_t e0, e1; HIP_CHECK(hipEventCreate(&e0)); HIP_CHECK(hipEventCreate(&e1)); float ms_f, ms_i;
        ntt2_fwd(c, dx, LOGMAX, 1, 0); ntt2_inv(c, dx, LOGMAX, 1, 0); HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipEventRecord(e0, 0)); ntt2_fwd(c, dx, LOGMAX, 1, 0); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms_f, e0, e1));
        HIP_CHECK(hipEventRecord(e0, 0)); ntt2_inv(c, dx, LOGMAX, 1, 0); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms_i, e0, e1));
        int np = ntt2_npass(LOGMAX);
        printf("   body %d 2^%d: fwd %.1f ms (%.2f TB/s), inv %.1f ms (%.2f TB/s)\n", body, LOGMAX, ms_f, 16.0 * nmax * np / (ms_f * 1e-3) / 1e12, ms_i, 16.0 * nmax * np / (ms_i * 1e-3) / 1e12);
        char nm[40]; snprintf(nm, sizeof nm, "fwd_TBps_body%d", body); harness_result(nm, "TB/s", 16.0 * nmax * np / (ms_f * 1e-3) / 1e12);
        ntt2_ctx_free(c);
    }
    return verify_done("t_ntt2");
}

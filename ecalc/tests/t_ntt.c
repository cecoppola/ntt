/* t_ntt - step 2 test: ntt.h against an O(n^2) DFT, round trips, convolution
 * against schoolbook and mpz_mul, pass-split independence, and rates.
 *
 *  1. n = 2^10, 2^11, 2^13: ntt_fwd vs the O(n^2) DFT in 128-bit arithmetic,
 *     in bit-reversed order; ntt_host_fwd vs the same
 *  2. round trip inv(fwd(x)) == x for logn = 10 .. LOGMAX (default 31) on
 *     APU0, uniform inputs; all five generators up to 2^24; every prime
 *  3. convolution: (a) cyclic convolution mod p vs schoolbook at 2^12;
 *     (b) integer product with 16-bit limbs vs mpz_mul at logn 16 .. 20,
 *     fused and unfused pointwise, every prime
 *  4. forward output and round trip bit-identical for NTT_B16_STG = 3 .. 7
 *     at 2^24 (STG 8 would need a 256-row tile: LDS 34 KB, 1 block/CU - not
 *     built, RESULTS.md 33)
 *  5. rate: ms per pass at 2^LOGMAX on all APUs (paper 1.08 TB/s effective =
 *     16 B x n per pass; bench/16: 1.17); batched log L = 14, 17
 *
 * Usage: t_ntt [LOGMAX (31)]
 */
#include "harness.h"
#include "../ntt.h"
#include <omp.h>

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

static int brv(size_t i, int bits) { size_t r = 0; for (int b = 0; b < bits; b++) r = (r << 1) | ((i >> b) & 1); return (int)r; }

static uint64_t hash_arr(const uint64_t *a, size_t n)
{
    uint64_t h = 0x243F6A8885A308D3ULL;
    for (size_t i = 0; i < n; i++) { h ^= a[i]; h *= 0x9E3779B97F4A7C15ULL; h ^= h >> 29; }
    return h;
}
static size_t count_diff(const uint64_t *a, const uint64_t *b, size_t n, size_t *first)
{
    size_t bad = 0; *first = n;
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) { if (!bad) *first = i; bad++; }
    return bad;
}

int main(int argc, char **argv)
{
    int LOGMAX = argc > 1 ? atoi(argv[1]) : 31;
    int nd = 0, pr, logn;
    HIP_CHECK(hipGetDeviceCount(&nd));
    printf("== t_ntt: LOGMAX %d, %d devices, NTT_B16_STG %d ==\n", LOGMAX, nd, ntt_stg);
    harness_meta("t_ntt");
    rng_t rng = {0x7E57ULL};

    HIP_CHECK(hipSetDevice(0));
    ntt_ctx *ctx[EC_NP];
    for (pr = 0; pr < EC_NP; pr++) ctx[pr] = ntt_ctx_create(pr);

    size_t nmax = (size_t)1 << LOGMAX;
    uint64_t *hx = (uint64_t *)malloc(nmax * 8), *hy = (uint64_t *)malloc(nmax * 8), *hz = (uint64_t *)malloc(nmax * 8);
    uint64_t *dx, *dy;
    HIP_CHECK(hipMalloc(&dx, nmax * 8)); HIP_CHECK(hipMalloc(&dy, nmax * 8));

    /* 1. O(n^2) DFT */
    printf("-- 1. fwd vs O(n^2) DFT\n");
    for (pr = 0; pr < EC_NP; pr++) {
        static const int ls[] = {10, 11, 13};
        for (int li = 0; li < 3; li++) {
            logn = ls[li];
            size_t n = (size_t)1 << logn;
            uint64_t p = ec_P[pr], wn = ec_root(pr, logn);
            for (size_t i = 0; i < n; i++) hx[i] = rng_next(&rng) % p;
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            ntt_fwd(ctx[pr], dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            memcpy(hz, hx, n * 8); ntt_host_fwd(hz, logn, pr);
            size_t bad_dev = 0, bad_host = 0;
#pragma omp parallel for reduction(+:bad_dev,bad_host)
            for (size_t k = 0; k < n; k++) {
                uint64_t wk = ec_powmod(wn, k, p), acc = 0, wj = 1;
                for (size_t j = 0; j < n; j++) { acc = (acc + ec_mulmod_ref(hx[j], wj, p)) % p; wj = ec_mulmod_ref(wj, wk, p); }
                bad_dev  += hy[brv(k, logn)] != acc;
                bad_host += hz[brv(k, logn)] != acc;
            }
            VERIFY(bad_dev == 0, "P%d 2^%d: device fwd %zu mismatches vs DFT", pr, logn, bad_dev);
            VERIFY(bad_host == 0, "P%d 2^%d: host fwd %zu mismatches vs DFT", pr, logn, bad_host);
        }
    }

    /* 2. round trips */
    printf("-- 2. round trips\n");
    for (pr = 0; pr < EC_NP; pr++) {
        for (logn = 10; logn <= LOGMAX; logn++) {
            size_t n = (size_t)1 << logn;
            int kinds = logn <= 24 ? GEN_KINDS : 1;
            for (int kind = 0; kind < kinds; kind++) {
                gen_limbs(hx, n, kind, &rng);
#pragma omp parallel for
                for (size_t i = 0; i < n; i++) hx[i] %= ec_P[pr];
                HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
                ntt_fwd(ctx[pr], dx, logn, 1, 0);
                ntt_inv(ctx[pr], dx, logn, 1, 0);
                HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
                size_t first, bad = count_diff(hx, hy, n, &first);
                VERIFY(bad == 0, "P%d 2^%d %s: %zu mismatches, first at %zu (%llu vs %llu)", pr, logn, gen_name[kind],
                       bad, first, first < n ? (unsigned long long)hy[first] : 0ULL, first < n ? (unsigned long long)hx[first] : 0ULL);
                if (logn <= 20 && kind == 0) {          /* inverse alone vs host inverse */
                    HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
                    ntt_inv(ctx[pr], dx, logn, 1, 0);
                    HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
                    memcpy(hz, hx, n * 8); ntt_host_inv(hz, logn, pr);
                    bad = count_diff(hz, hy, n, &first);
                    VERIFY(bad == 0, "P%d 2^%d: device inv vs host inv %zu mismatches", pr, logn, bad);
                }
            }
            if (pr == 0 && logn >= 28) printf("   2^%d ok\n", logn);
        }
    }
    /* batched round trip: 64 transforms of 2^14 in one call */
    {
        size_t n = (size_t)1 << 14, B = 64;
        gen_limbs(hx, n * B, GEN_UNIFORM, &rng);
#pragma omp parallel for
        for (size_t i = 0; i < n * B; i++) hx[i] %= ec_P[1];
        HIP_CHECK(hipMemcpy(dx, hx, n * B * 8, hipMemcpyHostToDevice));
        ntt_fwd(ctx[1], dx, 14, B, 0); ntt_inv(ctx[1], dx, 14, B, 0);
        HIP_CHECK(hipMemcpy(hy, dx, n * B * 8, hipMemcpyDeviceToHost));
        size_t first, bad = count_diff(hx, hy, n * B, &first);
        VERIFY(bad == 0, "batched 64 x 2^14 round trip: %zu mismatches", bad);
    }

    /* 3a. cyclic convolution vs schoolbook mod p */
    printf("-- 3. convolution\n");
    for (pr = 0; pr < EC_NP; pr++) {
        logn = 12; size_t n = (size_t)1 << logn; uint64_t p = ec_P[pr];
        for (size_t i = 0; i < n; i++) { hx[i] = rng_next(&rng) % p; hy[i] = rng_next(&rng) % p; }
        HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dy, hy, n * 8, hipMemcpyHostToDevice));
        ntt_fwd(ctx[pr], dx, logn, 1, 0); ntt_fwd(ctx[pr], dy, logn, 1, 0);
        ntt_pw(ctx[pr], dx, dy, n, 0); ntt_inv(ctx[pr], dx, logn, 1, 0);
        HIP_CHECK(hipMemcpy(hz, dx, n * 8, hipMemcpyDeviceToHost));
        size_t bad = 0;
#pragma omp parallel for reduction(+:bad)
        for (size_t k = 0; k < n; k++) {
            uint64_t acc = 0;
            for (size_t j = 0; j < n; j++) acc = (acc + ec_mulmod_ref(hx[j], hy[(k - j) & (n - 1)], p)) % p;
            bad += acc != hz[k];
        }
        VERIFY(bad == 0, "P%d cyclic convolution 2^12: %zu mismatches", pr, bad);
    }
    /* 3b. integer product, 16-bit limbs, vs mpz_mul */
    {
        mpz_t A, B, C, D;
        mpz_inits(A, B, C, D, NULL);
        for (pr = 0; pr < EC_NP; pr++)
        for (logn = 16; logn <= 20; logn++)
        for (int fuse = 0; fuse < 2; fuse++) {
            size_t n = (size_t)1 << logn, half = n / 2;
            uint16_t *a16 = (uint16_t *)malloc(half * 2), *b16 = (uint16_t *)malloc(half * 2);
            for (size_t i = 0; i < half; i++) { a16[i] = (uint16_t)rng_next(&rng); b16[i] = (uint16_t)rng_next(&rng); }
            a16[half - 1] |= 0x8000; b16[half - 1] |= 0x8000;
            for (size_t i = 0; i < n; i++) { hx[i] = i < half ? a16[i] : 0; hy[i] = i < half ? b16[i] : 0; }
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            HIP_CHECK(hipMemcpy(dy, hy, n * 8, hipMemcpyHostToDevice));
            ntt_fwd(ctx[pr], dx, logn, 1, 0); ntt_fwd(ctx[pr], dy, logn, 1, 0);
            if (fuse) { int save = ntt_pw_fuse; ntt_pw_fuse = 10; ntt_inv_pw(ctx[pr], dx, dy, logn, 1, 0); ntt_pw_fuse = save; }
            else      { ntt_pw(ctx[pr], dx, dy, n, 0); ntt_inv(ctx[pr], dx, logn, 1, 0); }
            HIP_CHECK(hipMemcpy(hz, dx, n * 8, hipMemcpyDeviceToHost));
            /* coefficients < 2^32 * 2^(logn-1) < p: carry into a GMP integer */
            mpz_set_ui(C, 0);
            for (size_t i = n; i-- > 0;) { mpz_mul_2exp(C, C, 16); mpz_add_ui(C, C, hz[i]); }
            mpz_import(A, half, -1, 2, 0, 0, a16); mpz_import(B, half, -1, 2, 0, 0, b16);
            mpz_mul(D, A, B);
            VERIFY(mpz_cmp(C, D) == 0, "P%d 2^%d %s: product != mpz_mul", pr, logn, fuse ? "fused" : "unfused");
            free(a16); free(b16);
        }
        mpz_clears(A, B, C, D, NULL);
    }

    /* 4. STG independence at 2^24 */
    printf("-- 4. pass splits\n");
    {
        logn = 24; size_t n = (size_t)1 << logn; uint64_t href = 0;
        gen_limbs(hx, n, GEN_UNIFORM, &rng);
#pragma omp parallel for
        for (size_t i = 0; i < n; i++) hx[i] %= ec_P[0];
        for (int stg = 7; stg >= 3; stg--) {
            ntt_stg = stg;
            ntt_ctx *c = ntt_ctx_create(0);                 /* fresh twiddle cache for this split */
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            ntt_fwd(c, dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            uint64_t h = hash_arr(hy, n);
            if (stg == 7) href = h;
            VERIFY(h == href, "STG %d forward differs from STG 7", stg);
            ntt_inv(c, dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            size_t first, bad = count_diff(hx, hy, n, &first);
            VERIFY(bad == 0, "STG %d round trip: %zu mismatches", stg, bad);
            printf("   STG %d: %d passes, fwd hash %016llx\n", stg, ntt_npass(logn), (unsigned long long)h);
            ntt_ctx_free(c);
        }
        ntt_stg = 7;
        /* register-blocked body (1) and with radix-4 stages (2): bit-identical forward and exact round trip */
        for (ntt_b16_body = 1; ntt_b16_body <= 2; ntt_b16_body++) {
            ntt_ctx *c = ntt_ctx_create(0);
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            ntt_fwd(c, dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            uint64_t h = hash_arr(hy, n);
            VERIFY(h == href, "register-blocked body: forward differs from the tile kernel");
            ntt_inv(c, dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            size_t first, bad = count_diff(hx, hy, n, &first);
            VERIFY(bad == 0, "register-blocked body: round trip %zu mismatches", bad);
            printf("   body %d: fwd hash %016llx %s\n", ntt_b16_body, (unsigned long long)h, h == href ? "== tile kernel" : "DIFFERS");
            ntt_ctx_free(c);
        }
        ntt_b16_body = 0;
        /* Shoup b1 pass: forward output canonical and identical; round trip exact */
        ntt_b1_shoup = 1;
        {
            ntt_ctx *c = ntt_ctx_create(0);
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            ntt_fwd(c, dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            uint64_t h = hash_arr(hy, n);
            VERIFY(h == href, "Shoup b1: forward differs");
            ntt_inv(c, dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            size_t first, bad = count_diff(hx, hy, n, &first);
            VERIFY(bad == 0, "Shoup b1: round trip %zu mismatches", bad);
            printf("   Shoup b1 pass: fwd hash %016llx %s\n", (unsigned long long)h, h == href ? "== tile kernel" : "DIFFERS");
            ntt_ctx_free(c);
        }
        ntt_b1_shoup = 0;
    }

    /* 5. rates on every device */
    for (int body = 0; body < 4; body++) {
    ntt_b16_body = body == 3 ? 1 : body; ntt_b1_shoup = body == 3;
    printf("-- 5. rates (P0, STG 7, body %d = %s)\n", body, body == 3 ? "register-blocked + Shoup b1" : body == 2 ? "register-blocked + radix-4" : body ? "register-blocked" : "tile kernel");
    {
        double sum_fwd = 0, sum_inv = 0;
        int np = ntt_npass(LOGMAX);
        for (int d = 0; d < nd; d++) {
            HIP_CHECK(hipSetDevice(d));
            ntt_ctx *c = ntt_ctx_create(0);
            uint64_t *ddx; HIP_CHECK(hipMalloc(&ddx, nmax * 8));
            HIP_CHECK(hipMemset(ddx, 0, nmax * 8));
            hipEvent_t e0, e1; HIP_CHECK(hipEventCreate(&e0)); HIP_CHECK(hipEventCreate(&e1));
            float ms_f, ms_i;
            ntt_fwd(c, ddx, LOGMAX, 1, 0); ntt_inv(c, ddx, LOGMAX, 1, 0);      /* warm: tables */
            HIP_CHECK(hipDeviceSynchronize());
            HIP_CHECK(hipEventRecord(e0, 0)); ntt_fwd(c, ddx, LOGMAX, 1, 0); HIP_CHECK(hipEventRecord(e1, 0));
            HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms_f, e0, e1));
            HIP_CHECK(hipEventRecord(e0, 0)); ntt_inv(c, ddx, LOGMAX, 1, 0); HIP_CHECK(hipEventRecord(e1, 0));
            HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms_i, e0, e1));
            double tb_f = 16.0 * nmax * np / (ms_f * 1e-3) / 1e12, tb_i = 16.0 * nmax * np / (ms_i * 1e-3) / 1e12;
            printf("   APU%d 2^%d: fwd %.1f ms (%.2f TB/s), inv %.1f ms (%.2f TB/s), %d passes\n", d, LOGMAX, ms_f, tb_f, ms_i, tb_i, np);
            sum_fwd += tb_f; sum_inv += tb_i;
            /* batched */
            for (int lgl = 14; lgl <= 17; lgl += 3) {
                size_t B = nmax >> lgl; int npb = ntt_npass(lgl);
                HIP_CHECK(hipEventRecord(e0, 0)); ntt_fwd(c, ddx, lgl, B, 0); HIP_CHECK(hipEventRecord(e1, 0));
                HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms_f, e0, e1));
                double gb = 16.0 * nmax * npb / (ms_f * 1e-3) / 1e9;
                printf("      batched log L = %d: %zu transforms, %.1f ms, %.0f GB/s\n", lgl, B, ms_f, gb);
                if (d == 0) { char nm[32]; snprintf(nm, sizeof nm, "batched_logL%d", lgl); harness_result(nm, "GB/s", gb); }
            }
            HIP_CHECK(hipFree(ddx)); ntt_ctx_free(c);
        }
        char nm[40]; snprintf(nm, sizeof nm, "fwd_TBps_per_apu_body%d", body); harness_result(nm, "TB/s", sum_fwd / nd);
        snprintf(nm, sizeof nm, "inv_TBps_per_apu_body%d", body); harness_result(nm, "TB/s", sum_inv / nd);
        VERIFY(sum_fwd / nd > 0.9, "forward rate %.2f TB/s below 0.9 (bench/16: 1.17)", sum_fwd / nd);
    }
    }
    ntt_b16_body = 0; ntt_b1_shoup = 0;
    return verify_done("t_ntt");
}

/* t_ntt3 - the 3 * 2^k transforms (WP8): inverse(forward) == identity, and the
 * cyclic convolution of length 3m equals the linear convolution computed by
 * the 2^(k+2) engine on zero-padded inputs, for every prime and k = 10..logmax-2. */
#include "harness.h"
#include "../ntt.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
static rng_t rg = { 777 };
int main(int argc, char **argv)
{
    int logmax = argc > 1 ? atoi(argv[1]) : 22;
    harness_meta("t_ntt3");
    if (!ec_has_radix3()) { printf("t_ntt3: prime set without 3 2^k roots; skipped\n"); return verify_done("t_ntt3"); }
    for (int prime = 0; prime < EC_NP; prime++) {
        uint64_t p = ec_P[prime]; ntt_ctx *ctx = ntt_ctx_create(prime);
        for (int logk = 10; logk + 2 <= logmax; logk++) {
            size_t m = (size_t)1 << logk, L = 3 * m, L2 = (size_t)1 << (logk + 2);
            uint64_t *hx = (uint64_t *)malloc(L2 * 8), *hy = (uint64_t *)malloc(L2 * 8), *r3 = (uint64_t *)malloc(L * 8), *r2 = (uint64_t *)malloc(L2 * 8);
            size_t na = L / 2 + rng_next(&rg) % (L / 2), nb = L - na;               /* na + nb == L: the linear product fits 3m exactly */
            for (size_t i = 0; i < L2; i++) { hx[i] = i < na ? rng_next(&rg) % p : 0; hy[i] = i < nb ? rng_next(&rg) % p : 0; }
            uint64_t *dx, *dy; HIP_CHECK(hipMalloc(&dx, L2 * 8)); HIP_CHECK(hipMalloc(&dy, L2 * 8));
            /* identity */
            HIP_CHECK(hipMemcpy(dx, hx, L * 8, hipMemcpyHostToDevice));
            ntt_fwd3(ctx, dx, logk, 1, 0); ntt_inv3(ctx, dx, logk, 1, 0);
            HIP_CHECK(hipMemcpy(r3, dx, L * 8, hipMemcpyDeviceToHost));
            size_t bad = 0; for (size_t i = 0; i < L; i++) bad += r3[i] != hx[i];
            VERIFY(bad == 0, "prime %d 3*2^%d inv(fwd) identity: %zu differ", prime, logk, bad);
            /* convolution, batch 2 (both halves the same data) */
            HIP_CHECK(hipMemcpy(dx, hx, L * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dx + L, hx, L * 8, hipMemcpyHostToDevice));
            HIP_CHECK(hipMemcpy(dy, hy, L * 8, hipMemcpyHostToDevice));
            ntt_fwd3(ctx, dx, logk, 2, 0); ntt_fwd3(ctx, dy, logk, 1, 0); ntt_inv3_pw_bcast(ctx, dx, dy, logk, 2, 0);
            HIP_CHECK(hipMemcpy(r3, dx + L, L * 8, hipMemcpyDeviceToHost));
            HIP_CHECK(hipMemcpy(dx, hx, L2 * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dy, hy, L2 * 8, hipMemcpyHostToDevice));
            ntt_fwd(ctx, dx, logk + 2, 1, 0); ntt_fwd(ctx, dy, logk + 2, 1, 0); ntt_pw(ctx, dx, dy, L2, 0); ntt_inv(ctx, dx, logk + 2, 1, 0);
            HIP_CHECK(hipMemcpy(r2, dx, L2 * 8, hipMemcpyDeviceToHost));
            bad = 0; for (size_t i = 0; i < L; i++) bad += r3[i] != r2[i];
            for (size_t i = L; i < L2; i++) bad += r2[i] != 0;
            VERIFY(bad == 0, "prime %d 3*2^%d convolution vs 2^%d: %zu differ", prime, logk, logk + 2, bad);
            HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy)); free(hx); free(hy); free(r3); free(r2);
        }
        ntt_ctx_free(ctx);
    }
    return verify_done("t_ntt3");
}

/* kernel/rowN - the batched transform length sweep the four-step design
 * asked for (R11's N = 512 hypothesis): ecalc's engine at log L = 10, 11, 12,
 * 13, 14 batched over 2^31 points, forward, per APU0; both pass bodies.
 * GB/s = 16 B x points x passes / time. */
#include "common_ntt.h"
#include "../ecalc/ntt.h"
int main(void)
{
    printf("== kernel/rowN ==\n"); meta("kernel/rowN");
    HIP_CHECK(hipSetDevice(0));
    size_t n = (size_t)1 << 31; uint64_t *x; HIP_CHECK(hipMalloc(&x, n * 8)); HIP_CHECK(hipMemset(x, 0, n * 8));
    hipEvent_t e0, e1; timer_events(&e0, &e1); float ms;
    header("batched forward transforms over 2^31 points, APU0");
    printf("%6s %6s %10s %10s %8s\n", "logL", "passes", "tile GB/s", "reg GB/s", "ms(reg)");
    for (int lg = 10; lg <= 14; lg++) {
        double g[2];
        for (int body = 0; body < 2; body++) {
            ntt_b16_body = body;
            ntt_ctx *c = ntt_ctx_create(0); size_t B = n >> lg; int np = ntt_npass(lg);
            ntt_fwd(c, x, lg, B, 0); HIP_CHECK(hipDeviceSynchronize());
            HIP_CHECK(hipEventRecord(e0, 0)); ntt_fwd(c, x, lg, B, 0); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
            HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); g[body] = 16.0 * n * np / (ms * 1e-3) / 1e9;
            ntt_ctx_free(c);
        }
        printf("%6d %6d %10.0f %10.0f %8.1f\n", lg, ntt_npass(lg), g[0], g[1], ms);
        char nm[32]; snprintf(nm, sizeof nm, "batched_logL%d_tile", lg); result(nm, "GB/s", g[0], &g[0], 1);
    }
    printf("(engine passes: b16 tiles only for logL >= 17 with STG 7; below that the pass is 1..4 stages + b1)\n");
    return 0;
}

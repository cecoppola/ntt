/* system/launch - launch latency, event cost, stream concurrency (PLAN.md 11).
 *   launch_us      empty kernel, back-to-back launches, wall per launch (throughput)
 *   launch_rt_us   launch + synchronize round trip
 *   event_us       hipEventRecord + hipEventSynchronize pair
 *   streams        two 100 ms kernels on 1 vs 2 streams: overlap ratio
 *   memcpy_small   1 KiB / 1 MiB D2H hipMemcpy latency
 */
#include "common_ntt.h"
__global__ void k_empty(void) {}
__global__ void k_spin(long long cycles, int *sink) { long long t0 = clock64(); while (clock64() - t0 < cycles) {} if (threadIdx.x == 12345) *sink = 1; }
int main(void)
{
    int nd = device_count();
    printf("== system/launch ==\n"); meta("system/launch");
    for (int d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        int *sink; HIP_CHECK(hipMalloc(&sink, 4));
        hipStream_t s1, s2; HIP_CHECK(hipStreamCreate(&s1)); HIP_CHECK(hipStreamCreate(&s2));
        for (int i = 0; i < 100; i++) k_empty<<<1, 64>>>(); HIP_CHECK(hipDeviceSynchronize());
        struct timespec a, b; double t;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int i = 0; i < 10000; i++) k_empty<<<1, 64>>>();
        HIP_CHECK(hipDeviceSynchronize());
        clock_gettime(CLOCK_MONOTONIC, &b); t = (b.tv_sec - a.tv_sec) + 1e-9 * (b.tv_nsec - a.tv_nsec);
        double launch_us = t / 10000 * 1e6;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int i = 0; i < 2000; i++) { k_empty<<<1, 64>>>(); HIP_CHECK(hipDeviceSynchronize()); }
        clock_gettime(CLOCK_MONOTONIC, &b); t = (b.tv_sec - a.tv_sec) + 1e-9 * (b.tv_nsec - a.tv_nsec);
        double rt_us = t / 2000 * 1e6;
        hipEvent_t e; HIP_CHECK(hipEventCreate(&e));
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int i = 0; i < 2000; i++) { HIP_CHECK(hipEventRecord(e, 0)); HIP_CHECK(hipEventSynchronize(e)); }
        clock_gettime(CLOCK_MONOTONIC, &b); t = (b.tv_sec - a.tv_sec) + 1e-9 * (b.tv_nsec - a.tv_nsec);
        double ev_us = t / 2000 * 1e6;
        /* streams: two 100 ms spins */
        long long cyc = 150000000LL;                  /* ~100 ms at 1.5 GHz */
        clock_gettime(CLOCK_MONOTONIC, &a);
        k_spin<<<1, 64, 0, s1>>>(cyc, sink); k_spin<<<1, 64, 0, s1>>>(cyc, sink); HIP_CHECK(hipDeviceSynchronize());
        clock_gettime(CLOCK_MONOTONIC, &b); double t1 = (b.tv_sec - a.tv_sec) + 1e-9 * (b.tv_nsec - a.tv_nsec);
        clock_gettime(CLOCK_MONOTONIC, &a);
        k_spin<<<1, 64, 0, s1>>>(cyc, sink); k_spin<<<1, 64, 0, s2>>>(cyc, sink); HIP_CHECK(hipDeviceSynchronize());
        clock_gettime(CLOCK_MONOTONIC, &b); double t2 = (b.tv_sec - a.tv_sec) + 1e-9 * (b.tv_nsec - a.tv_nsec);
        /* small memcpy latency */
        char *hb = (char *)malloc(1 << 20), *db; HIP_CHECK(hipMalloc(&db, 1 << 20));
        double mc[2]; size_t sz[2] = {1024, 1 << 20};
        for (int k = 0; k < 2; k++) {
            HIP_CHECK(hipMemcpy(hb, db, sz[k], hipMemcpyDeviceToHost));
            clock_gettime(CLOCK_MONOTONIC, &a);
            for (int i = 0; i < 200; i++) HIP_CHECK(hipMemcpy(hb, db, sz[k], hipMemcpyDeviceToHost));
            clock_gettime(CLOCK_MONOTONIC, &b); mc[k] = ((b.tv_sec - a.tv_sec) + 1e-9 * (b.tv_nsec - a.tv_nsec)) / 200 * 1e6;
        }
        printf("APU%d: launch %.1f us (throughput), launch+sync %.1f us, event pair %.1f us, 2 kernels 1 stream %.0f ms / 2 streams %.0f ms (overlap %.2f), memcpy D2H 1 KiB %.1f us, 1 MiB %.1f us\n",
               d, launch_us, rt_us, ev_us, t1 * 1e3, t2 * 1e3, t1 / t2, mc[0], mc[1]);
        if (d == 0) { result("launch_us", "us", launch_us, &launch_us, 1); result("launch_sync_us", "us", rt_us, &rt_us, 1); result("event_pair_us", "us", ev_us, &ev_us, 1);
                      double ov = t1 / t2; result("stream_overlap", "x", ov, &ov, 1); result("memcpy_1KiB_us", "us", mc[0], &mc[0], 1); }
        HIP_CHECK(hipFree(sink)); HIP_CHECK(hipFree(db)); free(hb);
    }
    return 0;
}

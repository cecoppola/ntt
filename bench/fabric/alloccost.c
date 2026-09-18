/* fabric/alloccost - hipMalloc cost per GB vs block size, with peer access enabled, on all four devices,
 * with and without a memset; and the cost of the second allocation after a free (reuse). */
#include "common_ntt.h"
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
int main(void)
{
    int nd = device_count();
    for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); for (int c = 0; c < nd; c++) if (c != d) { hipError_t e = hipDeviceEnablePeerAccess(c, 0); (void)e; (void)hipGetLastError(); } }
    size_t sizes[] = { (size_t)1 << 30, (size_t)4 << 30, (size_t)16 << 30, (size_t)32 << 30 };
    printf("%-10s %10s %10s %10s %10s\n", "block", "malloc s", "s/GB", "memset s", "free s");
    for (int si = 0; si < 4; si++) {
        size_t b = sizes[si]; void *p[4]; double tm = 0, ts = 0, tf = 0;
        for (int d = 0; d < nd; d++) {
            HIP_CHECK(hipSetDevice(d));
            double t0 = now(); HIP_CHECK(hipMalloc(&p[d], b)); double t1 = now();
            HIP_CHECK(hipMemset(p[d], 0, b)); HIP_CHECK(hipDeviceSynchronize()); double t2 = now();
            tm += t1 - t0; ts += t2 - t1;
        }
        for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); double t0 = now(); HIP_CHECK(hipFree(p[d])); tf += now() - t0; }
        printf("%6.0f GiB %10.3f %10.3f %10.3f %10.3f   (4 devices, summed)\n", b / 1073741824.0, tm, tm / (4.0 * b / 1e9), ts, tf);
    }
    /* many 4 GiB blocks: does the cost grow with the number of live allocations? */
    void *q[64]; int n = 0; double t0 = now();
    for (int i = 0; i < 40; i++) { HIP_CHECK(hipSetDevice(i % nd)); if (hipMalloc(&q[n], (size_t)4 << 30) != hipSuccess) break; n++; if (n % 8 == 0) { printf("  %2d x 4 GiB live: last 8 took %.3f s\n", n, now() - t0); t0 = now(); } }
    for (int i = 0; i < n; i++) { HIP_CHECK(hipSetDevice(i % nd)); HIP_CHECK(hipFree(q[i])); }
    return 0;
}

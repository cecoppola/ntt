/* fabric/p2p - per-pair xGMI bandwidth, uni- and bidirectional, by a copy
 * kernel running on the destination (pull) and on the source (push), 4 GiB
 * per direction; plus 4-byte remote store latency (one lane, dependent). */
#include "common_ntt.h"
__global__ void k_copy(uint64_t *dst, const uint64_t *src, size_t n)
{ size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x; for (; i < n; i += stride) dst[i] = src[i]; }
__global__ void k_chase(volatile uint64_t *p, int iters, uint64_t *out)
{ uint64_t v = 0; for (int i = 0; i < iters; i++) { p[v & 1023] = v; v = p[(v + 1) & 1023] + 1; } *out = v; }
int main(void)
{
    int nd = device_count(); size_t n = (size_t)1 << 29; /* 4 GiB */
    printf("== fabric/p2p ==\n"); meta("fabric/p2p");
    uint64_t *buf[MAXD], *buf2[MAXD];
    for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMalloc(&buf[d], n * 8)); HIP_CHECK(hipMalloc(&buf2[d], n * 8)); HIP_CHECK(hipMemset(buf[d], 1, n * 8));
        for (int c = 0; c < nd; c++) if (c != d) { hipError_t e = hipDeviceEnablePeerAccess(c, 0); if (e != hipSuccess && e != hipErrorPeerAccessAlreadyEnabled) { printf("peer %d->%d failed\n", d, c); return 1; } (void)hipGetLastError(); } }
    header("pull (kernel on dst reads src), push (kernel on src writes dst), bidirectional: GB/s");
    printf("%-6s %8s %8s %8s\n", "pair", "pull", "push", "bidir");
    double sum_pull = 0; int np = 0;
    for (int a = 0; a < nd; a++) for (int b = a + 1; b < nd; b++) {
        hipEvent_t e0, e1; float ms; double pull, push, bidir;
        HIP_CHECK(hipSetDevice(b)); timer_events(&e0, &e1);
        k_copy<<<228 * 8, 256>>>(buf2[b], buf[a], n); HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipEventRecord(e0, 0)); k_copy<<<228 * 8, 256>>>(buf2[b], buf[a], n); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
        HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); pull = n * 8.0 / (ms * 1e-3) / 1e9;
        HIP_CHECK(hipSetDevice(a)); timer_events(&e0, &e1);
        HIP_CHECK(hipEventRecord(e0, 0)); k_copy<<<228 * 8, 256>>>(buf2[b], buf[a], n); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
        HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); push = n * 8.0 / (ms * 1e-3) / 1e9;
        /* bidirectional: a pulls from b while b pulls from a */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        HIP_CHECK(hipSetDevice(a)); k_copy<<<228 * 8, 256>>>(buf2[a], buf[b], n);
        HIP_CHECK(hipSetDevice(b)); k_copy<<<228 * 8, 256>>>(buf2[b], buf[a], n);
        HIP_CHECK(hipSetDevice(a)); HIP_CHECK(hipDeviceSynchronize()); HIP_CHECK(hipSetDevice(b)); HIP_CHECK(hipDeviceSynchronize());
        clock_gettime(CLOCK_MONOTONIC, &t1); double s = (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec);
        bidir = 2 * n * 8.0 / s / 1e9;
        printf("%d<->%d  %8.0f %8.0f %8.0f\n", a, b, pull, push, bidir);
        sum_pull += pull; np++;
        if (a == 0 && b == 1) { result("pull_01", "GB/s", pull, &pull, 1); result("push_01", "GB/s", push, &push, 1); result("bidir_01", "GB/s", bidir, &bidir, 1); }
    }
    /* latency: dependent remote loads/stores from APU0 into APU1's memory */
    { HIP_CHECK(hipSetDevice(0)); uint64_t *out; HIP_CHECK(hipMalloc(&out, 8));
      hipEvent_t e0, e1; float ms; timer_events(&e0, &e1);
      k_chase<<<1, 1>>>(buf[1], 1000, out); HIP_CHECK(hipDeviceSynchronize());
      HIP_CHECK(hipEventRecord(e0, 0)); k_chase<<<1, 1>>>(buf[1], 20000, out); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
      HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); double lat = ms * 1e6 / 20000;
      HIP_CHECK(hipEventRecord(e0, 0)); k_chase<<<1, 1>>>(buf[0], 20000, out); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
      HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); double lloc = ms * 1e6 / 20000;
      printf("dependent store+load: remote (APU0 -> APU1 memory) %.0f ns, local %.0f ns\n", lat, lloc);
      result("remote_rt_ns", "ns", lat, &lat, 1); result("local_rt_ns", "ns", lloc, &lloc, 1); }
    printf("mean pull %.0f GB/s over %d pairs (bench/03: 909 GB/s push corner turn)\n", sum_pull / np, np);
    return 0;
}

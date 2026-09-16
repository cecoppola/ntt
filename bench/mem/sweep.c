/* mem/sweep - working-set sweep 1 MB .. 4 GB: streaming read bandwidth per
 * APU through L2 / Infinity Cache / HBM, and the same for a strided (one
 * 8-byte word per 4 KiB page) walk over a 64 GiB registered host arena for
 * TLB reach.  Usage: mem/sweep [host arena GiB (64)] */
#include "common_ntt.h"
__global__ void k_read(const uint64_t *a, size_t n, int reps, uint64_t *sink)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    uint64_t acc = 0;
    for (int r = 0; r < reps; r++) for (size_t k = i; k < n; k += stride) acc += a[k];
    if (acc == 0x12345) *sink = acc;
}
__global__ void k_stride(const uint64_t *a, size_t n, size_t step, uint64_t *sink)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    uint64_t acc = 0;
    for (size_t k = i * step; k < n; k += stride * step) acc += a[k];
    if (acc == 0x12345) *sink = acc;
}
int main(int argc, char **argv)
{
    double arena = argc > 1 ? atof(argv[1]) : 64;
    printf("== mem/sweep ==\n"); meta("mem/sweep");
    HIP_CHECK(hipSetDevice(0));
    uint64_t *d, *sink; HIP_CHECK(hipMalloc(&d, (size_t)4 << 30)); HIP_CHECK(hipMalloc(&sink, 8));
    HIP_CHECK(hipMemset(d, 1, (size_t)4 << 30));
    hipEvent_t e0, e1; timer_events(&e0, &e1);
    header("working set (device)");
    printf("%10s %10s\n", "bytes", "GB/s");
    for (size_t bytes = 1 << 20; bytes <= ((size_t)4 << 30); bytes <<= 1) {
        size_t n = bytes / 8; int reps = (int)(((size_t)8 << 30) / bytes); if (reps < 1) reps = 1; if (reps > 4096) reps = 4096;
        k_read<<<228 * 8, 256>>>(d, n, 1, sink); HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipEventRecord(e0, 0)); k_read<<<228 * 8, 256>>>(d, n, reps, sink); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
        float ms; HIP_CHECK(hipEventElapsedTime(&ms, e0, e1));
        double gbs = (double)bytes * reps / (ms * 1e-3) / 1e9;
        printf("%10zu %10.0f\n", bytes, gbs);
        if (bytes == (1 << 20) || bytes == (8 << 20) || bytes == (64 << 20) || bytes == ((size_t)1 << 30) || bytes == ((size_t)4 << 30)) {
            char nm[32]; snprintf(nm, sizeof nm, "read_%zuMB", bytes >> 20); result(nm, "GB/s", gbs, &gbs, 1); }
    }
    /* TLB reach: one word per 4 KiB page over a registered host arena, then per 2 MiB */
    header("host arena page walk");
    size_t hb = (size_t)(arena * (1 << 30));
    uint64_t *h = (uint64_t *)aligned_alloc(1 << 21, hb);
    if (!h) { printf("arena alloc failed\n"); return 1; }
#pragma omp parallel for schedule(static)
    for (size_t off = 0; off < hb; off += 4096) h[off / 8] = off;
    HIP_CHECK(hipHostRegister(h, hb, hipHostRegisterDefault));
    size_t steps[3] = {512, 1 << 18, 8};              /* 4 KiB, 2 MiB, 64 B strides in words */
    const char *names[3] = {"4KiB_stride", "2MiB_stride", "64B_stride"};
    for (int si = 0; si < 3; si++) {
        size_t n = hb / 8, step = steps[si], touches = n / step;
        k_stride<<<228 * 8, 256>>>(h, n, step, sink); HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipEventRecord(e0, 0)); k_stride<<<228 * 8, 256>>>(h, n, step, sink); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
        float ms; HIP_CHECK(hipEventElapsedTime(&ms, e0, e1));
        double rate = touches / (ms * 1e-3) / 1e6;
        printf("%-12s %.0f GiB arena: %zu touches in %.1f ms = %.1f M touches/s \n", names[si], arena, touches, ms, rate);
        char nm[40]; snprintf(nm, sizeof nm, "host_%s_Mtouch_s", names[si]); result(nm, "M/s", rate, &rate, 1);
    }
    HIP_CHECK(hipHostUnregister(h)); free(h);
    return 0;
}

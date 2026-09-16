/* mem/stride - HBM access pattern sweep on APU0 over a 4 GiB buffer: read
 * bandwidth vs element width (4 / 8 / 16 B per lane) and lane stride
 * (contiguous, 64 B, 256 B, 4 KiB): what coalescing and line utilisation cost. */
#include "common_ntt.h"
template <typename T>
__global__ void k_rd(const T *a, size_t n, size_t stride_elems, unsigned long long *sink)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, g = (size_t)gridDim.x * blockDim.x;
    unsigned long long acc = 0;
    for (size_t k = i * stride_elems; k < n; k += g * stride_elems) { T v = a[k]; acc += *(const unsigned int *)&v; }
    if (acc == 0x1234567) *sink = acc;
}
template <typename T>
static double run(const T *a, size_t bytes, size_t stride_bytes, unsigned long long *sink)
{
    size_t n = bytes / sizeof(T), se = stride_bytes / sizeof(T); if (se < 1) se = 1;
    hipEvent_t e0, e1; timer_events(&e0, &e1); float ms;
    k_rd<T><<<228 * 8, 256>>>(a, n, se, sink); HIP_CHECK(hipDeviceSynchronize());
    HIP_CHECK(hipEventRecord(e0, 0)); k_rd<T><<<228 * 8, 256>>>(a, n, se, sink); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
    HIP_CHECK(hipEventElapsedTime(&ms, e0, e1));
    return (double)(n / se) * sizeof(T) / (ms * 1e-3) / 1e9;         /* useful bytes */
}
int main(void)
{
    printf("== mem/stride ==\n"); meta("mem/stride");
    HIP_CHECK(hipSetDevice(0));
    size_t bytes = (size_t)4 << 30; void *a; unsigned long long *sink;
    HIP_CHECK(hipMalloc(&a, bytes)); HIP_CHECK(hipMemset(a, 1, bytes)); HIP_CHECK(hipMalloc(&sink, 8));
    header("useful GB/s (read) on 4 GiB, APU0: width x lane stride");
    size_t strides[4] = {0, 64, 256, 4096};
    printf("%8s %10s %10s %10s %10s\n", "width", "contig", "64B", "256B", "4KiB");
    double r4[4], r8[4], r16[4];
    for (int s = 0; s < 4; s++) { r4[s] = run<unsigned int>((unsigned int *)a, bytes, strides[s], sink); r8[s] = run<unsigned long long>((unsigned long long *)a, bytes, strides[s], sink); r16[s] = run<ulonglong2>((ulonglong2 *)a, bytes, strides[s], sink); }
    printf("%8s %10.0f %10.0f %10.0f %10.0f\n", "4 B", r4[0], r4[1], r4[2], r4[3]);
    printf("%8s %10.0f %10.0f %10.0f %10.0f\n", "8 B", r8[0], r8[1], r8[2], r8[3]);
    printf("%8s %10.0f %10.0f %10.0f %10.0f\n", "16 B", r16[0], r16[1], r16[2], r16[3]);
    result("read_8B_contig", "GB/s", r8[0], &r8[0], 1); result("read_16B_contig", "GB/s", r16[0], &r16[0], 1); result("read_8B_stride4K", "GB/s", r8[3], &r8[3], 1);
    return 0;
}

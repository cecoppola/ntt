/* lds/occupancy - LDS throughput vs bytes per block (blocks/CU) and access
 * width: ds_read_b64 vs b128 (uint64 vs ulong2), with and without the pad-17
 * layout, on a 128-row x 16-column tile like the NTT kernels'. APU0. */
#include "common_ntt.h"
template <int PAD, int BYTES_PER_BLOCK>
__global__ __launch_bounds__(256) void k_lds(int iters, uint64_t *sink)
{
    __shared__ uint64_t sh[BYTES_PER_BLOCK / 8];
    const int tt = threadIdx.x >> 4, bb = threadIdx.x & 15;
    const int rows = BYTES_PER_BLOCK / 8 / PAD; if (rows < 128) return;
    for (int j = tt; j < 128; j += 16) sh[j * PAD + bb] = j + bb;
    __syncthreads();
    uint64_t acc = 0;
    for (int i = 0; i < iters; i++) {
#pragma unroll
        for (int j = 0; j < 8; j++) { int r = (tt + 16 * j + i) & 127; acc += sh[r * PAD + bb]; sh[((r + 64) & 127) * PAD + bb] = acc; }
    }
    if (acc == 0x1234567) *sink = acc;
}
template <int BYTES_PER_BLOCK>
__global__ __launch_bounds__(256) void k_lds128(int iters, uint64_t *sink)
{
    __shared__ ulonglong2 sh[BYTES_PER_BLOCK / 16];
    ulonglong2 acc = {0, 0};
    for (int k = threadIdx.x; k < BYTES_PER_BLOCK / 16; k += 256) { ulonglong2 v = {(unsigned long long)k, 1}; sh[k] = v; }
    __syncthreads();
    for (int i = 0; i < iters; i++) {
#pragma unroll
        for (int j = 0; j < 8; j++) { int idx = (threadIdx.x + 256 * j + i) & (BYTES_PER_BLOCK / 16 - 1); ulonglong2 v = sh[idx]; acc.x += v.x; acc.y += v.y; sh[(idx + 512) & (BYTES_PER_BLOCK / 16 - 1)] = acc; }
    }
    if (acc.x == 0x1234567) *sink = acc.x;
}
int main(void)
{
    printf("== lds/occupancy ==\n"); meta("lds/occupancy");
    HIP_CHECK(hipSetDevice(0));
    uint64_t *sink; HIP_CHECK(hipMalloc(&sink, 8));
    hipEvent_t e0, e1; timer_events(&e0, &e1); float ms; int iters = 2000;
    header("8-byte LDS read+write per lane-iteration, 256 threads; TB/s of LDS traffic per APU");
#define RUN(PAD, B, label) do { unsigned blocks = 228 * 16; k_lds<PAD, B><<<blocks, 256>>>(10, sink); HIP_CHECK(hipDeviceSynchronize());    \
        HIP_CHECK(hipEventRecord(e0, 0)); k_lds<PAD, B><<<blocks, 256>>>(iters, sink); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); \
        HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); double tb = (double)blocks * 256 * 8 * 16 * iters / (ms * 1e-3) / 1e12;                 \
        printf("%-34s %6d B/block (%d blocks/CU): %.2f TB/s\n", label, B, 65536 / B, tb); result(label, "TB/s", tb, &tb, 1); } while (0)
    RUN(17, 17408, "pad17_17KB");
    RUN(16, 16384, "pad16_16KB");
    RUN(17, 34816, "pad17_34KB");
    RUN(17, 65536, "pad17_64KB");
    { unsigned blocks = 228 * 16; k_lds128<16384><<<blocks, 256>>>(10, sink); HIP_CHECK(hipDeviceSynchronize());
      HIP_CHECK(hipEventRecord(e0, 0)); k_lds128<16384><<<blocks, 256>>>(iters, sink); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
      HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); double tb = (double)blocks * 256 * 16 * 16 * iters / (ms * 1e-3) / 1e12;
      printf("%-34s %6d B/block: %.2f TB/s\n", "b128_16KB", 16384, tb); result("b128_16KB", "TB/s", tb, &tb, 1); }
    return 0;
}

/* arith/mfma - matrix-core rates, for the record (ALGORITHM.md R10 quotes
 * 880.4 T int8 MAC/s per APU with no source in bench/).
 *
 *   int8   v_mfma_i32_32x32x16_i8   32x32x16 = 16 384 MAC per wave-instruction
 *   bf16   v_mfma_f32_32x32x8_bf16  32x32x8  =  8 192
 *   f64    v_mfma_f64_16x16x4_f64   16x16x4  =  1 024
 *
 * Four independent accumulator chains per wave, all four APUs, with the
 * in-kernel clock so the per-CU issue rate can be read in cycles.
 * Usage: mfma
 */
#include "../common_ntt.h"

typedef int v16i __attribute__((ext_vector_type(16)));
typedef float v16f __attribute__((ext_vector_type(16)));
typedef short v4s __attribute__((ext_vector_type(4)));
typedef double v4d __attribute__((ext_vector_type(4)));
#define NCH 4

__global__ __launch_bounds__(256)
void k_i8(int iters, int *sink, unsigned long long *cyc, unsigned long long *wall)
{
    v16i acc[NCH]; long a = threadIdx.x * 0x0101010101010101L, b = a ^ 0x5555555555555555L;
    int i, j; long long c0 = clock64(), w0 = wall_clock64();
    for (j = 0; j < NCH; j++) for (i = 0; i < 16; i++) acc[j][i] = j;
    for (i = 0; i < iters; i++) {
#pragma unroll
        for (j = 0; j < NCH; j++) acc[j] = __builtin_amdgcn_mfma_i32_32x32x16_i8(a, b, acc[j], 0, 0, 0);
    }
    { int s = 0; for (j = 0; j < NCH; j++) s += acc[j][threadIdx.x & 15]; if (s == 0x7eadbeef) *sink = s; }
    if (threadIdx.x == 0) { atomicAdd(cyc, (unsigned long long)(clock64() - c0)); atomicAdd(wall, (unsigned long long)(wall_clock64() - w0)); }
}
__global__ __launch_bounds__(256)
void k_bf16(int iters, float *sink)
{
    v16f acc[NCH]; v4s a, b; int i, j;
    for (i = 0; i < 4; i++) { a[i] = (short)(0x3f80 + threadIdx.x + i); b[i] = (short)(0x3f00 + i); }
    for (j = 0; j < NCH; j++) for (i = 0; i < 16; i++) acc[j][i] = (float)j;
    for (i = 0; i < iters; i++) {
#pragma unroll
        for (j = 0; j < NCH; j++) acc[j] = __builtin_amdgcn_mfma_f32_32x32x8bf16_1k(a, b, acc[j], 0, 0, 0);
    }
    { float s = 0; for (j = 0; j < NCH; j++) s += acc[j][threadIdx.x & 15]; if (s == -1.0f) *sink = s; }
}
__global__ __launch_bounds__(256)
void k_f64(int iters, double *sink)
{
    v4d acc[NCH]; double a = 1.0 + threadIdx.x * 1e-9, b = 0.999999; int i, j;
    for (j = 0; j < NCH; j++) for (i = 0; i < 4; i++) acc[j][i] = (double)j;
    for (i = 0; i < iters; i++) {
#pragma unroll
        for (j = 0; j < NCH; j++) acc[j] = __builtin_amdgcn_mfma_f64_16x16x4f64(a, b, acc[j], 0, 0, 0);
    }
    { double s = 0; for (j = 0; j < NCH; j++) s += acc[j][threadIdx.x & 3]; if (s == -1.0) *sink = s; }
}

int main(void)
{
    int nd = device_count(), wallkhz = 0, iters = 2000;
    hipDeviceProp_t pr; int blocks;
    double r8[MAXD], rb[MAXD], rf[MAXD], mhz[MAXD];
    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    HIP_CHECK(hipDeviceGetAttribute(&wallkhz, hipDeviceAttributeWallClockRate, 0));
    blocks = pr.multiProcessorCount * 4;
    printf("== arith/mfma : matrix-core MAC rates ==\n");
    meta("arith/mfma");
    header("MFMA");
#pragma omp parallel num_threads(nd)
    {
        int dev = omp_get_thread_num(), rep; hipEvent_t e0, e1; float ms; double best;
        void *sink; unsigned long long *dc, hc[2];
        double waves = (double)blocks * 4;             /* 256 threads = 4 waves */
        HIP_CHECK(hipSetDevice(dev));
        HIP_CHECK(hipMalloc(&sink, 64)); HIP_CHECK(hipMalloc(&dc, 16)); HIP_CHECK(hipMemset(dc, 0, 16));
        timer_events(&e0, &e1);
#define BEST(launch, out, macs) do { best = 1e300; for (rep = 0; rep < 3; rep++) { \
            _Pragma("omp barrier") HIP_CHECK(hipEventRecord(e0, 0)); launch;       \
            HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));   \
            HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); best = dmin(best, ms); }   \
            out[dev] = waves * NCH * iters * (macs) / (best * 1e-3) / 1e12; } while (0)
        BEST((k_i8<<<blocks, 256>>>(iters, (int *)sink, dc, dc + 1)), r8, 16384.0);
        HIP_CHECK(hipMemcpy(hc, dc, 16, hipMemcpyDeviceToHost)); mhz[dev] = (double)hc[0] / hc[1] * wallkhz / 1000.0;
        BEST((k_bf16<<<blocks, 256>>>(iters, (float *)sink)), rb, 8192.0);
        BEST((k_f64<<<blocks, 256>>>(iters, (double *)sink)), rf, 1024.0);
        HIP_CHECK(hipFree(sink)); HIP_CHECK(hipFree(dc));
    }
    report_sum("int8 32x32x16", "TMAC/s", r8, nd);
    report_max("  sclk during int8", "MHz", mhz, nd);
    report_sum("bf16 32x32x8", "TMAC/s", rb, nd);
    report_sum("f64 16x16x4", "TMAC/s", rf, nd);
    printf("\nR10 quoted 880.4 T int8 MAC/s per APU; MI300A datasheet: 1961 TOPS int8 dense (= 980 TMAC/s), 61.3 TF matrix FP64 (= 30.7 TMAC/s) at 2.1 GHz\n");
    return 0;
}

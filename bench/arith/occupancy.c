/* arith/occupancy - modmul chain throughput vs threads/block and chains per
 * thread (ILP), FP64 Barrett (the paper's) and integer Montgomery; plus FP32
 * FMA and packed FP16 rates for the record.  Per APU0. */
#include "common_ntt.h"
#include "../ecalc/modarith.h"
#include <hip/hip_fp16.h>
template <int CH>
__global__ void k_f64(double p, double pinv, double c, int iters, double *sink)
{
    double x[CH]; for (int j = 0; j < CH; j++) x[j] = (double)((threadIdx.x * 7 + j * 13 + 1) % 1000);
    for (int i = 0; i < iters; i++) {
#pragma unroll
        for (int j = 0; j < CH; j++) x[j] = ec_mm(x[j], c, p, pinv); }
    double s = 0; for (int j = 0; j < CH; j++) s += x[j]; if (s == -1.0) *sink = s;
}
__device__ static inline uint64_t mont(uint64_t a, uint64_t b, uint64_t p, uint64_t pinv_neg)
{ uint64_t lo = a * b, hi = __umul64hi(a, b), q = lo * pinv_neg, t = __umul64hi(q, p); return hi + t + (lo != 0); }
template <int CH>
__global__ void k_mont(uint64_t p, uint64_t pinv_neg, uint64_t c, int iters, uint64_t *sink)
{
    uint64_t x[CH]; for (int j = 0; j < CH; j++) x[j] = (threadIdx.x * 7 + j * 13 + 1) % 1000;
    for (int i = 0; i < iters; i++) {
#pragma unroll
        for (int j = 0; j < CH; j++) x[j] = mont(x[j], c, p, pinv_neg); }
    uint64_t s = 0; for (int j = 0; j < CH; j++) s += x[j]; if (s == 7) *sink = s;
}
__global__ void k_f32(float c, int iters, float *sink)
{ float x[8]; for (int j = 0; j < 8; j++) x[j] = threadIdx.x + j; for (int i = 0; i < iters; i++) { for (int j = 0; j < 8; j++) x[j] = fmaf(x[j], c, 1.0f); }
  float s = 0; for (int j = 0; j < 8; j++) s += x[j]; if (s == -1) *sink = s; }
__global__ void k_f16(__half2 c, int iters, __half2 *sink)
{ __half2 x[8]; for (int j = 0; j < 8; j++) x[j] = __float2half2_rn(threadIdx.x + j); for (int i = 0; i < iters; i++) { for (int j = 0; j < 8; j++) x[j] = __hfma2(x[j], c, x[j]); }
  __half2 s = x[0]; for (int j = 1; j < 8; j++) s = __hadd2(s, x[j]); if (__low2float(s) == -1) *sink = s; }
int main(void)
{
    printf("== arith/occupancy ==\n"); meta("arith/occupancy");
    HIP_CHECK(hipSetDevice(0));
    double *sd; uint64_t *su; float *sf; __half2 *sh;
    HIP_CHECK(hipMalloc(&sd, 8)); HIP_CHECK(hipMalloc(&su, 8)); HIP_CHECK(hipMalloc(&sf, 8)); HIP_CHECK(hipMalloc(&sh, 8));
    ec_mod m = ec_mod_get(0); uint64_t p2 = 0x3FFEDF0000000001ULL, inv = 1; for (int k = 0; k < 6; k++) inv *= 2 - p2 * inv; uint64_t pn = 0 - inv;
    hipEvent_t e0, e1; timer_events(&e0, &e1); float ms;
    header("FP64 Barrett modmul chains, Gmodmul/s per APU: threads/block x chains/thread");
    int tpb[4] = {64, 128, 256, 512};
    printf("%6s %8s %8s %8s\n", "tpb", "ch=1", "ch=4", "ch=8");
    for (int ti = 0; ti < 4; ti++) {
        double r[3]; int iters = 4000;
        for (int ci = 0; ci < 3; ci++) {
            int CH = ci == 0 ? 1 : ci == 1 ? 4 : 8; unsigned blocks = 228 * 2048 / tpb[ti];
            if (CH == 1) k_f64<1><<<blocks, tpb[ti]>>>(m.p, m.pinv, 12345.0, 100, sd); else if (CH == 4) k_f64<4><<<blocks, tpb[ti]>>>(m.p, m.pinv, 12345.0, 100, sd); else k_f64<8><<<blocks, tpb[ti]>>>(m.p, m.pinv, 12345.0, 100, sd);
            HIP_CHECK(hipDeviceSynchronize());
            HIP_CHECK(hipEventRecord(e0, 0));
            if (CH == 1) k_f64<1><<<blocks, tpb[ti]>>>(m.p, m.pinv, 12345.0, iters, sd); else if (CH == 4) k_f64<4><<<blocks, tpb[ti]>>>(m.p, m.pinv, 12345.0, iters, sd); else k_f64<8><<<blocks, tpb[ti]>>>(m.p, m.pinv, 12345.0, iters, sd);
            HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms, e0, e1));
            r[ci] = (double)blocks * tpb[ti] * CH * iters / (ms * 1e-3) / 1e9;
        }
        printf("%6d %8.0f %8.0f %8.0f\n", tpb[ti], r[0], r[1], r[2]);
        if (tpb[ti] == 256) result("f64_modmul_256x8", "Gmodmul/s", r[2], &r[2], 1);
    }
    header("integer Montgomery modmul (62-bit p), chains/thread at 256 tpb");
    { double r[3]; int iters = 4000; unsigned blocks = 228 * 8;
      k_mont<8><<<blocks, 256>>>(p2, pn, 12345, 100, su); HIP_CHECK(hipDeviceSynchronize());
      HIP_CHECK(hipEventRecord(e0, 0)); k_mont<1><<<blocks, 256>>>(p2, pn, 12345, iters, su); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); r[0] = (double)blocks * 256 * 1 * iters / (ms * 1e-3) / 1e9;
      HIP_CHECK(hipEventRecord(e0, 0)); k_mont<4><<<blocks, 256>>>(p2, pn, 12345, iters, su); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); r[1] = (double)blocks * 256 * 4 * iters / (ms * 1e-3) / 1e9;
      HIP_CHECK(hipEventRecord(e0, 0)); k_mont<8><<<blocks, 256>>>(p2, pn, 12345, iters, su); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); r[2] = (double)blocks * 256 * 8 * iters / (ms * 1e-3) / 1e9;
      printf("ch=1 %.0f  ch=4 %.0f  ch=8 %.0f Gmodmul/s\n", r[0], r[1], r[2]); result("mont_modmul_256x8", "Gmodmul/s", r[2], &r[2], 1); }
    header("FP32 FMA and packed FP16 FMA, 8 chains, 256 tpb");
    { unsigned blocks = 228 * 8; int iters = 20000;
      k_f32<<<blocks, 256>>>(1.0001f, 100, sf); HIP_CHECK(hipDeviceSynchronize());
      HIP_CHECK(hipEventRecord(e0, 0)); k_f32<<<blocks, 256>>>(1.0001f, iters, sf); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms, e0, e1));
      double f32 = (double)blocks * 256 * 8 * iters * 2 / (ms * 1e-3) / 1e12;
      __half2 c = __float2half2_rn(1.0001f);
      k_f16<<<blocks, 256>>>(c, 100, sh); HIP_CHECK(hipDeviceSynchronize());
      HIP_CHECK(hipEventRecord(e0, 0)); k_f16<<<blocks, 256>>>(c, iters, sh); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms, e0, e1));
      double f16 = (double)blocks * 256 * 8 * iters * 4 / (ms * 1e-3) / 1e12;
      printf("FP32 %.1f TFLOP/s, packed FP16 %.1f TFLOP/s (datasheet at 2.1 GHz: FP32 vector 122.6 TF per APU; at the measured 1.5 GHz 87.6)\n", f32, f16);
      result("f32_TFLOPs", "TF", f32, &f32, 1); result("f16x2_TFLOPs", "TF", f16, &f16, 1); }
    return 0;
}

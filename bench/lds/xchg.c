/* lds/xchg - intra-wavefront exchange primitives on gfx942: ds_bpermute
 * (__shfl_xor), DPP row shifts (__builtin_amdgcn_mov_dpp), ds_swizzle
 * (__builtin_amdgcn_ds_swizzle), and an LDS round trip, as a dependent chain
 * per lane: ns per exchange and exchanges/s per APU. */
#include "common_ntt.h"
__global__ void k_shfl(int iters, unsigned *out) { unsigned v = threadIdx.x; for (int i = 0; i < iters; i++) v = __shfl_xor(v, 1 + (i & 15)) + 1; if (v == 0xdead) *out = v; }
__global__ void k_dpp(int iters, unsigned *out)
{ int v = threadIdx.x; for (int i = 0; i < iters; i++) { v = __builtin_amdgcn_mov_dpp(v, 0x111, 0xf, 0xf, 0) + 1; }  /* row_shr:1 */ if (v == 0xdead) *out = v; }
__global__ void k_swz(int iters, unsigned *out)
{ int v = threadIdx.x; for (int i = 0; i < iters; i++) { v = __builtin_amdgcn_ds_swizzle(v, 0x041F) + 1; }  /* swap adjacent lanes (xor 1) */ if (v == 0xdead) *out = v; }
__global__ void k_lds(int iters, unsigned *out)
{ __shared__ unsigned sh[256]; unsigned v = threadIdx.x; for (int i = 0; i < iters; i++) { sh[threadIdx.x] = v; __syncthreads(); v = sh[threadIdx.x ^ 1] + 1; __syncthreads(); } if (v == 0xdead) *out = v; }
int main(void)
{
    printf("== lds/xchg ==\n"); meta("lds/xchg");
    HIP_CHECK(hipSetDevice(0)); unsigned *out; HIP_CHECK(hipMalloc(&out, 4));
    hipEvent_t e0, e1; timer_events(&e0, &e1); float ms; int iters = 100000; unsigned blocks = 228 * 8;
    header("dependent chain per lane, 256 threads x 1824 blocks, APU0");
#define RUN(K, label) do { K<<<blocks, 256>>>(100, out); HIP_CHECK(hipDeviceSynchronize());                                         \
        HIP_CHECK(hipEventRecord(e0, 0)); K<<<blocks, 256>>>(iters, out); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); \
        HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); double per = ms * 1e6 / iters, rate = (double)blocks * 256 * iters / (ms * 1e-3) / 1e12; \
        printf("%-28s %7.2f ns per exchange (chain), %6.2f T lane-exchanges/s per APU\n", label, per, rate); result(label, "Tx/s", rate, &rate, 1); } while (0)
    RUN(k_shfl, "ds_bpermute (shfl_xor)");
    RUN(k_dpp, "DPP row_shr");
    RUN(k_swz, "ds_swizzle xor1");
    RUN(k_lds, "LDS store+sync+load");
    return 0;
}

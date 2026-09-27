/* V3 (Phase 14) throwaway: what the radix-3 pass of ntt3.c costs inside a 3 2^k transform (B5: ntt3.c on the reduced-correction
 * modmul, or the pass fused into the first 2^k pass).  fwd3(3 2^k) = the r3 pass + ntt_fwd(2^k, batch 3); the difference is the pass.
 * Built on aac6 in ecalc/:  hipcc -O3 -fopenmp -I. ../tests/v3_r3_bench.c -x none ntt.o ntt3.o bigint.o fatal.o -o ../tests/v3_r3_bench
 * Run: ./v3_r3_bench [logk ...]  (APU0, prime 0, median of 5; batch = 3 2^31 / 3 2^k transforms up to 2^31 points in all) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hip/hip_runtime.h>
#include "ntt.h"
#define HC(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
__global__ void k_fill(uint64_t *x, size_t n, uint64_t p) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; for (; i < n; i += (size_t)gridDim.x * blockDim.x) x[i] = (i * 0x9E3779B97F4A7C15ull >> 7) % p; }
int main(int argc, char **argv)
{
    HC(hipSetDevice(0));
    ntt_ctx *c = ntt_ctx_create(0); uint64_t p = ec_P[0];
    int ks[16], nk = 0; for (int i = 1; i < argc && nk < 16; i++) ks[nk++] = atoi(argv[i]);
    if (!nk) { int d[] = {7, 12, 17, 22, 26, 28, 29}; for (int i = 0; i < 7; i++) ks[nk++] = d[i]; }
    uint64_t *x; size_t tot = (size_t)3 << 29; HC(hipMalloc(&x, tot * 8));
    k_fill<<<4096, 256>>>(x, tot, p); HC(hipDeviceSynchronize());
    printf("logk | batch | fwd3 3*2^k ms | fwd 2^k x3 ms | r3 pass ms (share) | inv3 ms | inv 2^k x3 ms | r3 inv (share)\n");
    for (int i = 0; i < nk; i++) {
        int k = ks[i]; size_t m = (size_t)1 << k, batch = tot / (3 * m); if (!batch) continue;
        double a[5], b[5], ci[5], di[5];
        for (int r = 0; r < 5; r++) {
            double t = now(); ntt_fwd3(c, x, k, batch, 0); HC(hipDeviceSynchronize()); a[r] = now() - t;
            t = now(); ntt_fwd(c, x, k, 3 * batch, 0); HC(hipDeviceSynchronize()); b[r] = now() - t;
            t = now(); ntt_inv3(c, x, k, batch, 0); HC(hipDeviceSynchronize()); ci[r] = now() - t;
            t = now(); ntt_inv(c, x, k, 3 * batch, 0); HC(hipDeviceSynchronize()); di[r] = now() - t;
        }
        qsort(a, 5, 8, cmpd); qsort(b, 5, 8, cmpd); qsort(ci, 5, 8, cmpd); qsort(di, 5, 8, cmpd);
        printf("%4d | %5zu | %8.3f | %8.3f | %8.3f (%4.1f %%) | %8.3f | %8.3f | %8.3f (%4.1f %%)\n", k, batch, 1e3 * a[2], 1e3 * b[2], 1e3 * (a[2] - b[2]),
               100 * (a[2] - b[2]) / a[2], 1e3 * ci[2], 1e3 * di[2], 1e3 * (ci[2] - di[2]), 100 * (ci[2] - di[2]) / ci[2]);
    }
    return 0;
}

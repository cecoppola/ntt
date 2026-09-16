/* 21_alloc - what allocation, registration and first touch cost, by size.
 *
 * The paper's memory strategy (grow-only pools, pow2 grows, malloc over
 * calloc) rests on hipFree+hipMalloc costing 0.5-1 s per multi-GB buffer.
 * This measures on device 0, for 1..32 GiB:
 *   hipMalloc / GPU first touch / hipFree
 *   hipHostMalloc (non-coherent) / GPU first touch / hipHostFree
 *   malloc + CPU first touch (1 thread, 48 threads) / hipHostRegister /
 *   hipHostUnregister / free;  calloc + touch for comparison
 *
 * Usage: 21_alloc [max GiB]
 */
#include "common_ntt.h"
#include <malloc.h>

__global__ void k_touch(uint64_t *p, size_t n, uint64_t seed)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) p[i] = seed + i;
}

static double now(void) { return omp_get_wtime(); }

int main(int argc, char **argv)
{
    double maxg = argc > 1 ? atof(argv[1]) : 32.0;
    double g;
    hipDeviceProp_t pr;
    int blocks, thr = 256;

    HIP_CHECK(hipSetDevice(0));
    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    blocks = pr.multiProcessorCount * 4;
    printf("== 21_alloc : allocation, registration and first-touch cost vs size (APU0) ==\n");
    meta("21_alloc");
    printf("%6s | %8s %8s %8s | %8s %8s %8s | %8s %8s %8s %8s %8s %8s | %8s\n", "GiB",
           "hipMall", "gputch", "hipFree", "hostMal", "gputch", "hostFre",
           "malloc", "cput1", "cput48", "hReg", "hUnreg", "free", "calloc48");
    for (g = 1; g <= maxg; g *= 2) {
        size_t bytes = (size_t)(g * 1073741824.0), n = bytes / 8;
        double t[16];
        uint64_t *p;
        double v[1];
        char nm[32];
        int i;

        t[0] = now(); HIP_CHECK(hipMalloc(&p, bytes)); t[1] = now();
        k_touch<<<blocks, thr>>>(p, n, 1); HIP_CHECK(hipDeviceSynchronize()); t[2] = now();
        HIP_CHECK(hipFree(p)); t[3] = now();

        t[4] = now(); HIP_CHECK(hipHostMalloc((void **)&p, bytes, hipHostMallocNonCoherent)); t[5] = now();
        k_touch<<<blocks, thr>>>(p, n, 1); HIP_CHECK(hipDeviceSynchronize()); t[6] = now();
        HIP_CHECK(hipHostFree(p)); t[7] = now();

        t[8] = now(); p = (uint64_t *)malloc(bytes); t[9] = now();
        for (i = 0; i < (int)(n / 512); i++) p[(size_t)i * 512] = i;  t[10] = now();
        free(p);
        p = (uint64_t *)malloc(bytes);
        t[11] = now();
#pragma omp parallel for num_threads(48)
        for (i = 0; i < (int)(n / 512); i++) p[(size_t)i * 512] = i;
        t[12] = now();
        HIP_CHECK(hipHostRegister(p, bytes, hipHostRegisterDefault)); t[13] = now();
        HIP_CHECK(hipHostUnregister(p)); t[14] = now();
        free(p); t[15] = now();

        {
            double c0 = now(), c1;
            p = (uint64_t *)calloc(n, 8);
#pragma omp parallel for num_threads(48)
            for (i = 0; i < (int)(n / 512); i++) p[(size_t)i * 512] = i;
            c1 = now(); free(p);
            printf("%6.0f | %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f %8.3f %8.3f | %8.3f\n", g,
                   t[1]-t[0], t[2]-t[1], t[3]-t[2], t[5]-t[4], t[6]-t[5], t[7]-t[6],
                   t[9]-t[8], t[10]-t[9], t[12]-t[11], t[13]-t[12], t[14]-t[13], t[15]-t[14], c1 - c0);
            snprintf(nm, sizeof nm, "hipMalloc_%.0fGiB", g);     v[0] = t[1]-t[0];   result(nm, "s", v[0], v, 1);
            snprintf(nm, sizeof nm, "hipFree_%.0fGiB", g);       v[0] = t[3]-t[2];   result(nm, "s", v[0], v, 1);
            snprintf(nm, sizeof nm, "hipHostMalloc_%.0fGiB", g); v[0] = t[5]-t[4];   result(nm, "s", v[0], v, 1);
            snprintf(nm, sizeof nm, "hostRegister_%.0fGiB", g);  v[0] = t[13]-t[12]; result(nm, "s", v[0], v, 1);
            snprintf(nm, sizeof nm, "cpuTouch48_%.0fGiB", g);    v[0] = t[12]-t[11]; result(nm, "s", v[0], v, 1);
        }
    }
    printf("\n(times in seconds; gputch = GPU first-touch kernel; cput = CPU first touch, one write per page)\n");
    /* D10: does calloc cost more than malloc when the region is reused? */
    {
        size_t bytes = (size_t)(16 * 1073741824.0), n = bytes / 8; int r, i;
        printf("\nD10: 16 GiB, three rounds of alloc + 48-thread touch + free (glibc may reuse the mapping)\n");
        printf("%6s %10s %10s\n", "round", "malloc", "calloc");
        for (r = 0; r < 3; r++) {
            double t0, t1, t2, t3; uint64_t *p;
            t0 = now(); p = (uint64_t *)malloc(bytes);
#pragma omp parallel for num_threads(48)
            for (i = 0; i < (int)(n / 512); i++) p[(size_t)i * 512] = i;
            t1 = now(); free(p);
            t2 = now(); p = (uint64_t *)calloc(n, 8);
#pragma omp parallel for num_threads(48)
            for (i = 0; i < (int)(n / 512); i++) p[(size_t)i * 512] = i;
            t3 = now(); free(p);
            printf("%6d %10.3f %10.3f\n", r, t1 - t0, t3 - t2);
        }
        printf("(with M_MMAP_THRESHOLD default, 16 GiB is always a fresh mmap: calloc gets zero pages free.\n"
               " A pool that reuses a live region would pay a memset; see mallopt/M_MMAP_THRESHOLD)\n");
        mallopt(M_MMAP_THRESHOLD, 1 << 30); mallopt(M_TRIM_THRESHOLD, -1);
        printf("with M_MMAP_THRESHOLD=1 GiB, M_TRIM off (heap reuse):\n");
        for (r = 0; r < 3; r++) {
            double t0, t1, t2, t3; uint64_t *p;
            t0 = now(); p = (uint64_t *)malloc(bytes);
#pragma omp parallel for num_threads(48)
            for (i = 0; i < (int)(n / 512); i++) p[(size_t)i * 512] = i;
            t1 = now(); free(p);
            t2 = now(); p = (uint64_t *)calloc(n, 8);
#pragma omp parallel for num_threads(48)
            for (i = 0; i < (int)(n / 512); i++) p[(size_t)i * 512] = i;
            t3 = now(); free(p);
            printf("%6d %10.3f %10.3f\n", r, t1 - t0, t3 - t2);
        }
    }
    return 0;
}

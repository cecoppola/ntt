/* mem/infcache - the cache hierarchy under the operand split's access pattern
 * (PLAN.md 7.2: RESULTS.md 14 inferred that the packed operand "fits in the
 * 256 MB Infinity Cache" from a 0.032 ms split pass; this measures it).
 *
 *   gather   one 8-byte load per lane at a pseudo-random limb index, one
 *            cache line per lane -- the split's pattern -- over working sets
 *            8 MB .. 8 GB, all four APUs.  Reported as useful GB/s (8 B per
 *            access) and as line traffic (64 B per access).
 *   chase    one wave, one dependent pointer chase through the same working
 *            sets: latency per hop in the measured clock's cycles and in ns.
 *
 * Expect steps at the L2 (4 MB), the Infinity Cache (256 MB) and HBM.
 * Usage: infcache
 */
#include "../common_ntt.h"

__global__ void k_gather(const uint64_t *a, size_t mask, size_t per_thread, uint64_t *sink)
{
    size_t t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, i;
    uint64_t s = t * 0x9E3779B97F4A7C15ULL, acc = 0;
    for (i = 0; i < per_thread; i++) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;             /* xorshift */
        acc += a[(s * 0x2545F4914F6CDD1DULL) & mask];
    }
    if (acc == 0xdeadbeefULL) *sink = acc;
}
__global__ void k_chase(const uint64_t *a, int hops, uint64_t *out,
                        unsigned long long *cyc, unsigned long long *wall)
{
    uint64_t p = threadIdx.x;
    int i;
    long long c0 = clock64(), w0 = wall_clock64();
    for (i = 0; i < hops; i++) p = a[p];
    if (threadIdx.x == 0) { *cyc = clock64() - c0; *wall = wall_clock64() - w0; }
    out[threadIdx.x] = p;
}
__global__ void k_fill_chase(uint64_t *a, size_t n, uint64_t stride)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += st) a[i] = (i * stride + 64 * 8 / 8 * 3) % n;   /* a permutation-ish jump of a few lines */
}

int main(void)
{
    int nd = device_count(), wi, wallkhz = 0;
    hipDeviceProp_t pr; int blocks, thr = 256;
    double gbs[MAXD], lat[MAXD], latns[MAXD];
    size_t ws[] = { 8u << 20, 32u << 20, 128u << 20, 256u << 20, 512u << 20, (size_t)2 << 30, (size_t)8 << 30 };
    int nws = sizeof ws / sizeof ws[0];
    uint64_t *A[MAXD], *S[MAXD]; unsigned long long *C[MAXD];

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    HIP_CHECK(hipDeviceGetAttribute(&wallkhz, hipDeviceAttributeWallClockRate, 0));
    blocks = pr.multiProcessorCount * 8;
    printf("== mem/infcache : scattered 8-byte gather and pointer chase vs working set ==\n");
    meta("mem/infcache");
    for (int d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMalloc(&A[d], ws[nws - 1])); HIP_CHECK(hipMalloc(&S[d], 64 * 8)); HIP_CHECK(hipMalloc(&C[d], 16));
        HIP_CHECK(hipMemset(A[d], 0, ws[nws - 1]));
    }
    header("working set");
    for (wi = 0; wi < nws; wi++) {
        size_t n = ws[wi] / 8, mask = n - 1, per = (size_t)(1 << 26) * 4 / ((size_t)blocks * thr); /* 2^28 accesses */
#pragma omp parallel num_threads(nd)
        {
            int dev = omp_get_thread_num(), rep; hipEvent_t e0, e1; float ms; double best = 1e300;
            unsigned long long hc[2];
            HIP_CHECK(hipSetDevice(dev));
            timer_events(&e0, &e1);
            for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                HIP_CHECK(hipEventRecord(e0, 0));
                k_gather<<<blocks, thr>>>(A[dev], mask, per, S[dev]);
                HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
                HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); best = dmin(best, ms);
            }
            gbs[dev] = 8.0 * per * blocks * thr / (best * 1e-3) / 1e9;
            /* chase: fill with a stride that hops ~5 lines each time, one wave */
            k_fill_chase<<<blocks, thr>>>(A[dev], n, 41);
            HIP_CHECK(hipDeviceSynchronize());
            k_chase<<<1, 64>>>(A[dev], 200000, S[dev], C[dev], C[dev] + 1);
            HIP_CHECK(hipDeviceSynchronize());
            HIP_CHECK(hipMemcpy(hc, C[dev], 16, hipMemcpyDeviceToHost));
            latns[dev] = (double)hc[1] / (wallkhz / 1000.0) * 1000.0 / 200000.0;   /* ns per hop */
            lat[dev] = (double)hc[0] / 200000.0;                                  /* shader cycles per hop */
        }
        { char nm[40];
          snprintf(nm, sizeof nm, "gather %zu MB useful", ws[wi] >> 20); report_sum(nm, "GB/s", gbs, nd);
          snprintf(nm, sizeof nm, "chase %zu MB", ws[wi] >> 20); report_max(nm, "ns/hop", latns, nd);
          snprintf(nm, sizeof nm, "chase %zu MB", ws[wi] >> 20); report_max(nm, "cyc/hop", lat, nd); }
    }
    printf("\n(useful GB/s x 8 = cache-line traffic; L2 4 MB, Infinity Cache 256 MB per APU)\n");
    return 0;
}

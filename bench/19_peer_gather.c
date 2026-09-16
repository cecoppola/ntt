/* 19_peer_gather - the GPU CRT's access pattern across the fabric.
 *
 * The paper's crt4_gpu has every device read coefficient k from all four
 * residue planes dev_da[c] -- three of them remote -- and combine them.
 * bench/11 showed scattered remote access collapses to 0.18x of contiguous;
 * this pattern is contiguous per plane but interleaves four sources, and all
 * four APUs do it at once, so each xGMI link carries traffic both ways.
 *
 *   gather64    out[i] = f(A0[i], A1[i], A2[i], A3[i]) with 64-bit loads
 *   gather128   same with 128-bit loads
 *   staged      pull the three remote planes into local memory first
 *               (hipMemcpyPeerAsync), then gather locally; the fallback
 *
 * Reported as GB/s of plane data consumed per APU (4 planes x bytes / time).
 *
 * Usage: 19_peer_gather [GiB per plane]
 */
#include "common_ntt.h"

__global__ void k_fill(uint64_t *p, size_t n, uint64_t seed)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) p[i] = seed * i;
}
__global__ void k_gather64(uint64_t *out, const uint64_t *a0, const uint64_t *a1,
                           const uint64_t *a2, const uint64_t *a3, size_t n)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) out[i] = a0[i] + 3 * a1[i] + 5 * a2[i] + 7 * a3[i];
}
__global__ void k_gather128(ulong2 *out, const ulong2 *a0, const ulong2 *a1,
                            const ulong2 *a2, const ulong2 *a3, size_t n)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) {
        ulong2 x0 = a0[i], x1 = a1[i], x2 = a2[i], x3 = a3[i], r;
        r.x = x0.x + 3 * x1.x + 5 * x2.x + 7 * x3.x;
        r.y = x0.y + 3 * x1.y + 5 * x2.y + 7 * x3.y;
        out[i] = r;
    }
}

int main(int argc, char **argv)
{
    double gib = argc > 1 ? atof(argv[1]) : 2.0;
    size_t bytes = (size_t)(gib * 1073741824.0), n = bytes / 8;
    int nd = device_count(), d, j;
    hipDeviceProp_t pr;
    int blocks, thr = 256;
    uint64_t *A[MAXD], *O[MAXD], *ST[MAXD][MAXD];
    double r64[MAXD], r128[MAXD], rst[MAXD], rloc[MAXD], rcpy[MAXD];

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    blocks = pr.multiProcessorCount * 4;
    printf("== 19_peer_gather : 4-way interleaved peer read (the GPU CRT pattern) ==\n");
    meta("19_peer_gather");
    printf("%.1f GiB per plane, %d planes, every APU gathers all of them\n", gib, nd);

    for (d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMalloc(&A[d], bytes));
        HIP_CHECK(hipMalloc(&O[d], bytes));
        for (j = 0; j < nd; j++) {
            HIP_CHECK(hipMalloc(&ST[d][j], bytes));
            if (j != d) { int can = 0; HIP_CHECK(hipDeviceCanAccessPeer(&can, d, j));
                          if (can) (void)hipDeviceEnablePeerAccess(j, 0); }
        }
        k_fill<<<blocks, thr>>>(A[d], n, d + 1);
        HIP_CHECK(hipDeviceSynchronize());
    }

    header("pattern");
#pragma omp parallel num_threads(nd)
    {
        int dev = omp_get_thread_num(), rep;
        hipEvent_t e0, e1;
        float ms; double best;
        const uint64_t *p0 = A[0], *p1 = A[1 % nd], *p2 = A[2 % nd], *p3 = A[3 % nd];
        HIP_CHECK(hipSetDevice(dev));
        timer_events(&e0, &e1);
#define BEST(launch, out, nbytes) do { best = 1e300;                          \
        for (rep = 0; rep < 3; rep++) {                                       \
            _Pragma("omp barrier")                                            \
            HIP_CHECK(hipEventRecord(e0, 0)); launch;                         \
            HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); \
            HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); best = dmin(best, ms); } \
        out[dev] = (double)(nbytes) / (best * 1e-3) / 1e9; } while (0)
        /* local-only reference: four local planes (the staged copies, filled) */
        for (j = 0; j < nd; j++) { k_fill<<<blocks, thr>>>(ST[dev][j], n, j + 1); }
        HIP_CHECK(hipDeviceSynchronize());
        BEST((k_gather64<<<blocks, thr>>>(O[dev], ST[dev][0], ST[dev][1 % nd], ST[dev][2 % nd], ST[dev][3 % nd], n)), rloc, 4.0 * bytes);
        BEST((k_gather64<<<blocks, thr>>>(O[dev], p0, p1, p2, p3, n)), r64, 4.0 * bytes);
        BEST((k_gather128<<<blocks, thr>>>((ulong2 *)O[dev], (const ulong2 *)p0, (const ulong2 *)p1,
                                            (const ulong2 *)p2, (const ulong2 *)p3, n / 2)), r128, 4.0 * bytes);
        /* staged: copy the three remote planes in, then local gather */
        BEST(({ for (j = 0; j < nd; j++) if (j != dev)
                    HIP_CHECK(hipMemcpyPeerAsync(ST[dev][j], dev, A[j], j, bytes, 0)); }), rcpy, 3.0 * bytes);
        BEST(({ for (j = 0; j < nd; j++) if (j != dev)
                    HIP_CHECK(hipMemcpyPeerAsync(ST[dev][j], dev, A[j], j, bytes, 0));
                k_gather64<<<blocks, thr>>>(O[dev], dev == 0 ? A[0] : ST[dev][0],
                    dev == 1 ? A[1] : ST[dev][1 % nd], dev == 2 ? A[2] : ST[dev][2 % nd],
                    dev == 3 ? A[3] : ST[dev][3 % nd], n); }), rst, 4.0 * bytes);
    }
    report_sum("local gather (ref)", "GB/s", rloc, nd);
    report_sum("peer gather 64-bit", "GB/s", r64, nd);
    report_sum("peer gather 128-bit", "GB/s", r128, nd);
    report_sum("peer copy-in only", "GB/s", rcpy, nd);
    report_sum("staged copy+gather", "GB/s", rst, nd);
    printf("\nPASS criterion (PLAN.md B5): peer gather >= 100 GB/s per APU.\n");
    return 0;
}

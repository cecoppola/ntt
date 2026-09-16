/* 03_fabric - corner-turn bandwidth as a function of allocator.
 *
 * A distributed NTT is dominated by one all-to-all transpose per transform, so
 * the fabric rate on the memory we actually intend to use is the single most
 * important number in the design.  The node report measured 92.4 GB/s for a
 * kernel reading peer HBM, but that was hipMalloc memory; the NTT wants
 * hipHostMalloc so it can exceed the 96 GiB/APU device cap.  This measures the
 * penalty, and whether push (remote store) beats pull (remote load).
 *
 * Usage: 03_fabric [GiB per buffer]
 */
#include "common_ntt.h"

#define NMODE 3

/* pull: read src, write dst; used both locally and across the fabric */
__global__ void k_copy(ulong2 *dst, const ulong2 *src, size_t n)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) dst[i] = src[i];
}

/* all-to-all by pulling: recv[j] <- send_j[me], P-1 remote chunks */
__global__ void k_a2a_pull(ulong2 *recv, ulong2 *const *sends,
                           size_t chunk, int me)
{
    int j = blockIdx.y;
    const ulong2 *s = sends[j] + (size_t)me * chunk;
    ulong2 *r = recv + (size_t)j * chunk;
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < chunk; i += stride) r[i] = s[i];
}

/* all-to-all by pushing: recv_j[me] <- send[j] */
__global__ void k_a2a_push(ulong2 *const *recvs, const ulong2 *send,
                           size_t chunk, int me)
{
    int j = blockIdx.y;
    ulong2 *r = recvs[j] + (size_t)me * chunk;
    const ulong2 *s = send + (size_t)j * chunk;
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < chunk; i += stride) r[i] = s[i];
}

static hipError_t alloc_mode(void **p, size_t bytes, int kind)
{
    switch (kind) {
    case 0: return hipMalloc(p, bytes);
    case 1: return hipHostMalloc(p, bytes, hipHostMallocCoherent);
    default: return hipHostMalloc(p, bytes, hipHostMallocNonCoherent);
    }
}

static void free_mode(void *p, int kind)
{
    if (kind == 0) HIP_CHECK(hipFree(p));
    else           HIP_CHECK(hipHostFree(p));
}

int main(int argc, char **argv)
{
    const char *names[NMODE] = { "hipMalloc", "hipHostMalloc coherent",
                                 "hipHostMalloc nonCoherent" };
    ulong2 *S[MAXD], *R[MAXD], **dS[MAXD], **dR[MAXD];
    double loc[MAXD], a2a[MAXD], a2p[MAXD];
    hipDeviceProp_t pr;
    double gib = argc > 1 ? atof(argv[1]) : 4.0;
    size_t bytes, n, chunk;
    int nd = device_count(), blocks, thr = 256, m, d, p;

    bytes = (size_t)(gib * (double)(1ull << 30));
    n = bytes / sizeof(ulong2);
    chunk = n / (size_t)nd;

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    blocks = pr.multiProcessorCount * 2;

    printf("== 03_fabric : corner-turn bandwidth vs allocator ==\n");
    meta("03_fabric");
    printf("%d APUs, %.2f GiB per buffer, chunk %.2f GiB, 128-bit accesses\n",
           nd, gib, chunk * sizeof(ulong2) / 1073741824.0);
    printf("fabric figures count only the bytes that actually crossed.\n");

    for (m = 0; m < NMODE; m++) {
        int ok = 1;
        for (d = 0; d < nd && ok; d++) {
            hipError_t e;
            HIP_CHECK(hipSetDevice(d));
            e = alloc_mode((void **)&S[d], bytes, m);
            if (!e) e = alloc_mode((void **)&R[d], bytes, m);
            if (e) {
                printf("\n-- %s -- ALLOC FAILED: %s\n", names[m],
                       hipGetErrorString(e));
                ok = 0;
                break;
            }
            HIP_CHECK(hipMemset(S[d], d + 1, bytes));
            HIP_CHECK(hipMemset(R[d], 0, bytes));
            for (p = 0; p < nd; p++)
                if (p != d) { hipError_t pe = hipDeviceEnablePeerAccess(p, 0); (void)pe; }
        }
        if (!ok) continue;
        for (d = 0; d < nd; d++) {
            HIP_CHECK(hipSetDevice(d));
            HIP_CHECK(hipMalloc(&dS[d], nd * sizeof(ulong2 *)));
            HIP_CHECK(hipMalloc(&dR[d], nd * sizeof(ulong2 *)));
            HIP_CHECK(hipMemcpy(dS[d], S, nd * sizeof(ulong2 *),
                                hipMemcpyHostToDevice));
            HIP_CHECK(hipMemcpy(dR[d], R, nd * sizeof(ulong2 *),
                                hipMemcpyHostToDevice));
        }

        printf("\n-- %s --\n", names[m]);
        header("pattern");

#pragma omp parallel num_threads(nd)
        {
            int dev = omp_get_thread_num(), rep;
            hipEvent_t t0, t1;
            double best;
            float ms;
            HIP_CHECK(hipSetDevice(dev));
            timer_events(&t0, &t1);

            best = 1e300;
            for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                HIP_CHECK(hipEventRecord(t0, 0));
                k_copy<<<blocks, thr>>>(R[dev], S[dev], n);
                HIP_CHECK(hipEventRecord(t1, 0));
                HIP_CHECK(hipEventSynchronize(t1));
                HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
                best = dmin(best, (double)ms);
            }
            loc[dev] = 2.0 * bytes / (best * 1e-3) / 1e9;

            best = 1e300;
            for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                HIP_CHECK(hipEventRecord(t0, 0));
                k_a2a_pull<<<dim3(blocks, nd), thr>>>(R[dev], dS[dev], chunk, dev);
                HIP_CHECK(hipEventRecord(t1, 0));
                HIP_CHECK(hipEventSynchronize(t1));
                HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
                best = dmin(best, (double)ms);
            }
            a2a[dev] = (double)(nd - 1) * chunk * sizeof(ulong2)
                       / (best * 1e-3) / 1e9;

            best = 1e300;
            for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                HIP_CHECK(hipEventRecord(t0, 0));
                k_a2a_push<<<dim3(blocks, nd), thr>>>(dR[dev], S[dev], chunk, dev);
                HIP_CHECK(hipEventRecord(t1, 0));
                HIP_CHECK(hipEventSynchronize(t1));
                HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
                best = dmin(best, (double)ms);
            }
            a2p[dev] = (double)(nd - 1) * chunk * sizeof(ulong2)
                       / (best * 1e-3) / 1e9;
        }
        report_sum("local copy (r+w)", "GB/s", loc, nd);
        report_sum("all-to-all pull", "GB/s", a2a, nd);
        report_sum("all-to-all push", "GB/s", a2p, nd);

        /* one flow only: APU0 pulls from APU1 */
        {
            hipEvent_t t0, t1;
            double best = 1e300;
            float ms;
            int rep;
            HIP_CHECK(hipSetDevice(0));
            timer_events(&t0, &t1);
            for (rep = 0; rep < 3; rep++) {
                HIP_CHECK(hipEventRecord(t0, 0));
                k_copy<<<blocks, thr>>>(R[0], S[1], n);
                HIP_CHECK(hipEventRecord(t1, 0));
                HIP_CHECK(hipEventSynchronize(t1));
                HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
                best = dmin(best, (double)ms);
            }
            printf("  %-26s %8.1f GB/s (single flow APU0<-APU1)\n",
                   "pair pull", bytes / (best * 1e-3) / 1e9);
        }

        for (d = 0; d < nd; d++) {
            HIP_CHECK(hipSetDevice(d));
            free_mode(S[d], m);
            free_mode(R[d], m);
            HIP_CHECK(hipFree(dS[d]));
            HIP_CHECK(hipFree(dR[d]));
        }
    }
    return 0;
}

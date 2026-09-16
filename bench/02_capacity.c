/* 02_capacity - how many bytes can the NTT own, and how fast are they?
 *
 * The digit count of the whole computation is set by node memory capacity, so
 * this establishes (a) the largest total footprint that can be allocated and
 * first-touched, (b) its GPU read/write bandwidth, and (c) whether the
 * 96 GiB/APU device-allocator cap applies to hipHostMalloc (the ROCm MI300A
 * tuning guide says it does not).
 *
 * Usage: 02_capacity <GiB per APU> [host|hostc|device|managed]
 */
#include "common_ntt.h"

__global__ void k_touch(uint64_t *p, size_t n, uint64_t seed)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) p[i] = seed + i;
}

__global__ void k_triad(uint64_t *a, const uint64_t *b, const uint64_t *c,
                        size_t n, uint64_t s)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) a[i] = b[i] + s * c[i];
}

__global__ void k_read(const uint64_t *b, size_t n, uint64_t *sink)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, a;
    for (; i + 3 * stride < n; i += 4 * stride) {
        a0 += b[i];
        a1 += b[i + stride];
        a2 += b[i + 2 * stride];
        a3 += b[i + 3 * stride];
    }
    a = a0 + a1 + a2 + a3;
    if (a == 0xdeadbeefULL) *sink = a;
}

static hipError_t alloc_mode(void **p, size_t bytes, int kind)
{
    switch (kind) {
    case 0: return hipHostMalloc(p, bytes, hipHostMallocNonCoherent);
    case 1: return hipHostMalloc(p, bytes, hipHostMallocCoherent);
    case 2: return hipMalloc(p, bytes);
    default: return hipMallocManaged(p, bytes);
    }
}

int main(int argc, char **argv)
{
    uint64_t *A[MAXD], *B[MAXD], *C[MAXD], *S[MAXD];
    double talloc[MAXD], ttouch[MAXD], rt[MAXD], rr[MAXD], rp[MAXD];
    hipDeviceProp_t pr;
    double gib = argc > 1 ? atof(argv[1]) : 100.0;
    const char *mode = argc > 2 ? argv[2] : "host";
    size_t total, n, bytes;
    int nd = device_count(), blocks, thr = 256, kind = 0;

    if (!strcmp(mode, "host"))         kind = 0;
    else if (!strcmp(mode, "hostc"))   kind = 1;
    else if (!strcmp(mode, "device"))  kind = 2;
    else                               kind = 3;

    total = (size_t)(gib * (double)(1ull << 30));
    n = total / (3 * sizeof(uint64_t));
    bytes = n * sizeof(uint64_t);

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    blocks = pr.multiProcessorCount * 4;

    printf("== 02_capacity : %.1f GiB/APU (%.1f GiB node) with %s ==\n",
           gib, gib * nd, mode);
    meta("02_capacity");
    printf("per APU: 3 arrays x %.2f GiB;  %zu u64 each\n",
           bytes / 1073741824.0, n);

    header("metric");

#pragma omp parallel num_threads(nd)
    {
        int d = omp_get_thread_num();
        double t0, t1, t2;
        HIP_CHECK(hipSetDevice(d));
        t0 = omp_get_wtime();
        HIP_CHECK(alloc_mode((void **)&A[d], bytes, kind));
        HIP_CHECK(alloc_mode((void **)&B[d], bytes, kind));
        HIP_CHECK(alloc_mode((void **)&C[d], bytes, kind));
        HIP_CHECK(hipMalloc(&S[d], sizeof(uint64_t)));
        t1 = omp_get_wtime();
        k_touch<<<blocks, thr>>>(A[d], n, 1);
        k_touch<<<blocks, thr>>>(B[d], n, 2);
        k_touch<<<blocks, thr>>>(C[d], n, 3);
        HIP_CHECK(hipDeviceSynchronize());
        t2 = omp_get_wtime();
        talloc[d] = t1 - t0;
        ttouch[d] = t2 - t1;
    }
    report_max("alloc wall (s)", "s", talloc, nd);
    report_max("GPU first-touch (s)", "s", ttouch, nd);

#pragma omp parallel num_threads(nd)
    {
        int d = omp_get_thread_num(), rep, peer;
        hipEvent_t t0, t1;
        double best;
        float ms;
        HIP_CHECK(hipSetDevice(d));
        timer_events(&t0, &t1);

        best = 1e300;
        for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
            HIP_CHECK(hipEventRecord(t0, 0));
            k_triad<<<blocks, thr>>>(A[d], B[d], C[d], n, 3);
            HIP_CHECK(hipEventRecord(t1, 0));
            HIP_CHECK(hipEventSynchronize(t1));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            best = dmin(best, (double)ms);
        }
        rt[d] = 3.0 * bytes / (best * 1e-3) / 1e9;

        best = 1e300;
        for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
            HIP_CHECK(hipEventRecord(t0, 0));
            k_read<<<blocks, thr>>>(B[d], n, S[d]);
            HIP_CHECK(hipEventRecord(t1, 0));
            HIP_CHECK(hipEventSynchronize(t1));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            best = dmin(best, (double)ms);
        }
        rr[d] = (double)bytes / (best * 1e-3) / 1e9;

        peer = (d + 1) % nd;
        if (peer != d) {
            int can = 0;
            hipError_t pe = hipDeviceCanAccessPeer(&can, d, peer);
            if (!pe && can) pe = hipDeviceEnablePeerAccess(peer, 0);
            (void)pe;
        }
        best = 1e300;
        for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
            HIP_CHECK(hipEventRecord(t0, 0));
            k_read<<<blocks, thr>>>(B[peer], n, S[d]);
            HIP_CHECK(hipEventRecord(t1, 0));
            HIP_CHECK(hipEventSynchronize(t1));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            best = dmin(best, (double)ms);
        }
        rp[d] = (double)bytes / (best * 1e-3) / 1e9;
    }
    report_sum("triad", "GB/s", rt, nd);
    report_sum("read", "GB/s", rr, nd);
    report_sum("kernel peer read (ring)", "GB/s", rp, nd);

    printf("\nOK: %.1f GiB node allocated, touched and read.\n",
           3.0 * bytes * nd / 1073741824.0);
    return 0;
}

/* 11_fabric_ntt - the interconnect, for the one pattern a distributed NTT uses.
 *
 * A four-step NTT split over 4 APUs needs exactly one all-to-all corner turn
 * per transform, moving 3/4 of a plane.  bench/03 established that pushing
 * (remote stores) beats pulling by 1.75x at 697 GB/s.  This pins down the
 * remaining parameters:
 *
 *   width      64 / 128 / 256-bit accesses across the fabric
 *   chunk      how large a contiguous run must be before the fabric saturates,
 *              which sets the smallest four-step tile that is worth splitting
 *   stride     scattered remote access, the thing to never do
 *   overlap    whether a corner turn hides behind butterflies.  The node report
 *              measured 16% overlap for hipMemcpyPeerAsync (a blit kernel that
 *              occupies CUs); a push kernel may behave differently, and the
 *              answer decides whether fabric time adds to or maxes with compute.
 *
 * Usage: 11_fabric_ntt [GiB per buffer]
 */
#include "common_ntt.h"

#define P1 0x3FFEDF0000000001ULL

/* all-to-all push at three access widths */
__global__ void k_push64(uint64_t *const *recvs, const uint64_t *send,
                         size_t chunk, int me)
{
    int j = blockIdx.y;
    uint64_t *r = recvs[j] + (size_t)me * chunk;
    const uint64_t *s = send + (size_t)j * chunk;
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t st = (size_t)gridDim.x * blockDim.x;
    for (; i < chunk; i += st) r[i] = s[i];
}

__global__ void k_push128(ulong2 *const *recvs, const ulong2 *send,
                          size_t chunk, int me)
{
    int j = blockIdx.y;
    ulong2 *r = recvs[j] + (size_t)me * chunk;
    const ulong2 *s = send + (size_t)j * chunk;
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t st = (size_t)gridDim.x * blockDim.x;
    for (; i < chunk; i += st) r[i] = s[i];
}

__global__ void k_push256(ulong4 *const *recvs, const ulong4 *send,
                          size_t chunk, int me)
{
    int j = blockIdx.y;
    ulong4 *r = recvs[j] + (size_t)me * chunk;
    const ulong4 *s = send + (size_t)j * chunk;
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t st = (size_t)gridDim.x * blockDim.x;
    for (; i < chunk; i += st) r[i] = s[i];
}

/* contiguous runs of `run` elements, then jump: models a tiled corner turn */
__global__ void k_push_chunk(ulong2 *const *recvs, const ulong2 *send,
                             size_t chunk, int me, int run)
{
    int j = blockIdx.y;
    ulong2 *r = recvs[j] + (size_t)me * chunk;
    const ulong2 *s = send + (size_t)j * chunk;
    size_t tiles = chunk / (size_t)run;
    size_t t = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t st = (size_t)gridDim.x * blockDim.x;
    for (; t < tiles * (size_t)run; t += st) {
        size_t tile = t / (size_t)run, off = t % (size_t)run;
        size_t src = tile * (size_t)run + off;
        size_t dst = (tile ^ 1u) * (size_t)run + off;   /* shuffle the tiles */
        if (dst < chunk) r[dst] = s[src];
    }
}

/* scattered remote store */
__global__ void k_push_stride(ulong2 *const *recvs, const ulong2 *send,
                              size_t chunk, int me, int stride)
{
    int j = blockIdx.y;
    ulong2 *r = recvs[j] + (size_t)me * chunk;
    const ulong2 *s = send + (size_t)j * chunk;
    size_t n = chunk / (size_t)stride;
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t st = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += st) r[i * stride] = s[i * stride];
}

/* butterflies, to run alongside the corner turn */
__global__ __launch_bounds__(256)
void k_bfly(uint64_t *sink, int iters)
{
    uint64_t x[8], c[8], p = P1, p2 = 2 * p, s = 0;
    int t = blockIdx.x * blockDim.x + threadIdx.x, i, it;
#pragma unroll
    for (i = 0; i < 8; i++) { x[i] = t + i; c[i] = 0x9E3779B97F4A7C15ULL * (i + 1); }
    for (it = 0; it < iters; it++) {
#pragma unroll
        for (i = 0; i < 8; i++) {
            uint64_t u = x[i], q, v;
            if (u >= p2) u -= p2;
            q = __umul64hi(c[i], x[i]);
            v = c[i] * x[i] - q * p;
            x[i] = (u + v) ^ (u - v + p2);
        }
    }
#pragma unroll
    for (i = 0; i < 8; i++) s += x[i];
    if (s == 0xdeadbeefULL) *sink = s;
}

int main(int argc, char **argv)
{
    ulong2 *S[MAXD], *R[MAXD], **dR[MAXD];
    uint64_t *sink[MAXD];
    double v[MAXD];
    hipDeviceProp_t pr;
    hipStream_t sc[MAXD], sf[MAXD];
    double gib = argc > 1 ? atof(argv[1]) : 4.0;
    size_t bytes, n, chunk;
    int nd = device_count(), blocks, thr = 256, d, p, i;
    static const int runs[] = { 8, 32, 128, 512, 4096, 65536 };
    static const int strides[] = { 1, 2, 4, 8, 16 };

    bytes = (size_t)(gib * (double)(1ull << 30));
    n = bytes / sizeof(ulong2);
    chunk = n / (size_t)nd;

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    blocks = pr.multiProcessorCount * 2;

    printf("== 11_fabric_ntt : interconnect for the NTT corner turn ==\n");
    meta("11_fabric_ntt");
    printf("%d APUs, %.2f GiB/buffer, chunk %.2f GiB; all figures count only\n",
           nd, gib, chunk * sizeof(ulong2) / 1073741824.0);
    printf("the bytes that crossed.  bench/03 reference: push 697 GB/s node.\n");

    for (d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMalloc(&S[d], bytes));
        HIP_CHECK(hipMalloc(&R[d], bytes));
        HIP_CHECK(hipMalloc(&sink[d], 64));
        HIP_CHECK(hipMemset(S[d], d + 1, bytes));
        HIP_CHECK(hipMemset(R[d], 0, bytes));
        for (p = 0; p < nd; p++)
            if (p != d) { hipError_t e = hipDeviceEnablePeerAccess(p, 0); (void)e; }
        HIP_CHECK(hipStreamCreate(&sc[d]));
        HIP_CHECK(hipStreamCreate(&sf[d]));
    }
    for (d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMalloc(&dR[d], nd * sizeof(ulong2 *)));
        HIP_CHECK(hipMemcpy(dR[d], R, nd * sizeof(ulong2 *), hipMemcpyHostToDevice));
    }

#define SWEEP(LABEL, LAUNCH, MOVED)                                           \
    do {                                                                      \
        _Pragma("omp parallel num_threads(nd)")                               \
        {                                                                     \
            int dev = omp_get_thread_num(), rep;                              \
            hipEvent_t t0, t1;                                                \
            double best = 1e300;                                              \
            float ms;                                                         \
            HIP_CHECK(hipSetDevice(dev));                                     \
            timer_events(&t0, &t1);                                           \
            for (rep = 0; rep < 3; rep++) {                                   \
                _Pragma("omp barrier")                                        \
                HIP_CHECK(hipEventRecord(t0, 0));                             \
                LAUNCH;                                                       \
                HIP_CHECK(hipEventRecord(t1, 0));                             \
                HIP_CHECK(hipEventSynchronize(t1));                           \
                HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));                  \
                best = dmin(best, (double)ms);                                \
            }                                                                 \
            v[dev] = (MOVED) / (best * 1e-3) / 1e9;                           \
        }                                                                     \
        report_sum(LABEL, "GB/s", v, nd);                                     \
    } while (0)

    header("access width");
    SWEEP("push  64-bit",
          (k_push64<<<dim3(blocks, nd), thr>>>((uint64_t *const *)dR[dev],
              (const uint64_t *)S[dev], chunk * 2, dev)),
          (double)(nd - 1) * chunk * sizeof(ulong2));
    SWEEP("push 128-bit",
          (k_push128<<<dim3(blocks, nd), thr>>>(dR[dev], S[dev], chunk, dev)),
          (double)(nd - 1) * chunk * sizeof(ulong2));
    SWEEP("push 256-bit",
          (k_push256<<<dim3(blocks, nd), thr>>>((ulong4 *const *)dR[dev],
              (const ulong4 *)S[dev], chunk / 2, dev)),
          (double)(nd - 1) * (chunk / 2) * sizeof(ulong4));

    printf("\n-- contiguous run length (128-bit accesses) --\n");
    header("run x 16 B");
    for (i = 0; i < 6; i++) {
        char lab[64];
        int run = runs[i];
        sprintf(lab, "%d B runs", run * 16);
        SWEEP(lab, (k_push_chunk<<<dim3(blocks, nd), thr>>>(dR[dev], S[dev],
                       chunk, dev, run)),
              (double)(nd - 1) * chunk * sizeof(ulong2));
    }

    printf("\n-- scattered remote stores --\n");
    header("stride x 16 B");
    for (i = 0; i < 5; i++) {
        char lab[64];
        int st = strides[i];
        sprintf(lab, "stride %d", st);
        SWEEP(lab, (k_push_stride<<<dim3(blocks, nd), thr>>>(dR[dev], S[dev],
                       chunk, dev, st)),
              (double)(nd - 1) * (chunk / st) * sizeof(ulong2));
    }

    /* overlap: corner turn concurrently with butterflies, on separate streams */
    printf("\n-- does a corner turn hide behind butterflies? --\n");
    {
        double tf = 0, tc = 0, tb = 0;
        int bf_iters = 4000;
#pragma omp parallel num_threads(nd)
        {
            int dev = omp_get_thread_num();
            hipEvent_t t0, t1;
            float ms;
            HIP_CHECK(hipSetDevice(dev));
            timer_events(&t0, &t1);

#pragma omp barrier
            HIP_CHECK(hipEventRecord(t0, 0));
            k_push128<<<dim3(blocks, nd), thr, 0, sf[dev]>>>(dR[dev], S[dev], chunk, dev);
            HIP_CHECK(hipEventRecord(t1, sf[dev]));
            HIP_CHECK(hipStreamSynchronize(sf[dev]));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            if (dev == 0) tf = ms;

#pragma omp barrier
            HIP_CHECK(hipEventRecord(t0, 0));
            k_bfly<<<blocks, thr, 0, sc[dev]>>>(sink[dev], bf_iters);
            HIP_CHECK(hipEventRecord(t1, sc[dev]));
            HIP_CHECK(hipStreamSynchronize(sc[dev]));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            if (dev == 0) tc = ms;

#pragma omp barrier
            HIP_CHECK(hipEventRecord(t0, 0));
            k_push128<<<dim3(blocks, nd), thr, 0, sf[dev]>>>(dR[dev], S[dev], chunk, dev);
            k_bfly<<<blocks, thr, 0, sc[dev]>>>(sink[dev], bf_iters);
            HIP_CHECK(hipStreamSynchronize(sf[dev]));
            HIP_CHECK(hipStreamSynchronize(sc[dev]));
            HIP_CHECK(hipEventRecord(t1, 0));
            HIP_CHECK(hipEventSynchronize(t1));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            if (dev == 0) tb = ms;
        }
        printf("  corner turn alone   %8.2f ms\n", tf);
        printf("  butterflies alone   %8.2f ms\n", tc);
        printf("  concurrent          %8.2f ms   (serial would be %.2f)\n", tb, tf + tc);
        printf("  overlap efficiency  %8.0f %%   (100%% = the shorter one is free)\n",
               100.0 * (tf + tc - tb) / dmin(tf, tc));
    }
    return 0;
}

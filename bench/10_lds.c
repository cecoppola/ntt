/* 10_lds - LDS and cross-lane units, for the access patterns an NTT uses.
 *
 * The register-blocked NTT uses LDS purely as a transpose buffer between
 * groups of register-resident stages, so what matters is: bandwidth by access
 * width, the real cost of each bank-conflict degree, the cost of a barrier,
 * and whether cross-lane shuffles (ds_bpermute) beat LDS for the intra-wave
 * part of an exchange.
 *
 * Conflict classes: an 8-byte element occupies banks 2e, 2e+1 of 32, so its
 * class is e mod 16 and 64 lanes need at least 4 LDS cycles.  A stride s in
 * elements gives 16/gcd(s,16) distinct classes.
 *
 * Usage: 10_lds [iters]
 */
#include "common_ntt.h"

#define SZ32  4096                 /* 16 KiB, matching the NTT kernel */
#define SZ64  2048
#define SZ128 1024
#define THREADS 256

__global__ __launch_bounds__(THREADS)
void k_bw32(uint32_t *sink, int iters, int stride)
{
    __shared__ uint32_t s[SZ32];
    int tid = threadIdx.x, i, it;
    uint32_t acc = tid;
    for (i = tid; i < SZ32; i += THREADS) s[i] = i;
    __syncthreads();
    for (it = 0; it < iters; it++) {
        int idx = ((tid * stride) + it) & (SZ32 - 1);
        acc ^= s[idx];
        s[(idx + 257) & (SZ32 - 1)] = acc;
    }
    if (acc == 0xdeadbeefu) *sink = acc;
}

__global__ __launch_bounds__(THREADS)
void k_bw64(uint64_t *sink, int iters, int stride)
{
    __shared__ uint64_t s[SZ64];
    int tid = threadIdx.x, i, it;
    uint64_t acc = tid;
    for (i = tid; i < SZ64; i += THREADS) s[i] = i;
    __syncthreads();
    for (it = 0; it < iters; it++) {
        int idx = ((tid * stride) + it) & (SZ64 - 1);
        acc ^= s[idx];
        s[(idx + 257) & (SZ32 - 1)] = acc;
    }
    if (acc == 0xdeadbeefULL) *sink = acc;
}

__global__ __launch_bounds__(THREADS)
void k_bw128(uint64_t *sink, int iters, int stride)
{
    __shared__ ulong2 s[SZ128];
    int tid = threadIdx.x, i, it;
    ulong2 acc;
    acc.x = tid; acc.y = tid;
    for (i = tid; i < SZ128; i += THREADS) { s[i].x = i; s[i].y = i; }
    __syncthreads();
    for (it = 0; it < iters; it++) {
        int idx = ((tid * stride) + it) & (SZ128 - 1);
        ulong2 v = s[idx];
        acc.x ^= v.x; acc.y ^= v.y;
        s[(idx + 257) & (SZ128 - 1)] = acc;
    }
    if (acc.x == 0xdeadbeefULL) *sink = acc.x;
}

/* cross-lane: ds_bpermute / dpp, no barrier and no bank conflicts by design */
__global__ __launch_bounds__(THREADS)
void k_shfl32(uint32_t *sink, int iters, int mask)
{
    uint32_t x[8];
    int it, i;
#pragma unroll
    for (i = 0; i < 8; i++) x[i] = threadIdx.x + i + 1;
    for (it = 0; it < iters; it++) {
#pragma unroll
        for (i = 0; i < 8; i++) x[i] = __shfl_xor(x[i], mask ^ (it & 7), 64) + 1;
    }
    { uint32_t a = 0; for (i = 0; i < 8; i++) a += x[i];
      if (a == 0xdeadbeefu) *sink = a; }
}

__global__ __launch_bounds__(THREADS)
void k_shfl64(uint64_t *sink, int iters, int mask)
{
    uint64_t x[8];
    int it, i;
#pragma unroll
    for (i = 0; i < 8; i++) x[i] = threadIdx.x + i + 1;
    for (it = 0; it < iters; it++) {
#pragma unroll
        for (i = 0; i < 8; i++) x[i] = __shfl_xor(x[i], mask ^ (it & 7), 64) + 1;
    }
    { uint64_t a = 0; for (i = 0; i < 8; i++) a += x[i];
      if (a == 0xdeadbeefULL) *sink = a; }
}

/* barrier cost: one LDS touch plus a barrier per iteration */
__global__ __launch_bounds__(THREADS)
void k_barrier(uint64_t *sink, int iters, int unused)
{
    __shared__ uint64_t s[THREADS];
    int tid = threadIdx.x, it;
    uint64_t acc = tid;
    (void)unused;
    s[tid] = tid;
    __syncthreads();
    for (it = 0; it < iters; it++) {
        s[tid] = acc;
        __syncthreads();
        acc += s[(tid + 1) & (THREADS - 1)];
        __syncthreads();
    }
    if (acc == 0xdeadbeefULL) *sink = acc;
}

/* dependent chain through LDS: latency, not bandwidth */
__global__ __launch_bounds__(THREADS)
void k_lat(uint64_t *sink, int iters, int unused)
{
    __shared__ uint64_t s[SZ64];
    int tid = threadIdx.x, i, it;
    uint64_t idx = tid;
    (void)unused;
    for (i = tid; i < SZ64; i += THREADS) s[i] = (i * 1103515245u + 12345u) & (SZ64 - 1);
    __syncthreads();
    for (it = 0; it < iters; it++) idx = s[idx];
    if (idx == 0xdeadbeefULL) *sink = idx;
}

typedef void (*kk)(void *, int, int);

static double run(const void *k, int blocks, int iters, int arg, void *sink)
{
    hipEvent_t t0, t1;
    double best = 1e300;
    float ms;
    int rep;
    timer_events(&t0, &t1);
    for (rep = 0; rep < 3; rep++) {
        ((kk)k)<<<blocks, THREADS>>>(sink, 16, arg);
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipEventRecord(t0, 0));
        ((kk)k)<<<blocks, THREADS>>>(sink, iters, arg);
        HIP_CHECK(hipEventRecord(t1, 0));
        HIP_CHECK(hipEventSynchronize(t1));
        HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
        best = dmin(best, (double)ms);
    }
    return best * 1e-3;
}

int main(int argc, char **argv)
{
    hipDeviceProp_t pr;
    void *sink;
    double ghz, lanecyc, acc, sec;
    int iters = argc > 1 ? atoi(argv[1]) : 20000;
    int blocks, i;
    static const int strides[] = { 1, 2, 4, 8, 16, 17, 32, 33 };

    HIP_CHECK(hipSetDevice(0));
    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    blocks = pr.multiProcessorCount * 4;
    ghz = pr.clockRate / 1.0e6;
    lanecyc = (double)pr.multiProcessorCount * 64.0 * ghz * 1e9;
    acc = (double)blocks * THREADS * iters;
    HIP_CHECK(hipMalloc(&sink, 64));

    printf("== 10_lds : LDS and cross-lane units for NTT access patterns ==\n");
    meta("10_lds");
    printf("%s  %d CUs @ %.2f GHz, %d blocks x %d threads, 16 KiB LDS/block\n",
           pr.gcnArchName, pr.multiProcessorCount, ghz, blocks, THREADS);
    printf("hardware ceiling ~128 B/clk/CU = %.1f TB/s per APU; node report read: 47.7\n\n",
           pr.multiProcessorCount * 128.0 * ghz * 1e9 / 1e12);

    printf("-- bandwidth by access width (stride 1, conflict-free) --\n");
    printf("%-22s %12s %12s %10s\n", "width", "GB/s (1 APU)", "Gaccess/s", "cyc/access");
    {
        struct { const char *n; const void *k; int w; } W[3] = {
            { "32-bit  (ds_b32)",  (const void *)k_bw32,  4 },
            { "64-bit  (ds_b64)",  (const void *)k_bw64,  8 },
            { "128-bit (ds_b128)", (const void *)k_bw128, 16 }
        };
        for (i = 0; i < 3; i++) {
            sec = run(W[i].k, blocks, iters, 1, sink);
            printf("%-22s %12.1f %12.1f %10.2f\n", W[i].n,
                   2.0 * acc * W[i].w / sec / 1e9, acc / sec / 1e9,
                   lanecyc / (acc / sec));
        }
    }

    printf("\n-- 64-bit bandwidth by stride (conflict class = e mod 16) --\n");
    printf("%-10s %10s %12s %12s %10s\n", "stride", "classes", "way", "GB/s", "vs stride 1");
    {
        double base = 0;
        for (i = 0; i < 8; i++) {
            int s = strides[i], g = 16, cls, tmp = s % 16, a, b2;
            a = tmp ? tmp : 16; b2 = 16;
            while (a) { int t = b2 % a; b2 = a; a = t; }
            g = b2 ? b2 : 16;
            cls = 16 / g;
            sec = run((const void *)k_bw64, blocks, iters, s, sink);
            {
                double gbs = 2.0 * acc * 8 / sec / 1e9;
                if (i == 0) base = gbs;
                printf("%-10d %10d %12d %12.1f %10.2fx\n", s, cls, 64 / cls, gbs, gbs / base);
            }
        }
    }

    printf("\n-- cross-lane and synchronisation --\n");
    sec = run((const void *)k_shfl32, blocks, iters, 1, sink);
    printf("%-30s %10.2f cyc/op  (%.1f Gop/s)\n", "__shfl_xor 32-bit",
           lanecyc / (8 * acc / sec), 8 * acc / sec / 1e9);
    sec = run((const void *)k_shfl64, blocks, iters, 1, sink);
    printf("%-30s %10.2f cyc/op  (%.1f Gop/s)\n", "__shfl_xor 64-bit",
           lanecyc / (8 * acc / sec), 8 * acc / sec / 1e9);
    sec = run((const void *)k_barrier, blocks, iters / 4, 0, sink);
    printf("%-30s %10.2f cyc      (per barrier, 2 per iter + 2 LDS)\n", "__syncthreads",
           lanecyc / ((double)blocks * THREADS * (iters / 4) * 2 / sec) );
    sec = run((const void *)k_lat, blocks, iters, 0, sink);
    printf("%-30s %10.2f cyc      (per resident wave-slot; x4 waves/SIMD for true latency)\n",
           "LDS load-to-use", lanecyc / (acc / sec));
    return 0;
}

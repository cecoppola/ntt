/* 17_hostreg - can we stage memory the way the paper does?
 *
 * The paper's mdev tier keeps hstage_buf[4] = 64 GB of malloc'd host memory,
 * first-touched by OpenMP threads (it says NUMA-interleave policy regresses
 * by 20 s) and pinned with hipHostRegister.  Kernels read pinned host memory
 * directly and results come back by hipMemcpy D2H.  On this node ulimit -l is
 * 31.4 GiB (RESULTS.md 17), so the first question is whether registration of
 * 4 x 16 GiB and of one 64 GiB block succeeds at all.  Then, per placement:
 *
 *   local     first-touched by CPU threads pinned to the APU's own NUMA node
 *   remote    first-touched from the next node over (the wrong-NUMA cost)
 *   interlv   MPOL_INTERLEAVE over all four nodes (the paper's regression)
 *
 * measured concurrently on all four APUs: registration time, kernel read and
 * write bandwidth into the registered memory, and D2H hipMemcpy bandwidth
 * from device memory into it.  hipHostMalloc at the same size is the baseline
 * (bench/02 reached full HBM speed with it).
 *
 * Usage: 17_hostreg [GiB per APU] [big GiB for the single-block test]
 */
#include "common_ntt.h"
#include <sched.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/resource.h>

#define MPOL_BIND 2
#define MPOL_INTERLEAVE 3

__global__ void k_touch(uint64_t *p, size_t n, uint64_t seed)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) p[i] = seed + i;
}
__global__ void k_read(const ulong2 *b, size_t n, uint64_t *sink)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    uint64_t a0 = 0, a1 = 0;
    for (; i < n; i += stride) { ulong2 v = b[i]; a0 += v.x; a1 += v.y; }
    if (a0 + a1 == 0xdeadbeefULL) *sink = a0;
}

static long mbind_(void *a, size_t len, int mode, unsigned long mask)
{ return syscall(SYS_mbind, a, len, mode, &mask, 8 * sizeof mask, 0); }

/* pin the calling thread to the CPUs of NUMA node `node` */
static void pin_to_node(int node)
{
    char path[80], buf[512];
    FILE *f;
    cpu_set_t set;
    CPU_ZERO(&set);
    snprintf(path, sizeof path, "/sys/devices/system/node/node%d/cpulist", node);
    f = fopen(path, "r");
    if (f && fgets(buf, sizeof buf, f)) {
        char *tok = strtok(buf, ",\n");
        while (tok) {
            int lo, hi;
            if (sscanf(tok, "%d-%d", &lo, &hi) == 2) { int c; for (c = lo; c <= hi; c++) CPU_SET(c, &set); }
            else if (sscanf(tok, "%d", &lo) == 1) CPU_SET(lo, &set);
            tok = strtok(NULL, ",\n");
        }
    }
    if (f) fclose(f);
    sched_setaffinity(0, sizeof set, &set);
}

/* first-touch with 1 GiB stripes so page placement follows the policy */
static void cpu_touch(uint64_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i += 512) p[i] = i;      /* one write per 4 KiB page */
}

int main(int argc, char **argv)
{
    double gib = argc > 1 ? atof(argv[1]) : 16.0;
    double big = argc > 2 ? atof(argv[2]) : 64.0;
    size_t bytes = (size_t)(gib * 1073741824.0), n = bytes / 8;
    int nd = device_count(), mode, d;
    hipDeviceProp_t pr;
    int blocks, thr = 256;
    static const char *modes[4] = { "hipHostMalloc", "reg local", "reg remote", "reg interlv" };
    double treg[MAXD], rr[MAXD], rw[MAXD], rd2h[MAXD];

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    blocks = pr.multiProcessorCount * 4;
    printf("== 17_hostreg : hipHostRegister staging, %.0f GiB per APU (%.0f GiB node) ==\n", gib, gib * nd);
    meta("17_hostreg");

    for (mode = 0; mode < 4; mode++) {
        uint64_t *H[MAXD], *D[MAXD], *S[MAXD], *HD[MAXD];
        int ok = 1;
        printf("\n-- %s --\n", modes[mode]);
#pragma omp parallel num_threads(nd) reduction(&&:ok)
        {
            int dev = omp_get_thread_num(), rep;
            double t0, t1;
            hipEvent_t e0, e1;
            float ms; double best;
            HIP_CHECK(hipSetDevice(dev));
            HIP_CHECK(hipMalloc(&D[dev], bytes));
            HIP_CHECK(hipMalloc(&S[dev], 8));
            k_touch<<<blocks, thr>>>(D[dev], n, 7);
            HIP_CHECK(hipDeviceSynchronize());
            timer_events(&e0, &e1);
            t0 = omp_get_wtime();
            if (mode == 0) {
                HIP_CHECK(hipHostMalloc((void **)&H[dev], bytes, hipHostMallocNonCoherent));
                HD[dev] = H[dev];
                k_touch<<<blocks, thr>>>(H[dev], n, 3);
                HIP_CHECK(hipDeviceSynchronize());
                treg[dev] = omp_get_wtime() - t0;
            } else {
                hipError_t e;
                H[dev] = (uint64_t *)aligned_alloc(1 << 21, bytes);
                if (!H[dev]) { printf("APU%d: malloc failed\n", dev); ok = 0; }
                else {
                    if (mode == 1) { pin_to_node(dev); mbind_(H[dev], bytes, MPOL_BIND, 1UL << dev); }
                    if (mode == 2) { pin_to_node((dev + 1) % nd); mbind_(H[dev], bytes, MPOL_BIND, 1UL << ((dev + 1) % nd)); }
                    if (mode == 3) { pin_to_node(dev); mbind_(H[dev], bytes, MPOL_INTERLEAVE, (1UL << nd) - 1); }
                    cpu_touch(H[dev], n);
                    t1 = omp_get_wtime();
                    e = hipHostRegister(H[dev], bytes, hipHostRegisterDefault);
                    treg[dev] = omp_get_wtime() - t1;
                    if (e != hipSuccess) {
                        printf("APU%d: hipHostRegister(%.0f GiB) FAILED: %s (touch %.1f s)\n",
                               dev, gib, hipGetErrorString(e), t1 - t0);
                        ok = 0;
                    } else {
                        HIP_CHECK(hipHostGetDevicePointer((void **)&HD[dev], H[dev], 0));
                        printf("APU%d: touch %.1f s, register %.2f s, devptr %s hostptr\n", dev,
                               t1 - t0, treg[dev], HD[dev] == H[dev] ? "==" : "!=");
                    }
                }
            }
#pragma omp barrier
            if (ok) {
#define BEST(launch, out, nbytes) do { best = 1e300;                          \
                for (rep = 0; rep < 3; rep++) {                               \
                    _Pragma("omp barrier")                                    \
                    HIP_CHECK(hipEventRecord(e0, 0)); launch;                 \
                    HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); \
                    HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); best = dmin(best, ms); } \
                out[dev] = (double)(nbytes) / (best * 1e-3) / 1e9; } while (0)
                BEST((k_read<<<blocks, thr>>>((const ulong2 *)HD[dev], n / 2, S[dev])), rr, bytes);
                BEST((k_touch<<<blocks, thr>>>(HD[dev], n, 5)), rw, bytes);
                BEST(HIP_CHECK(hipMemcpyAsync(H[dev], D[dev], bytes, hipMemcpyDeviceToHost, 0)), rd2h, bytes);
            }
#pragma omp barrier
            if (mode == 0) HIP_CHECK(hipHostFree(H[dev]));
            else if (H[dev]) { if (ok) HIP_CHECK(hipHostUnregister(H[dev])); free(H[dev]); }
            HIP_CHECK(hipFree(D[dev])); HIP_CHECK(hipFree(S[dev]));
        }
        if (!ok) { printf("  (skipped bandwidth: allocation or registration failed)\n"); continue; }
        header(modes[mode]);
        report_max("alloc/register (s)", "s", treg, nd);
        report_sum("kernel read", "GB/s", rr, nd);
        report_sum("kernel write", "GB/s", rw, nd);
        report_sum("D2H memcpy", "GB/s", rd2h, nd);
    }

    /* one big block, as a single hipHostRegister */
    {
        size_t bb = (size_t)(big * 1073741824.0);
        uint64_t *H = (uint64_t *)aligned_alloc(1 << 21, bb);
        double t0, t1;
        hipError_t e;
        struct rlimit rl;
        getrlimit(RLIMIT_MEMLOCK, &rl);
        printf("\n-- single %.0f GiB block (ulimit -l is %.1f GiB) --\n", big,
               rl.rlim_cur == RLIM_INFINITY ? -1.0 : rl.rlim_cur / 1073741824.0);
        HIP_CHECK(hipSetDevice(0));
        if (!H) { printf("malloc failed\n"); return 1; }
        mbind_(H, bb, MPOL_INTERLEAVE, (1UL << nd) - 1);
        t0 = omp_get_wtime();
#pragma omp parallel for num_threads(32)
        for (size_t i = 0; i < bb / 8; i += 512) H[i] = i;
        t1 = omp_get_wtime();
        e = hipHostRegister(H, bb, hipHostRegisterDefault);
        printf("touch %.1f s; hipHostRegister(%.0f GiB): %s (%.2f s)\n", t1 - t0, big,
               e == hipSuccess ? "OK" : hipGetErrorString(e), omp_get_wtime() - t1);
        {
            double v[1] = { e == hipSuccess ? 1.0 : 0.0 };
            result("register_64GiB_ok", "bool", v[0], v, 1);
        }
        if (e == hipSuccess) HIP_CHECK(hipHostUnregister(H));
        free(H);
    }
    (void)d;
    return 0;
}

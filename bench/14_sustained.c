/* 14_sustained - two things the review could not settle by argument.
 *
 * 1. TRIVIAL TWIDDLES.  table[m+0] = root^0 = 1 for every stage, so a fixed
 *    set of butterflies multiply by one.  In the register-blocked kernel
 *    group A is FWD3(1,2,4) and w[1] = w[2] = w[4] = 1, making 7 of the 12
 *    butterflies in that group -- 7 of 44 per thread -- pure add/sub.  The
 *    first stage is better still: its inputs are freshly loaded and already
 *    in [0,p), so it needs no lazy correction either.
 *
 * 2. SUSTAINED THROUGHPUT.  Every figure in RESULTS.md is a best-of-3 burst of
 *    a few milliseconds.  The node report calls the absence of any power,
 *    clock or sustained-throughput measurement "the largest single gap in the
 *    study", and notes it cannot tell whether the four APUs share a package
 *    power budget.  The real computation runs for minutes at high VALU
 *    utilisation on all four APUs, so if throughput decays under load every
 *    time estimate in ALGORITHM.md is optimistic.  This runs the kernel
 *    continuously and reports throughput per window.
 *
 * With a second argument, R GiB per APU of hipHostMalloc memory is allocated
 * and first-touched, and the sustained loop rotates the kernel through it in
 * 512 MiB slices, so the run has the full memory footprint of the real
 * computation (PLAN.md B7: 300 s at 64 GiB per APU = 256 GiB node).
 *
 * Usage: 14_sustained [seconds] [resident GiB per APU]
 */
#include "ntt_kernels.h"

#define N       2048
#define THREADS 256
#define RPT     8

/* twiddle is 1 and both operands are known < p: no multiply, no correction */
#define FB1_FRESH(a, b)                                                       \
    do { uint64_t U = x[a], V = x[b];                                         \
         x[a] = U + V; x[b] = U - V + p; } while (0)

/* twiddle is 1, operands in [0,4p): drop the multiply, keep one correction */
#define FB1(a, b)                                                             \
    do { uint64_t U = x[a], V = x[b];                                         \
         if (U >= p2) U -= p2;                                                \
         if (V >= p2) V -= p2;                                                \
         x[a] = U + V; x[b] = U - V + p2; } while (0)

/* group A specialised: w[1]=w[2]=w[4]=1, and stage one sees fresh inputs */
#define FWD3_A()                                                              \
    do { FB1_FRESH(0,4); FB1_FRESH(1,5); FB1_FRESH(2,6); FB1_FRESH(3,7);      \
         FB1(0,2); FB1(1,3); FB(4,6,3); FB(5,7,3);                            \
         FB1(0,1); FB(2,3,5); FB(4,5,6); FB(6,7,7); } while (0)

#define TAIL()                                                                \
    do {                                                                      \
        int span, base, grp, b2;                                              \
        EXW(tid + 256 * k);                                                   \
        span = tid >> 5; base = tid & 31;                                     \
        EXR(256 * span + base + 32 * k);                                      \
        FWD3(8 + span, 16 + 2 * span, 32 + 4 * span);                         \
        EXW(256 * span + base + 32 * k);                                      \
        grp = tid >> 2; b2 = tid & 3;                                         \
        EXR(32 * grp + b2 + 4 * k);                                           \
        FWD3(64 + grp, 128 + 2 * grp, 256 + 4 * grp);                         \
        EXW(32 * grp + b2 + 4 * k);                                           \
        EXR(8 * tid + k);                                                     \
        FB(0,2,512+2*tid+0); FB(1,3,512+2*tid+0);                             \
        FB(4,6,512+2*tid+1); FB(5,7,512+2*tid+1);                             \
        FB(0,1,1024+4*tid+0); FB(2,3,1024+4*tid+1);                           \
        FB(4,5,1024+4*tid+2); FB(6,7,1024+4*tid+3);                           \
    } while (0)

#define STORE()                                                               \
    do { _Pragma("unroll")                                                    \
         for (k = 0; k < RPT; k++) {                                          \
             uint64_t z = x[k];                                               \
             if (z >= p2) z -= p2;                                            \
             if (z >= p)  z -= p;                                             \
             g[8 * tid + k] = z; } } while (0)

__global__ __launch_bounds__(THREADS)
void k_base(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N];
    uint64_t *g = data + (size_t)blockIdx.x * N;
    uint64_t x[RPT], p2 = 2 * p;
    int tid = threadIdx.x, k;
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = g[tid + 256 * k];
    FWD3(1, 2, 4);
    TAIL();
    STORE();
}

__global__ __launch_bounds__(THREADS)
void k_triv(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N];
    uint64_t *g = data + (size_t)blockIdx.x * N;
    uint64_t x[RPT], p2 = 2 * p;
    int tid = threadIdx.x, k;
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = g[tid + 256 * k];
    FWD3_A();
    TAIL();
    STORE();
}

/* ------------------------------ host ------------------------------------ */
int main(int argc, char **argv)
{
    uint64_t p = PRIME, *hw, *hwp, *ha, *hb, *hc;
    uint64_t *dd[MAXD], *dw[MAXD], *dwp[MAXD];
    double rb[MAXD], rt[MAXD];
    hipDeviceProp_t pr;
    double secs = argc > 1 ? atof(argv[1]) : 60.0;
    double rgib = argc > 2 ? atof(argv[2]) : 0.0;
    size_t bytes = 512u * 1048576u, npt;
    size_t rbytes = (size_t)(rgib * 1073741824.0), nslice = rbytes / bytes;
    uint64_t *rr[MAXD];
    double tres[MAXD];
    int nd = device_count(), blocks, i, d, ok;

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    npt = bytes / 8;
    blocks = (int)(npt / N);
    printf("== 14_sustained : trivial twiddles, and throughput under load ==\n");
    meta("14_sustained");
    printf("%d APUs, %d blocks x %d threads, N=%d, %.0f s sustained run, %.0f GiB resident per APU\n",
           nd, blocks, THREADS, N, secs, rgib);

    hw = (uint64_t *)malloc(N * 8); hwp = (uint64_t *)malloc(N * 8);
    ha = (uint64_t *)malloc(N * 8); hb = (uint64_t *)malloc(N * 8);
    hc = (uint64_t *)malloc(N * 8);
    build_table(hw, hwp, N, powmod(PROOT, (p - 1) / (uint64_t)N, p), p);
    printf("check w[1]=%llu w[2]=%llu w[4]=%llu (all must be 1)\n",
           (unsigned long long)hw[1], (unsigned long long)hw[2],
           (unsigned long long)hw[4]);
    for (i = 0; i < N; i++) ha[i] = (uint64_t)(i * 2654435761u + 77u) % p;

    for (d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMalloc(&dd[d], bytes));
        HIP_CHECK(hipMalloc(&dw[d], N * 8)); HIP_CHECK(hipMalloc(&dwp[d], N * 8));
        HIP_CHECK(hipMemcpy(dw[d], hw, N * 8, hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dwp[d], hwp, N * 8, hipMemcpyHostToDevice));
        HIP_CHECK(hipMemset(dd[d], 1, bytes));
    }

    HIP_CHECK(hipSetDevice(0));
    HIP_CHECK(hipMemcpy(dd[0], ha, N * 8, hipMemcpyHostToDevice));
    k_base<<<1, THREADS>>>(dd[0], dw[0], dwp[0], p);
    HIP_CHECK(hipDeviceSynchronize());
    HIP_CHECK(hipMemcpy(hb, dd[0], N * 8, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(dd[0], ha, N * 8, hipMemcpyHostToDevice));
    k_triv<<<1, THREADS>>>(dd[0], dw[0], dwp[0], p);
    HIP_CHECK(hipDeviceSynchronize());
    HIP_CHECK(hipMemcpy(hc, dd[0], N * 8, hipMemcpyDeviceToHost));
    ok = 1;
    for (i = 0; i < N; i++) if (hb[i] != hc[i]) { ok = 0; break; }
    printf("VERIFY trivial-twiddle kernel == baseline : %s\n", ok ? "OK" : "FAILED");
    if (!ok) { printf("  index %d: %llu vs %llu\n", i,
                      (unsigned long long)hc[i], (unsigned long long)hb[i]); return 1; }
    for (d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemset(dd[d], 1, bytes)); }

    header("kernel");
#pragma omp parallel num_threads(nd)
    {
        int dev = omp_get_thread_num(), rep;
        hipEvent_t t0, t1;
        double best;
        float ms;
        HIP_CHECK(hipSetDevice(dev));
        timer_events(&t0, &t1);
        best = 1e300;
        for (rep = 0; rep < 5; rep++) {
#pragma omp barrier
            HIP_CHECK(hipEventRecord(t0, 0));
            k_base<<<blocks, THREADS>>>(dd[dev], dw[dev], dwp[dev], p);
            HIP_CHECK(hipEventRecord(t1, 0));
            HIP_CHECK(hipEventSynchronize(t1));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            best = dmin(best, (double)ms);
        }
        rb[dev] = (double)blocks * (N / 2) * 11 / (best * 1e-3) / 1e9;
        best = 1e300;
        for (rep = 0; rep < 5; rep++) {
#pragma omp barrier
            HIP_CHECK(hipEventRecord(t0, 0));
            k_triv<<<blocks, THREADS>>>(dd[dev], dw[dev], dwp[dev], p);
            HIP_CHECK(hipEventRecord(t1, 0));
            HIP_CHECK(hipEventSynchronize(t1));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            best = dmin(best, (double)ms);
        }
        rt[dev] = (double)blocks * (N / 2) * 11 / (best * 1e-3) / 1e9;
    }
    report_sum("baseline", "Gbfly/s", rb, nd);
    report_sum("trivial-twiddle", "Gbfly/s", rt, nd);
    {
        double a = 0, b2 = 0;
        for (i = 0; i < nd; i++) { a += rb[i]; b2 += rt[i]; }
        printf("  speedup %.3fx  (predicted ~1.09x from 7 of 44 butterflies)\n", b2 / a);
    }

    if (nslice > 0) {
#pragma omp parallel num_threads(nd)
        {
            int dev = omp_get_thread_num();
            double t0 = omp_get_wtime();
            HIP_CHECK(hipSetDevice(dev));
            HIP_CHECK(hipHostMalloc((void **)&rr[dev], rbytes, hipHostMallocNonCoherent));
            HIP_CHECK(hipMemset(rr[dev], 1, rbytes));
            HIP_CHECK(hipDeviceSynchronize());
            tres[dev] = omp_get_wtime() - t0;
        }
        report_max("resident alloc+touch (s)", "s", tres, nd);
    }
    printf("\n-- sustained: all 4 APUs, continuous, 5 s windows --\n");
    printf("%8s %14s %10s\n", "window", "node Gbfly/s", "vs first");
    {
        double first = 0;
        int win;
        for (win = 0; win < (int)(secs / 5.0); win++) {
            double gb[MAXD];
#pragma omp parallel num_threads(nd)
            {
                int dev = omp_get_thread_num();
                double t0, t1;
                long iter = 0;
                HIP_CHECK(hipSetDevice(dev));
#pragma omp barrier
                t0 = omp_get_wtime();
                do {
                    int q;
                    for (q = 0; q < 20; q++) {
                        uint64_t *buf = nslice ? rr[dev] + (size_t)((iter + q) % nslice) * (bytes / 8) : dd[dev];
                        k_triv<<<blocks, THREADS>>>(buf, dw[dev], dwp[dev], p);
                    }
                    HIP_CHECK(hipDeviceSynchronize());
                    iter += 20;
                    t1 = omp_get_wtime();
                } while (t1 - t0 < 5.0);
                gb[dev] = (double)iter * blocks * (N / 2) * 11 / (t1 - t0) / 1e9;
            }
            {
                double tot = 0;
                for (i = 0; i < nd; i++) tot += gb[i];
                if (win == 0) first = tot;
                printf("%6ds %14.1f %9.3fx\n", (win + 1) * 5, tot, tot / first);
                if (win == 0 || (win + 1) * 5 == (int)secs) {
                    char nm[32]; snprintf(nm, sizeof nm, "sustained_%ds", (win + 1) * 5);
                    result(nm, "Gbfly/s", tot, gb, nd);
                }
            }
        }
    }
    return 0;
}

/* 22_clock - what clock does the shader actually run at during a burst?
 *
 * bench/14 with amd-smi sampling (RESULTS.md 23) showed every APU pinned at
 * its 550 W package cap at ~1.49 GHz during a 300 s NTT run, not the 2.10 GHz
 * that campaigns 2-3 divided by to get "cycles per op".  Those were
 * millisecond bursts, and power management may not have reacted yet.  This
 * measures the effective shader clock from inside the kernel: clock64() counts
 * shader cycles, wall_clock64() counts a constant-rate timer
 * (hipDeviceAttributeWallClockRate), so their ratio over the kernel's life is
 * the mean sclk that the kernel saw.
 *
 * The workload is the bench/01 Shoup butterfly chain (issue-bound VALU, the
 * heaviest case), run as bursts of 1, 3, 10, 30, 100, 300, 1000 ms on all
 * four APUs at once, and once on a single APU alone.
 *
 * Usage: 22_clock
 */
#include "ntt_kernels.h"

#define CH 8
#define THREADS 256

__global__ __launch_bounds__(THREADS)
void k_burst(uint64_t p, uint64_t c, uint64_t cp, int iters,
             unsigned long long *cyc, unsigned long long *wall, uint64_t *sink)
{
    uint64_t x[CH], p2 = 2 * p;
    int i, j;
    long long c0 = clock64(), w0 = wall_clock64();
    for (j = 0; j < CH; j++) x[j] = (threadIdx.x * 7 + j * 13 + 1) % 1000;
    for (i = 0; i < iters; i++) {
#pragma unroll
        for (j = 0; j < CH; j += 2) {
            uint64_t U = x[j], V;
            if (U >= p2) U -= p2;
            V = smul(c, cp, x[j + 1], p);
            x[j] = U + V; x[j + 1] = U - V + p2;
        }
    }
    {
        uint64_t s = 0; for (j = 0; j < CH; j++) s += x[j];
        if (s == 1) *sink = s;
    }
    if (threadIdx.x == 0) {
        atomicAdd(cyc, (unsigned long long)(clock64() - c0));
        atomicAdd(wall, (unsigned long long)(wall_clock64() - w0));
    }
}

int main(void)
{
    int nd = device_count(), d, bi, wallkhz = 0;
    hipDeviceProp_t pr;
    int blocks;
    static const double target_ms[] = { 1, 3, 10, 30, 100, 300, 1000 };
    int nb = (int)(sizeof target_ms / sizeof target_ms[0]);
    uint64_t p = PRIME, c = p / 3 + 12345, cp = shoup_pre(c, p);
    double mhz[MAXD], ms_[MAXD], rate[MAXD];
    int iters_per_ms = 0;

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    HIP_CHECK(hipDeviceGetAttribute(&wallkhz, hipDeviceAttributeWallClockRate, 0));
    blocks = pr.multiProcessorCount * 8;
    printf("== 22_clock : effective shader clock during compute bursts ==\n");
    meta("22_clock");
    printf("wall clock %d kHz, max sclk %d MHz, %d blocks x %d threads\n",
           wallkhz, pr.clockRate / 1000, blocks, THREADS);

    /* calibrate iterations per ms on APU0 */
    {
        hipEvent_t e0, e1; float ms;
        unsigned long long *dc; uint64_t *ds;
        HIP_CHECK(hipSetDevice(0));
        HIP_CHECK(hipMalloc(&dc, 16)); HIP_CHECK(hipMalloc(&ds, 8));
        timer_events(&e0, &e1);
        k_burst<<<blocks, THREADS>>>(p, c, cp, 1000, dc, dc + 1, ds);
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipEventRecord(e0, 0));
        k_burst<<<blocks, THREADS>>>(p, c, cp, 1000, dc, dc + 1, ds);
        HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
        HIP_CHECK(hipEventElapsedTime(&ms, e0, e1));
        iters_per_ms = (int)(1000.0 / ms);
        HIP_CHECK(hipFree(dc)); HIP_CHECK(hipFree(ds));
    }

    for (d = 0; d < 2; d++) {   /* d = 0: all APUs together; d = 1: APU0 alone */
        int nrun = d == 0 ? nd : 1;
        printf("\n-- %s --\n", d == 0 ? "all APUs concurrently" : "APU0 alone");
        header("burst");
        for (bi = 0; bi < nb; bi++) {
            int iters = (int)(target_ms[bi] * iters_per_ms);
            char nm[32];
#pragma omp parallel num_threads(nrun)
            {
                int dev = omp_get_thread_num();
                hipEvent_t e0, e1; float ms;
                unsigned long long *dc, h[2]; uint64_t *ds;
                HIP_CHECK(hipSetDevice(dev));
                HIP_CHECK(hipMalloc(&dc, 16)); HIP_CHECK(hipMalloc(&ds, 8));
                HIP_CHECK(hipMemset(dc, 0, 16));
                timer_events(&e0, &e1);
                /* let clocks settle to idle first */
                HIP_CHECK(hipDeviceSynchronize());
#pragma omp barrier
                HIP_CHECK(hipEventRecord(e0, 0));
                k_burst<<<blocks, THREADS>>>(p, c, cp, iters, dc, dc + 1, ds);
                HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
                HIP_CHECK(hipEventElapsedTime(&ms, e0, e1));
                HIP_CHECK(hipMemcpy(h, dc, 16, hipMemcpyDeviceToHost));
                mhz[dev] = (double)h[0] / (double)h[1] * wallkhz / 1000.0;
                ms_[dev] = ms;
                rate[dev] = (double)blocks * THREADS * (CH / 2) * iters / (ms * 1e-3) / 1e9;
                HIP_CHECK(hipFree(dc)); HIP_CHECK(hipFree(ds));
            }
            snprintf(nm, sizeof nm, "%s sclk %.0fms", d ? "solo" : "all", target_ms[bi]);
            report_max(nm, "MHz", mhz, nrun);
            snprintf(nm, sizeof nm, "%s bfly %.0fms", d ? "solo" : "all", target_ms[bi]);
            report_sum(nm, "Gbfly/s", rate, nrun);
            /* sleep so the next burst starts from idle */
            usleep(500000);
        }
    }
    printf("\ncyc/op in RESULTS.md 9-10 assumed 2100 MHz; scale by (measured/2100).\n");
    return 0;
}

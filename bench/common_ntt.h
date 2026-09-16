/* Shared harness for the NTT benchmarks.  Plain C: no STL, no templates,
 * no lambdas.  Compiled with hipcc -x hip. */
#ifndef COMMON_NTT_H
#define COMMON_NTT_H

#include <hip/hip_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <omp.h>
#include <time.h>
#include <unistd.h>

#define MAXD 16

#define HIP_CHECK(x) do {                                                     \
    hipError_t _e = (x);                                                      \
    if (_e != hipSuccess) {                                                   \
        fprintf(stderr, "HIP error: %s at %s:%d\n",                           \
                hipGetErrorString(_e), __FILE__, __LINE__);                   \
        exit(1);                                                              \
    }                                                                         \
} while (0)

static double dmin(double a, double b) { return a < b ? a : b; }
static double dmax(double a, double b) { return a > b ? a : b; }

/* Timing events cached per host thread and per device, so repeated
 * measurements do not leak one pair each. */
static void timer_events(hipEvent_t *a, hipEvent_t *b)
{
    static __thread hipEvent_t ev0[MAXD], ev1[MAXD];
    static __thread int made[MAXD];
    int d = 0;
    if (hipGetDevice(&d) != hipSuccess || d < 0 || d >= MAXD) {
        HIP_CHECK(hipEventCreate(a));
        HIP_CHECK(hipEventCreate(b));
        return;
    }
    if (!made[d]) {
        HIP_CHECK(hipEventCreate(&ev0[d]));
        HIP_CHECK(hipEventCreate(&ev1[d]));
        made[d] = 1;
    }
    *a = ev0[d];
    *b = ev1[d];
}

static int device_count(void)
{
    int n;
    HIP_CHECK(hipGetDeviceCount(&n));
    return n;
}


/* Machine-readable output.  Every metric also goes out as one line
 *   RESULT <bench> <metric> <unit> <node> <apu0> <apu1> ...
 * and meta() prints one line of provenance.  ./suite collects these. */
static const char *g_bench = "?";

static void result(const char *metric, const char *unit, double node,
                   const double *v, int n)
{
    int i;
    char m[64];
    /* metric names are single tokens: spaces become underscores */
    for (i = 0; metric[i] && i < 63; i++) m[i] = metric[i] == ' ' ? '_' : metric[i];
    m[i] = 0;
    printf("RESULT %s %s %s %.6g", g_bench, m, unit, node);
    for (i = 0; i < n; i++) printf(" %.6g", v[i]);
    printf("\n");
}

static void meta(const char *bench)
{
    char host[128] = "?";
    int rt = 0, clk = 0;
    time_t now = time(NULL);
    struct tm tmv;
    char date[40];
    const char *rocm = getenv("ROCM_PATH");
    g_bench = bench;
    gethostname(host, sizeof host);
    (void)hipRuntimeGetVersion(&rt);
    (void)hipDeviceGetAttribute(&clk, hipDeviceAttributeClockRate, 0);
    localtime_r(&now, &tmv);
    strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tmv);
    printf("META bench=%s host=%s hip=%d rocm=%s sclk_max_MHz=%d devices=%d date=%s\n",
           bench, host, rt, rocm ? rocm : "?", clk / 1000, device_count(), date);
    fflush(stdout);
}

/* Throughput: print per-APU values and the node sum. */
static void report_sum(const char *label, const char *unit,
                       const double *v, int n)
{
    double total = 0.0, mn = 1e300, mx = -1e300, mean, sp;
    int i;
    for (i = 0; i < n; i++) {
        total += v[i];
        mn = dmin(mn, v[i]);
        mx = dmax(mx, v[i]);
    }
    mean = total / (double)n;
    sp = mean != 0.0 ? (mx - mn) / mean * 100.0 : 0.0;
    printf("  %-26s", label);
    for (i = 0; i < n; i++) printf(" %8.1f", v[i]);
    printf("  | node %9.1f %s  (spread %.1f%%)\n", total, unit, sp);
    result(label, unit, total, v, n);
}

/* Wall-clock style figures measured concurrently on every APU: the node cost
 * is the slowest APU, never the sum. */
static void report_max(const char *label, const char *unit,
                       const double *v, int n)
{
    double mn = 1e300, mx = -1e300;
    int i;
    for (i = 0; i < n; i++) {
        mn = dmin(mn, v[i]);
        mx = dmax(mx, v[i]);
    }
    printf("  %-26s", label);
    for (i = 0; i < n; i++) printf(" %8.1f", v[i]);
    printf("  | node %9.1f %s  (max; min %.1f)\n", mx, unit, mn);
    result(label, unit, mx, v, n);
}

static void header(const char *what)
{
    int n = device_count(), i;
    printf("\n%-28s", what);
    for (i = 0; i < n; i++) printf("     APU%d", i);
    printf("  |  node total\n\n");
}

#endif /* COMMON_NTT_H */

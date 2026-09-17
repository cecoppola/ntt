/* fabric/cpuhbm - WP3: can the CPU side of the pipeline work directly on
 * device-resident pools?  For each allocation kind -- malloc (first-touched),
 * hipHostMalloc, hipMallocManaged, hipMalloc (coarse), hipExtMallocWithFlags
 * (fine-grained) on APU 0 -- measure CPU read and write bandwidth (all
 * threads of NUMA node 0, then of node 3: remote) and a GPU kernel's read
 * bandwidth from APU 0 and APU 3.  4 GiB each.  Run with HSA_XNACK=0 and 1. */
#include "common_ntt.h"
#include <omp.h>
#include <sched.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
__global__ void k_read(const uint64_t *p, size_t n, uint64_t *sink)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x; uint64_t acc = 0;
    for (; i < n; i += stride) acc += p[i];
    if (acc == 0x1234567) *sink = acc;
}
static void pin_node(int node)
{
    cpu_set_t s; CPU_ZERO(&s);
    char path[64]; snprintf(path, sizeof path, "/sys/devices/system/node/node%d/cpulist", node);
    FILE *f = fopen(path, "r"); char buf[256] = ""; if (f) { if (!fgets(buf, sizeof buf, f)) buf[0] = 0; fclose(f); }
    for (char *t = strtok(buf, ",\n"); t; t = strtok(NULL, ",\n")) { int a, b; if (sscanf(t, "%d-%d", &a, &b) == 2) for (int c = a; c <= b; c++) CPU_SET(c, &s); else if (sscanf(t, "%d", &a) == 1) CPU_SET(a, &s); }
    sched_setaffinity(0, sizeof s, &s);
}
static double cpu_bw(uint64_t *p, size_t n, int node, int write)
{
    pin_node(node);
    double best = 0;
    for (int rep = 0; rep < 3; rep++) {
        double t0 = now(); uint64_t tot = 0;
#pragma omp parallel reduction(+:tot)
        { pin_node(node);
#pragma omp for schedule(static)
          for (size_t i = 0; i < n; i++) { if (write) p[i] = i; else tot += p[i]; } }
        double bw = n * 8.0 / (now() - t0) / 1e9; if (bw > best) best = bw; if (tot == 1) printf("");
    }
    return best;
}
static double gpu_bw(int dev, const uint64_t *p, size_t n)
{
    HIP_CHECK(hipSetDevice(dev)); uint64_t *sink; HIP_CHECK(hipMalloc(&sink, 8));
    double best = 0;
    for (int rep = 0; rep < 3; rep++) {
        hipEvent_t e0, e1; HIP_CHECK(hipEventCreate(&e0)); HIP_CHECK(hipEventCreate(&e1));
        HIP_CHECK(hipEventRecord(e0, 0)); k_read<<<228 * 8, 256>>>(p, n, sink); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
        float ms; HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); double bw = n * 8.0 / (ms * 1e-3) / 1e9; if (bw > best) best = bw;
    }
    HIP_CHECK(hipFree(sink)); return best;
}
int main(int argc, char **argv)
{
    double gib = argc > 1 ? atof(argv[1]) : 4; size_t bytes = (size_t)(gib * (1 << 30)), n = bytes / 8;
    int nd = device_count(), far = nd - 1;
    printf("== fabric/cpuhbm: %.0f GiB, HSA_XNACK=%s ==\n", gib, getenv("HSA_XNACK") ? getenv("HSA_XNACK") : "unset"); meta("fabric/cpuhbm");
    for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); for (int c = 0; c < nd; c++) if (c != d) { hipError_t e = hipDeviceEnablePeerAccess(c, 0); (void)e; (void)hipGetLastError(); } }
    HIP_CHECK(hipSetDevice(0));
    const char *names[5] = {"malloc (touched node 0)", "hipHostMalloc", "hipMallocManaged", "hipMalloc (coarse)", "hipExtMalloc fine-grained"};
    printf("%-28s %8s %8s %8s %8s %9s %9s   GB/s\n", "allocation on APU0", "cpu0 rd", "cpu0 wr", "cpu3 rd", "cpu3 wr", "gpu0 rd", "gpu3 rd");
    for (int kind = 0; kind < 5; kind++) {
        uint64_t *p = 0; hipError_t e = hipSuccess;
        if (kind == 0) { p = (uint64_t *)aligned_alloc(1 << 21, bytes); pin_node(0); memset(p, 1, bytes); }
        else if (kind == 1) e = hipHostMalloc((void **)&p, bytes, 0);
        else if (kind == 2) e = hipMallocManaged((void **)&p, bytes, hipMemAttachGlobal);
        else if (kind == 3) e = hipMalloc((void **)&p, bytes);
        else e = hipExtMallocWithFlags((void **)&p, bytes, hipDeviceMallocFinegrained);
        if (e != hipSuccess || !p) { printf("%-28s allocation failed: %s\n", names[kind], hipGetErrorString(e)); continue; }
        if (kind >= 1) { HIP_CHECK(hipMemset(p, 1, bytes)); HIP_CHECK(hipDeviceSynchronize()); }
        double c0r = cpu_bw(p, n, 0, 0), c0w = cpu_bw(p, n, 0, 1), c3r = cpu_bw(p, n, far, 0), c3w = cpu_bw(p, n, far, 1);
        double g0 = gpu_bw(0, p, n), g3 = gpu_bw(far, p, n);
        printf("%-28s %8.1f %8.1f %8.1f %8.1f %9.0f %9.0f\n", names[kind], c0r, c0w, c3r, c3w, g0, g3);
        printf("RESULT fabric/cpuhbm kind%d GB/s %.1f %.1f %.1f %.1f %.0f %.0f\n", kind, c0r, c0w, c3r, c3w, g0, g3);
        if (kind == 0) free(p); else if (kind == 1) HIP_CHECK(hipHostFree(p)); else HIP_CHECK(hipFree(p));
    }
    return 0;
}

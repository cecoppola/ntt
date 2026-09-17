/* fabric/poolread - WP3 go/no-go (PLAN.md 15): the batch tier's scatter reads a
 * level pool from every APU.  Compare the aggregate read rate when the pool is
 * (a) registered host memory first-touched across all nodes (today's layout),
 * (b) split by quarters over the four APUs' HBM (each APU reads 1/4 local,
 *     3/4 peer over xGMI), (c) replicated in every APU's HBM (all local).
 * 32 GiB pool, each APU reads the whole pool in 8-byte lanes (the scatter's
 * pattern), 4 APUs concurrently.  Reports GB/s per APU and aggregate. */
#include "common_ntt.h"
#include <omp.h>
#include <sched.h>
#include <string.h>
static void pin_node(int node)
{
    cpu_set_t s; CPU_ZERO(&s);
    char path[64]; snprintf(path, sizeof path, "/sys/devices/system/node/node%d/cpulist", node);
    FILE *f = fopen(path, "r"); char buf[256] = ""; if (f) { if (!fgets(buf, sizeof buf, f)) buf[0] = 0; fclose(f); }
    for (char *t = strtok(buf, ",\n"); t; t = strtok(NULL, ",\n")) { int a, b; if (sscanf(t, "%d-%d", &a, &b) == 2) for (int c = a; c <= b; c++) CPU_SET(c, &s); else if (sscanf(t, "%d", &a) == 1) CPU_SET(a, &s); }
    sched_setaffinity(0, sizeof s, &s);
}
static void unpin(void) { cpu_set_t s; CPU_ZERO(&s); for (int c = 0; c < CPU_SETSIZE; c++) CPU_SET(c, &s); sched_setaffinity(0, sizeof s, &s); }
__global__ void k_read4(const uint64_t *p0, const uint64_t *p1, const uint64_t *p2, const uint64_t *p3, size_t nq, uint64_t *sink)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    uint64_t acc = 0;
    for (; i < nq; i += stride) acc += p0[i] + p1[i] + p2[i] + p3[i];
    if (acc == 0x1234567) *sink = acc;
}
int main(int argc, char **argv)
{
    double gib = argc > 1 ? atof(argv[1]) : 32; size_t bytes = (size_t)(gib * (1 << 30)), n = bytes / 8, nq = n / 4;
    int nd = device_count();
    printf("== fabric/poolread: %.0f GiB pool, %d APUs ==\n", gib, nd); meta("fabric/poolread");
    for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); for (int c = 0; c < nd; c++) if (c != d) { hipError_t e = hipDeviceEnablePeerAccess(c, 0); (void)e; (void)hipGetLastError(); } }
    /* (a) host pool, touched by unpinned threads (pages spread) */
    uint64_t *h = (uint64_t *)aligned_alloc(1 << 21, bytes);
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < n; i += 512) h[i] = i;
    HIP_CHECK(hipHostRegister(h, bytes, hipHostRegisterDefault));
    /* (b) quarters in HBM; (c) full copies in HBM (only if memory allows: 32 GiB each) */
    uint64_t *q[MAXD], *full[MAXD], *sink[MAXD];
    for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMalloc(&q[d], nq * 8)); HIP_CHECK(hipMemset(q[d], 1, nq * 8)); HIP_CHECK(hipMalloc(&full[d], bytes)); HIP_CHECK(hipMemset(full[d], 1, bytes)); HIP_CHECK(hipMalloc(&sink[d], 8)); }
    /* (d) host pool in four NUMA-local quarters (touched by each node's CPUs, registered), each APU
     * reads only its own quarter -- the locality-aware batch tier; (e) same pool, every APU reads all */
    uint64_t *hq = (uint64_t *)aligned_alloc(1 << 21, bytes);
    for (int d = 0; d < nd; d++) {
#pragma omp parallel
        { pin_node(d);
#pragma omp for schedule(static)
          for (size_t i = d * nq; i < (d + 1) * nq; i += 512) hq[i] = i; }
    }
#pragma omp parallel
    unpin();
    HIP_CHECK(hipHostRegister(hq, bytes, hipHostRegisterDefault));
    const char *names[5] = {"(a) host registered, pages spread", "(b) quarters in HBM, 3/4 peer", "(c) replicated in HBM, all local", "(d) host NUMA quarters, own quarter only", "(e) host NUMA quarters, all read all"};
    for (int mode = 0; mode < 5; mode++) {
        double t0, t1; float ms[MAXD];
#pragma omp parallel num_threads(nd)
        {
            int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d));
            const uint64_t *p0, *p1, *p2, *p3;
            if (mode == 0) { p0 = h; p1 = h + nq; p2 = h + 2 * nq; p3 = h + 3 * nq; }
            else if (mode == 1) { p0 = q[0]; p1 = q[1]; p2 = q[2]; p3 = q[3]; }
            else if (mode == 2) { p0 = full[d]; p1 = full[d] + nq; p2 = full[d] + 2 * nq; p3 = full[d] + 3 * nq; }
            else if (mode == 3) { p0 = hq + d * nq; p1 = p0; p2 = p0; p3 = p0; }      /* own quarter, read 4 times (same bytes as the others) */
            else { p0 = hq; p1 = hq + nq; p2 = hq + 2 * nq; p3 = hq + 3 * nq; }
            hipEvent_t e0, e1; HIP_CHECK(hipEventCreate(&e0)); HIP_CHECK(hipEventCreate(&e1));
            k_read4<<<228 * 8, 256>>>(p0, p1, p2, p3, nq, sink[d]); HIP_CHECK(hipDeviceSynchronize());
#pragma omp barrier
            HIP_CHECK(hipEventRecord(e0, 0)); k_read4<<<228 * 8, 256>>>(p0, p1, p2, p3, nq, sink[d]); HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
            HIP_CHECK(hipEventElapsedTime(&ms[d], e0, e1));
        }
        double agg = 0; printf("%-36s", names[mode]);
        for (int d = 0; d < nd; d++) { double g = bytes / (ms[d] * 1e-3) / 1e9; agg += g; printf(" APU%d %5.0f", d, g); }
        printf("  | aggregate %.0f GB/s\n", agg);
        char nm[16]; snprintf(nm, sizeof nm, "mode_%c", 'a' + mode); result(nm, "GB/s", agg, &agg, 1);
    }
    return 0;
}

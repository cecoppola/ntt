/* 18_staging - kernels working directly on pinned host staging (PLAN.md B4).
 *
 * The paper's batch tier has scatter_expand_k read operands straight from the
 * registered host buffer at prefix-sum offsets and zero-extend each of M
 * sub-operands of L_sub limbs into a 2 L_sub-point device plane ("no
 * intermediate device hop").  Results go back with hipMemcpy D2H, which
 * bench/17 measured at a flat 58.5 GB/s per APU.  This measures, per APU with
 * all four running and the staging buffer NUMA-local:
 *
 *   expand    scatter_expand_k for M x L_sub = 2^28 limbs total, L_sub in
 *             {2^10, 2^14, 2^17, 2^20}: GB/s of host bytes read
 *   contig    plain contiguous host->device kernel copy, the ceiling
 *   store     kernel writing a 2^28-limb result into host staging vs D2H
 *
 * Usage: 18_staging
 */
#include "common_ntt.h"
#include <sched.h>
#include <sys/syscall.h>

static long mbind_(void *a, size_t len, int mode, unsigned long mask)
{ return syscall(SYS_mbind, a, len, mode, &mask, 8 * sizeof mask, 0); }
static void pin_to_node(int node)
{
    char path[80], buf[512]; FILE *f; cpu_set_t set; CPU_ZERO(&set);
    snprintf(path, sizeof path, "/sys/devices/system/node/node%d/cpulist", node);
    f = fopen(path, "r");
    if (f && fgets(buf, sizeof buf, f)) {
        char *tok = strtok(buf, ",\n");
        while (tok) { int lo, hi;
            if (sscanf(tok, "%d-%d", &lo, &hi) == 2) { int c; for (c = lo; c <= hi; c++) CPU_SET(c, &set); }
            else if (sscanf(tok, "%d", &lo) == 1) CPU_SET(lo, &set);
            tok = strtok(NULL, ",\n"); }
    }
    if (f) fclose(f);
    sched_setaffinity(0, sizeof set, &set);
}

/* sub-operand m occupies host[off[m] .. off[m]+len); device plane m is 2*lsub
 * points: copy then zero the upper half */
__global__ void k_expand(uint64_t *dev, const uint64_t *host, const size_t *off,
                         size_t lsub, int M)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x, total = (size_t)M * 2 * lsub;
    for (; i < total; i += stride) {
        size_t m = i / (2 * lsub), j = i % (2 * lsub);
        dev[i] = j < lsub ? host[off[m] + j] : 0;
    }
}
__global__ void k_copy(uint64_t *dst, const uint64_t *src, size_t n)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) dst[i] = src[i];
}

int main(void)
{
    const size_t N = (size_t)1 << 28, bytes = N * 8;
    int nd = device_count(), li;
    hipDeviceProp_t pr; int blocks, thr = 256;
    int logl[4] = { 10, 14, 17, 20 };
    double r[MAXD], rc[MAXD], rs[MAXD], rd[MAXD];
    uint64_t *H[MAXD], *D[MAXD], *D2[MAXD]; size_t *OFF[MAXD];

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    blocks = pr.multiProcessorCount * 4;
    printf("== 18_staging : kernels on pinned host staging (2^28 limbs = 2 GiB per APU) ==\n");
    meta("18_staging");

#pragma omp parallel num_threads(nd)
    {
        int dev = omp_get_thread_num(); size_t i;
        HIP_CHECK(hipSetDevice(dev));
        pin_to_node(dev);
        H[dev] = (uint64_t *)aligned_alloc(1 << 21, bytes);
        mbind_(H[dev], bytes, 2, 1UL << dev);
        for (i = 0; i < N; i += 512) H[dev][i] = i;
        HIP_CHECK(hipHostRegister(H[dev], bytes, hipHostRegisterDefault));
        HIP_CHECK(hipMalloc(&D[dev], 2 * bytes));
        HIP_CHECK(hipMalloc(&D2[dev], bytes));
        HIP_CHECK(hipMalloc(&OFF[dev], (N >> 10) * sizeof(size_t)));
        HIP_CHECK(hipMemset(D2[dev], 3, bytes));
    }
    header("pattern");
    for (li = 0; li < 4; li++) {
        size_t lsub = (size_t)1 << logl[li]; int M = (int)(N / lsub);
#pragma omp parallel num_threads(nd)
        {
            int dev = omp_get_thread_num(), rep, m; hipEvent_t e0, e1; float ms; double best = 1e300;
            size_t *hoff = (size_t *)malloc(M * sizeof(size_t));
            HIP_CHECK(hipSetDevice(dev));
            for (m = 0; m < M; m++) hoff[m] = (size_t)m * lsub;     /* densely packed, prefix offsets */
            HIP_CHECK(hipMemcpy(OFF[dev], hoff, M * sizeof(size_t), hipMemcpyHostToDevice));
            timer_events(&e0, &e1);
            for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                HIP_CHECK(hipEventRecord(e0, 0));
                k_expand<<<blocks, thr>>>(D[dev], H[dev], OFF[dev], lsub, M);
                HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
                HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); best = dmin(best, ms);
            }
            r[dev] = (double)bytes / (best * 1e-3) / 1e9;
            free(hoff);
        }
        { char nm[40]; snprintf(nm, sizeof nm, "expand L_sub=2^%d host read", logl[li]); report_sum(nm, "GB/s", r, nd); }
    }
#pragma omp parallel num_threads(nd)
    {
        int dev = omp_get_thread_num(), rep; hipEvent_t e0, e1; float ms; double best;
        HIP_CHECK(hipSetDevice(dev));
        timer_events(&e0, &e1);
#define BEST(launch, out) do { best = 1e300; for (rep = 0; rep < 3; rep++) {   \
            _Pragma("omp barrier") HIP_CHECK(hipEventRecord(e0, 0)); launch;   \
            HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1)); \
            HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); best = dmin(best, ms); } \
            out[dev] = (double)bytes / (best * 1e-3) / 1e9; } while (0)
        BEST((k_copy<<<blocks, thr>>>(D2[dev], H[dev], N)), rc);
        BEST((k_copy<<<blocks, thr>>>(H[dev], D2[dev], N)), rs);
        BEST(HIP_CHECK(hipMemcpyAsync(H[dev], D2[dev], bytes, hipMemcpyDeviceToHost, 0)), rd);
    }
    report_sum("contiguous host->dev kernel", "GB/s", rc, nd);
    report_sum("result store dev->host kernel", "GB/s", rs, nd);
    report_sum("result D2H hipMemcpy", "GB/s", rd, nd);
    printf("\nthe paper's batch tier reads pinned host directly; its mdev tier returns results by D2H memcpy.\n");
    return 0;
}

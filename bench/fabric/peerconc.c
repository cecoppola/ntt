/* fabric/peerconc - do concurrent peer reads in all directions fault?  Four host
 * threads, one per APU; each APU's kernel sums all four buffers (3 remote).
 * Variants: buffers allocated before / after peer access is enabled; serial /
 * concurrent kernels; hipMalloc / mem_dev_alloc-style (with memset). */
#include "common_ntt.h"
#include <omp.h>
__global__ void k_sum4(const uint64_t *p0, const uint64_t *p1, const uint64_t *p2, const uint64_t *p3, size_t n, uint64_t *out)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x; uint64_t acc = 0;
    for (; i < n; i += stride) acc += p0[i] + p1[i] + p2[i] + p3[i];
    if (acc == 0x5555) *out = acc;
}
static void run(const char *name, uint64_t **q, size_t n, int par)
{
    hipError_t err[4] = {hipSuccess, hipSuccess, hipSuccess, hipSuccess};
#pragma omp parallel for num_threads(4) if (par)
    for (int d = 0; d < 4; d++) {
        hipSetDevice(d); uint64_t *out; hipMalloc(&out, 8);
        k_sum4<<<1024, 256>>>(q[0], q[1], q[2], q[3], n, out);
        err[d] = hipDeviceSynchronize(); if (err[d] == hipSuccess) err[d] = hipGetLastError();
        hipFree(out);
    }
    printf("%-44s %s\n", name, (err[0] || err[1] || err[2] || err[3]) ? hipGetErrorString(err[0] ? err[0] : err[1] ? err[1] : err[2] ? err[2] : err[3]) : "ok");
}
int main(void)
{
    size_t n = (size_t)1 << 26; uint64_t *before[4], *after[4], *after_ms[4];
    for (int d = 0; d < 4; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMalloc(&before[d], n * 8)); HIP_CHECK(hipMemset(before[d], 1, n * 8)); }
    for (int d = 0; d < 4; d++) { HIP_CHECK(hipSetDevice(d)); for (int c = 0; c < 4; c++) if (c != d) { hipError_t e = hipDeviceEnablePeerAccess(c, 0); (void)e; (void)hipGetLastError(); } }
    for (int d = 0; d < 4; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMalloc(&after[d], n * 8)); HIP_CHECK(hipMalloc(&after_ms[d], n * 8)); HIP_CHECK(hipMemset(after_ms[d], 1, n * 8)); HIP_CHECK(hipDeviceSynchronize()); }
    run("allocated before peer enable, serial", before, n, 0);
    run("allocated before peer enable, concurrent", before, n, 1);
    run("allocated after, no memset, serial", after, n, 0);
    run("allocated after, no memset, concurrent", after, n, 1);
    run("allocated after, memset, serial", after_ms, n, 0);
    run("allocated after, memset, concurrent", after_ms, n, 1);
    run("allocated after, memset, concurrent again", after_ms, n, 1);
    return 0;
}

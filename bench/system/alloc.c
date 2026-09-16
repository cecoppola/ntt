/* system/alloc - allocator curves: hipMalloc + hipFree, hipHostMalloc,
 * hipHostRegister (+ unregister) and first-touch cost vs size, 1 MiB .. 32 GiB,
 * APU0.  The grow-only pool policy of the pipeline rests on these. */
#include "common_ntt.h"
#include <sys/mman.h>
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
int main(void)
{
    printf("== system/alloc ==\n"); meta("system/alloc");
    HIP_CHECK(hipSetDevice(0));
    header("seconds per operation, APU0");
    printf("%10s %10s %10s %11s %11s %11s %11s\n", "MiB", "hipMalloc", "hipFree", "hostMalloc", "touch(1thr)", "register", "unregister");
    for (size_t mib = 1; mib <= 32768; mib *= 4) {
        size_t bytes = mib << 20; double t0, tm, tf, th, tt, tr, tu; void *d, *h;
        t0 = now_s(); HIP_CHECK(hipMalloc(&d, bytes)); tm = now_s() - t0;
        HIP_CHECK(hipMemset(d, 0, bytes)); HIP_CHECK(hipDeviceSynchronize());
        t0 = now_s(); HIP_CHECK(hipFree(d)); tf = now_s() - t0;
        t0 = now_s(); HIP_CHECK(hipHostMalloc(&h, bytes, 0)); th = now_s() - t0; HIP_CHECK(hipHostFree(h));
        h = aligned_alloc(1 << 21, bytes); madvise(h, bytes, MADV_HUGEPAGE);
        t0 = now_s(); for (size_t o = 0; o < bytes; o += 4096) ((char *)h)[o] = 1; tt = now_s() - t0;
        t0 = now_s(); HIP_CHECK(hipHostRegister(h, bytes, hipHostRegisterDefault)); tr = now_s() - t0;
        t0 = now_s(); HIP_CHECK(hipHostUnregister(h)); tu = now_s() - t0;
        free(h);
        printf("%10zu %10.4f %10.4f %11.4f %11.4f %11.4f %11.4f\n", mib, tm, tf, th, tt, tr, tu);
        if (mib == 16384) { result("hipMalloc_16GiB", "s", tm, &tm, 1); result("hipFree_16GiB", "s", tf, &tf, 1); result("register_16GiB", "s", tr, &tr, 1); result("touch1thr_16GiB", "s", tt, &tt, 1); }
    }
    return 0;
}

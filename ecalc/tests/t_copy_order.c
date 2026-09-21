/* t_copy_order.c - Phase 12 R (D5): the ordering of a device-to-device hipMemcpy against the other devices' streams.
 *
 * The level loop of binsplit.c copies the odd node of a level from one region (device) to another with hipMemcpy on
 * the destination device's null stream (mem_dev_copy_on).  This probes, on the machine itself, whether
 *   (1) hipMemcpy returns to the host before a device-to-device copy has completed (it is queued behind whatever the
 *       copying device's null stream is doing);
 *   (2) a kernel on the SOURCE device's own (blocking) stream can then overwrite the source before the copy read it;
 *   (3) a kernel on a THIRD device can read the destination before the copy wrote it.
 * A blocking stream is ordered after its own device's null stream only, so (2) and (3) are expected to be unordered by
 * the HIP model; the test measures what happens on this ROCm.  With the fix (mem_dev_copy_on waits for the copy) the
 * three counts must be 0 -- run it as `tests/t_copy_order [fix]`: with "fix" the copy is followed by hipStreamSynchronize(0).
 * Prints the timings and the number of corrupted words; VERIFY OK when the fixed variant sees no corruption.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hip/hip_runtime.h>
#include "harness.h"

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

__global__ void k_fill(uint64_t *p, size_t n, uint64_t v)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += st) p[i] = v;
}
__global__ void k_spin(long long cycles)            /* occupies the null stream of a device for about `cycles` */
{
    long long t0 = clock64(); while (clock64() - t0 < cycles) { }
}
__global__ void k_count(const uint64_t *p, size_t n, uint64_t v, unsigned long long *cnt)   /* words equal to v */
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; unsigned long long c = 0;
    for (; i < n; i += st) c += p[i] == v;
    if (c) atomicAdd(cnt, c);
}

int main(int argc, char **argv)
{
    int fix = argc > 1 && !strcmp(argv[1], "fix"), nd = 0; HIP_CHECK(hipGetDeviceCount(&nd));
    if (nd < 3) { printf("t_copy_order: needs 3 devices, have %d\n", nd); return 0; }
    const int A = 0, B = 2, C = 1; const size_t n = (size_t)64 << 20;   /* 512 MB */
    uint64_t *a, *b; unsigned long long *cnt, h[2];
    for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); for (int c = 0; c < nd; c++) if (c != d) { hipError_t e = hipDeviceEnablePeerAccess(c, 0); if (e != hipSuccess && e != hipErrorPeerAccessAlreadyEnabled) HIP_CHECK(e); (void)hipGetLastError(); } }
    HIP_CHECK(hipSetDevice(A)); HIP_CHECK(hipMalloc(&a, n * 8)); hipStream_t sa; HIP_CHECK(hipStreamCreate(&sa));
    HIP_CHECK(hipSetDevice(B)); HIP_CHECK(hipMalloc(&b, n * 8)); HIP_CHECK(hipMalloc(&cnt, 16));
    HIP_CHECK(hipSetDevice(C)); hipStream_t sc; HIP_CHECK(hipStreamCreate(&sc));
    int bad2 = 0, bad3 = 0; double tret = 0;
    for (int rep = 0; rep < 5; rep++) {
        /* (1), (2): a on A holds 1; B's null stream is busy; copy a -> b issued from B; A's stream then overwrites a with 2 */
        HIP_CHECK(hipSetDevice(A)); k_fill<<<1024, 256>>>(a, n, 1); HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipSetDevice(B)); k_fill<<<1024, 256>>>(b, n, 0); HIP_CHECK(hipDeviceSynchronize());
        k_spin<<<1, 1>>>(400000000LL);                                    /* ~0.2-0.4 s on the null stream of B */
        double t0 = now();
        HIP_CHECK(hipMemcpy(b, a, n * 8, hipMemcpyDefault));
        if (fix) HIP_CHECK(hipStreamSynchronize(0));
        double t1 = now(); tret += t1 - t0;
        HIP_CHECK(hipSetDevice(A)); k_fill<<<1024, 256, 0, sa>>>(a, n, 2); HIP_CHECK(hipStreamSynchronize(sa));
        HIP_CHECK(hipSetDevice(B)); HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipMemset(cnt, 0, 16)); k_count<<<1024, 256>>>(b, n, 2, cnt); HIP_CHECK(hipMemcpy(h, cnt, 8, hipMemcpyDeviceToHost));
        printf("rep %d: hipMemcpy D2D of %zu MB returned after %.3f s (B's null stream was busy); words of the LATER value in the destination: %llu of %zu\n", rep, n * 8 >> 20, t1 - t0, h[0], n);
        if (h[0]) bad2++;
        /* (3): B's null stream busy again; copy a (=2) -> b (=0) issued from B; a kernel on C's stream counts the words of b still 0 */
        HIP_CHECK(hipSetDevice(B)); k_fill<<<1024, 256>>>(b, n, 0); HIP_CHECK(hipDeviceSynchronize());
        k_spin<<<1, 1>>>(400000000LL);
        HIP_CHECK(hipMemcpy(b, a, n * 8, hipMemcpyDefault));
        if (fix) HIP_CHECK(hipStreamSynchronize(0));
        HIP_CHECK(hipSetDevice(C)); unsigned long long *cc; HIP_CHECK(hipMalloc(&cc, 8)); HIP_CHECK(hipMemset(cc, 0, 8));
        k_count<<<1024, 256, 0, sc>>>(b, n, 0, cc); HIP_CHECK(hipStreamSynchronize(sc)); HIP_CHECK(hipMemcpy(h + 1, cc, 8, hipMemcpyDeviceToHost)); HIP_CHECK(hipFree(cc));
        HIP_CHECK(hipSetDevice(B)); HIP_CHECK(hipDeviceSynchronize());
        printf("rep %d: a kernel on device %d read the destination right after the copy call: %llu of %zu words still the OLD value\n", rep, C, h[1], n);
        if (h[1]) bad3++;
    }
    printf("%s: hipMemcpy returned after %.3f s on average; source overwritten before the copy read it in %d of 5, destination read before the copy wrote it in %d of 5\n", fix ? "fixed (hipStreamSynchronize(0) after the copy)" : "as mem_dev_copy_on was", tret / 5, bad2, bad3);
    if (fix) { VERIFY(bad2 == 0, "source overwritten under the fix"); VERIFY(bad3 == 0, "destination read early under the fix"); }
    else VERIFY(1, "");
    return verify_done("t_copy_order");
}

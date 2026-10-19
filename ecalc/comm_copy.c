/* comm_copy.c - N7D (F2b): a plain GPU copy kernel for copies that touch the SHMEM proxies' host-registered pool (results/SHMEM_ITEMS.md §N7C).
 * hipMemcpyAsync to or from host-registered memory runs on the SDMA engine (about 21 GB/s on MI300A); a kernel runs at the memory's
 * speed.  Used by comm_shmem.c's COPY under COMM_SHMEM_PX_KCOPY=1; HSA_ENABLE_SDMA=0 had the same effect for every copy of the process. */
#include "comm.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#define CC_THREADS 256
#define CC_MAX_BLOCKS 456            /* two waves of the 228 CUs; the grid-stride loop covers the rest */
template <typename T> __global__ void __launch_bounds__(CC_THREADS) k_ccopy(T *d, const T *s, size_t n)
{
    size_t i = (size_t)blockIdx.x * CC_THREADS + threadIdx.x, st = (size_t)gridDim.x * CC_THREADS;
    for (; i + 3 * st < n; i += 4 * st) { T a = s[i], b = s[i + st], c = s[i + 2 * st], e = s[i + 3 * st]; d[i] = a; d[i + st] = b; d[i + 2 * st] = c; d[i + 3 * st] = e; }
    for (; i < n; i += st) d[i] = s[i];
}
__global__ void k_ccopy_tail(unsigned char *d, const unsigned char *s, size_t n) { for (size_t i = threadIdx.x; i < n; i += blockDim.x) d[i] = s[i]; }
template <typename T> static void launch(void *d, const void *s, size_t n, hipStream_t st)   /* n elements of T */
{
    size_t blocks = (n + CC_THREADS - 1) / CC_THREADS; if (blocks > CC_MAX_BLOCKS) blocks = CC_MAX_BLOCKS;
    hipLaunchKernelGGL(k_ccopy<T>, dim3((unsigned)blocks), dim3(CC_THREADS), 0, st, (T *)d, (const T *)s, n);
}
/* d <- s, n bytes, on stream st; the same ordering as hipMemcpyAsync.  Misaligned (not 8 bytes) or tiny copies go to hipMemcpyAsync. */
hipError_t comm_copy_async(void *d, const void *s, size_t n, hipStream_t st)
{
    uintptr_t a = (uintptr_t)d | (uintptr_t)s;
    if (n < 4096 || (a & 7)) return hipMemcpyAsync(d, s, n, hipMemcpyDefault, st);
    if (!(a & 15)) { size_t m = n / 16; launch<uint4>(d, s, m, st); if (n & 15) hipLaunchKernelGGL(k_ccopy_tail, dim3(1), dim3(64), 0, st, (unsigned char *)d + m * 16, (const unsigned char *)s + m * 16, n & 15); }
    else { size_t m = n / 8; launch<uint64_t>(d, s, m, st); if (n & 7) hipLaunchKernelGGL(k_ccopy_tail, dim3(1), dim3(64), 0, st, (unsigned char *)d + m * 8, (const unsigned char *)s + m * 8, n & 7); }
    return hipGetLastError();
}

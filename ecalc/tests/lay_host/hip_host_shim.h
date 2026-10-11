/* S45: a CPU shim of the few HIP calls comm_layered.c makes, so the layered communicator builds host-only (g++ -DCOMM_HOST_ONLY
 * -include hip_host_shim.h, on the sed-preprocessed copy made by tests/lay_host/build.sh).  Device memory = malloc, streams are
 * dummies, copies are synchronous memcpy, and hipPointerGetAttributes reports plain host memory so the kernel paths (k_btrans,
 * k_bcopy: never launched here) take the memcpy fallbacks. */
#ifndef LAY_HIP_SHIM_H
#define LAY_HIP_SHIM_H
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
typedef int hipError_t;
enum { hipSuccess = 0, hipErrorOutOfMemory = 2 };
enum hipMemcpyKind { hipMemcpyHostToHost, hipMemcpyHostToDevice, hipMemcpyDeviceToHost, hipMemcpyDeviceToDevice, hipMemcpyDefault };
enum hipMemoryType { hipMemoryTypeHost, hipMemoryTypeDevice, hipMemoryTypeManaged, hipMemoryTypeUnified };
typedef struct { enum hipMemoryType type; void *devicePointer; } hipPointerAttribute_t;
struct uint4 { unsigned x, y, z, w; };
struct dim3 { unsigned x, y, z; dim3(unsigned a = 1, unsigned b = 1, unsigned c = 1) : x(a), y(b), z(c) {} };
#define __global__
static struct { unsigned x, y; } blockIdx, threadIdx, blockDim, gridDim;
static inline const char *hipGetErrorString(hipError_t e) { return e == hipErrorOutOfMemory ? "out of memory" : "error"; }
static inline hipError_t hipGetLastError(void) { return hipSuccess; }
static inline hipError_t hipSetDevice(int) { return hipSuccess; }
static inline hipError_t hipStreamCreate(void **s) { *s = 0; return hipSuccess; }
static inline hipError_t hipStreamDestroy(void *) { return hipSuccess; }
static inline hipError_t hipStreamSynchronize(void *) { return hipSuccess; }
/* live bytes of the "device" allocations (the layered communicator's scratch and v-slots only: the harness uses malloc): size header + counters */
inline size_t shim_live = 0, shim_peak = 0, shim_nalloc = 0;
static inline hipError_t hipMalloc(void **p, size_t n) { char *q = (char *)malloc(n + 64); if (!q) { *p = 0; return hipErrorOutOfMemory; } *(size_t *)q = n; *p = q + 64; shim_live += n; if (shim_live > shim_peak) shim_peak = shim_live; shim_nalloc++; return hipSuccess; }
static inline hipError_t hipFree(void *p) { if (p) { char *q = (char *)p - 64; shim_live -= *(size_t *)q; free(q); } return hipSuccess; }
static inline hipError_t hipHostMalloc(void **p, size_t n, unsigned) { *p = malloc(n ? n : 1); return *p ? hipSuccess : hipErrorOutOfMemory; }
static inline hipError_t hipHostFree(void *p) { free(p); return hipSuccess; }
static inline hipError_t hipMemcpy(void *d, const void *s, size_t n, hipMemcpyKind) { memmove(d, s, n); return hipSuccess; }
static inline hipError_t hipMemcpyAsync(void *d, const void *s, size_t n, hipMemcpyKind, void *) { memmove(d, s, n); return hipSuccess; }
static inline hipError_t hipPointerGetAttributes(hipPointerAttribute_t *a, const void *) { a->type = hipMemoryTypeHost; a->devicePointer = 0; return hipSuccess; }
/* the kernel launches are rewritten by build.sh to these; never reached because dev_ok() is false */
template <class... A> static inline void k_btrans_h(A...) { abort(); }
template <class... A> static inline void k_bcopy_h(A...) { abort(); }
#endif

/* hipmm.c - S35 (results/S34.md "S35"): ECALC_VMM_BG=2, the process-wide HIP memory-management lock.
 *
 * The rc139 segfault of multi-node runs is inside hipMemMap, called by dbig.c's background VMM mapper (vmm_bg_map) while other
 * threads use the HIP runtime.  ECALC_VMM_BG=2 keeps the background mapper (its overlap with init) but serialises it against
 * every HIP memory-management call of the process: ecalc is linked with -Wl,--wrap=<api> for the APIs below (only the ecalc
 * link, not the tests), so each call ecalc's objects make goes through __wrap_<api>, which holds db_hipmm_lock() (a recursive
 * process-wide mutex in dbig.c) around __real_<api> when the switch is on, and calls __real_<api> directly otherwise (the default:
 * the same calls, one branch more).  The mapper holds the same lock around each chunk's create + map + access (dbig.c).
 * Not covered: calls the libraries make on their own (libfabric reaches HSA through dlsym; libsma imports no HIP symbol).
 * Digits: unchanged (the same calls, the same order within each thread). */
#include <hip/hip_runtime.h>
#include "dbig.h"

#define HIPMM_WRAP(name, params, args)                                                        \
    extern "C" hipError_t __real_##name params;                                              \
    extern "C" hipError_t __wrap_##name params                                               \
    {                                                                                        \
        if (!db_hipmm_on()) return __real_##name args;                                       \
        db_hipmm_lock(); hipError_t e = __real_##name args; db_hipmm_unlock(); return e;     \
    }

HIPMM_WRAP(hipMalloc, (void **p, size_t n), (p, n))
HIPMM_WRAP(hipFree, (void *p), (p))
HIPMM_WRAP(hipExtMallocWithFlags, (void **p, size_t n, unsigned int f), (p, n, f))
HIPMM_WRAP(hipMallocManaged, (void **p, size_t n, unsigned int f), (p, n, f))
HIPMM_WRAP(hipHostMalloc, (void **p, size_t n, unsigned int f), (p, n, f))
HIPMM_WRAP(hipHostFree, (void *p), (p))
HIPMM_WRAP(hipHostRegister, (void *p, size_t n, unsigned int f), (p, n, f))
HIPMM_WRAP(hipHostUnregister, (void *p), (p))
HIPMM_WRAP(hipHostGetDevicePointer, (void **d, void *h, unsigned int f), (d, h, f))
HIPMM_WRAP(hipPointerGetAttributes, (hipPointerAttribute_t *a, const void *p), (a, p))
HIPMM_WRAP(hipMemGetInfo, (size_t *f, size_t *t), (f, t))
HIPMM_WRAP(hipMallocAsync, (void **p, size_t n, hipStream_t s), (p, n, s))
HIPMM_WRAP(hipFreeAsync, (void *p, hipStream_t s), (p, s))
HIPMM_WRAP(hipMallocFromPoolAsync, (void **p, size_t n, hipMemPool_t m, hipStream_t s), (p, n, m, s))
HIPMM_WRAP(hipMemCreate, (hipMemGenericAllocationHandle_t *h, size_t n, const hipMemAllocationProp *pr, unsigned long long f), (h, n, pr, f))
HIPMM_WRAP(hipMemMap, (void *p, size_t n, size_t o, hipMemGenericAllocationHandle_t h, unsigned long long f), (p, n, o, h, f))
HIPMM_WRAP(hipMemSetAccess, (void *p, size_t n, const hipMemAccessDesc *d, size_t c), (p, n, d, c))
HIPMM_WRAP(hipMemUnmap, (void *p, size_t n), (p, n))
HIPMM_WRAP(hipMemRelease, (hipMemGenericAllocationHandle_t h), (h))
HIPMM_WRAP(hipMemAddressReserve, (void **p, size_t n, size_t a, void *ad, unsigned long long f), (p, n, a, ad, f))
HIPMM_WRAP(hipMemAddressFree, (void *p, size_t n), (p, n))
HIPMM_WRAP(hipStreamCreate, (hipStream_t *s), (s))
HIPMM_WRAP(hipStreamCreateWithFlags, (hipStream_t *s, unsigned int f), (s, f))
HIPMM_WRAP(hipStreamDestroy, (hipStream_t s), (s))

/* heap_vmm_shim.c: LD_PRELOAD shim (benchmark only, branch devsrc3): back libsma's symmetric heap with HIP VMM device memory (RECVDEV1.md, design 1).
 * libsma / libdsmml create the heap with mmap(base, len, RW, MAP_SHARED|MAP_FIXED[|MAP_ANONYMOUS|MAP_HUGETLB], fd) at 0x30000000000 (or _SHMEM_SHEAP_BASE).
 * This shim intercepts that mmap (a fixed shared mapping of >= HEAPVMM_MIN_MB, default 256, at the heap base) and returns a mapping of one hipMemCreate
 * allocation at the SAME address instead (hipMemAddressReserve with the address, hipMemMap, hipMemSetAccess for all devices, zero-filled).
 * With the FI_HMEM shim (fi_hmem_shim.so, FI_SHIM_MODE=B) and _SHMEM_OFI_USE_MR_HMEM=1 the cxi provider then registers the heap as device memory.
 *   HEAPVMM_SHIM=1   enable (default: pass-through, nothing happens)       HEAPVMM_DEV=<n>  device that owns the memory (default 0)
 *   HEAPVMM_MIN_MB   smallest mapping taken over (default 256)              HEAPVMM_VERBOSE=1 log every fixed shared mmap seen
 * Any failure (reserve at another address, map, CPU access test) logs one line and falls back to the real mmap.  The CPU access test writes and reads
 * one byte per 2 MiB under a SIGSEGV guard.  Build (HIP, host code only): hipcc -x hip -O1 -shared -fPIC -o heap_vmm_shim.so heap_vmm_shim.c -ldl */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <hip/hip_runtime.h>

static void say(const char *fmt, ...) { char b[500]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap); if (n > 0) { if (n > (int)sizeof b - 1) n = sizeof b - 1; if (write(2, b, n) < 0) {} } }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }

typedef struct { void *va; size_t len; hipMemGenericAllocationHandle_t h; } ent_t;
#define NENT 8
static ent_t g_ent[NENT]; static int g_nent;
static __thread int g_busy;     /* HIP's own mmap calls while we set up must go straight through */
static sigjmp_buf g_jb; static volatile int g_in_probe;
static void on_segv(int s) { (void)s; if (g_in_probe) siglongjmp(g_jb, 1); signal(SIGSEGV, SIG_DFL); raise(SIGSEGV); }

static int cpu_probe(volatile char *p, size_t len)   /* 1 = the CPU can write and read it back */
{
    struct sigaction sa, old, oldb; memset(&sa, 0, sizeof sa); sa.sa_handler = on_segv; sigaction(SIGSEGV, &sa, &old); sigaction(SIGBUS, &sa, &oldb);
    int ok = 0;
    if (sigsetjmp(g_jb, 1) == 0) { g_in_probe = 1; ok = 1; for (size_t o = 0; o < len; o += (size_t)2 << 20) { p[o] = (char)(o >> 21) | 1; if (p[o] != (char)((o >> 21) | 1)) { ok = 0; break; } } p[len - 1] = 7; if (p[len - 1] != 7) ok = 0; }
    g_in_probe = 0; sigaction(SIGSEGV, &old, 0); sigaction(SIGBUS, &oldb, 0);
    return ok;
}

static void *vmm_take(void *addr, size_t len)   /* the mapped address (== addr) or 0 */
{
    const char *de = getenv("HEAPVMM_DEV"); int dev = de ? atoi(de) : 0, cur = 0, nd = 0; double t0 = now();
    if (g_nent >= NENT) return 0;
    if (hipGetDevice(&cur) != hipSuccess) cur = 0;
    if (hipGetDeviceCount(&nd) != hipSuccess || nd < 1) { say("heap_vmm_shim: pid %d no HIP device\n", (int)getpid()); return 0; }
    if (dev >= nd) dev = 0;
    if (hipSetDevice(dev) != hipSuccess) return 0;
    hipMemAllocationProp prop; memset(&prop, 0, sizeof prop); prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = dev;
    size_t gran = 0; if (hipMemGetAllocationGranularity(&gran, &prop, hipMemAllocationGranularityRecommended) != hipSuccess || !gran) gran = (size_t)2 << 20;
    size_t L = (len + gran - 1) / gran * gran; void *p = 0; hipMemGenericAllocationHandle_t h; hipError_t e; const char *why = "";
    if ((e = hipMemCreate(&h, L, &prop, 0)) != hipSuccess) { why = "hipMemCreate"; goto fail0; }
    if ((e = hipMemAddressReserve(&p, L, 0, addr, 0)) != hipSuccess) { why = "hipMemAddressReserve"; goto fail1; }
    if (p != addr) { say("heap_vmm_shim: pid %d reserve returned %p, wanted %p\n", (int)getpid(), p, addr); why = "address hint refused"; e = hipSuccess; goto fail2; }
    if ((e = hipMemMap(p, L, 0, h, 0)) != hipSuccess) { why = "hipMemMap"; goto fail2; }
    { hipMemAccessDesc ad[16]; memset(ad, 0, sizeof ad); int n = nd < 16 ? nd : 16;
      for (int i = 0; i < n; i++) { ad[i].location.type = hipMemLocationTypeDevice; ad[i].location.id = i; ad[i].flags = hipMemAccessFlagsProtReadWrite; }
      if ((e = hipMemSetAccess(p, L, ad, n)) != hipSuccess) { why = "hipMemSetAccess"; goto fail3; } }
    if (!cpu_probe((volatile char *)p, L)) { why = "CPU write/read faulted or mismatched"; e = hipSuccess; goto fail3; }
    if ((e = hipMemset(p, 0, L)) != hipSuccess || (e = hipDeviceSynchronize()) != hipSuccess) { why = "hipMemset"; goto fail3; }
    g_ent[g_nent].va = p; g_ent[g_nent].len = L; g_ent[g_nent].h = h; g_nent++;
    hipSetDevice(cur);
    say("heap_vmm_shim: pid %d heap %p len %zu (rounded %zu, granularity %zu) now VMM device memory of device %d, %.3f s\n", (int)getpid(), p, len, L, gran, dev, now() - t0);
    return p;
fail3: hipMemUnmap(p, L);
fail2: hipMemAddressFree(p, L);
fail1: hipMemRelease(h);
fail0: say("heap_vmm_shim: pid %d FALLBACK to the real mmap: %s (%s)\n", (int)getpid(), why, e == hipSuccess ? "-" : hipGetErrorString(e)); (void)hipGetLastError();
    hipSetDevice(cur); return 0;
}

static int heap_like(void *addr, size_t len, int flags)
{
    static int en = -1; static size_t minb;
    if (en < 0) { const char *s = getenv("HEAPVMM_SHIM"); en = s && atoi(s) == 1; const char *m = getenv("HEAPVMM_MIN_MB"); minb = (m ? (size_t)atol(m) : 256) << 20; }
    if (!en || g_busy || !addr || !(flags & MAP_FIXED) || !(flags & MAP_SHARED)) return 0;
    if (getenv("HEAPVMM_VERBOSE") && len >= ((size_t)1 << 20)) say("heap_vmm_shim: pid %d fixed shared mmap %p len %zu flags 0x%x\n", (int)getpid(), addr, len, flags);
    return len >= minb;
}

typedef void *(*mmap_f)(void *, size_t, int, int, int, off_t);
typedef int (*munmap_f)(void *, size_t);
#ifdef __cplusplus
extern "C" {
#endif
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    static mmap_f real; if (!real) real = (mmap_f)dlsym(RTLD_NEXT, "mmap");
    if (heap_like(addr, len, flags)) { g_busy = 1; void *p = vmm_take(addr, len); g_busy = 0; if (p) return p; }
    return real(addr, len, prot, flags, fd, off);
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) { return mmap(addr, len, prot, flags, fd, off); }
int munmap(void *addr, size_t len)
{
    static munmap_f real; if (!real) real = (munmap_f)dlsym(RTLD_NEXT, "munmap");
    for (int i = 0; i < g_nent; i++) if (g_ent[i].va == addr) {      /* the heap: release the VMM mapping (finalize) */
        g_busy = 1; hipMemUnmap(addr, g_ent[i].len); hipMemAddressFree(addr, g_ent[i].len); hipMemRelease(g_ent[i].h); g_busy = 0;
        g_ent[i] = g_ent[--g_nent]; return 0; }
    for (int i = 0; i < g_nent; i++) if ((char *)addr >= (char *)g_ent[i].va && (char *)addr < (char *)g_ent[i].va + g_ent[i].len) { say("heap_vmm_shim: partial munmap %p len %zu inside the VMM heap ignored\n", addr, len); return 0; }
    return real(addr, len);
}
#ifdef __cplusplus
}
#endif

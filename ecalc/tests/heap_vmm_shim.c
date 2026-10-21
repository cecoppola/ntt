/* heap_vmm_shim.c: LD_PRELOAD shim (benchmark only, branch devsrc3): back libsma's symmetric heap with device-class memory that the CPU can use (RECVDEV1.md, design 1 / 2b).
 * libsma / libdsmml create the heap with mmap(base, len, RW, MAP_SHARED|MAP_FIXED[|MAP_ANONYMOUS|MAP_HUGETLB], fd) at 0x30000000000 (or _SHMEM_SHEAP_BASE).
 * This shim intercepts exactly that call (addr == the heap base, len >= HEAPVMM_MIN_MB, default 256; the ROCr/KFD mappings of the HIP runtime also have MAP_SHARED|MAP_FIXED and must NOT be touched: 9lj hung on them)
 * and returns memory of the kind HEAPVMM_MODE at the SAME address:
 *   hip (9lj: CPU SIGSEGV, kept for the record) | hipuc | hsagpu | hsagpufine | hsacpu   (see tests/t_vmmprobe.c: variants 0-4)   | ext  (hipExtMallocWithFlags fine-grained + mremap to the base; HIP/ROCr then know the old VA only)
 *   HEAPVMM_SHIM=1 enable (default: pass-through)   HEAPVMM_DEV=<n> device (default 0)   HEAPVMM_VERBOSE=1 log the base-address mmap candidates
 * The CPU write/read test runs under a SIGSEGV/SIGBUS guard; any failure logs one FALLBACK line (with the signal) and the real mmap is used.  Build: hipcc -x hip -O1 -shared -fPIC -o heap_vmm_shim.so heap_vmm_shim.c -ldl -lhsa-runtime64 */
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
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
static void say(const char *fmt, ...) { char b[500]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap); if (n > 0) { if (n > (int)sizeof b - 1) n = sizeof b - 1; if (write(2, b, n) < 0) {} } }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
/* ---- SIGSEGV guard */
static sigjmp_buf g_jb; static volatile int g_guard; static volatile int g_sig;
static void on_sig(int s) { if (g_guard) { g_sig = s; siglongjmp(g_jb, 1); } signal(s, SIG_DFL); raise(s); }
static int cpu_touch(volatile char *p, size_t len, int *sig)   /* 1 = write + read back of one byte per 2 MiB worked */
{
    struct sigaction sa, o1, o2; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sig; sigaction(SIGSEGV, &sa, &o1); sigaction(SIGBUS, &sa, &o2); int ok = 0; g_sig = 0;
    if (sigsetjmp(g_jb, 1) == 0) { g_guard = 1; ok = 1; for (size_t o = 0; o < len; o += (size_t)2 << 20) { p[o] = (char)((o >> 21) | 1); if (p[o] != (char)((o >> 21) | 1)) { ok = 0; break; } } p[len - 1] = 7; if (p[len - 1] != 7) ok = 0; }
    g_guard = 0; sigaction(SIGSEGV, &o1, 0); sigaction(SIGBUS, &o2, 0); *sig = g_sig; return ok;
}
/* ---- HSA helpers (agents, pools, vmem) */
typedef struct { hsa_agent_t gpu[16], cpu[8]; int ngpu, ncpu; } agents_t;
static hsa_status_t ag_cb(hsa_agent_t a, void *d) { agents_t *A = (agents_t *)d; hsa_device_type_t t; hsa_agent_get_info(a, HSA_AGENT_INFO_DEVICE, &t); if (t == HSA_DEVICE_TYPE_GPU && A->ngpu < 16) A->gpu[A->ngpu++] = a; else if (t == HSA_DEVICE_TYPE_CPU && A->ncpu < 8) A->cpu[A->ncpu++] = a; return HSA_STATUS_SUCCESS; }
typedef struct { hsa_amd_memory_pool_t c, f; int hc, hf; size_t gc, gf; } pools_t;
static hsa_status_t pool_cb(hsa_amd_memory_pool_t p, void *d)
{
    pools_t *P = (pools_t *)d; hsa_amd_segment_t seg; uint32_t fl = 0; bool ra = false; size_t g = 0;
    hsa_amd_memory_pool_get_info(p, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &seg); if (seg != HSA_AMD_SEGMENT_GLOBAL) return HSA_STATUS_SUCCESS;
    hsa_amd_memory_pool_get_info(p, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &fl); hsa_amd_memory_pool_get_info(p, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALLOWED, &ra); if (!ra) return HSA_STATUS_SUCCESS;
    hsa_amd_memory_pool_get_info(p, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_REC_GRANULE, &g); if (!g) g = (size_t)2 << 20;
    if ((fl & HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_COARSE_GRAINED) && !P->hc) { P->c = p; P->hc = 1; P->gc = g; }
    if ((fl & HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_FINE_GRAINED) && !P->hf) { P->f = p; P->hf = 1; P->gf = g; }
    return HSA_STATUS_SUCCESS;
}
typedef struct { void *va; size_t len; hsa_amd_vmem_alloc_handle_t h; hipMemGenericAllocationHandle_t hh; int kind; } var_t;   /* kind: 0 hip vmem, 1 hsa vmem, 2 hipMalloc-like, 3 none */

static void *hsa_vmem(int which, int dev, void *want, size_t len, var_t *v, char *why, size_t wl)   /* which 2 gpu coarse, 3 gpu fine, 4 cpu fine/coarse */
{
    agents_t A; memset(&A, 0, sizeof A); hsa_iterate_agents(ag_cb, &A); pools_t P; memset(&P, 0, sizeof P);
    if (dev >= A.ngpu) dev = 0; hsa_agent_t ag = which == 4 ? A.cpu[dev < A.ncpu ? dev : 0] : A.gpu[dev];
    hsa_amd_agent_iterate_memory_pools(ag, pool_cb, &P);
    hsa_amd_memory_pool_t pool; size_t g;
    if (which == 2) { if (!P.hc) { snprintf(why, wl, "no coarse pool"); return 0; } pool = P.c; g = P.gc; }
    else if (which == 3) { if (!P.hf) { snprintf(why, wl, "no fine-grained pool on the GPU agent"); return 0; } pool = P.f; g = P.gf; }
    else { if (P.hf) { pool = P.f; g = P.gf; } else if (P.hc) { pool = P.c; g = P.gc; } else { snprintf(why, wl, "no CPU pool"); return 0; } }
    size_t L = (len + g - 1) / g * g; void *va = 0; hsa_status_t s;
    if ((s = hsa_amd_vmem_address_reserve(&va, L, (uint64_t)(uintptr_t)want, 0)) != HSA_STATUS_SUCCESS) { snprintf(why, wl, "address_reserve rc %d", (int)s); return 0; }
    if (va != want) snprintf(why, wl, "VA hint refused (%p)", va);
    if ((s = hsa_amd_vmem_handle_create(pool, L, MEMORY_TYPE_NONE, 0, &v->h)) != HSA_STATUS_SUCCESS) { snprintf(why, wl, "handle_create rc %d (gran %zu)", (int)s, g); hsa_amd_vmem_address_free(va, L); return 0; }
    if ((s = hsa_amd_vmem_map(va, L, 0, v->h, 0)) != HSA_STATUS_SUCCESS) { snprintf(why, wl, "vmem_map rc %d", (int)s); hsa_amd_vmem_handle_release(v->h); hsa_amd_vmem_address_free(va, L); return 0; }
    hsa_amd_memory_access_desc_t d[24]; int n = 0; for (int i = 0; i < A.ngpu; i++) { d[n].permissions = HSA_ACCESS_PERMISSION_RW; d[n++].agent_handle = A.gpu[i]; } for (int i = 0; i < A.ncpu; i++) { d[n].permissions = HSA_ACCESS_PERMISSION_RW; d[n++].agent_handle = A.cpu[i]; }
    if ((s = hsa_amd_vmem_set_access(va, L, d, n)) != HSA_STATUS_SUCCESS) { snprintf(why, wl, "set_access rc %d (%d gpu + %d cpu agents)", (int)s, A.ngpu, A.ncpu); hsa_amd_vmem_unmap(va, L); hsa_amd_vmem_handle_release(v->h); hsa_amd_vmem_address_free(va, L); return 0; }
    v->va = va; v->len = L; v->kind = 1; snprintf(why, wl, "gran %zu, %d gpu + %d cpu agents%s", g, A.ngpu, A.ncpu, va == want ? "" : " (address differs from the wish)"); return va;
}
static void *hip_vmem(int uncached, int dev, void *want, size_t len, var_t *v, char *why, size_t wl)
{
    hipMemAllocationProp prop; memset(&prop, 0, sizeof prop); prop.type = uncached ? hipMemAllocationTypeUncached : hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = dev;
    size_t g = 0; if (hipMemGetAllocationGranularity(&g, &prop, hipMemAllocationGranularityRecommended) != hipSuccess || !g) g = (size_t)2 << 20; size_t L = (len + g - 1) / g * g; hipError_t e;
    if ((e = hipMemCreate(&v->hh, L, &prop, 0)) != hipSuccess) { snprintf(why, wl, "hipMemCreate: %s", hipGetErrorString(e)); return 0; }
    void *p = 0; if ((e = hipMemAddressReserve(&p, L, 0, want, 0)) != hipSuccess) { snprintf(why, wl, "reserve: %s", hipGetErrorString(e)); hipMemRelease(v->hh); return 0; }
    if ((e = hipMemMap(p, L, 0, v->hh, 0)) != hipSuccess) { snprintf(why, wl, "map: %s", hipGetErrorString(e)); hipMemAddressFree(p, L); hipMemRelease(v->hh); return 0; }
    int nd = 0; hipGetDeviceCount(&nd); hipMemAccessDesc ad[16]; memset(ad, 0, sizeof ad); int na = nd < 16 ? nd : 16; for (int i = 0; i < na; i++) { ad[i].location.type = hipMemLocationTypeDevice; ad[i].location.id = i; ad[i].flags = hipMemAccessFlagsProtReadWrite; }
    if ((e = hipMemSetAccess(p, L, ad, na)) != hipSuccess) { snprintf(why, wl, "setaccess: %s", hipGetErrorString(e)); hipMemUnmap(p, L); hipMemAddressFree(p, L); hipMemRelease(v->hh); return 0; }
    v->va = p; v->len = L; v->kind = 0; snprintf(why, wl, "gran %zu%s", g, p == want ? "" : " (address differs from the wish)"); return p;
}
static void var_free(var_t *v) { if (!v->va) return; if (v->kind == 0) { hipMemUnmap(v->va, v->len); hipMemAddressFree(v->va, v->len); hipMemRelease(v->hh); } else if (v->kind == 1) { hsa_amd_vmem_unmap(v->va, v->len); hsa_amd_vmem_handle_release(v->h); hsa_amd_vmem_address_free(v->va, v->len); } else if (v->kind == 2) hipFree(v->va); else if (v->kind == 3) munmap(v->va, v->len); v->va = 0; }


#define NENT 8
static var_t g_ent[NENT]; static int g_nent; static __thread int g_busy;   /* HIP/ROCr's own mmap calls while we set up go straight through */

static void *take(void *addr, size_t len)
{
    const char *de = getenv("HEAPVMM_DEV"), *md = getenv("HEAPVMM_MODE"); int dev = de ? atoi(de) : 0, nd = 0, cur = 0; double t0 = now(); char why[200] = ""; var_t v; memset(&v, 0, sizeof v); void *p = 0; int sig = 0;
    if (!md) md = "hsagpu"; if (g_nent >= NENT) return 0;
    if (hipGetDevice(&cur) != hipSuccess) cur = 0; if (hipGetDeviceCount(&nd) != hipSuccess || nd < 1) { say("heap_vmm_shim: pid %d no HIP device\n", (int)getpid()); return 0; } if (dev >= nd) dev = 0; (void)hipSetDevice(dev); (void)hsa_init();
    if (!strcmp(md, "hip")) p = hip_vmem(0, dev, addr, len, &v, why, sizeof why);
    else if (!strcmp(md, "hipuc")) p = hip_vmem(1, dev, addr, len, &v, why, sizeof why);
    else if (!strcmp(md, "hsagpu")) p = hsa_vmem(2, dev, addr, len, &v, why, sizeof why);
    else if (!strcmp(md, "hsagpufine")) p = hsa_vmem(3, dev, addr, len, &v, why, sizeof why);
    else if (!strcmp(md, "hsacpu")) p = hsa_vmem(4, dev, addr, len, &v, why, sizeof why);
    else if (!strcmp(md, "ext")) { void *q = 0; hipError_t e = hipExtMallocWithFlags(&q, len, hipDeviceMallocFinegrained); if (e != hipSuccess) snprintf(why, sizeof why, "hipExtMalloc: %s", hipGetErrorString(e));
        else { void *m = mremap(q, len, len, MREMAP_MAYMOVE | MREMAP_FIXED, addr); if (m == MAP_FAILED) { snprintf(why, sizeof why, "mremap: %s", strerror(errno)); hipFree(q); } else { v.va = m; v.len = len; v.kind = 3; p = m; } } }
    else snprintf(why, sizeof why, "unknown HEAPVMM_MODE %s", md);
    if (p && p != addr) { snprintf(why, sizeof why, "mapped at %p, wanted %p", p, addr); var_free(&v); p = 0; }
    if (p && !cpu_touch((volatile char *)p, v.len, &sig)) { snprintf(why, sizeof why, "CPU touch failed, signal %d", sig); var_free(&v); p = 0; }
    if (p && (hipMemset(p, 0, v.len) != hipSuccess || hipDeviceSynchronize() != hipSuccess)) { snprintf(why, sizeof why, "hipMemset failed"); var_free(&v); p = 0; }
    (void)hipSetDevice(cur);
    if (!p) { say("heap_vmm_shim: pid %d FALLBACK to the real mmap (mode %s): %s\n", (int)getpid(), md, why); (void)hipGetLastError(); return 0; }
    g_ent[g_nent++] = v; say("heap_vmm_shim: pid %d heap %p len %zu (mapped %zu) now %s memory of device %d (%s), %.3f s\n", (int)getpid(), p, len, v.len, md, dev, why, now() - t0);
    return p;
}
static int heap_like(void *addr, size_t len, int flags)
{
    static int en = -1; static size_t minb; static uintptr_t base;
    if (en < 0) { const char *s = getenv("HEAPVMM_SHIM"); en = s && atoi(s) == 1; const char *m = getenv("HEAPVMM_MIN_MB"); minb = (m ? (size_t)atol(m) : 256) << 20;
        const char *b = getenv("_SHMEM_SHEAP_BASE"); base = 0x30000000000ull; if (b) { unsigned long long x; if (sscanf(b, "%llx", &x) == 1) base = (uintptr_t)x; } }
    if (!en || g_busy || !addr || !(flags & MAP_FIXED) || !(flags & MAP_SHARED) || (uintptr_t)addr != base) return 0;
    if (getenv("HEAPVMM_VERBOSE")) say("heap_vmm_shim: pid %d heap-base mmap %p len %zu flags 0x%x\n", (int)getpid(), addr, len, flags);
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
    if (heap_like(addr, len, flags)) { g_busy = 1; void *p = take(addr, len); g_busy = 0; if (p) return p; }
    return real(addr, len, prot, flags, fd, off);
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) { return mmap(addr, len, prot, flags, fd, off); }
int munmap(void *addr, size_t len)
{
    static munmap_f real; if (!real) real = (munmap_f)dlsym(RTLD_NEXT, "munmap");
    for (int i = 0; i < g_nent; i++) if ((char *)addr >= (char *)g_ent[i].va && (char *)addr < (char *)g_ent[i].va + g_ent[i].len) {
        if (addr == g_ent[i].va) { g_busy = 1; var_free(&g_ent[i]); g_busy = 0; g_ent[i] = g_ent[--g_nent]; return 0; }
        say("heap_vmm_shim: partial munmap %p len %zu inside the heap ignored\n", addr, len); return 0; }
    return real(addr, len);
}
#ifdef __cplusplus
}
#endif

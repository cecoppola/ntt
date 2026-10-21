/* t_vmmprobe - 1-node probe: which way gives CPU-accessible, NIC-registrable (dmabuf) device memory at a fixed VA on MI300A?  (RECVDEV1.md, branch devsrc3, benchmark only)
 * 9lj result: hipMemCreate(Pinned)+hipMemMap+hipMemSetAccess honours the VA hint but the CPU touch is a SIGSEGV.  This probe tries, per variant, at its own fixed VA:
 *   0 hip-pinned     hipMemCreate(type Pinned), hipMemMap, hipMemSetAccess (all devices)           [9lj: CPU SIGSEGV]
 *   1 hip-uncached   same with hipMemAllocationTypeUncached
 *   2 hsa-gpu        hsa_amd_vmem_address_reserve / handle_create(GPU coarse pool) / map / set_access(all GPU + CPU agents)
 *   3 hsa-gpufine    same, the GPU agent's fine-grained pool (if any)
 *   4 hsa-cpu        same, the CPU agent's fine-grained (else coarse) pool (system memory; GPU access through set_access)
 *   5 ext-fine       hipExtMallocWithFlags(hipDeviceMallocFinegrained) at its natural VA; then mremap(MREMAP_MAYMOVE|FIXED) to the fixed VA (design 2b)
 *   6 hipMalloc      control (natural VA)
 * Per variant one line each: CPU touch (guarded by SIGSEGV/SIGBUS: failure is reported, the probe goes on), CPU fill GB/s, GPU sees CPU data, hsa_amd_pointer_info, dmabuf export, process_vm_writev/readv from a child.
 * Last lines: "VMMPROBE MATRIX <variant> cpu=.. gpu=.. ptr=.. dmabuf=.. cma=.." and "VMMPROBE SUMMARY".  Usage: t_vmmprobe [MB 256] [hex VA 0x30000000000] [device 0] [only-variant -1].
 * Build: hipcc -x hip -O2 --offload-arch=gfx942 -o t_vmmprobe t_vmmprobe.c -lhsa-runtime64 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
#define NV 7
static const char *VN[NV] = { "hip-pinned", "hip-uncached", "hsa-gpu", "hsa-gpufine", "hsa-cpu", "ext-fine", "hipMalloc" };

/* ---- SIGSEGV guard */
static sigjmp_buf g_jb; static volatile int g_guard; static volatile int g_sig;
static void on_sig(int s) { if (g_guard) { g_sig = s; siglongjmp(g_jb, 1); } signal(s, SIG_DFL); raise(s); }
static int cpu_touch(volatile char *p, size_t len, int *sig)   /* 1 = write + read back of one byte per 2 MiB worked */
{
    struct sigaction sa, o1, o2; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sig; sigaction(SIGSEGV, &sa, &o1); sigaction(SIGBUS, &sa, &o2); int ok = 0; g_sig = 0;
    if (sigsetjmp(g_jb, 1) == 0) { g_guard = 1; ok = 1; for (size_t o = 0; o < len; o += (size_t)2 << 20) { p[o] = (char)((o >> 21) | 1); if (p[o] != (char)((o >> 21) | 1)) { ok = 0; break; } } p[len - 1] = 7; if (p[len - 1] != 7) ok = 0; }
    g_guard = 0; sigaction(SIGSEGV, &o1, 0); sigaction(SIGBUS, &o2, 0); *sig = g_sig; return ok;
}
__global__ void kcheck(const uint64_t *p, size_t n, unsigned long long *bad) { size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; for (; i < n; i += st) if (p[i] != (i * 0x9E3779B97F4A7C15ull)) atomicAdd(bad, 1ull); }
static int fill_check(volatile uint64_t *p, size_t n, double *w, double *r)
{
    double t0 = now(); for (size_t i = 0; i < n; i++) p[i] = i * 0x9E3779B97F4A7C15ull; double t1 = now();
    size_t bad = 0; for (size_t i = 0; i < n; i++) if (p[i] != i * 0x9E3779B97F4A7C15ull) bad++; double t2 = now(); *w = n * 8 / 1e9 / (t1 - t0); *r = n * 8 / 1e9 / (t2 - t1); return bad == 0;
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
static void var_free(var_t *v) { if (!v->va) return; if (v->kind == 0) { hipMemUnmap(v->va, v->len); hipMemAddressFree(v->va, v->len); hipMemRelease(v->hh); } else if (v->kind == 1) { hsa_amd_vmem_unmap(v->va, v->len); hsa_amd_vmem_handle_release(v->h); hsa_amd_vmem_address_free(v->va, v->len); } else if (v->kind == 2) hipFree(v->va); v->va = 0; }

static int ptrinfo(void *p, size_t len, char *out, size_t ol)
{
    hsa_amd_pointer_info_t in; memset(&in, 0, sizeof in); in.size = sizeof in; hsa_status_t s = hsa_amd_pointer_info(p, &in, 0, 0, 0);
    snprintf(out, ol, "rc %d type %d size %zu agentBase %p hostBase %p (len %zu)", (int)s, (int)in.type, (size_t)in.sizeInBytes, in.agentBaseAddress, in.hostBaseAddress, len);
    return s == HSA_STATUS_SUCCESS && in.type == HSA_EXT_POINTER_TYPE_HSA;
}
static int dmabuf(void *p, size_t len, int *rc) { int fd = -1; uint64_t off = 0; hsa_status_t s = hsa_amd_portable_export_dmabuf(p, len, &fd, &off); *rc = (int)s; if (s == HSA_STATUS_SUCCESS) { hsa_amd_portable_close_dmabuf(fd); return 1; } return 0; }
static void maps_line(void *p, char *out, size_t ol)
{
    out[0] = 0; FILE *f = fopen("/proc/self/maps", "r"); char l[300]; unsigned long a = (unsigned long)p;
    while (f && fgets(l, sizeof l, f)) { unsigned long lo, hi; if (sscanf(l, "%lx-%lx", &lo, &hi) == 2 && a >= lo && a < hi) { l[strcspn(l, "\n")] = 0; snprintf(out, ol, "%s", l); break; } }
    if (f) fclose(f); if (!out[0]) snprintf(out, ol, "(no CPU mapping at %p)", p);
}
static void cma_child(int rd, int wr, pid_t parent)
{
    uint64_t a[2]; char buf[65536], back[65536]; int res[4];
    while (read(rd, a, sizeof a) == sizeof a) {
        memset(buf, (int)a[1], sizeof buf); struct iovec l = { buf, sizeof buf }, r = { (void *)a[0], sizeof buf }; errno = 0; ssize_t w = process_vm_writev(parent, &l, 1, &r, 1, 0); res[0] = (int)w; res[1] = w < 0 ? errno : 0;
        memset(back, 0, sizeof back); struct iovec l2 = { back, sizeof back }; errno = 0; ssize_t rr = process_vm_readv(parent, &l2, 1, &r, 1, 0); res[2] = (int)rr; res[3] = rr < 0 ? errno : 0;
        if (write(wr, res, sizeof res) != sizeof res) _exit(1);
    }
    _exit(0);
}
static int pc[2], pr[2];
static int cma_test(void *p, int pattern, int *w, int *r, int *e)
{
    uint64_t a[2] = { (uint64_t)(uintptr_t)p + 4096, (uint64_t)pattern }; int res[4] = { -9, 0, -9, 0 }; if (write(pc[1], a, sizeof a) != sizeof a || read(pr[0], res, sizeof res) != sizeof res) return 0;
    *w = res[0]; *r = res[2]; *e = res[1] ? res[1] : res[3]; return res[0] == 65536 && res[2] == 65536 && ((volatile unsigned char *)p)[4096 + 100] == pattern;
}

int main(int argc, char **argv)
{
    size_t mb = argc > 1 ? (size_t)atol(argv[1]) : 256; uintptr_t base = argc > 2 ? strtoull(argv[2], 0, 16) : 0x30000000000ull; int dev = argc > 3 ? atoi(argv[3]) : 0, only = argc > 4 ? atoi(argv[4]) : -1;
    char host[64] = "?"; gethostname(host, sizeof host); printf("VMMPROBE start host %s MB %zu base %p dev %d pid %d (ROCm HIP %d)\n", host, mb, (void *)base, dev, (int)getpid(), HIP_VERSION); fflush(stdout);
    if (pipe(pc) || pipe(pr)) return 2;
    pid_t ch = fork(); if (ch == 0) { close(pc[1]); close(pr[0]); cma_child(pc[0], pr[1], getppid()); }
    close(pc[0]); close(pr[1]); prctl(PR_SET_PTRACER, (unsigned long)-1, 0, 0, 0);
    if (hipSetDevice(dev) != hipSuccess) { printf("VMMPROBE FATAL hipSetDevice\n"); return 2; } (void)hsa_init();
    { agents_t A; memset(&A, 0, sizeof A); hsa_iterate_agents(ag_cb, &A); printf("VMMPROBE agents: %d gpu %d cpu\n", A.ngpu, A.ncpu);
      for (int k = 0; k < 2; k++) { pools_t P; memset(&P, 0, sizeof P); hsa_amd_agent_iterate_memory_pools(k ? A.cpu[0] : A.gpu[dev], pool_cb, &P); printf("VMMPROBE pools %s agent: coarse %d (gran %zu) fine %d (gran %zu)\n", k ? "cpu0" : "gpu", P.hc, P.gc, P.hf, P.gf); } }
    size_t len = mb << 20, n = len / 8; unsigned long long *dbad; hipMalloc((void **)&dbad, 8);
    char rows[NV][400]; memset(rows, 0, sizeof rows);
    for (int vi = 0; vi < NV; vi++) {
        if (only >= 0 && vi != only) continue;
        const char *nm = VN[vi]; var_t v; memset(&v, 0, sizeof v); char why[200] = ""; void *want = (void *)(base + (uintptr_t)vi * ((len + ((size_t)1 << 30)) & ~(((size_t)1 << 30) - 1))), *p = 0; double t0 = now();
        if (vi == 0 || vi == 1) p = hip_vmem(vi, dev, want, len, &v, why, sizeof why);
        else if (vi >= 2 && vi <= 4) p = hsa_vmem(vi, dev, want, len, &v, why, sizeof why);
        else if (vi == 5) { hipError_t e = hipExtMallocWithFlags(&p, len, hipDeviceMallocFinegrained); if (e == hipSuccess) { v.va = p; v.len = len; v.kind = 2; snprintf(why, sizeof why, "natural VA"); } else { p = 0; snprintf(why, sizeof why, "hipExtMalloc: %s", hipGetErrorString(e)); } }
        else { hipError_t e = hipMalloc(&p, len); if (e == hipSuccess) { v.va = p; v.len = len; v.kind = 2; snprintf(why, sizeof why, "natural VA"); } else { p = 0; snprintf(why, sizeof why, "hipMalloc: %s", hipGetErrorString(e)); } }
        printf("VMMPROBE %s alloc: %s va %p wanted %p %s (%.3f s)\n", nm, p ? "ok" : "FAILED", p, want, why, now() - t0); fflush(stdout);
        if (!p) { snprintf(rows[vi], sizeof rows[vi], "VMMPROBE MATRIX %s alloc=FAILED(%s)", nm, why); continue; }
        char ml[300]; maps_line(p, ml, sizeof ml); printf("VMMPROBE %s cpu-vma: %s\n", nm, ml);
        int sig = 0, cpu = cpu_touch((volatile char *)p, v.len, &sig), gpuok = -1, ptr, dm, drc = 0, cma = -1, cw = 0, cr = 0, ce = 0; char pi[300];
        if (cpu) { double w, r; int ok = fill_check((volatile uint64_t *)p, n < v.len / 8 ? n : v.len / 8, &w, &r); hipMemset(dbad, 0, 8); hipLaunchKernelGGL(kcheck, dim3(1024), dim3(256), 0, 0, (const uint64_t *)p, n < v.len / 8 ? n : v.len / 8, dbad); hipDeviceSynchronize(); unsigned long long bad = 0; hipMemcpy(&bad, dbad, 8, hipMemcpyDeviceToHost); gpuok = ok && bad == 0;
            printf("VMMPROBE %s G1 PASS: CPU write %.2f read %.2f GB/s verified %d, GPU sees CPU data %s (bad %llu)\n", nm, w, r, ok, bad == 0 ? "yes" : "NO", bad); }
        else printf("VMMPROBE %s G1 FAIL: CPU touch raised signal %d (%s)\n", nm, sig, sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "none: data mismatch");
        ptr = ptrinfo(p, v.len, pi, sizeof pi); printf("VMMPROBE %s G2 %s: pointer_info %s\n", nm, ptr ? "PASS" : "FAIL", pi);
        dm = dmabuf(p, v.len, &drc); int dm2 = 0, drc2 = 0; if (v.len > ((size_t)128 << 20)) dm2 = dmabuf((char *)p + ((size_t)64 << 20), (size_t)64 << 20, &drc2); printf("VMMPROBE %s G3 %s: dmabuf export whole rc %d, 64 MiB sub-range rc %d\n", nm, dm ? "PASS" : "FAIL", drc, drc2);
        if (cpu) { cma = cma_test(p, 0x40 + vi, &cw, &cr, &ce); printf("VMMPROBE %s G4 %s: process_vm_writev rc %d readv rc %d errno %d (%s)\n", nm, cma ? "PASS" : "FAIL", cw, cr, ce, ce ? strerror(ce) : "ok"); }
        fflush(stdout);
        if (vi == 5 && cpu) {   /* design 2b: move the fine-grained mapping to the wished VA */
            void *q = mremap(p, v.len, v.len, MREMAP_MAYMOVE | MREMAP_FIXED, want); int er = errno;
            if (q == MAP_FAILED) printf("VMMPROBE ext-fine mremap to %p FAILED: %s\n", want, strerror(er));
            else { int s2; int c2 = cpu_touch((volatile char *)q, v.len, &s2); char pi2[300]; int p2 = ptrinfo(q, v.len, pi2, sizeof pi2); int rc2; int d2 = dmabuf(q, v.len, &rc2); int so; int oldok = cpu_touch((volatile char *)p, 4096, &so); char pi3[300]; int p3 = ptrinfo(p, v.len, pi3, sizeof pi3);
                printf("VMMPROBE ext-fine mremap to %p -> %p: CPU touch %s, pointer_info %s [%s], dmabuf rc %d, old VA CPU touch %s (HIP still knows the old VA: %s)\n", want, q, c2 ? "ok" : "FAIL", p2 ? "HSA" : "not HSA", pi2, rc2, oldok ? "ok" : "fails", p3 ? "yes" : "no");
                mremap(q, v.len, v.len, MREMAP_MAYMOVE | MREMAP_FIXED, p); }
        }
        snprintf(rows[vi], sizeof rows[vi], "VMMPROBE MATRIX %-12s cpu=%s gpu=%s ptr=%s dmabuf=%s cma=%s va=%s", nm, cpu ? "PASS" : "FAIL", gpuok < 0 ? "n/a" : gpuok ? "PASS" : "FAIL", ptr ? "PASS" : "FAIL", dm ? "PASS" : "FAIL", cma < 0 ? "n/a" : cma ? "PASS" : "FAIL", p == want ? "fixed-ok" : "natural");
        var_free(&v);
    }
    close(pc[1]); waitpid(ch, 0, 0);
    for (int vi = 0; vi < NV; vi++) if (rows[vi][0]) puts(rows[vi]);
    printf("VMMPROBE SUMMARY done\n");
    return 0;
}

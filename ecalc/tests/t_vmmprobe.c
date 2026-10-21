/* t_vmmprobe - 1-node go/no-go probe for a HIP-VMM-backed libsma symmetric heap (RECVDEV1.md, branch devsrc3; benchmark only, one process, no SHMEM).
 * Maps ONE hipMemCreate allocation of <MB> MiB (default 1024) at a fixed VA (arg 2, hex; default 0x30000000000), like tests/heap_vmm_shim.c, then prints one line per gate:
 *   VMMPROBE G0 reserve/map: the address hint honoured (p == wanted), granularity, times.
 *   VMMPROBE G1 cpu: CPU write + read-back over the whole range (pattern verified), GB/s next to malloc and shared-anonymous mmap; plus a GPU kernel check of the CPU-written data.
 *   VMMPROBE G2 ptrinfo: hsa_amd_pointer_info type (1 = HSA, a GPU allocation), sizeInBytes, hostBaseAddress; same for a hipMalloc control.
 *   VMMPROBE G3 dmabuf: hsa_amd_portable_export_dmabuf of the whole range and of a 64 MiB sub-range (rc, fd, offset); same for the hipMalloc control.
 *   VMMPROBE G4 cma: a forked, unrelated-VMA child does process_vm_writev / process_vm_readv on the range (rc, errno), and the same on a shared-anonymous control.
 * Usage: t_vmmprobe [MB] [hex VA] [device].  Build: hipcc -x hip -O2 -o t_vmmprobe t_vmmprobe.c -lhsa-runtime64 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
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
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
#define CK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { printf("VMMPROBE FATAL %s: %s\n", #x, hipGetErrorString(e_)); fflush(stdout); return 2; } } while (0)

__global__ void kcheck(const uint64_t *p, size_t n, unsigned long long *bad) { size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; for (; i < n; i += st) if (p[i] != (i * 0x9E3779B97F4A7C15ull)) atomicAdd(bad, 1ull); }

static int fill_check(volatile uint64_t *p, size_t n, double *wgbs, double *rgbs)
{
    double t0 = now(); for (size_t i = 0; i < n; i++) p[i] = i * 0x9E3779B97F4A7C15ull; double t1 = now();
    size_t bad = 0; for (size_t i = 0; i < n; i++) if (p[i] != i * 0x9E3779B97F4A7C15ull) bad++; double t2 = now();
    *wgbs = n * 8 / 1e9 / (t1 - t0); *rgbs = n * 8 / 1e9 / (t2 - t1); return bad == 0;
}
static int ptrinfo(const char *tag, void *p, size_t len)
{
    hsa_amd_pointer_info_t in; memset(&in, 0, sizeof in); in.size = sizeof in; hsa_status_t s = hsa_amd_pointer_info(p, &in, 0, 0, 0);
    printf("VMMPROBE G2 ptrinfo %s: rc %d type %d size %zu agentBase %p hostBase %p devBase %p (want size %zu)\n", tag, (int)s, (int)in.type, (size_t)in.sizeInBytes, in.agentBaseAddress, in.hostBaseAddress, in.agentBaseAddress, len);
    return s == HSA_STATUS_SUCCESS && in.type == HSA_EXT_POINTER_TYPE_HSA;
}
static int dmabuf(const char *tag, void *p, size_t len)
{
    int fd = -1; uint64_t off = 0; hsa_status_t s = hsa_amd_portable_export_dmabuf(p, len, &fd, &off);
    printf("VMMPROBE G3 dmabuf %s len %zu: rc %d fd %d offset %llu\n", tag, len, (int)s, fd, (unsigned long long)off);
    if (s == HSA_STATUS_SUCCESS) { hsa_amd_portable_close_dmabuf(fd); return 1; } return 0;
}
/* the child: unrelated process that targets the parent's VMA through CMA */
static void cma_child(int rd, int wr, pid_t parent)
{
    uint64_t a[3]; char buf[65536]; int res[4] = { 0 };
    for (int round = 0; round < 2; round++) {
        if (read(rd, a, sizeof a) != sizeof a) _exit(1);           /* a[0] = remote address, a[1] = pattern */
        memset(buf, (int)a[1], sizeof buf); struct iovec l = { buf, sizeof buf }, r = { (void *)a[0], sizeof buf };
        errno = 0; ssize_t w = process_vm_writev(parent, &l, 1, &r, 1, 0); res[0] = (int)w; res[1] = w < 0 ? errno : 0;
        char back[65536]; memset(back, 0, sizeof back); struct iovec l2 = { back, sizeof back };
        errno = 0; ssize_t rr = process_vm_readv(parent, &l2, 1, &r, 1, 0); res[2] = (int)rr; res[3] = rr < 0 ? errno : 0;
        if (write(wr, res, sizeof res) != sizeof res) _exit(1);
    }
    _exit(0);
}
int main(int argc, char **argv)
{
    size_t mb = argc > 1 ? (size_t)atol(argv[1]) : 1024; void *want = (void *)(uintptr_t)(argc > 2 ? strtoull(argv[2], 0, 16) : 0x30000000000ull); int dev = argc > 3 ? atoi(argv[3]) : 0;
    char host[64] = "?"; gethostname(host, sizeof host); printf("VMMPROBE start host %s MB %zu va %p dev %d pid %d\n", host, mb, want, dev, (int)getpid());
    int pc[2], pr[2]; if (pipe(pc) || pipe(pr)) return 2;
    pid_t ch = fork(); if (ch == 0) { close(pc[1]); close(pr[0]); cma_child(pc[0], pr[1], getppid()); }
    close(pc[0]); close(pr[1]); prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    CK(hipSetDevice(dev)); int nd = 0; CK(hipGetDeviceCount(&nd)); (void)hsa_init();
    size_t len = mb << 20, n = len / 8;
    hipMemAllocationProp prop; memset(&prop, 0, sizeof prop); prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = dev;
    size_t gran = 0; CK(hipMemGetAllocationGranularity(&gran, &prop, hipMemAllocationGranularityRecommended)); len = (len + gran - 1) / gran * gran; n = len / 8;
    double t0 = now(); hipMemGenericAllocationHandle_t h; CK(hipMemCreate(&h, len, &prop, 0)); double t1 = now();
    void *p = 0; hipError_t e = hipMemAddressReserve(&p, len, 0, want, 0); double t2 = now();
    int g0 = e == hipSuccess && p == want;
    printf("VMMPROBE G0 %s: reserve rc %d got %p wanted %p gran %zu create %.3f s reserve %.3f s\n", g0 ? "PASS" : "FAIL", (int)e, p, want, gran, t1 - t0, t2 - t1);
    if (e != hipSuccess) { printf("VMMPROBE FATAL no VA reserved\n"); return 1; }
    CK(hipMemMap(p, len, 0, h, 0)); double t3 = now();
    hipMemAccessDesc ad[16]; memset(ad, 0, sizeof ad); int na = nd < 16 ? nd : 16; for (int i = 0; i < na; i++) { ad[i].location.type = hipMemLocationTypeDevice; ad[i].location.id = i; ad[i].flags = hipMemAccessFlagsProtReadWrite; }
    CK(hipMemSetAccess(p, len, ad, na)); double t4 = now();
    printf("VMMPROBE G0 timing: map %.3f s setaccess(%d devices) %.3f s\n", t3 - t2, na, t4 - t3); fflush(stdout);
    /* G1: CPU access (a SIGSEGV here ends the probe: the last line printed tells) */
    printf("VMMPROBE G1 about to touch the range from the CPU\n"); fflush(stdout);
    double w, r, wm, rm, ws, rs; int ok = fill_check((volatile uint64_t *)p, n, &w, &r);
    uint64_t *hm = (uint64_t *)malloc(len); int okm = fill_check((volatile uint64_t *)hm, n, &wm, &rm); free(hm);
    uint64_t *sm = (uint64_t *)mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0); int oks = fill_check((volatile uint64_t *)sm, n, &ws, &rs);
    unsigned long long *dbad; CK(hipMalloc((void **)&dbad, 8)); CK(hipMemset(dbad, 0, 8)); hipLaunchKernelGGL(kcheck, dim3(1024), dim3(256), 0, 0, (const uint64_t *)p, n, dbad); CK(hipDeviceSynchronize());
    unsigned long long bad = 0; CK(hipMemcpy(&bad, dbad, 8, hipMemcpyDeviceToHost));
    printf("VMMPROBE G1 %s: cpu write %.2f read %.2f GB/s (verified %d) | malloc write %.2f read %.2f | shared-anon write %.2f read %.2f | GPU sees CPU data: %s (bad %llu)\n", ok && bad == 0 ? "PASS" : "FAIL", w, r, ok, wm, rm, ws, rs, bad == 0 ? "yes" : "NO", bad);
    (void)okm; (void)oks;
    /* G2 / G3 */
    void *ctl = 0; CK(hipMalloc(&ctl, len));
    int g2 = ptrinfo("vmm", p, len); (void)ptrinfo("hipMalloc-control", ctl, len);
    int d_all = dmabuf("vmm-whole", p, len), d_sub = dmabuf("vmm-sub64M", (char *)p + ((size_t)64 << 20), (size_t)64 << 20); int c_all = dmabuf("hipMalloc-control", ctl, len);
    printf("VMMPROBE G2 %s: hsa_amd_pointer_info reports the VMM range as an HSA allocation\n", g2 ? "PASS" : "FAIL");
    printf("VMMPROBE G3 %s: dmabuf export whole %d sub-range %d (control %d)\n", d_all && d_sub ? "PASS" : "FAIL", d_all, d_sub, c_all); fflush(stdout);
    /* G4: CMA from another process, onto the VMM range and onto a shared-anonymous control */
    int res[4]; uint64_t a[3]; int cma_vmm = 0, cma_anon = 0, rc_;
    a[0] = (uint64_t)(uintptr_t)p + 4096; a[1] = 0x5a; rc_ = (int)write(pc[1], a, sizeof a); rc_ = (int)read(pr[0], res, sizeof res);
    printf("VMMPROBE G4 vmm: writev rc %d errno %d (%s), readv rc %d errno %d, CPU sees %02x\n", res[0], res[1], res[1] ? strerror(res[1]) : "ok", res[2], res[3], ((volatile unsigned char *)p)[4096 + 100]);
    cma_vmm = res[0] == 65536 && res[2] == 65536 && ((volatile unsigned char *)p)[4096 + 100] == 0x5a;
    a[0] = (uint64_t)(uintptr_t)sm + 4096; a[1] = 0x6b; rc_ = (int)write(pc[1], a, sizeof a); rc_ = (int)read(pr[0], res, sizeof res); (void)rc_;
    cma_anon = res[0] == 65536 && res[2] == 65536;
    printf("VMMPROBE G4 %s: CMA onto the VMM range %s; control (shared-anon) %s (writev rc %d errno %d)\n", cma_vmm ? "PASS" : "FAIL", cma_vmm ? "works" : "does NOT work", cma_anon ? "works" : "failed too (method invalid)", res[0], res[1]);
    waitpid(ch, 0, 0);
    printf("VMMPROBE SUMMARY G0 %s G1 %s G2 %s G3 %s G4 %s\n", g0 ? "PASS" : "FAIL", ok && bad == 0 ? "PASS" : "FAIL", g2 ? "PASS" : "FAIL", d_all && d_sub ? "PASS" : "FAIL", cma_vmm ? "PASS" : "FAIL");
    hipMemUnmap(p, len); hipMemAddressFree(p, len); hipMemRelease(h);
    return 0;
}

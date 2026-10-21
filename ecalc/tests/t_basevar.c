/* t_basevar.c - RECVDEV1.md probes B and A (benchmark only; branch devsrc3; build on the node: hipcc -x hip -O1 + the SHMEM flags, see day/multi/gen_vmmprobe.sh).
 * One process per PE (plain srun).  usage: t_basevar <mode> [MB]
 *   0  control: libsma's default heap base (same on every PE)
 *   1  probe B: _SHMEM_SHEAP_BASE = 0x30000000000 + PE * 1 TiB (different on every PE) before shmem_init
 *   2  probe B: base = 0x30000000000 + PE * 4 MiB (different, near each other)
 *   3  probes A+B: the heap is a hipExtMallocWithFlags(fine-grained) range at its NATURAL address (per PE; run under setarch -R): _SHMEM_SHEAP_BASE = that address, and an
 *      mmap wrapper in this executable (the same place as comm_shmem.c's heap_mmap) returns it instead of mapping.  libsma then registers device memory.
 *   5  VA identity only (no SHMEM): hipSetDevice(local id % 4), hipExtMalloc, ptr/dmabuf gates, one line per process
 *   6  as 5 but every process uses device 0
 * Modes 0-3: puts, gets (all pairs, 1 MiB blocks), atomics, verify; lines "BASEPROBE ...".  Modes 5/6: "VAID ...". */
#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/personality.h>

static void *g_dev_va; static size_t g_dev_len; static volatile int g_arm, g_hit; static size_t g_hit_len;
typedef void *(*mmap_fn)(void *, size_t, int, int, int, off_t);
static void *my_mmap(void *a, size_t l, int prot, int fl, int fd, off_t o)
{
    if (g_arm && !g_hit && a == g_dev_va && (fl & MAP_SHARED) && l <= g_dev_len) { g_hit = 1; g_hit_len = l; return g_dev_va; }
    static mmap_fn next; if (!next) { next = (mmap_fn)dlsym(RTLD_NEXT, "mmap"); if (!next) return (void *)syscall(SYS_mmap, a, l, prot, fl, fd, o); }
    return next(a, l, prot, fl, fd, o);
}
extern "C" {
void *mmap(void *a, size_t l, int prot, int fl, int fd, off_t o) __THROW { return my_mmap(a, l, prot, fl, fd, o); }
void *mmap64(void *a, size_t l, int prot, int fl, int fd, off64_t o) __THROW { return my_mmap(a, l, prot, fl, fd, (off_t)o); }
}

static int ptrinfo(void *p, char *out, size_t ol)
{
    hsa_amd_pointer_info_t in; memset(&in, 0, sizeof in); in.size = sizeof in; hsa_status_t s = hsa_amd_pointer_info(p, &in, 0, 0, 0);
    snprintf(out, ol, "rc %d type %d size %zu", (int)s, (int)in.type, (size_t)in.sizeInBytes);
    return s == HSA_STATUS_SUCCESS && in.type == HSA_EXT_POINTER_TYPE_HSA;
}
static int dmabuf(void *p, size_t len, int *rc)
{
    int fd = -1; uint64_t off = 0; hsa_status_t s = hsa_amd_portable_export_dmabuf(p, len, &fd, &off); *rc = (int)s;
    if (s == HSA_STATUS_SUCCESS) { hsa_amd_portable_close_dmabuf(fd); return 1; } return 0;
}
static int geti(const char *n, int d) { const char *v = getenv(n); return v && *v ? atoi(v) : d; }
static unsigned char pat(int i, int j, size_t k) { return (unsigned char)(i * 31 + j * 7 + k * 13 + 1); }

int main(int argc, char **argv)
{
    int mode = argc > 1 ? atoi(argv[1]) : 0; size_t mb = argc > 2 ? (size_t)atol(argv[2]) : 256;
    int proc = geti("SLURM_PROCID", 0), loc = geti("SLURM_LOCALID", 0), node = geti("SLURM_NODEID", 0), nprocs = geti("SLURM_NTASKS", 1);
    char host[64] = "?"; gethostname(host, sizeof host); int pers = personality(0xffffffff);
    char tag[200]; snprintf(tag, sizeof tag, "mode %d proc %d node %d local %d host %s pid %d personality 0x%x (ASLR %s)", mode, proc, node, loc, host, (int)getpid(), pers, (pers & ADDR_NO_RANDOMIZE) ? "off" : "on");
    if (mode >= 5) {
        hipSetDevice(mode == 5 ? loc % 4 : 0); void *p = 0; size_t L = mb << 20; hipError_t e = hipExtMallocWithFlags(&p, L, hipDeviceMallocFinegrained);
        if (e != hipSuccess || !p) { printf("VAID %s: hipExtMalloc %zu MB failed: %s\n", tag, mb, hipGetErrorString(e)); return 1; }
        volatile char *c = (volatile char *)p; c[0] = 1; c[L - 1] = 2; int cpu = c[0] == 1 && c[L - 1] == 2; char pi[200]; int ptr = ptrinfo(p, pi, sizeof pi); int rc; int dm = dmabuf(p, L, &rc);
        printf("VAID %s: va %p len %zu MB cpu %s ptr %s [%s] dmabuf %s (rc %d)\n", tag, p, mb, cpu ? "PASS" : "FAIL", ptr ? "PASS" : "FAIL", pi, dm ? "PASS" : "FAIL", rc); fflush(stdout); return 0;
    }
    int dev = 0;
    if (mode == 1) { char b[40]; snprintf(b, sizeof b, "%llx", 0x30000000000ull + (unsigned long long)proc * 0x10000000000ull); setenv("_SHMEM_SHEAP_BASE", b, 1); }
    if (mode == 2) { char b[40]; snprintf(b, sizeof b, "%llx", 0x30000000000ull + (unsigned long long)proc * 0x400000ull); setenv("_SHMEM_SHEAP_BASE", b, 1); }
    if (mode == 3) {
        dev = loc % 4; hipSetDevice(dev); size_t L = (mb + 64) << 20; void *p = 0; hipError_t e = hipExtMallocWithFlags(&p, L, hipDeviceMallocFinegrained);
        if (e != hipSuccess || !p) { printf("BASEPROBE %s: hipExtMalloc failed: %s\n", tag, hipGetErrorString(e)); return 1; }
        memset(p, 0, L); char pi[200]; int ptr = ptrinfo(p, pi, sizeof pi); int rc; int dm = dmabuf(p, L, &rc);
        printf("BASEPROBE %s: device heap va %p len %zu MB ptr %s [%s] dmabuf %s (rc %d)\n", tag, p, L >> 20, ptr ? "PASS" : "FAIL", pi, dm ? "PASS" : "FAIL", rc);
        char b[40]; snprintf(b, sizeof b, "%llx", (unsigned long long)(uintptr_t)p); setenv("_SHMEM_SHEAP_BASE", b, 1); g_dev_va = p; g_dev_len = L; g_arm = 1;
    }
    char sz[40]; snprintf(sz, sizeof sz, "%zuM", mb); setenv("SHMEM_SYMMETRIC_SIZE", sz, 0); setenv("XT_SYMMETRIC_HEAP_SIZE", sz, 0);
    printf("BASEPROBE %s: before shmem_init, _SHMEM_SHEAP_BASE=%s\n", tag, getenv("_SHMEM_SHEAP_BASE") ? getenv("_SHMEM_SHEAP_BASE") : "(default)"); fflush(stdout);
    shmem_init(); g_arm = 0; int me = shmem_my_pe(), np = shmem_n_pes();
    const size_t BLK = (size_t)1 << 20; unsigned char *sym = (unsigned char *)shmem_malloc(BLK * np), *src = (unsigned char *)malloc(BLK), *dst = (unsigned char *)malloc(BLK); long *ctr = (long *)shmem_malloc(sizeof(long));
    printf("BASEPROBE %s: pe %d/%d initialised; sym %p ctr %p; wrapper hit %d (len %zu MB)\n", tag, me, np, (void *)sym, (void *)ctr, g_hit, g_hit_len >> 20); fflush(stdout);
    if (!sym || !ctr) { printf("BASEPROBE %s: shmem_malloc failed\n", tag); return 1; }
    memset(sym, 0, BLK * np); *ctr = 0; shmem_barrier_all();
    for (int j = 0; j < np; j++) { for (size_t k = 0; k < BLK; k++) src[k] = pat(me, j, k); shmem_putmem(sym + (size_t)me * BLK, src, BLK, j); }
    shmem_quiet(); shmem_barrier_all();
    long badp = 0; for (int i = 0; i < np; i++) for (size_t k = 0; k < BLK; k++) badp += sym[(size_t)i * BLK + k] != pat(i, me, k);
    long badg = 0; for (int j = 0; j < np; j++) for (int i = 0; i < np; i++) { memset(dst, 0, BLK); shmem_getmem(dst, sym + (size_t)i * BLK, BLK, j); for (size_t k = 0; k < BLK; k++) badg += dst[k] != pat(i, j, k); }
    for (int j = 0; j < np; j++) shmem_long_atomic_add(ctr, 1, j);
    shmem_quiet(); shmem_barrier_all(); long fa = shmem_long_atomic_fetch_add(ctr, 0, (me + 1) % np); int bada = (*ctr != np) || (fa != np);
    printf("BASEPROBE %s: pe %d RESULT puts %s (%ld bad) gets %s (%ld bad) atomics %s (ctr %ld, remote %ld, want %d)\n", tag, me, badp ? "FAIL" : "PASS", badp, badg ? "FAIL" : "PASS", badg, bada ? "FAIL" : "PASS", *ctr, fa, np);
    fflush(stdout); shmem_barrier_all(); shmem_finalize(); return (badp || badg || bada) ? 2 : 0;
}

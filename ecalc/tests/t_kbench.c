/* t_kbench.c - the exchange block-copy kernels on three memory kinds (KT: results/SHMEM.md; the prerequisite of the staging question).
 *
 * Runs the shapes of k_btrans / k_ccopy (and a wide, unrolled variant) from a source buffer to a destination buffer, each hipMalloc
 * ("dev"), fine-grained hipExtMallocWithFlags ("fine") or 2 MiB-THP host memory, NUMA-bound to the APU's node and hipHostRegister'd ("host"),
 * on one or several APUs at once (one thread each), and prints the GB/s (bytes copied per second; median of the repetitions).
 *
 *   t_kbench [--apus N] [--mib 16,64 | --kib 512,2048,6784] [--kern bt,cc,wide] [--reps R] [--secs S] [--kinds dev,fine,host,shm] [--gap-us U]
 *     --kib K,..  sizes in KiB (the in-situ k_ccopy mean is 6.8 MB per launch; the default sizes are 16 and 64 MiB)
 *     shm         a MAP_SHARED /dev/shm file (the proxy pool's form), MADV_HUGEPAGE + MADV_COLLAPSE, hipHostRegister (host = anonymous THP)
 *     --gap-us U  spin U microseconds after every repetition (an idle stream between launches)
 *     --secs S   keep looping every case for at least S seconds (to overlap NIC traffic that runs in another process on the node, e.g. a t_comm
 *                bandwidth loop); the line then also carries the slowest/fastest repetition.
 * One line per case and APU:
 *   kbench apu <d> kern <bt|cc|wide> src <kind> dst <kind> mib <M> reps <R> gbps_med <x> gbps_min <x> gbps_max <x> gbps_mean <x> kib <K>   (mean = bytes over the summed time, the in-situ number)
 * Kernels: bt = k_btrans shape (64 blocks per block-row x 64 rows, one uint4 per thread-iteration), cc = k_ccopy (456 blocks, unroll 4),
 * wide = 1024 blocks, unroll 8.  Digits of ecalc are not involved; the copy result is checked (VERIFY OK). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <hip/hip_runtime.h>

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define THP (2u << 20)

__global__ void k_bt(uint4 *dst, const uint4 *src, size_t w)               /* k_btrans: block-row blockIdx.y, grid.x strides */
{
    uint4 *d = dst + (size_t)blockIdx.y * w; const uint4 *s = src + (size_t)blockIdx.y * w;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < w; i += (size_t)gridDim.x * blockDim.x) d[i] = s[i];
}
template <int U> __global__ void __launch_bounds__(256) k_un(uint4 *d, const uint4 *s, size_t n)   /* k_ccopy: U independent loads, then the stores */
{
    size_t i = (size_t)blockIdx.x * 256 + threadIdx.x, st = (size_t)gridDim.x * 256;
    for (; i + (U - 1) * st < n; i += U * st) { uint4 v[U];
#pragma unroll
        for (int u = 0; u < U; u++) v[u] = s[i + u * st];
#pragma unroll
        for (int u = 0; u < U; u++) d[i + u * st] = v[u]; }
    for (; i < n; i += st) d[i] = s[i];
}

enum { K_DEV, K_FINE, K_HOST, K_SHM };
static const char *kind_n[4] = { "dev", "fine", "host", "shm" };
struct cfg { double gap_us; int apus, nmib, mib[8], nk, kern[3], reps, nkind, kinds[8]; double secs; };   /* mib[]: sizes in KiB (--mib M gives M*1024, --kib K gives K) */
static struct cfg C;
static int g_bad;

static int numa_of(int dev)
{
    char bus[32], path[128], b[16]; if (hipDeviceGetPCIBusId(bus, sizeof bus, dev) != hipSuccess) return -1;
    for (char *p = bus; *p; p++) if (*p >= 'A' && *p <= 'F') *p += 32;
    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/numa_node", bus);
    int f = open(path, O_RDONLY); if (f < 0) return -1; ssize_t n = read(f, b, 15); close(f); if (n <= 0) return -1; b[n] = 0; return atoi(b);
}
static void *alloc_kind(int kind, size_t bytes, int dev)
{
    void *p = 0;
    if (kind == K_DEV) HIP_CHECK(hipMalloc(&p, bytes));
    else if (kind == K_FINE) HIP_CHECK(hipExtMallocWithFlags(&p, bytes, hipDeviceMallocFinegrained));
    else {
        size_t len = (bytes + THP - 1) & ~(size_t)(THP - 1);
        char *m;
        if (kind == K_SHM) {                                                                 /* the pool's form: a MAP_SHARED /dev/shm file (as the proxy heap), unlinked at once */
            static int seq; char path[96]; snprintf(path, sizeof path, "/dev/shm/ecalc_kbench_%d_%d_%d", (int)getpid(), dev, __atomic_fetch_add(&seq, 1, __ATOMIC_RELAXED));
            int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600); if (fd < 0) { perror("open /dev/shm"); exit(1); }
            unlink(path); if (ftruncate(fd, (off_t)(len + THP))) { perror("ftruncate"); exit(1); }
            m = (char *)mmap(0, len + THP, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd);
        } else m = (char *)mmap(0, len + THP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) { perror("mmap"); exit(1); }
        p = (void *)(((uintptr_t)m + THP - 1) & ~(uintptr_t)(THP - 1));
        madvise(p, len, MADV_HUGEPAGE);
        int nd = numa_of(dev);
        if (nd >= 0 && nd < 64) { unsigned long mask = 1ul << nd; syscall(SYS_mbind, p, len, 2 /*MPOL_BIND*/, &mask, 64ul, 0); }
        memset(p, 0, len);                                                                   /* first touch here: the pages land on the bound node */
        if (kind == K_SHM) { if (madvise(p, len, 25 /*MADV_COLLAPSE*/)) fprintf(stderr, "kbench: shm MADV_COLLAPSE: errno %d (2 MiB pages not guaranteed)\n", errno); }
        HIP_CHECK(hipHostRegister(p, len, hipHostRegisterPortable));
    }
    return p;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

struct targ { int dev; char out[4096 * 64]; size_t olen; };
static void *run(void *v)
{
    struct targ *ta = (struct targ *)v; int dev = ta->dev;
    HIP_CHECK(hipSetDevice(dev)); hipStream_t s; HIP_CHECK(hipStreamCreate(&s)); hipEvent_t e0, e1; HIP_CHECK(hipEventCreate(&e0)); HIP_CHECK(hipEventCreate(&e1));
    for (int mi = 0; mi < C.nmib; mi++) {
        size_t bytes = (size_t)C.mib[mi] << 10; size_t n = bytes >> 4;
        for (int sk = 0; sk < C.nkind; sk++) for (int dk = 0; dk < C.nkind; dk++) {
            void *src = alloc_kind(C.kinds[sk], bytes, dev), *dst = alloc_kind(C.kinds[dk], bytes, dev);
            { uint64_t *h = (uint64_t *)malloc(bytes); for (size_t i = 0; i < bytes / 8; i++) h[i] = i * 0x9E3779B97F4A7C15ull; HIP_CHECK(hipMemcpy(src, h, bytes, hipMemcpyDefault)); free(h); }
            for (int ki = 0; ki < C.nk; ki++) {
                int kern = C.kern[ki]; int cap = 64 * 64; int cap_reps = 1 << 20;
                (void)cap;
                double *t = (double *)malloc(sizeof(double) * (size_t)cap_reps); int nr = 0; double t_start = now();
                for (int r = -2; ; r++) {
                    HIP_CHECK(hipEventRecord(e0, s));
                    if (kern == 0) { unsigned rows = 64; size_t w = n / rows; hipLaunchKernelGGL(k_bt, dim3(64, rows), dim3(256), 0, s, (uint4 *)dst, (const uint4 *)src, w); }
                    else if (kern == 1) hipLaunchKernelGGL(k_un<4>, dim3(456), dim3(256), 0, s, (uint4 *)dst, (const uint4 *)src, n);
                    else hipLaunchKernelGGL(k_un<8>, dim3(1024), dim3(256), 0, s, (uint4 *)dst, (const uint4 *)src, n);
                    HIP_CHECK(hipEventRecord(e1, s)); HIP_CHECK(hipEventSynchronize(e1));
                    if (C.gap_us > 0) { double t0 = now(); while (now() - t0 < C.gap_us * 1e-6) ; }                 /* an idle gap between launches (the exchange's stream is not busy back to back) */
                    float ms; HIP_CHECK(hipEventElapsedTime(&ms, e0, e1));
                    if (r >= 0 && nr < cap_reps) t[nr++] = ms * 1e-3;
                    if (r + 1 >= C.reps && now() - t_start >= C.secs) break;
                }
                { uint64_t *h = (uint64_t *)malloc(bytes); HIP_CHECK(hipMemcpy(h, dst, bytes, hipMemcpyDefault));
                  for (size_t i = 0; i < bytes / 8; i += 4099) if (h[i] != i * 0x9E3779B97F4A7C15ull) { g_bad = 1; break; } free(h); }
                double tsum = 0; for (int i = 0; i < nr; i++) tsum += t[i];
                qsort(t, nr, sizeof(double), cmpd);
                double g = bytes / 1e9;
                ta->olen += snprintf(ta->out + ta->olen, sizeof ta->out - ta->olen, "kbench apu %d kern %s src %s dst %s mib %d reps %d gbps_med %.1f gbps_min %.1f gbps_max %.1f gbps_mean %.1f kib %d\n",
                    dev, kern == 0 ? "bt" : kern == 1 ? "cc" : "wide", kind_n[C.kinds[sk]], kind_n[C.kinds[dk]], C.mib[mi] >> 10, nr, g / t[nr / 2], g / t[nr - 1], g / t[0], g * nr / tsum, C.mib[mi]);
                free(t);
            }
            if (C.kinds[sk] >= K_HOST) (void)hipHostUnregister(src);
            if (C.kinds[dk] >= K_HOST) (void)hipHostUnregister(dst);
            if (C.kinds[sk] < K_HOST) (void)hipFree(src);
            if (C.kinds[dk] < K_HOST) (void)hipFree(dst);
        }
    }
    return 0;
}
static int split(const char *a, char **out)
{
    static char buf[256]; strncpy(buf, a, 255); int n = 0; for (char *p = strtok(buf, ","); p && n < 8; p = strtok(0, ",")) out[n++] = p; return n;
}
int main(int argc, char **argv)
{
    C.apus = 1; C.nmib = 2; C.mib[0] = 16 << 10; C.mib[1] = 64 << 10; C.nk = 3; C.kern[0] = 0; C.kern[1] = 1; C.kern[2] = 2; C.reps = 20; C.nkind = 3; C.kinds[0] = K_DEV; C.kinds[1] = K_FINE; C.kinds[2] = K_HOST; C.secs = 0;
    for (int i = 1; i < argc; i++) {
        char *l[8]; int n;
        if (!strcmp(argv[i], "--apus") && i + 1 < argc) C.apus = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) C.reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) C.secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--mib") && i + 1 < argc) { n = split(argv[++i], l); C.nmib = n; for (int j = 0; j < n; j++) C.mib[j] = atoi(l[j]) * 1024; }
        else if (!strcmp(argv[i], "--kib") && i + 1 < argc) { n = split(argv[++i], l); C.nmib = n; for (int j = 0; j < n; j++) C.mib[j] = atoi(l[j]); }
        else if (!strcmp(argv[i], "--gap-us") && i + 1 < argc) C.gap_us = atof(argv[++i]);
        else if (!strcmp(argv[i], "--kern") && i + 1 < argc) { n = split(argv[++i], l); C.nk = n; for (int j = 0; j < n; j++) C.kern[j] = !strcmp(l[j], "bt") ? 0 : !strcmp(l[j], "cc") ? 1 : 2; }
        else if (!strcmp(argv[i], "--kinds") && i + 1 < argc) { n = split(argv[++i], l); C.nkind = n; for (int j = 0; j < n; j++) C.kinds[j] = !strcmp(l[j], "dev") ? K_DEV : !strcmp(l[j], "fine") ? K_FINE : !strcmp(l[j], "shm") ? K_SHM : K_HOST; }
        else { fprintf(stderr, "usage: t_kbench [--apus N] [--mib 16,64] [--kern bt,cc,wide] [--kib K,..] [--kinds dev,fine,host,shm] [--gap-us U] [--reps R] [--secs S]\n"); return 2; }
    }
    int nd = 0; HIP_CHECK(hipGetDeviceCount(&nd)); if (C.apus > nd) C.apus = nd; if (C.apus > 8) C.apus = 8;
    pthread_t th[8]; static struct targ ta[8];
    for (int d = 0; d < C.apus; d++) { ta[d].dev = d; pthread_create(&th[d], 0, run, &ta[d]); }
    for (int d = 0; d < C.apus; d++) { pthread_join(th[d], 0); fputs(ta[d].out, stdout); }
    printf(g_bad ? "VERIFY FAILED\n" : "VERIFY OK\n");
    return g_bad;
}

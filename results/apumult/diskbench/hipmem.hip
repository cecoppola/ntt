// hipmem.hip -- APU memory / spill experiments (agent M, apumult spill study).
// module load rocm; hipcc -O2 hipmem.hip -o hipmem -lpthread
//   ./hipmem info                      hipMemGetInfo on every device + /proc/meminfo
//   ./hipmem alloc G [chunkGiB]        hipMalloc G GiB on each of the 4 APUs (in chunks, default 4 GiB) + hipMemset; report
//   ./hipmem madv G                    malloc G GiB, touch, madvise(MADV_DONTNEED), free -- hipMemGetInfo at each step
//   ./hipmem spill G mode ndev [dir]   hipMalloc G GiB on devices 0..ndev-1 (one thread each), write to dir/spill_d.bin,
//                                      clear device buffer, read back, verify. mode: bounce (hipHostMalloc 2x1 GiB, D2H then
//                                      O_DIRECT write, double-buffered) | direct (O_DIRECT write()/read() on the hipMalloc pointer)
//                                      | cpucopy (CPU memcpy from hipMalloc pointer into malloc'd aligned buffer, then O_DIRECT)
#define _GNU_SOURCE
#include <hip/hip_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#define CK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "%s:%d %s -> %s\n", __FILE__, __LINE__, #x, hipGetErrorString(e_)); exit(1); } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static const double GiB = 1073741824.0;
static int ndevs(void) { int n; CK(hipGetDeviceCount(&n)); return n; }
static void info(const char *tag) {
    FILE *f = fopen("/proc/meminfo", "r"); char l[256]; unsigned long long v; double a = 0, c = 0, fr = 0;
    while (fgets(l, sizeof l, f)) { if (sscanf(l, "MemAvailable: %llu", &v) == 1) a = v / 1048576.0; else if (sscanf(l, "Cached: %llu", &v) == 1) c = v / 1048576.0; else if (sscanf(l, "MemFree: %llu", &v) == 1) fr = v / 1048576.0; }
    fclose(f);
    printf("INFO %-22s MemFree %6.1f MemAvail %6.1f Cached %6.1f GiB | hipMemGetInfo free/total GiB:", tag, fr, a, c);
    for (int d = 0; d < ndevs(); d++) { size_t fb, tb; CK(hipSetDevice(d)); CK(hipMemGetInfo(&fb, &tb)); printf(" %d:%.1f/%.1f", d, fb / GiB, tb / GiB); }
    printf("\n"); fflush(stdout);
}
__global__ void kfill(unsigned long long *p, size_t n, unsigned long long seed) { for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) p[i] = (i + 1) * 0x9E3779B97F4A7C15ull ^ seed; }
__global__ void kcheck(const unsigned long long *p, size_t n, unsigned long long seed, unsigned long long *bad) { unsigned long long b = 0; for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) b += p[i] != ((i + 1) * 0x9E3779B97F4A7C15ull ^ seed); if (b) atomicAdd(bad, b); }

static void do_alloc(double g, double chunk) {
    int nd = ndevs(); info("alloc-before");
    size_t cb = (size_t)(chunk * GiB); int nc = (int)(g / chunk + 0.5); double t0 = now(); size_t tot = 0; int fail = 0;
    void **ptr = (void **)calloc(nd * nc, sizeof(void *));
    for (int d = 0; d < nd && !fail; d++) { CK(hipSetDevice(d)); for (int k = 0; k < nc; k++) { hipError_t e = hipMalloc(&ptr[d * nc + k], cb); if (e != hipSuccess) { printf("hipMalloc FAILED on APU%d chunk %d after %.1f GiB total: %s\n", d, k, tot / GiB, hipGetErrorString(e)); fail = 1; break; } CK(hipMemset(ptr[d * nc + k], 1, cb)); tot += cb; } CK(hipDeviceSynchronize()); }
    printf("ALLOC %s: %.1f GiB hipMalloc'd + memset across %d APUs in %.1f s\n", fail ? "PARTIAL" : "OK", tot / GiB, nd, now() - t0);
    info("alloc-holding");
    for (int i = 0; i < nd * nc; i++) if (ptr[i]) { CK(hipSetDevice(i / nc)); CK(hipFree(ptr[i])); }
    info("alloc-freed");
}
static void do_madv(double g) {
    size_t n = (size_t)(g * GiB); info("madv-before");
    char *p = (char *)mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    double t0 = now(); memset(p, 1, n); printf("touched %.0f GiB in %.1f s\n", g, now() - t0); info("madv-touched");
    t0 = now(); if (madvise(p, n, MADV_DONTNEED)) perror("madvise"); printf("madvise(DONTNEED) %.0f GiB in %.2f s\n", g, now() - t0); info("madv-after-DONTNEED");
    // same for a malloc'd buffer (glibc: large malloc = mmap)
    char *q = (char *)malloc(n); memset(q, 2, n); info("malloc-touched");
    t0 = now(); if (madvise((void *)(((size_t)q + 4095) & ~4095ull), n - 8192, MADV_DONTNEED)) perror("madvise"); printf("madvise malloc'd %.2f s\n", now() - t0); info("malloc-after-DONTNEED");
    free(q); munmap(p, n); info("madv-freed");
}

struct job { int dev; size_t n; const char *mode, *dir; double tw, tr; unsigned long long bad; int err; };
static int wr_all(int fd, const void *b, size_t len, off_t off) { size_t got = 0; while (got < len) { ssize_t r = pwrite(fd, (const char *)b + got, len - got, off + got); if (r <= 0) { fprintf(stderr, "pwrite(%p,%zu): %s\n", b, len - got, r < 0 ? strerror(errno) : "0"); return -1; } got += r; } return 0; }
static int rd_all(int fd, void *b, size_t len, off_t off) { size_t got = 0; while (got < len) { ssize_t r = pread(fd, (char *)b + got, len - got, off + got); if (r <= 0) { fprintf(stderr, "pread(%p,%zu): %s\n", b, len - got, r < 0 ? strerror(errno) : "0"); return -1; } got += r; } return 0; }
static void *spill(void *arg) {
    struct job *j = (struct job *)arg; CK(hipSetDevice(j->dev)); char fn[512]; snprintf(fn, sizeof fn, "%s/spill_%d.bin", j->dir, j->dev);
    unsigned long long *d, *bad; if (!strcmp(j->mode, "managed")) CK(hipMallocManaged((void **)&d, j->n, hipMemAttachGlobal)); else CK(hipMalloc(&d, j->n)); CK(hipMalloc(&bad, 8)); CK(hipMemset(bad, 0, 8));
    unsigned long long seed = 0x1234567ull * (j->dev + 1); size_t nw = j->n / 8;
    kfill<<<1024, 256>>>(d, nw, seed); CK(hipDeviceSynchronize());
    const size_t C = 1ull << 30; size_t nch = j->n / C; hipStream_t s; CK(hipStreamCreate(&s));
    char *hb[2] = {0, 0}; hipEvent_t ev[2];
    if (!strcmp(j->mode, "bounce")) { for (int k = 0; k < 2; k++) { CK(hipHostMalloc((void **)&hb[k], C, 0)); CK(hipEventCreate(&ev[k])); } }
    if (!strcmp(j->mode, "cpucopy")) { for (int k = 0; k < 2; k++) if (posix_memalign((void **)&hb[k], 4096, C)) abort(); }
    int fd = open(fn, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644); if (fd < 0) { perror("open"); exit(1); }
    // ---- write ----
    double t0 = now();
    if (!strcmp(j->mode, "bounce")) {
        CK(hipMemcpyAsync(hb[0], (char *)d, C, hipMemcpyDeviceToHost, s)); CK(hipEventRecord(ev[0], s));
        for (size_t i = 0; i < nch; i++) {
            if (i + 1 < nch) { CK(hipMemcpyAsync(hb[(i + 1) & 1], (char *)d + (i + 1) * C, C, hipMemcpyDeviceToHost, s)); CK(hipEventRecord(ev[(i + 1) & 1], s)); }
            CK(hipEventSynchronize(ev[i & 1])); if (wr_all(fd, hb[i & 1], C, i * C)) { j->err = 1; break; }
            // the next async copy into hb[i&1] is issued only after this write returns (next iteration), so no overlap hazard
        }
    } else if (!strcmp(j->mode, "direct") || !strcmp(j->mode, "managed")) {
        for (size_t i = 0; i < nch; i++) if (wr_all(fd, (char *)d + i * C, C, i * C)) { j->err = 1; break; }
    } else {  // cpucopy
        for (size_t i = 0; i < nch; i++) { memcpy(hb[0], (char *)d + i * C, C); if (wr_all(fd, hb[0], C, i * C)) { j->err = 1; break; } }
    }
    fsync(fd); j->tw = now() - t0;
    CK(hipMemset(d, 0, j->n)); CK(hipDeviceSynchronize());
    // ---- read back ----
    t0 = now();
    if (!j->err) {
        if (!strcmp(j->mode, "bounce")) {
            for (size_t i = 0; i < nch; i++) {   // read chunk i into hb[i&1] while the H2D copy of chunk i-1 runs
                if (i >= 2) CK(hipEventSynchronize(ev[i & 1]));
                if (rd_all(fd, hb[i & 1], C, i * C)) { j->err = 2; break; }
                CK(hipMemcpyAsync((char *)d + i * C, hb[i & 1], C, hipMemcpyHostToDevice, s)); CK(hipEventRecord(ev[i & 1], s));
            }
            CK(hipStreamSynchronize(s));
        } else if (!strcmp(j->mode, "direct") || !strcmp(j->mode, "managed")) {
            for (size_t i = 0; i < nch; i++) if (rd_all(fd, (char *)d + i * C, C, i * C)) { j->err = 2; break; }
        } else {
            for (size_t i = 0; i < nch; i++) { if (rd_all(fd, hb[0], C, i * C)) { j->err = 2; break; } memcpy((char *)d + i * C, hb[0], C); }
        }
    }
    j->tr = now() - t0;
    kcheck<<<1024, 256>>>(d, nw, seed, bad); CK(hipDeviceSynchronize()); CK(hipMemcpy(&j->bad, bad, 8, hipMemcpyDeviceToHost));
    close(fd); unlink(fn);
    for (int k = 0; k < 2; k++) if (hb[k]) { if (!strcmp(j->mode, "bounce")) CK(hipHostFree(hb[k])); else free(hb[k]); }
    CK(hipFree(d)); CK(hipFree(bad));
    return NULL;
}
static void do_spill(double g, const char *mode, int nd, const char *dir) {
    struct job j[8]; pthread_t th[8]; info("spill-before"); double t0 = now();
    for (int i = 0; i < nd; i++) { j[i] = (struct job){i, (size_t)(g * GiB), mode, dir, 0, 0, 0, 0}; pthread_create(&th[i], NULL, spill, &j[i]); }
    for (int i = 0; i < nd; i++) pthread_join(th[i], NULL);
    double wall = now() - t0; double tw = 0, tr = 0;
    for (int i = 0; i < nd; i++) { printf("  APU%d %s %.0f GiB: write+fsync %.2f s = %.2f GB/s, read %.2f s = %.2f GB/s, err %d, mismatches %llu\n", i, mode, g, j[i].tw, j[i].n / j[i].tw / 1e9, j[i].tr, j[i].n / j[i].tr / 1e9, j[i].err, j[i].bad); if (j[i].tw > tw) tw = j[i].tw; if (j[i].tr > tr) tr = j[i].tr; }
    printf("SPILL %s %d APU x %.0f GiB: aggregate write %.2f GB/s, read %.2f GB/s (slowest thread), wall %.1f s\n", mode, nd, g, nd * g * GiB / tw / 1e9, nd * g * GiB / tr / 1e9, wall);
    info("spill-after");
}
// madvalloc H G: touch H GiB of host memory, try hipMalloc G GiB per APU (4 GiB chunks); free; madvise(DONTNEED) the host
// buffer; retry the same hipMalloc.  Answers: does host memory given back by madvise become available to hipMalloc?
static int try_alloc(double g, const char *tag) {
    int nd = ndevs(); int nc = (int)(g / 4 + 0.5); void **ptr = (void **)calloc(nd * nc, sizeof(void *)); double tot = 0; int ok = 1; double t0 = now();
    for (int d = 0; d < nd && ok; d++) { CK(hipSetDevice(d)); for (int k = 0; k < nc; k++) { if (hipMalloc(&ptr[d * nc + k], 4 * (size_t)GiB) != hipSuccess) { (void)hipGetLastError(); ok = 0; break; } if (hipMemset(ptr[d * nc + k], 1, 4 * (size_t)GiB) != hipSuccess || hipDeviceSynchronize() != hipSuccess) { ok = 0; break; } tot += 4; } }
    printf("TRYALLOC %-18s %s: %.0f of %.0f GiB hipMalloc'd+memset in %.1f s\n", tag, ok ? "OK" : "FAILED", tot, g * nd, now() - t0); info(tag);
    for (int i = 0; i < nd * nc; i++) if (ptr[i]) { CK(hipSetDevice(i / nc)); CK(hipFree(ptr[i])); }
    free(ptr); return ok;
}
static void do_madvalloc(double h, double g, int skip_held) {
    size_t n = (size_t)(h * GiB); info("ma-before");
    char *p = (char *)mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); memset(p, 1, n); info("ma-host-touched");
    if (!skip_held) try_alloc(g, "with-host-held");   // NB: if it does not fit, the OOM killer kills the process (hipMalloc does not fail)
    madvise(p, n, MADV_DONTNEED); info("ma-after-madvise");
    try_alloc(g, "after-madvise");
    munmap(p, n);
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "info")) info(argc > 2 ? argv[2] : "now");
    else if (!strcmp(argv[1], "alloc")) do_alloc(atof(argv[2]), argc > 3 ? atof(argv[3]) : 4);
    else if (!strcmp(argv[1], "madv")) do_madv(atof(argv[2]));
    else if (!strcmp(argv[1], "madvalloc")) do_madvalloc(atof(argv[2]), atof(argv[3]), argc > 4 ? atoi(argv[4]) : 0);
    else if (!strcmp(argv[1], "tryalloc")) try_alloc(atof(argv[2]), "tryalloc");
    else if (!strcmp(argv[1], "spill")) do_spill(atof(argv[2]), argv[3], atoi(argv[4]), argc > 5 ? argv[5] : "/tmp");
    return 0;
}

/* mem.c - see mem.h */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include <omp.h>
#include <sys/mman.h>
#include <hip/hip_runtime.h>
#include "mem.h"

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

double mem_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }

int mem_numa_node_of_device(int dev) { return dev; }

static int node_cpuset(int node, cpu_set_t *set)
{
    char path[80], buf[1024];
    int n = 0;
    CPU_ZERO(set);
    snprintf(path, sizeof path, "/sys/devices/system/node/node%d/cpulist", node);
    FILE *f = fopen(path, "r");
    if (f && fgets(buf, sizeof buf, f)) {
        char *tok = strtok(buf, ",\n");
        while (tok) {
            int lo, hi;
            if (sscanf(tok, "%d-%d", &lo, &hi) == 2) { for (int c = lo; c <= hi; c++) { CPU_SET(c, set); n++; } }
            else if (sscanf(tok, "%d", &lo) == 1) { CPU_SET(lo, set); n++; }
            tok = strtok(NULL, ",\n");
        }
    }
    if (f) fclose(f);
    return n;
}
int mem_ncpus_node(int node) { cpu_set_t s; return node_cpuset(node, &s); }
void mem_pin_to_node(int node)
{
    cpu_set_t set;
    if (node_cpuset(node, &set) > 0) sched_setaffinity(0, sizeof set, &set);
}
static __thread int t_home = -1;                    /* WP3: the thread's home node once mem_pin_threads ran */
void mem_unpin(void)
{
    if (t_home >= 0) { mem_pin_to_node(t_home); return; }
    cpu_set_t set; CPU_ZERO(&set);
    for (int c = 0; c < CPU_SETSIZE; c++) CPU_SET(c, &set);
    sched_setaffinity(0, sizeof set, &set);
}
static int g_nnodes = 0;
void mem_pin_threads(int nnodes)
{
    g_nnodes = nnodes;
#pragma omp parallel
    {
        int nth = omp_get_num_threads(), tid = omp_get_thread_num();
        t_home = tid * nnodes / nth;
        mem_pin_to_node(t_home);
    }
}
int mem_thread_home(void) { return t_home; }
int mem_region_threads(int *rank)                    /* this thread's rank and count within its node's threads */
{
    int nth = omp_get_num_threads(), tid = omp_get_thread_num(), n = g_nnodes ? g_nnodes : 1;
    int node = tid * n / nth, t0 = (node * nth + n - 1) / n, t1 = ((node + 1) * nth + n - 1) / n;
    *rank = tid - t0;
    return t1 - t0;
}

/* registry of registered blocks (dev = -1) and device pools (dev >= 0: hipMalloc, CPU-accessible) */
static struct { void *p; size_t bytes; int dev; } *reg;
static int nreg = 0, reg_cap = 0;
static void reg_grow(void) { if (nreg >= reg_cap) { reg_cap = reg_cap ? 2 * reg_cap : 256; reg = (typeof(reg))realloc(reg, reg_cap * sizeof *reg); } }
static void reg_add(void *p, size_t bytes) { reg_grow(); reg[nreg].p = p; reg[nreg].bytes = bytes; reg[nreg].dev = -1; nreg++; }
static void reg_del(void *p) { for (int i = 0; i < nreg; i++) if (reg[i].p == p) { reg[i] = reg[--nreg]; return; } }
int mem_is_registered(const void *p, size_t bytes)
{
    const char *c = (const char *)p;
    for (int i = 0; i < nreg; i++) {
        const char *b = (const char *)reg[i].p;
        if (c >= b && c + bytes <= b + reg[i].bytes) return 1;
    }
    return 0;
}
int mem_dev_of(const void *p)
{
    const char *c = (const char *)p;
    for (int i = 0; i < nreg; i++) {
        const char *b = (const char *)reg[i].p;
        if (c >= b && c < b + reg[i].bytes) return reg[i].dev;
    }
    return -1;
}
void *mem_dev_alloc(int dev, size_t bytes)
{
    void *p; int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev));
    double t0 = mem_now();
    HIP_CHECK(hipMalloc(&p, bytes));
    double t1 = mem_now();
    if (!getenv("MEM_NO_DEV_MEMSET")) { HIP_CHECK(hipMemset(p, 0, bytes)); HIP_CHECK(hipDeviceSynchronize()); }   /* map the pages now */
    if (getenv("RNS_VERBOSE")) printf("mem_dev_alloc: dev %d %.1f GB: malloc %.2f s memset %.2f s\n", dev, bytes / 1e9, t1 - t0, mem_now() - t1);
    HIP_CHECK(hipSetDevice(cur));
    reg_grow(); reg[nreg].p = p; reg[nreg].bytes = bytes; reg[nreg].dev = dev; nreg++;
    return p;
}
void mem_dev_forget(void *p) { reg_del(p); }         /* drop from the registry without freeing (ownership passed on) */
void mem_dev_free(void *p)
{
    int dev = mem_dev_of(p), cur; if (dev < 0) return;
    HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev)); HIP_CHECK(hipFree(p)); HIP_CHECK(hipSetDevice(cur));
    reg_del(p);
}
void mem_dev_copy_on(int dev, void *dst, const void *src, size_t bytes)   /* DMA copy on device dev's engine */
{
    int cur; HIP_CHECK(hipGetDevice(&cur));
    HIP_CHECK(hipSetDevice(dev));
    HIP_CHECK(hipMemcpy(dst, src, bytes, hipMemcpyDefault));
    HIP_CHECK(hipSetDevice(cur));
}
void mem_dev_copy(void *dst, const void *src, size_t bytes)   /* DMA copy between any of: device pools, registered host, pageable host */
{
    int cur, dd = mem_dev_of(dst), ds = mem_dev_of(src); HIP_CHECK(hipGetDevice(&cur));
    mem_dev_copy_on(dd >= 0 ? dd : ds >= 0 ? ds : cur, dst, src, bytes);
}
int mem_device_count(void) { int n = 0; if (hipGetDeviceCount(&n) != hipSuccess) n = 0; return n; }
size_t mem_dev_pool_bytes(void) { size_t s = 0; for (int i = 0; i < nreg; i++) if (reg[i].dev >= 0) s += reg[i].bytes; return s; }

void *mem_hreg_alloc(size_t bytes)
{
    const size_t huge = 2u << 20;
    bytes = (bytes + huge - 1) & ~(huge - 1);
    void *p = 0;
    if (posix_memalign(&p, huge, bytes)) { fprintf(stderr, "hreg: posix_memalign %zu failed\n", bytes); exit(1); }
    madvise(p, bytes, MADV_HUGEPAGE);
    unsigned char *b = (unsigned char *)p;
#pragma omp parallel for schedule(static)
    for (size_t off = 0; off < bytes; off += 4096) b[off] = 0;
    HIP_CHECK(hipHostRegister(p, bytes, hipHostRegisterDefault));
    reg_add(p, bytes);
    return p;
}
void mem_hreg_free(void *p)
{
    if (!p) return;
    HIP_CHECK(hipHostUnregister(p));
    reg_del(p);
    free(p);
}

void *mem_hstage_alloc(int dev, size_t bytes, double *touch_s, double *reg_s)
{
    const size_t huge = 2u << 20;
    bytes = (bytes + huge - 1) & ~(huge - 1);
    void *p = 0;
    if (posix_memalign(&p, huge, bytes)) { fprintf(stderr, "hstage: posix_memalign %zu failed\n", bytes); exit(1); }
    madvise(p, bytes, MADV_HUGEPAGE);
    int node = mem_numa_node_of_device(dev);
    double t0 = mem_now();
    /* first touch from threads on the node: one page per stride, in parallel */
#pragma omp parallel
    {
        mem_pin_to_node(node);
        unsigned char *b = (unsigned char *)p;
#pragma omp for schedule(static)
        for (size_t off = 0; off < bytes; off += 4096) b[off] = 0;
        mem_unpin();
    }
    double t1 = mem_now();
    HIP_CHECK(hipHostRegister(p, bytes, hipHostRegisterDefault));
    reg_add(p, bytes);
    double t2 = mem_now();
    if (touch_s) *touch_s = t1 - t0;
    if (reg_s) *reg_s = t2 - t1;
    return p;
}
void mem_hstage_free(void *p)
{
    if (!p) return;
    HIP_CHECK(hipHostUnregister(p));
    reg_del(p);
    free(p);
}

static size_t pow2_ceil(size_t x) { size_t c = 1; while (c < x) c <<= 1; return c; }

void *dpool_get(dpool *d, int dev, size_t bytes)
{
    if (d->p && d->cap >= bytes && d->dev == dev) return d->p;
    size_t cap = pow2_ceil(bytes);
    if (d->p) { HIP_CHECK(hipSetDevice(d->dev)); HIP_CHECK(hipFree(d->p)); }
    HIP_CHECK(hipSetDevice(dev));
    HIP_CHECK(hipMalloc(&d->p, cap));
    d->cap = cap; d->dev = dev;
    return d->p;
}
void dpool_free(dpool *d)
{
    if (d->p) { HIP_CHECK(hipSetDevice(d->dev)); HIP_CHECK(hipFree(d->p)); }
    d->p = 0; d->cap = 0;
}
void *hpool_get(hpool *h, size_t bytes)
{
    if (h->p && h->cap >= bytes) return h->p;
    size_t cap = pow2_ceil(bytes);
    free(h->p);
    if (posix_memalign(&h->p, 4096, cap)) { fprintf(stderr, "hpool: %zu failed\n", cap); exit(1); }
    h->cap = cap;
    return h->p;
}
void hpool_free(hpool *h) { free(h->p); h->p = 0; h->cap = 0; }

static size_t proc_status_kb(const char *key)
{
    FILE *f = fopen("/proc/self/status", "r"); char line[256]; size_t v = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, strlen(key))) { sscanf(line + strlen(key), "%zu", &v); break; }
    fclose(f);
    return v;
}
size_t mem_vmhwm(void) { return proc_status_kb("VmHWM:") << 10; }
size_t mem_vmrss(void) { return proc_status_kb("VmRSS:") << 10; }

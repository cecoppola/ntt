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
static struct { void *p; size_t bytes; int dev; int stage; } *reg;   /* stage: a pinned NUMA-local staging block (M9 accounting) */
static int nreg = 0, reg_cap = 0;
static void reg_grow(void) { if (nreg >= reg_cap) { reg_cap = reg_cap ? 2 * reg_cap : 256; reg = (typeof(reg))realloc(reg, reg_cap * sizeof *reg); } }
int mem_par_init = 0;                                 /* Phase 8: init runs per device in parallel; touch teams sized to the node */
static void reg_add(void *p, size_t bytes) {
#pragma omp critical(memreg)
    { reg_grow(); reg[nreg].p = p; reg[nreg].bytes = bytes; reg[nreg].dev = -1; reg[nreg].stage = 0; nreg++; } }
static void reg_add_stage(void *p, size_t bytes) {
#pragma omp critical(memreg)
    { reg_grow(); reg[nreg].p = p; reg[nreg].bytes = bytes; reg[nreg].dev = -1; reg[nreg].stage = 1; nreg++; } }
static void reg_del(void *p) {
#pragma omp critical(memreg)
    { for (int i = 0; i < nreg; i++) if (reg[i].p == p) { reg[i] = reg[--nreg]; break; } } }
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
/* ---- Phase 12 I: the allocation form (mem.h) ---- */
enum { AF_HIPMALLOC, AF_FINE, AF_UNCACHED, AF_MANAGED, AF_HOST, AF_MMAP };
static const char *af_name[] = { "hipmalloc", "fine", "uncached", "managed", "host", "mmap" };
static int alloc_form(void)
{
    static int f = -1;
    if (f < 0) {
        const char *e = getenv("MEM_ALLOC"); f = AF_FINE;
        if (e) { int k; for (k = 0; k < 6; k++) if (!strcmp(e, af_name[k])) f = k; if (k == 6 && strcmp(e, af_name[f])) fprintf(stderr, "MEM_ALLOC=%s unknown: hipmalloc|fine|uncached|managed|host|mmap (using %s)\n", e, af_name[f]); }
    }
    return f;
}
const char *mem_alloc_form_name(void) { return af_name[alloc_form()]; }
static struct { void *p; size_t bytes; } *mm_tab; static int mm_n, mm_cap;   /* the mmap form's blocks (munmap needs the length) */
void *mem_dev_malloc(int dev, size_t bytes)
{
    void *p = 0; int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev));
    switch (alloc_form()) {
    case AF_HIPMALLOC: if (hipMalloc(&p, bytes) != hipSuccess) p = 0; break;
    case AF_FINE:      if (hipExtMallocWithFlags(&p, bytes, hipDeviceMallocFinegrained) != hipSuccess) p = 0; break;
    case AF_UNCACHED:  if (hipExtMallocWithFlags(&p, bytes, hipDeviceMallocUncached) != hipSuccess) p = 0; break;
    case AF_MANAGED:   if (hipMallocManaged(&p, bytes, hipMemAttachGlobal) != hipSuccess) p = 0; break;
    case AF_HOST:      { mem_pin_to_node(mem_numa_node_of_device(dev)); if (hipHostMalloc(&p, bytes, hipHostMallocNumaUser) != hipSuccess) p = 0; mem_unpin(); break; }
    case AF_MMAP: {
        const size_t huge = (size_t)2 << 20; size_t b = (bytes + huge - 1) & ~(huge - 1);
        p = mmap(0, b, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { p = 0; break; }
        madvise(p, b, MADV_HUGEPAGE);
        int node = mem_numa_node_of_device(dev), nt = mem_par_init ? mem_ncpus_node(node) : omp_get_max_threads();
#pragma omp parallel num_threads(nt)
        { mem_pin_to_node(node); unsigned char *c = (unsigned char *)p;
#pragma omp for schedule(static)
          for (size_t off = 0; off < b; off += 4096) c[off] = 0;
          mem_unpin(); }
        if (hipHostRegister(p, b, hipHostRegisterDefault) != hipSuccess) { munmap(p, b); p = 0; break; }
#pragma omp critical(memreg)
        { if (mm_n >= mm_cap) { mm_cap = mm_cap ? 2 * mm_cap : 64; mm_tab = (typeof(mm_tab))realloc(mm_tab, mm_cap * sizeof *mm_tab); } mm_tab[mm_n].p = p; mm_tab[mm_n].bytes = b; mm_n++; }
        break; }
    }
    HIP_CHECK(hipSetDevice(cur));
    return p;
}
void mem_dev_release(int dev, void *p)
{
    if (!p) return;
    int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev));
    switch (alloc_form()) {
    case AF_HOST: HIP_CHECK(hipHostFree(p)); break;
    case AF_MMAP: { size_t b = 0;
#pragma omp critical(memreg)
        { for (int i = 0; i < mm_n; i++) if (mm_tab[i].p == p) { b = mm_tab[i].bytes; mm_tab[i] = mm_tab[--mm_n]; break; } }
        HIP_CHECK(hipHostUnregister(p)); if (b) munmap(p, b); break; }
    default: HIP_CHECK(hipFree(p));
    }
    HIP_CHECK(hipSetDevice(cur));
}
void mem_oom(const char *where, int dev, size_t bytes)
{
    fprintf(stderr, "%s: allocation (%s) of %.2f GB on APU %d failed (out of memory)\n", where, mem_alloc_form_name(), bytes / 1e9, dev);
    fflush(stderr); mem_report("OOM"); mem_report_summary(); fflush(stdout);
    exit(1);
}
void *mem_dev_alloc(int dev, size_t bytes)
{
    void *p; int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev));
    double t0 = mem_now();
    if (!(p = mem_dev_malloc(dev, bytes))) mem_oom("mem_dev_alloc", dev, bytes);
    double t1 = mem_now();
    if (!getenv("MEM_NO_DEV_MEMSET")) { HIP_CHECK(hipMemset(p, 0, bytes)); HIP_CHECK(hipDeviceSynchronize()); }   /* map the pages now */
    if (getenv("RNS_VERBOSE")) printf("mem_dev_alloc: dev %d %.1f GB (%s): malloc %.2f s memset %.2f s\n", dev, bytes / 1e9, mem_alloc_form_name(), t1 - t0, mem_now() - t1);
    HIP_CHECK(hipSetDevice(cur));
#pragma omp critical(memreg)
    { reg_grow(); reg[nreg].p = p; reg[nreg].bytes = bytes; reg[nreg].dev = dev; reg[nreg].stage = 0; nreg++; }
    return p;
}
void mem_dev_forget(void *p) { reg_del(p); }         /* drop from the registry without freeing (ownership passed on) */
void mem_dev_free(void *p)
{
    int dev = mem_dev_of(p), cur; if (dev < 0) return;
    HIP_CHECK(hipGetDevice(&cur)); mem_dev_release(dev, p); HIP_CHECK(hipSetDevice(cur));
    reg_del(p);
}
void mem_dev_free_raw(int dev, void *p) { mem_dev_release(dev, p); }
void mem_dev_copy_on(int dev, void *dst, const void *src, size_t bytes)   /* DMA copy on device dev's engine */
{
    int cur; HIP_CHECK(hipGetDevice(&cur));
    HIP_CHECK(hipSetDevice(dev));
    HIP_CHECK(hipMemcpy(dst, src, bytes, hipMemcpyDefault));
    HIP_CHECK(hipSetDevice(cur));
}
/* Phase 10 H (B2): an asynchronous copy on a non-blocking stream of device dev (pinned host memory), and the wait for
 * every copy issued on it -- the seeds' chunks go to the regions while init's hipMemset of the plane pools runs on the
 * same devices' null streams (a hipMemcpy there queues behind them: 4 GB/s measured) */
static hipStream_t g_cs[MEM_MAX_DEV]; static int g_cs_init[MEM_MAX_DEV];
void mem_dev_copy_async(int dev, void *dst, const void *src, size_t bytes)
{
    int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev));
    if (!g_cs_init[dev]) { HIP_CHECK(hipStreamCreateWithFlags(&g_cs[dev], hipStreamNonBlocking)); g_cs_init[dev] = 1; }
    HIP_CHECK(hipMemcpyAsync(dst, src, bytes, hipMemcpyDefault, g_cs[dev]));
    HIP_CHECK(hipSetDevice(cur));
}
void mem_dev_copy_wait(int dev) { if (g_cs_init[dev]) HIP_CHECK(hipStreamSynchronize(g_cs[dev])); }
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
    int nt = mem_par_init ? mem_ncpus_node(node) : omp_get_max_threads();
#pragma omp parallel num_threads(nt)
    {
        mem_pin_to_node(node);
        unsigned char *b = (unsigned char *)p;
#pragma omp for schedule(static)
        for (size_t off = 0; off < bytes; off += 4096) b[off] = 0;
        mem_unpin();
    }
    double t1 = mem_now();
    HIP_CHECK(hipHostRegister(p, bytes, hipHostRegisterDefault));
    reg_add_stage(p, bytes);
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

/* Phase 11 V (D5, a test knob): MEM_DPOOL_FILL=1 fills a plane pool with 0xA5 bytes when it is grown inside a phase, =2 at
 * every allocation -- a tier that relied on fresh device memory being zero would fail deterministically instead of once in
 * twenty runs (the grown pool may be recycled, dirty memory) */
static void dpool_fill(dpool *d, int grew)
{
    static int fill = -1; if (fill < 0) fill = getenv("MEM_DPOOL_FILL") ? atoi(getenv("MEM_DPOOL_FILL")) : 0;
    if (!fill || (fill == 1 && !grew)) return;
    HIP_CHECK(hipMemset(d->p, 0xA5, d->cap)); HIP_CHECK(hipDeviceSynchronize());
    printf("mem: plane pool on APU %d %s: %.2f GB filled with 0xA5 (MEM_DPOOL_FILL)\n", d->dev, grew ? "grown" : "allocated", d->cap / 1e9);
}
void *dpool_get(dpool *d, int dev, size_t bytes)
{
    if (d->p && d->cap >= bytes && d->dev == dev) return d->p;
    size_t cap = pow2_ceil(bytes);
    if (d->p) mem_dev_release(d->dev, d->p);
    HIP_CHECK(hipSetDevice(dev));
    int grew = d->p != 0;
    if (!(d->p = mem_dev_malloc(dev, cap))) mem_oom("dpool_get", dev, cap);
    d->cap = cap; d->dev = dev;
    dpool_fill(d, grew);
    return d->p;
}
void *dpool_get_exact(dpool *d, int dev, size_t bytes)
{
    if (d->p && d->cap >= bytes && d->dev == dev) return d->p;
    const size_t al = (size_t)2 << 20; size_t cap = (bytes + al - 1) / al * al;
    if (d->p) mem_dev_release(d->dev, d->p);
    HIP_CHECK(hipSetDevice(dev));
    int grew = d->p != 0;
    if (!(d->p = mem_dev_malloc(dev, cap))) mem_oom("dpool_get_exact", dev, cap);
    d->cap = cap; d->dev = dev;
    dpool_fill(d, grew);
    return d->p;
}
void dpool_free(dpool *d)
{
    if (d->p) mem_dev_release(d->dev, d->p);
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

/* ---- Phase 9 M9: memory accounting (mem.h) ---- */
static mem_acct_fn g_acct[16]; static int g_nacct;
static size_t g_host_item[MEM_HOST_NCAT];
#define MEM_MAX_PHASES 32
static struct { char name[24]; size_t dev[MEM_DEV_NCAT], devmax[MEM_DEV_NCAT], host[MEM_HOST_NCAT]; size_t dev_total, dev_total_max; int ndev; } g_ph[MEM_MAX_PHASES];
static int g_nph; static size_t g_last_dev_total;
void mem_acct_register(mem_acct_fn fn)
{
    for (int i = 0; i < g_nacct; i++) if (g_acct[i] == fn) return;
    if (g_nacct < 16) g_acct[g_nacct++] = fn;
}
void mem_report_host_item(int cat, size_t bytes) { if (cat >= 0 && cat < MEM_HOST_NCAT) g_host_item[cat] = bytes; }
size_t mem_report_dev_total(void) { return g_last_dev_total; }
/* device bytes in use per category: the sum of what the providers report; "in use" = planes + regions +
 * pool regions (donated, borrowed, hipMalloc) + tables + other -- live/free/peak are views inside the pool */
static size_t dev_in_use(const size_t *c) { return c[MEM_DEV_PLANES] + c[MEM_DEV_REGIONS] + c[MEM_DEV_POOL_DONATED] + c[MEM_DEV_POOL_BORROWED] + c[MEM_DEV_POOL_HIPMALLOC] + c[MEM_DEV_TABLES] + c[MEM_DEV_OTHER]; }
static const char *dev_cat_name[MEM_DEV_NCAT] = { "planes", "regions", "pool:donated", "pool:borrowed", "pool:hipMalloc", "pool:live", "pool:free", "pool:peak-live", "tables", "other" };
static const char *host_cat_name[MEM_HOST_NCAT] = { "staging", "registered", "X", "digits", "named", "other", "VmRSS", "VmHWM" };
static void gather(size_t dev[][MEM_DEV_NCAT], int ndev, size_t host[MEM_HOST_NCAT])
{
    memset(dev, 0, (size_t)ndev * MEM_DEV_NCAT * sizeof(size_t)); memset(host, 0, MEM_HOST_NCAT * sizeof(size_t));
    for (int i = 0; i < g_nacct; i++) g_acct[i](ndev, dev);
    for (int i = 0; i < nreg; i++) {
        if (reg[i].dev >= 0 && reg[i].dev < ndev) dev[reg[i].dev][MEM_DEV_REGIONS] += reg[i].bytes;
        else if (reg[i].dev < 0) host[reg[i].stage ? MEM_HOST_STAGING : MEM_HOST_REGISTERED] += reg[i].bytes;
    }
    host[MEM_HOST_X] = g_host_item[MEM_HOST_X]; host[MEM_HOST_DIGITS] = g_host_item[MEM_HOST_DIGITS]; host[MEM_HOST_NAMED] = g_host_item[MEM_HOST_NAMED];
    host[MEM_HOST_RSS] = mem_vmrss(); host[MEM_HOST_HWM] = mem_vmhwm();
    size_t known = host[MEM_HOST_STAGING] + host[MEM_HOST_REGISTERED] + host[MEM_HOST_X] + host[MEM_HOST_DIGITS] + host[MEM_HOST_NAMED];
    host[MEM_HOST_OTHER] = host[MEM_HOST_RSS] > known ? host[MEM_HOST_RSS] - known : 0;
}
static const char *rank_prefix(void) { static char b[24]; const char *r = getenv("COMM_RANK"), *s = getenv("COMM_SIZE"); if (r && s && atoi(s) > 1) snprintf(b, sizeof b, "mem[%s] ", r); else snprintf(b, sizeof b, "mem "); return b; }
void mem_report(const char *phase)
{
    int ndev = mem_device_count(); if (ndev > MEM_MAX_DEV) ndev = MEM_MAX_DEV;
    size_t dev[MEM_MAX_DEV][MEM_DEV_NCAT], host[MEM_HOST_NCAT];
    gather(dev, ndev, host);
    size_t sum[MEM_DEV_NCAT] = {0}, mx[MEM_DEV_NCAT] = {0}, tot = 0, totmax = 0, totmin = (size_t)-1;
    for (int d = 0; d < ndev; d++) { size_t u = dev_in_use(dev[d]); tot += u; if (u > totmax) totmax = u; if (u < totmin) totmin = u;
        for (int c = 0; c < MEM_DEV_NCAT; c++) { sum[c] += dev[d][c]; if (dev[d][c] > mx[c]) mx[c] = dev[d][c]; } }
    g_last_dev_total = tot;
    const char *pf = rank_prefix();
    printf("%s[%s] device %.1f GB in use (per APU %.1f..%.1f): planes %.1f, regions %.1f, block pool %.1f (donated %.1f, borrowed %.1f, hipMalloc %.1f; live %.1f, peak live %.1f, free %.1f), tables %.2f, other %.1f\n",
           pf, phase, tot / 1e9, ndev ? totmin / 1e9 : 0.0, totmax / 1e9, sum[MEM_DEV_PLANES] / 1e9, sum[MEM_DEV_REGIONS] / 1e9,
           (sum[MEM_DEV_POOL_DONATED] + sum[MEM_DEV_POOL_BORROWED] + sum[MEM_DEV_POOL_HIPMALLOC]) / 1e9, sum[MEM_DEV_POOL_DONATED] / 1e9, sum[MEM_DEV_POOL_BORROWED] / 1e9,
           sum[MEM_DEV_POOL_HIPMALLOC] / 1e9, sum[MEM_DEV_POOL_LIVE] / 1e9, sum[MEM_DEV_POOL_PEAK_LIVE] / 1e9, sum[MEM_DEV_POOL_FREE] / 1e9, sum[MEM_DEV_TABLES] / 1e9, sum[MEM_DEV_OTHER] / 1e9);
    printf("%s[%s] host %.1f GB RSS (HWM %.1f): staging %.1f pinned, registered %.1f, X %.1f, digits %.1f, named %.1f, other %.1f\n",
           pf, phase, host[MEM_HOST_RSS] / 1e9, host[MEM_HOST_HWM] / 1e9, host[MEM_HOST_STAGING] / 1e9, host[MEM_HOST_REGISTERED] / 1e9,
           host[MEM_HOST_X] / 1e9, host[MEM_HOST_DIGITS] / 1e9, host[MEM_HOST_NAMED] / 1e9, host[MEM_HOST_OTHER] / 1e9);
    if (getenv("MEM_REPORT_DEVS")) for (int d = 0; d < ndev; d++) {
        printf("%s[%s]   APU%d: in use %.1f GB:", pf, phase, d, dev_in_use(dev[d]) / 1e9);
        for (int c = 0; c < MEM_DEV_NCAT; c++) if (dev[d][c]) printf(" %s %.2f", dev_cat_name[c], dev[d][c] / 1e9);
        size_t f = 0, t = 0; int cur; if (hipGetDevice(&cur) == hipSuccess && hipSetDevice(d) == hipSuccess) { if (hipMemGetInfo(&f, &t) != hipSuccess) f = t = 0; (void)hipSetDevice(cur); }
        if (t) printf("  (driver: %.1f of %.1f GB used)", (t - f) / 1e9, t / 1e9);
        printf("\n");
    }
    if (g_nph < MEM_MAX_PHASES) {
        snprintf(g_ph[g_nph].name, sizeof g_ph[g_nph].name, "%s", phase);
        memcpy(g_ph[g_nph].dev, sum, sizeof sum); memcpy(g_ph[g_nph].devmax, mx, sizeof mx); memcpy(g_ph[g_nph].host, host, sizeof host);
        g_ph[g_nph].dev_total = tot; g_ph[g_nph].dev_total_max = totmax; g_ph[g_nph].ndev = ndev; g_nph++;
    }
}
void mem_report_summary(void)
{
    if (!g_nph) return;
    const char *pf = rank_prefix();
    printf("%ssummary: bytes in use at each phase boundary, GB (device: all APUs / largest APU; host: this process)\n", pf);
    printf("%s%-14s %7s %7s | %7s %7s %7s %7s %7s %7s %7s %6s | %7s %7s %7s %7s %7s %7s %7s\n", pf, "phase", "device", "maxAPU",
           "planes", "regions", "pl:don", "pl:bor", "pl:hip", "pl:live", "pl:peak", "tables", "host", "staging", "regist", "X", "digits", "other", "HWM");
    for (int i = 0; i < g_nph; i++) {
        const size_t *c = g_ph[i].dev, *h = g_ph[i].host;
        printf("%s%-14s %7.1f %7.1f | %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f %6.2f | %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f\n", pf, g_ph[i].name,
               g_ph[i].dev_total / 1e9, g_ph[i].dev_total_max / 1e9, c[MEM_DEV_PLANES] / 1e9, c[MEM_DEV_REGIONS] / 1e9, c[MEM_DEV_POOL_DONATED] / 1e9,
               c[MEM_DEV_POOL_BORROWED] / 1e9, c[MEM_DEV_POOL_HIPMALLOC] / 1e9, c[MEM_DEV_POOL_LIVE] / 1e9, c[MEM_DEV_POOL_PEAK_LIVE] / 1e9, c[MEM_DEV_TABLES] / 1e9,
               h[MEM_HOST_RSS] / 1e9, h[MEM_HOST_STAGING] / 1e9, h[MEM_HOST_REGISTERED] / 1e9, h[MEM_HOST_X] / 1e9, h[MEM_HOST_DIGITS] / 1e9, h[MEM_HOST_OTHER] / 1e9, h[MEM_HOST_HWM] / 1e9);
    }
}

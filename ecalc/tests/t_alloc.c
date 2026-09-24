/* t_alloc - Phase 12 I: every allocation form on the APU, measured the way init uses them (PLAN.md 27 row I).
 *
 * For each form, GB per APU on all four APUs at once (one thread per device, as rns_init and binsplit_pregrow do):
 *   alloc     the allocation call (wall of the slowest thread; the driver serialises them)
 *   fill      a first-touch kernel over the whole buffer (a store per 8 bytes) -- the pages' mapping cost if the
 *             allocation deferred it
 *   bw        a read+write copy kernel over the second half from the first half: GB/s (HBM speed = 3.7 TB/s read on
 *             an APU's own node)
 *   ntt       ntt_fwd at 2^28 points x 4 batches (8 GiB of planes) in the buffer: the transform kernels' rate, which
 *             must not drop against hipMalloc
 *   d2d       hipMemcpyAsync between the two halves on the same device, and to the next APU's buffer (peer): GB/s --
 *             registered host memory could be routed to the SDMA engine (58 GB/s) instead of the blit kernels
 *   cpu       CPU streaming stores into the buffer from threads on the buffer's NUMA node (the seed thread's pattern)
 *   free      the release
 *
 * Forms (argument list; default: all): hipmalloc, async (hipMallocAsync from a device mempool with the release
 * threshold at max, then a second alloc after a free: the warm pool), managed (hipMallocManaged), uncached and
 * fine (hipExtMallocWithFlags), hostc / hostnc / hostnuma (hipHostMalloc coherent / non-coherent / NumaUser with the
 * thread pinned to the node), mmap (anonymous mmap + MADV_HUGEPAGE + parallel first touch by threads on the node +
 * hipHostRegister), mmap4k (the same without huge pages), hugetlb (MAP_HUGETLB, if the node has reserved huge pages).
 *
 * Then the contention question: hipMalloc of the same bytes while a "seed" team streams (all cores computing and
 * storing into device memory, as binsplit's seed thread does) -- with all cores, with 4 cores left free, and with the
 * computing alone (no device stores); and the seeds' rate under the mapping.
 *
 * Usage: t_alloc [GB per APU (50)] [forms...]     T_ALLOC_NTT=0 skips the transform, T_ALLOC_SEED=0 the contention part
 * Phase 14 R1 (E8): the form "vmm" (hipMemAddressReserve / hipMemCreate / hipMemMap / hipMemSetAccess in chunks of T_ALLOC_VMM_CHUNK_GB (2)):
 * a row in the table and its own section (results/R114.md 5): `t_alloc 32 hipmalloc vmm`
 */
#include "harness.h"
#include "../ntt.h"
#include "../mem.h"
#include <omp.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <hip/hip_runtime.h>

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

__global__ void k_fill(uint64_t *p, size_t n, uint64_t seed)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) p[i] = seed + i;
}
__global__ void k_copy(uint64_t *dst, const uint64_t *src, size_t n)   /* 16-byte accesses, grid-stride */
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    const ulonglong2 *s = (const ulonglong2 *)src; ulonglong2 *d = (ulonglong2 *)dst;
    for (; i < n / 2; i += stride) d[i] = s[i];
}

enum { F_HIPMALLOC, F_ASYNC, F_MANAGED, F_UNCACHED, F_FINE, F_HOSTC, F_HOSTNC, F_HOSTNUMA, F_MMAP, F_MMAP4K, F_HUGETLB, F_VMM, F_N };
static const char *fname[F_N] = { "hipmalloc", "async", "managed", "uncached", "fine", "hostc", "hostnc", "hostnuma", "mmap", "mmap4k", "hugetlb", "vmm" };
static int is_mmap(int f) { return f == F_MMAP || f == F_MMAP4K || f == F_HUGETLB; }

struct buf { void *p; size_t bytes; int dev, form; hipMemPool_t pool; double t_touch, t_reg;
             hipMemGenericAllocationHandle_t *h; int nh; size_t chunk, reserved; double t_create, t_map, t_access; };   /* vmm (Phase 14 R1, E8) */

/* ---- Phase 14 R1 (E8): the HIP virtual-memory form -- one VA range reserved (2 x the bytes: room to grow in place), physical chunks
 * created (hipMemCreate, pinned device memory) and mapped one after the other (hipMemMap), access set for the owner and every peer
 * (hipMemSetAccess).  The pool's question: can a contiguous VA be backed by non-contiguous physical chunks, mapped and unmapped at will? */
static size_t vmm_chunk_bytes(void) { const char *e = getenv("T_ALLOC_VMM_CHUNK_GB"); double g = e ? atof(e) : 2.0; return (size_t)(g * 1073741824.0); }
static int vmm_create_map(struct buf *b, size_t off, size_t len, int nd)   /* one chunk at [off, off + len) of b's range, access for all nd devices */
{
    hipMemAllocationProp prop; memset(&prop, 0, sizeof prop);
    prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = b->dev;
    hipMemGenericAllocationHandle_t h; double t0 = now();
    if (hipMemCreate(&h, len, &prop, 0) != hipSuccess) return 0;
    double t1 = now();
    if (hipMemMap((char *)b->p + off, len, 0, h, 0) != hipSuccess) { (void)hipMemRelease(h); return 0; }
    double t2 = now();
    hipMemAccessDesc ad[4]; memset(ad, 0, sizeof ad);
    for (int c = 0; c < nd; c++) { ad[c].location.type = hipMemLocationTypeDevice; ad[c].location.id = c; ad[c].flags = hipMemAccessFlagsProtReadWrite; }
    if (hipMemSetAccess((char *)b->p + off, len, ad, nd) != hipSuccess) { (void)hipMemUnmap((char *)b->p + off, len); (void)hipMemRelease(h); return 0; }
    double t3 = now();
    b->h = (hipMemGenericAllocationHandle_t *)realloc(b->h, (b->nh + 1) * sizeof *b->h); b->h[b->nh++] = h;
    b->t_create += t1 - t0; b->t_map += t2 - t1; b->t_access += t3 - t2;
    return 1;
}
static int vmm_alloc(struct buf *b, int nd)
{
    size_t gran = 0; hipMemAllocationProp prop; memset(&prop, 0, sizeof prop);
    prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = b->dev;
    if (hipMemGetAllocationGranularity(&gran, &prop, hipMemAllocationGranularityRecommended) != hipSuccess || !gran) return 0;
    size_t ch = vmm_chunk_bytes(); ch = (ch + gran - 1) / gran * gran; if (ch > b->bytes) ch = b->bytes;
    size_t n = (b->bytes + ch - 1) / ch; b->chunk = ch; b->reserved = 2 * n * ch; b->h = 0; b->nh = 0; b->t_create = b->t_map = b->t_access = 0;
    if (hipMemAddressReserve(&b->p, b->reserved, gran, 0, 0) != hipSuccess) return 0;
    for (size_t i = 0; i < n; i++) if (!vmm_create_map(b, i * ch, ch, nd)) return 0;
    return 1;
}
static void vmm_free(struct buf *b, double *t_unmap, double *t_release)
{
    double t0 = now();
    for (int i = 0; i < b->nh; i++) if (b->h[i]) HIP_CHECK(hipMemUnmap((char *)b->p + (size_t)i * b->chunk, b->chunk));
    double t1 = now();
    for (int i = 0; i < b->nh; i++) if (b->h[i]) HIP_CHECK(hipMemRelease(b->h[i]));
    double t2 = now();
    HIP_CHECK(hipMemAddressFree(b->p, b->reserved));
    if (t_unmap) *t_unmap = t1 - t0; if (t_release) *t_release = t2 - t1;
    free(b->h); b->h = 0; b->nh = 0;
}
static long meminfo_kb(const char *key)   /* /proc/meminfo (MEM_REPORT_DEVS' source: hipMemGetInfo does not see the APU's pages) */
{
    FILE *f = fopen("/proc/meminfo", "r"); if (!f) return -1; char line[256]; long v = -1; size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, kl) && line[kl] == ':') { v = atol(line + kl + 1); break; }
    fclose(f); return v;
}
static int g_vmm_nd = 1;

static void touch_on_node(void *p, size_t bytes, int node)   /* first touch by threads pinned to the node (the NUMA placement is everything: RESULTS 20) */
{
    int nt = mem_ncpus_node(node); if (nt < 1) nt = 16;
#pragma omp parallel num_threads(nt)
    {
        mem_pin_to_node(node);
        unsigned char *b = (unsigned char *)p;
#pragma omp for schedule(static)
        for (size_t off = 0; off < bytes; off += 4096) b[off] = 0;
        mem_unpin();
    }
}
static int alloc_form(struct buf *b, int form, int dev, size_t bytes)
{
    b->form = form; b->dev = dev; b->bytes = bytes; b->p = 0; b->t_touch = b->t_reg = 0;
    HIP_CHECK(hipSetDevice(dev));
    switch (form) {
    case F_HIPMALLOC: return hipMalloc(&b->p, bytes) == hipSuccess;
    case F_VMM: return vmm_alloc(b, g_vmm_nd);
    case F_ASYNC: {
        if (!b->pool) {
            hipMemPoolProps pr; memset(&pr, 0, sizeof pr); pr.allocType = hipMemAllocationTypePinned; pr.handleTypes = hipMemHandleTypeNone;
            pr.location.type = hipMemLocationTypeDevice; pr.location.id = dev;
            if (hipMemPoolCreate(&b->pool, &pr) != hipSuccess) return 0;
            uint64_t th = UINT64_MAX; HIP_CHECK(hipMemPoolSetAttribute(b->pool, hipMemPoolAttrReleaseThreshold, &th));
        }
        if (hipMallocFromPoolAsync(&b->p, bytes, b->pool, 0) != hipSuccess) return 0;
        HIP_CHECK(hipStreamSynchronize(0)); return 1; }
    case F_MANAGED: return hipMallocManaged(&b->p, bytes, hipMemAttachGlobal) == hipSuccess;
    case F_UNCACHED: return hipExtMallocWithFlags(&b->p, bytes, hipDeviceMallocUncached) == hipSuccess;
    case F_FINE: return hipExtMallocWithFlags(&b->p, bytes, hipDeviceMallocFinegrained) == hipSuccess;
    case F_HOSTC: return hipHostMalloc(&b->p, bytes, hipHostMallocCoherent) == hipSuccess;
    case F_HOSTNC: return hipHostMalloc(&b->p, bytes, hipHostMallocNonCoherent) == hipSuccess;
    case F_HOSTNUMA: { mem_pin_to_node(dev); int ok = hipHostMalloc(&b->p, bytes, hipHostMallocNumaUser) == hipSuccess; mem_unpin(); return ok; }
    case F_MMAP: case F_MMAP4K: case F_HUGETLB: {
        int flags = MAP_PRIVATE | MAP_ANONYMOUS | (form == F_HUGETLB ? MAP_HUGETLB : 0);
        void *p = mmap(0, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (p == MAP_FAILED) return 0;
        if (form == F_MMAP) madvise(p, bytes, MADV_HUGEPAGE); else if (form == F_MMAP4K) madvise(p, bytes, MADV_NOHUGEPAGE);
        double t0 = now(); touch_on_node(p, bytes, dev); double t1 = now();
        if (hipHostRegister(p, bytes, hipHostRegisterDefault) != hipSuccess) { munmap(p, bytes); return 0; }
        b->t_reg = now() - t1; b->t_touch = t1 - t0; b->p = p; return 1; }
    }
    return 0;
}
static void free_form(struct buf *b)
{
    HIP_CHECK(hipSetDevice(b->dev));
    switch (b->form) {
    case F_ASYNC: HIP_CHECK(hipFreeAsync(b->p, 0)); HIP_CHECK(hipStreamSynchronize(0)); break;
    case F_HOSTC: case F_HOSTNC: case F_HOSTNUMA: HIP_CHECK(hipHostFree(b->p)); break;
    case F_MMAP: case F_MMAP4K: case F_HUGETLB: HIP_CHECK(hipHostUnregister(b->p)); munmap(b->p, b->bytes); break;
    case F_VMM: vmm_free(b, 0, 0); break;
    default: HIP_CHECK(hipFree(b->p));
    }
    b->p = 0;
}

static double tmax(const double *t, int n) { double m = 0; for (int i = 0; i < n; i++) if (t[i] > m) m = t[i]; return m; }
static double tsum(const double *t, int n) { double m = 0; for (int i = 0; i < n; i++) m += t[i]; return m; }

/* ---- the seed-like background team: compute-bound threads that store 1/8 of their time into device memory ---- */
struct seedbg { volatile int stop; int nthreads, store; uint64_t *dst[4]; size_t bytes; double gbs; long iters; };
static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }
static void *seedbg_run(void *a)
{
    struct seedbg *s = (struct seedbg *)a; double t0 = now(); size_t stored = 0; long iters = 0;
#pragma omp parallel num_threads(s->nthreads) reduction(+:stored,iters)
    {
        int tid = omp_get_thread_num(); uint64_t x = 0x9E3779B97F4A7C15ULL * (tid + 1);
        size_t slice = s->bytes / s->nthreads / 8, base = (size_t)tid * slice; int dev = tid % 4;
        while (!s->stop) {
            for (int k = 0; k < 4000000; k++) x = mix(x) + k;                       /* ~ 15 ms of schoolbook-like arithmetic */
            if (s->store) { uint64_t *d = s->dst[dev] + base; for (size_t i = 0; i < slice; i += 8) d[i] = x + i; stored += slice * 8; }   /* 4 KB stores per 8 KB: the seeds write ~ 6 GB/s at 4e10 */
            iters++;
        }
    }
    s->gbs = stored / 1e9 / (now() - t0); s->iters = iters; return 0;
}

int main(int argc, char **argv)
{
    double gb = argc > 1 ? atof(argv[1]) : 50.0;
    int nd = 0; HIP_CHECK(hipGetDeviceCount(&nd)); if (nd > 4) nd = 4;
    size_t bytes = ((size_t)(gb * 1e9) + ((size_t)2 << 20) - 1) / ((size_t)2 << 20) * ((size_t)2 << 20);
    int do_ntt = getenv("T_ALLOC_NTT") ? atoi(getenv("T_ALLOC_NTT")) : 1, do_seed = getenv("T_ALLOC_SEED") ? atoi(getenv("T_ALLOC_SEED")) : 1;
    int sel[F_N] = {0}, nsel = 0;
    for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "none")) nsel = -1; for (int f = 0; f < F_N; f++) if (!strcmp(argv[i], fname[f])) { sel[f] = 1; nsel++; } }   /* "none": only the probe (a fresh process's first allocation) */
    if (!nsel) for (int f = 0; f < F_N; f++) sel[f] = f != F_ASYNC && f != F_VMM;   /* vmm (E8) on request: its section follows the table */
    g_vmm_nd = nd;   /* async only when asked: its warm re-allocation faulted (illegal access in the fill) on ROCm 6.x -- run it last, alone */
    printf("== t_alloc: %.1f GB per APU on %d APUs in parallel, %d cpus ==\n", bytes / 1e9, nd, omp_get_max_threads());
    harness_meta("t_alloc");
    for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); for (int c = 0; c < nd; c++) if (c != d) { (void)hipDeviceEnablePeerAccess(c, 0); (void)hipGetLastError(); } }
    ntt_ctx *ctx[4] = {0}; hipStream_t st[4];
    for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); if (do_ntt) ctx[d] = ntt_ctx_create(0); HIP_CHECK(hipStreamCreateWithFlags(&st[d], hipStreamNonBlocking)); }
    /* the reference ntt time on hipMalloc memory, per device, for the "must not drop" check */
    double ntt_ref[4] = {0}, bw_ref[4] = {0};
    printf("(s/GB* = wall of the slowest thread / the bytes of all APUs: the driver serialises the four)\n");
    printf("%-9s | %7s %7s %7s | %6s %7s | %7s %7s | %7s %7s %7s | %6s %6s | %s\n", "form", "alloc", "s/GB*", "fill", "touch", "regist", "bw GB/s", "ntt s", "d2d loc", "d2d pr", "cpu GB/s", "free", "sum/4", "note");
    for (int f = 0; f < F_N; f++) {
        if (!sel[f]) continue;
        struct buf b[4]; memset(b, 0, sizeof b);
        double ta[4] = {0}, tf[4] = {0}, tb[4] = {0}, tn[4] = {0}, td[4] = {0}, tp[4] = {0}, tc[4] = {0}, tfree[4] = {0}; int ok[4] = {0}, allok = 1;
        for (int rep = 0; rep < (f == F_ASYNC ? 2 : 1); rep++) {   /* async: the second allocation after a free comes from the warm pool */
            double t0 = now();
#pragma omp parallel num_threads(nd)
            { int d = omp_get_thread_num(); double x = now(); ok[d] = alloc_form(&b[d], f, d, bytes); ta[d] = now() - x; }
            (void)t0;
            for (int d = 0; d < nd; d++) if (!ok[d]) allok = 0;
            if (!allok) { printf("%-9s | not available (%s)\n", fname[f], f == F_HUGETLB ? "no reserved huge pages?" : "allocation failed"); break; }
            if (rep == 0 && f == F_ASYNC) {
                double tf0[4];
#pragma omp parallel num_threads(nd)
                { int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); double x = now(); k_fill<<<228 * 8, 256>>>((uint64_t *)b[d].p, bytes / 8, 1); HIP_CHECK(hipDeviceSynchronize()); tf0[d] = now() - x; }
                double tfr[4];
#pragma omp parallel num_threads(nd)
                { int d = omp_get_thread_num(); double x = now(); free_form(&b[d]); tfr[d] = now() - x; }
                printf("%-9s | %7.2f %7.3f %7.2f | %6s %7s | %7s %7s | %7s %7s | %7s | %6.2f %6.2f | first allocation from the pool (cold), then freed to it\n", "async", tmax(ta, nd), tmax(ta, nd) / (nd * bytes / 1e9), tmax(tf0, nd), "", "", "", "", "", "", "", tmax(tfr, nd), tsum(ta, nd) / nd);
                continue;
            }
        }
        if (!allok) continue;
        /* fill: first touch by a kernel */
#pragma omp parallel num_threads(nd)
        { int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); double x = now(); k_fill<<<228 * 8, 256, 0, st[d]>>>((uint64_t *)b[d].p, bytes / 8, 1); HIP_CHECK(hipStreamSynchronize(st[d])); tf[d] = now() - x; }
        /* bw: copy the first half to the second, twice (the second timed) */
        size_t half = bytes / 2 / 16 * 16;
#pragma omp parallel num_threads(nd)
        { int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); uint64_t *p = (uint64_t *)b[d].p;
          k_copy<<<228 * 8, 256, 0, st[d]>>>(p + half / 8, p, half / 8); HIP_CHECK(hipStreamSynchronize(st[d]));
          double x = now(); k_copy<<<228 * 8, 256, 0, st[d]>>>(p + half / 8, p, half / 8); HIP_CHECK(hipStreamSynchronize(st[d])); tb[d] = 2.0 * half / 1e9 / (now() - x); }
        /* ntt: 2^28 points x 4 batches, forward, twice (the second timed) */
        if (do_ntt && bytes >= ((size_t)8 << 30)) {
#pragma omp parallel num_threads(nd)
            { int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); uint64_t *p = (uint64_t *)b[d].p;
              ntt_fwd(ctx[d], p, 28, 4, st[d]); HIP_CHECK(hipStreamSynchronize(st[d]));
              double x = now(); ntt_fwd(ctx[d], p, 28, 4, st[d]); HIP_CHECK(hipStreamSynchronize(st[d])); tn[d] = now() - x; }
        }
        /* d2d: hipMemcpyAsync half -> half on the same device, and to the next device's first half (peer) */
        size_t cp = half < ((size_t)8 << 30) ? half : ((size_t)8 << 30);
#pragma omp parallel num_threads(nd)
        { int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); uint64_t *p = (uint64_t *)b[d].p;
          HIP_CHECK(hipMemcpyAsync(p + half / 8, p, cp, hipMemcpyDefault, st[d])); HIP_CHECK(hipStreamSynchronize(st[d]));
          double x = now(); HIP_CHECK(hipMemcpyAsync(p + half / 8, p, cp, hipMemcpyDefault, st[d])); HIP_CHECK(hipStreamSynchronize(st[d])); td[d] = cp / 1e9 / (now() - x); }
        if (nd > 1) {
#pragma omp parallel num_threads(nd)
            { int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); uint64_t *p = (uint64_t *)b[d].p, *q = (uint64_t *)b[(d + 1) % nd].p;
              HIP_CHECK(hipMemcpyAsync(q + half / 8, p, cp, hipMemcpyDefault, st[d])); HIP_CHECK(hipStreamSynchronize(st[d]));
              double x = now(); HIP_CHECK(hipMemcpyAsync(q + half / 8, p, cp, hipMemcpyDefault, st[d])); HIP_CHECK(hipStreamSynchronize(st[d])); tp[d] = cp / 1e9 / (now() - x); }
        }
        /* cpu: streaming stores from threads on the buffer's node, 4 GB per device */
        {
            size_t cb = bytes < ((size_t)4 << 30) ? bytes : ((size_t)4 << 30);
#pragma omp parallel num_threads(nd)
            { int d = omp_get_thread_num(); int nt = mem_ncpus_node(d); if (nt < 1) nt = 16; double x = now();
#pragma omp parallel num_threads(nt)
              { mem_pin_to_node(d); uint64_t *p = (uint64_t *)b[d].p;
#pragma omp for schedule(static)
                for (size_t i = 0; i < cb / 8; i += 8) { p[i] = i; p[i + 1] = i; p[i + 2] = i; p[i + 3] = i; p[i + 4] = i; p[i + 5] = i; p[i + 6] = i; p[i + 7] = i; }
                mem_unpin(); }
              tc[d] = cb / 1e9 / (now() - x); }
        }
#pragma omp parallel num_threads(nd)
        { int d = omp_get_thread_num(); double x = now(); free_form(&b[d]); tfree[d] = now() - x; }
        double touch[4], reg[4]; for (int d = 0; d < nd; d++) { touch[d] = b[d].t_touch; reg[d] = b[d].t_reg; }
        if (f == F_HIPMALLOC) for (int d = 0; d < nd; d++) { ntt_ref[d] = tn[d]; bw_ref[d] = tb[d]; }
        char note[160] = ""; double bwmin = 1e30, ntmax = 0;
        for (int d = 0; d < nd; d++) { if (tb[d] < bwmin) bwmin = tb[d]; if (tn[d] > ntmax) ntmax = tn[d]; }
        if (ntt_ref[0] > 0 && f != F_HIPMALLOC) snprintf(note, sizeof note, "ntt %.2fx, bw %.2fx of hipMalloc", ntmax / tmax(ntt_ref, nd), bwmin / (bw_ref[0] > 0 ? bw_ref[0] : 1));
        printf("%-9s | %7.2f %7.3f %7.2f | %6.2f %7.2f | %7.0f %7.3f | %7.0f %7.0f | %7.1f | %6.2f %6.2f | %s\n", fname[f], tmax(ta, nd), tmax(ta, nd) / (nd * bytes / 1e9), tmax(tf, nd),
               is_mmap(f) ? tmax(touch, nd) : 0.0, is_mmap(f) ? tmax(reg, nd) : 0.0, bwmin, ntmax, td[0], tp[0], tc[0], tmax(tfree, nd), tsum(ta, nd) / nd, note);
        char nm[48]; snprintf(nm, sizeof nm, "alloc_%s_s_per_GB", fname[f]); harness_result(nm, "s/GB", tmax(ta, nd) / (nd * bytes / 1e9));
        if (f == F_ASYNC) for (int d = 0; d < nd; d++) if (b[d].pool) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemPoolDestroy(b[d].pool)); }
    }
    if (sel[F_VMM]) {
        /* Phase 14 R1 (E8): the VMM form's own questions -- the cost of create / map / access / unmap / release per GiB, whether unmapping
         * returns the memory (/proc/meminfo), peer access from a kernel on another APU, and the pool's shape: chunks unmapped from the
         * middle of a live range, new chunks mapped into the holes and beyond the end (growing in place), the whole VA read by one kernel */
        printf("\n-- vmm (E8): %.1f GB per APU in chunks of %.1f GiB, the VA reserved at 2x --\n", bytes / 1e9, vmm_chunk_bytes() / 1073741824.0);
        long free0 = meminfo_kb("MemFree"), avail0 = meminfo_kb("MemAvailable");
        struct buf b[4]; memset(b, 0, sizeof b); int ok[4]; double ta[4], tu[4], tr[4];
#pragma omp parallel num_threads(nd)
        { int d = omp_get_thread_num(); double x = now(); ok[d] = alloc_form(&b[d], F_VMM, d, bytes); ta[d] = now() - x; }
        int allok = 1; for (int d = 0; d < nd; d++) if (!ok[d]) allok = 0;
        if (!allok) printf("  vmm: allocation failed on some device\n");
        else {
            long free1 = meminfo_kb("MemFree"), avail1 = meminfo_kb("MemAvailable");
            for (int d = 0; d < nd; d++) printf("  APU %d: %d chunks of %.2f GiB in %.2f s: create %.3f s/GiB, map %.3f s/GiB, set-access (%d devices) %.3f s/GiB; VA %p .. +%.1f GB\n", d, b[d].nh, b[d].chunk / 1073741824.0, ta[d],
                                                b[d].t_create / (bytes / 1073741824.0), b[d].t_map / (bytes / 1073741824.0), nd, b[d].t_access / (bytes / 1073741824.0), b[d].p, b[d].reserved / 1e9);
            /* first touch and a checksum over the whole range from the owner, then the same range read by a kernel on the next APU (peer) */
            double tf[4], tb[4], tpk[4];
#pragma omp parallel num_threads(nd)
            { int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); double x = now(); k_fill<<<228 * 8, 256, 0, st[d]>>>((uint64_t *)b[d].p, bytes / 8, 7 + d); HIP_CHECK(hipStreamSynchronize(st[d])); tf[d] = now() - x;
              size_t half = bytes / 2 / 16 * 16; uint64_t *p = (uint64_t *)b[d].p;
              k_copy<<<228 * 8, 256, 0, st[d]>>>(p + half / 8, p, half / 8); HIP_CHECK(hipStreamSynchronize(st[d]));
              x = now(); k_copy<<<228 * 8, 256, 0, st[d]>>>(p + half / 8, p, half / 8); HIP_CHECK(hipStreamSynchronize(st[d])); tb[d] = 2.0 * half / 1e9 / (now() - x); }
            long free2 = meminfo_kb("MemFree");
            if (nd > 1) {
#pragma omp parallel num_threads(nd)
                { int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); size_t half = bytes / 2 / 16 * 16; uint64_t *p = (uint64_t *)b[d].p, *q = (uint64_t *)b[(d + 1) % nd].p;
                  k_copy<<<228 * 8, 256, 0, st[d]>>>(p + half / 8, q, half / 8); HIP_CHECK(hipStreamSynchronize(st[d]));   /* device d's kernel reads the next APU's VMM range */
                  double x = now(); k_copy<<<228 * 8, 256, 0, st[d]>>>(p + half / 8, q, half / 8); HIP_CHECK(hipStreamSynchronize(st[d])); tpk[d] = 2.0 * half / 1e9 / (now() - x); }
            }
            printf("  fill %.2f s (slowest APU); copy kernel %.0f GB/s (min over APUs); a kernel on APU d reading APU d+1's VMM range: %.0f GB/s (min)\n", tmax(tf, nd), (tb[0] < tb[nd - 1] ? tb[0] : tb[nd - 1]), nd > 1 ? tpk[0] : 0.0);
            /* verify: sample limbs across chunk boundaries after a peer copy landed in the second half */
            { int bad = 0; for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); for (int i = 1; i < b[d].nh; i++) { size_t lim = (size_t)i * b[d].chunk / 8; uint64_t v[2];
                HIP_CHECK(hipMemcpy(v, (uint64_t *)b[d].p + lim - 1, 16, hipMemcpyDeviceToHost)); if (lim - 1 < bytes / 16 && v[0] != 7 + (uint64_t)d + (lim - 1)) bad++; if (lim < bytes / 16 && v[1] != 7 + (uint64_t)d + lim) bad++; } }
              printf("  chunk boundaries: %s\n", bad ? "MISMATCH" : "the fill's values continue across every boundary (ok)"); VERIFY(!bad, "vmm chunk boundaries"); }
            if (do_ntt && bytes >= ((size_t)8 << 30)) {
                double tn[4];
#pragma omp parallel num_threads(nd)
                { int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); uint64_t *p = (uint64_t *)b[d].p;
                  ntt_fwd(ctx[d], p, 28, 4, st[d]); HIP_CHECK(hipStreamSynchronize(st[d]));
                  double x = now(); ntt_fwd(ctx[d], p, 28, 4, st[d]); HIP_CHECK(hipStreamSynchronize(st[d])); tn[d] = now() - x; }
                printf("  ntt 2^28 x 4 on the VMM range: %.3f s (slowest APU)%s\n", tmax(tn, nd), ntt_ref[0] > 0 ? "" : " (run with hipmalloc for the reference)");
                if (ntt_ref[0] > 0) printf("  ntt %.3fx of hipMalloc (%.3f s)\n", tmax(tn, nd) / tmax(ntt_ref, nd), tmax(ntt_ref, nd));
            }
            /* the pool's shape: unmap the odd chunks (holes in a live range), does the memory come back?  then map fresh chunks into the
             * holes and one past the end (growth in place), and read the whole range again */
            double tu1[4], tr1[4];
#pragma omp parallel num_threads(nd)
            { int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); double x = now();
              for (int i = 1; i < b[d].nh; i += 2) HIP_CHECK(hipMemUnmap((char *)b[d].p + (size_t)i * b[d].chunk, b[d].chunk)); double y = now();
              for (int i = 1; i < b[d].nh; i += 2) { HIP_CHECK(hipMemRelease(b[d].h[i])); b[d].h[i] = 0; } tu1[d] = y - x; tr1[d] = now() - y; }
            long free3 = meminfo_kb("MemFree");
            size_t holes = 0; for (int d = 0; d < nd; d++) holes += (size_t)((b[d].nh) / 2) * b[d].chunk;
            printf("  odd chunks unmapped + released (%.1f GB over the APUs): unmap %.3f s/GiB, release %.3f s/GiB; MemFree %+.1f GB (MemAvailable at start %.1f GB)\n",
                   holes / 1e9, tmax(tu1, nd) / (holes / nd / 1073741824.0), tmax(tr1, nd) / (holes / nd / 1073741824.0), (free3 - free2) / 1e6, avail0 / 1e6);
            int ok2[4]; double tm2[4];
#pragma omp parallel num_threads(nd)
            { int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); double x = now(); ok2[d] = 1; int nh = b[d].nh;
              for (int i = 1; i < nh; i += 2) { struct buf t = b[d]; t.h = 0; t.nh = 0; if (!vmm_create_map(&t, (size_t)i * b[d].chunk, b[d].chunk, nd)) { ok2[d] = 0; break; } b[d].h[i] = t.h[0]; free(t.h); }
              if (ok2[d]) { struct buf t = b[d]; t.h = 0; t.nh = 0; if (vmm_create_map(&t, (size_t)nh * b[d].chunk, b[d].chunk, nd)) { b[d].h = (hipMemGenericAllocationHandle_t *)realloc(b[d].h, (nh + 1) * sizeof *b[d].h); b[d].h[nh] = t.h[0]; b[d].nh = nh + 1; free(t.h); } else ok2[d] = 0; }
              tm2[d] = now() - x; }
            int all2 = 1; for (int d = 0; d < nd; d++) if (!ok2[d]) all2 = 0;
            if (all2) {
                double tf2[4]; int bad = 0;
#pragma omp parallel num_threads(nd)
                { int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); size_t n2 = (size_t)b[d].nh * b[d].chunk / 8; double x = now();
                  k_fill<<<228 * 8, 256, 0, st[d]>>>((uint64_t *)b[d].p, n2, 11 + d); HIP_CHECK(hipStreamSynchronize(st[d]));
                  k_copy<<<228 * 8, 256, 0, st[d]>>>((uint64_t *)b[d].p + n2 / 2 / 2 * 2, (uint64_t *)b[d].p, n2 / 2 / 2 * 2); HIP_CHECK(hipStreamSynchronize(st[d])); tf2[d] = now() - x; }
                for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); size_t n2 = (size_t)b[d].nh * b[d].chunk / 8, h2 = n2 / 2 / 2 * 2; uint64_t v; HIP_CHECK(hipMemcpy(&v, (uint64_t *)b[d].p + h2 + 5, 8, hipMemcpyDeviceToHost)); if (v != 11 + (uint64_t)d + 5) bad++; HIP_CHECK(hipMemcpy(&v, (uint64_t *)b[d].p + n2 - 1, 8, hipMemcpyDeviceToHost)); if (v != 11 + (uint64_t)d + n2 - 1) bad++; }
                printf("  the holes re-mapped with fresh chunks and one chunk mapped past the end (%.2f s per APU, slowest): the range is now %d chunks = %.1f GB contiguous VA over non-contiguous physical memory; fill + copy over all of it %.2f s; values %s\n",
                       tmax(tm2, nd), b[0].nh, (double)b[0].nh * b[0].chunk / 1e9, tmax(tf2, nd), bad ? "MISMATCH" : "ok");
                VERIFY(!bad, "vmm re-mapped range");
            } else printf("  re-mapping into the holes / past the end FAILED on some APU\n");
            for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); vmm_free(&b[d], &tu[d], &tr[d]); }
            long free4 = meminfo_kb("MemFree");
            printf("  all unmapped + released: unmap %.3f s/GiB, release %.3f s/GiB; MemFree: start %.1f GB, mapped+filled %.1f GB (%+.1f), after release %.1f GB (%+.1f vs start)\n",
                   tmax(tu, nd) / ((double)b[0].nh * b[0].chunk / 1073741824.0), tmax(tr, nd) / ((double)b[0].nh * b[0].chunk / 1073741824.0), free0 / 1e6, free2 / 1e6, (free2 - free0) / 1e6, free4 / 1e6, (free4 - free0) / 1e6);
            (void)free1; (void)avail1;
            harness_result("vmm_map_s_per_GiB", "s/GiB", tmax(ta, nd) / (bytes / 1073741824.0));
        }
    }
    if (getenv("T_ALLOC_PROBE") ? atoi(getenv("T_ALLOC_PROBE")) : 1) {
        /* where the time goes: the allocating thread's CPU time (user / system) against the wall -- kernel page work in the
         * caller, or waiting on a lock; 4 threads vs 1 thread vs 4 processes (a per-process lock would let processes run in parallel) */
        printf("\n-- probe: hipMalloc %.1f GB per APU: the allocating threads' CPU time, and 1 thread / 4 threads / 4 processes --\n", bytes / 1e9);
        struct buf b[4]; memset(b, 0, sizeof b); double ta[4], ut[4], st[4];
        double t0 = now();
#pragma omp parallel num_threads(nd)
        { int d = omp_get_thread_num(); struct rusage r0, r1; getrusage(RUSAGE_THREAD, &r0); double x = now(); alloc_form(&b[d], F_HIPMALLOC, d, bytes); ta[d] = now() - x; getrusage(RUSAGE_THREAD, &r1);
          ut[d] = (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) + 1e-6 * (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec); st[d] = (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) + 1e-6 * (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec); }
        double w4 = now() - t0;
        for (int d = 0; d < nd; d++) printf("  thread %d: wall %.2f s, user %.2f s, system %.2f s\n", d, ta[d], ut[d], st[d]);
        for (int d = 0; d < nd; d++) free_form(&b[d]);
        t0 = now(); for (int d = 0; d < nd; d++) alloc_form(&b[d], F_HIPMALLOC, d, bytes); double w1 = now() - t0;
        for (int d = 0; d < nd; d++) free_form(&b[d]);
        printf("  4 threads: %.2f s wall (%.3f s/GB aggregate); 1 thread, 4 calls: %.2f s (%.3f)   (a fork of a HIP process cannot allocate: 4 processes are not measurable here)\n",
               w4, w4 / (nd * bytes / 1e9), w1, w1 / (nd * bytes / 1e9));
        double t2 = now(); struct rusage r0, r1; getrusage(RUSAGE_THREAD, &r0); alloc_form(&b[0], F_HIPMALLOC, 0, bytes); getrusage(RUSAGE_THREAD, &r1); double w2 = now() - t2;
        printf("  one thread, one device, %.1f GB: wall %.2f s, user %.2f s, system %.2f s (%.3f s/GB)\n", bytes / 1e9, w2, (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) + 1e-6 * (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec), (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) + 1e-6 * (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec), w2 / (bytes / 1e9));
        free_form(&b[0]);
        harness_result("alloc_4threads", "s", w4); harness_result("alloc_1thread", "s", w1);
    }
    if (do_seed) {
        /* the contention: hipMalloc of the same bytes with a seed-like team running (compute + device stores) */
        printf("\n-- hipMalloc under a seed-like team (%.1f GB per APU on %d APUs; the team: compute-bound threads storing into device memory) --\n", bytes / 1e9, nd);
        int ncpu = omp_get_max_threads();
        uint64_t *dst[4]; size_t sb = (size_t)4 << 30;
        for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMalloc(&dst[d], sb)); HIP_CHECK(hipMemset(dst[d], 0, sb)); HIP_CHECK(hipDeviceSynchronize()); }
        struct { int nth, store; const char *name; } cases[] = { { 0, 0, "no team (reference)" }, { ncpu, 1, "all cores, storing" }, { ncpu - 4, 1, "4 cores free, storing" }, { ncpu - 8, 1, "8 cores free, storing" }, { ncpu, 0, "all cores, compute only" }, { ncpu / 2, 1, "half the cores, storing" } };
        int ncase = sizeof cases / sizeof cases[0];
        printf("%-26s | %7s %7s | %8s %9s\n", "team", "alloc", "s/GB", "team GB/s", "team iters");
        for (int c = 0; c < ncase; c++) {
            struct seedbg s; memset(&s, 0, sizeof s); s.nthreads = cases[c].nth; s.store = cases[c].store; s.bytes = sb * nd; for (int d = 0; d < nd; d++) s.dst[d] = dst[d];
            s.bytes = sb; pthread_t th; int team = s.nthreads > 0;
            if (team) { pthread_create(&th, 0, seedbg_run, &s); usleep(300000); }
            struct buf b[4]; memset(b, 0, sizeof b); double ta[4]; int ok[4];
            double t0 = now();
#pragma omp parallel num_threads(nd)
            { int d = omp_get_thread_num(); double x = now(); ok[d] = alloc_form(&b[d], F_HIPMALLOC, d, bytes); ta[d] = now() - x; }
            double t1 = now() - t0;
            if (team) { s.stop = 1; pthread_join(th, 0); }
            for (int d = 0; d < nd; d++) if (ok[d]) free_form(&b[d]);
            printf("%-26s | %7.2f %7.3f | %8.2f %9ld\n", cases[c].name, t1, t1 / (nd * bytes / 1e9), team ? s.gbs : 0.0, team ? s.iters : 0L);
            char nm[64]; snprintf(nm, sizeof nm, "alloc_under_team_%d", c); harness_result(nm, "s/GB", t1 / (nd * bytes / 1e9));
        }
        for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipFree(dst[d])); }
    }
    for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); if (ctx[d]) ntt_ctx_free(ctx[d]); HIP_CHECK(hipStreamDestroy(st[d])); }
    VERIFY(1, "ran");
    return verify_done("t_alloc");
}

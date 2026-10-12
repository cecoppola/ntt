/* k8b_probe.cpp (K8b, results/K8b.md) - micro-probe of the seed transfers' queueing behind the background VMM mappers.
 *
 * Reproduces ecalc's init window at one node (4 APUs): four "background mapper" threads, one chunk at a time under a mutex
 * (as dbig.c's vmm_bg_map: hipMemCreate + hipMemMap + hipMemSetAccess for the 4 APUs + hipMemsetAsync + sync per 2 GiB chunk),
 * while the main thread plays the seed thread: 8 chunks of 8 GiB, each "computed" (filled) into one of two pinned host buffers
 * and transferred into pre-mapped device memory (chunk c to APU c % 4).  The transfer variant is the argument:
 *   kern      ecalc's default: db_copy_h2d_async (hipSetDevice, hipHostGetDevicePointer, k_copy_h2d on a pre-created
 *             non-blocking stream, hipSetDevice back), wait = hipStreamSynchronize; the three runtime calls timed apart
 *   kern0     the launch alone (device pointer and stream resolved before the mappers start, no hipSetDevice)
 *   masync    hipMemcpyAsync on the pre-created stream into the VMM range, wait = hipStreamSynchronize
 *   masyncm   the same into hipMalloc'd memory (the SDMA path)
 *   memcpy    hipMemcpy (synchronous) into the VMM range
 *   pull      a persistent kernel per APU launched before the mappers start; issue = a descriptor + a release store in pinned
 *             host memory, wait = a host spin on a done counter (no HIP runtime call inside the window)
 *   pull_late the same, the four kernels launched after the mappers started (the launch cost under contention)
 *   cpustore  CPU stores (memcpy) into hipMalloc'd memory (ecalc's DB_POOL_VMM=0 path; a VMM range would fault)
 *   none      the mappers alone
 * Env: CHUNKS (12: 2 GiB chunks per APU, 96 GiB in all, as the 90 GiB parity-1 halves at 6.441e10), CHUNK_GB (2), SEED_CHUNKS (8),
 *      SEED_GB (8), NOMAP=1 (no mappers: the variant's cost alone), MAP_PAR=1 (mappers without the mutex), REG_LATE=1 (register the
 *      two host buffers after the mappers start, as ecalc does: 1.50 s measured in K8), FILL=0 (no fill between chunks).
 * Every variant verifies the device content (a checksum kernel per APU against the host's sum) and prints one RESULT line.
 * Build: hipcc -O2 --offload-arch=gfx942 -fopenmp -o k8b_probe k8b_probe.cpp */
#include <hip/hip_runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <omp.h>

#define ND 4
#define CK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "%s:%d: %s -> %s\n", __FILE__, __LINE__, #x, hipGetErrorString(e_)); exit(3); } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static double T0;
static void tl(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tl(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("tl %7.2f  ", now() - T0); vprintf(fmt, ap); printf("\n"); fflush(stdout); va_end(ap); }
static long envl(const char *k, long d) { const char *v = getenv(k); return v ? atol(v) : d; }

/* ---- the copy kernels ---- */
__global__ void k_copy_h2d(uint64_t *dst, const uint64_t *src, size_t n)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) dst[i] = src[i];
}
struct pullctl { unsigned long long seq, done, exited, quit; uint64_t *dst; const uint64_t *src; unsigned long long n; };
#define PULL_BLOCKS 456
__global__ void k_pull(pullctl *c)
{
    unsigned long long my = 0;
    for (;;) {
        unsigned long long s;
        while ((s = __hip_atomic_load(&c->seq, __ATOMIC_ACQUIRE, __HIP_MEMORY_SCOPE_SYSTEM)) == my) {
            if (__hip_atomic_load(&c->quit, __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_SYSTEM)) {
                __syncthreads();
                if (threadIdx.x == 0) __hip_atomic_fetch_add(&c->exited, 1ull, __ATOMIC_RELEASE, __HIP_MEMORY_SCOPE_SYSTEM);
                return;
            }
            __builtin_amdgcn_s_sleep(127);
        }
        my = s;
        uint64_t *dst = (uint64_t *)__hip_atomic_load((unsigned long long *)&c->dst, __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_SYSTEM);
        const uint64_t *src = (const uint64_t *)__hip_atomic_load((unsigned long long *)&c->src, __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_SYSTEM);
        size_t n = __hip_atomic_load(&c->n, __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_SYSTEM);
        size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
        for (; i < n; i += stride) dst[i] = src[i];
        __threadfence_system();
        __syncthreads();
        if (threadIdx.x == 0) __hip_atomic_fetch_add(&c->done, 1ull, __ATOMIC_RELEASE, __HIP_MEMORY_SCOPE_SYSTEM);
    }
}
__global__ void k_sum(const uint64_t *p, size_t n, unsigned long long *out)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    unsigned long long s = 0;
    for (; i < n; i += stride) s += p[i];
    __shared__ unsigned long long sh[256]; sh[threadIdx.x] = s; __syncthreads();
    for (int w = 128; w > 0; w >>= 1) { if (threadIdx.x < w) sh[threadIdx.x] += sh[threadIdx.x + w]; __syncthreads(); }
    if (threadIdx.x == 0) atomicAdd(out, sh[0]);
}

/* ---- VMM helpers (as dbig.c) ---- */
static size_t vmm_map_chunk(int d, char *va, size_t C, double *tc, double *tm, double *ta)
{
    hipMemAllocationProp prop; memset(&prop, 0, sizeof prop);
    prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = d;
    hipMemGenericAllocationHandle_t h; double t0 = now();
    CK(hipMemCreate(&h, C, &prop, 0)); double t1 = now();
    CK(hipMemMap(va, C, 0, h, 0)); double t2 = now();
    hipMemAccessDesc ad[ND]; memset(ad, 0, sizeof ad);
    for (int c = 0; c < ND; c++) { ad[c].location.type = hipMemLocationTypeDevice; ad[c].location.id = c; ad[c].flags = hipMemAccessFlagsProtReadWrite; }
    CK(hipMemSetAccess(va, C, ad, ND)); double t3 = now();
    if (tc) { *tc += t1 - t0; *tm += t2 - t1; *ta += t3 - t2; }
    return C;
}

/* ---- the mappers ---- */
static pthread_mutex_t g_map_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_go_mx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_go_cv = PTHREAD_COND_INITIALIZER; static int g_go;
static int g_chunks, g_map_par; static size_t g_C;
static struct mapper { int d; char *va; hipStream_t st; double t_total, t_cr, t_mp, t_ac, t_ms, t_q, t_end; } g_map[ND];
static void *mapper(void *a)
{
    struct mapper *m = (struct mapper *)a;
    CK(hipSetDevice(m->d));
    pthread_mutex_lock(&g_go_mx); while (!g_go) pthread_cond_wait(&g_go_cv, &g_go_mx); pthread_mutex_unlock(&g_go_mx);
    double t0 = now();
    for (int k = 0; k < g_chunks; k++) {
        double tq = now();
        if (!g_map_par) pthread_mutex_lock(&g_map_mx);
        m->t_q += now() - tq;
        vmm_map_chunk(m->d, m->va + (size_t)k * g_C, g_C, &m->t_cr, &m->t_mp, &m->t_ac);
        double tz = now();
        CK(hipMemsetAsync(m->va + (size_t)k * g_C, 0, g_C, m->st)); CK(hipStreamSynchronize(m->st));
        m->t_ms += now() - tz;
        if (!g_map_par) pthread_mutex_unlock(&g_map_mx);
    }
    m->t_total = now() - t0; m->t_end = now() - T0;
    tl("APU%d mapper done: %d chunks in %.2f s (create %.2f, map %.2f, access %.2f, zero %.2f, queued %.2f)", m->d, g_chunks, m->t_total, m->t_cr, m->t_mp, m->t_ac, m->t_ms, m->t_q);
    return 0;
}

/* ---- pinned host buffers (as mem_hstage_alloc) ---- */
static void *hstage_alloc(size_t bytes, int reg, double *t_reg)
{
    const size_t huge = 2u << 20; bytes = (bytes + huge - 1) & ~(huge - 1); void *p = 0;
    if (posix_memalign(&p, huge, bytes)) { fprintf(stderr, "posix_memalign failed\n"); exit(3); }
    madvise(p, bytes, MADV_HUGEPAGE);
#pragma omp parallel for schedule(static)
    for (size_t off = 0; off < bytes; off += 4096) ((unsigned char *)p)[off] = 0;
    if (reg) { double t = now(); CK(hipHostRegister(p, bytes, hipHostRegisterDefault)); if (t_reg) *t_reg += now() - t; }
    return p;
}
static unsigned long long fill(uint64_t *b, size_t n, unsigned c)   /* the "compute": chunk c's pattern, and its sum */
{
    unsigned long long s = 0; uint64_t k = (uint64_t)(c + 1) * 0x9E3779B97F4A7C15ull;
#pragma omp parallel for schedule(static) reduction(+:s)
    for (size_t i = 0; i < n; i++) { uint64_t v = k + i; b[i] = v; s += v; }
    return s;
}

int main(int argc, char **argv)
{
    const char *var = argc > 1 ? argv[1] : "kern";
    g_chunks = (int)envl("CHUNKS", 12); g_C = (size_t)envl("CHUNK_GB", 2) << 30; g_map_par = (int)envl("MAP_PAR", 0);
    int nseed = (int)envl("SEED_CHUNKS", 8); size_t SB = (size_t)envl("SEED_GB", 8) << 30; int nomap = (int)envl("NOMAP", 0), reg_late = (int)envl("REG_LATE", 0), dofill = (int)envl("FILL", 1);
    int nd; CK(hipGetDeviceCount(&nd)); if (nd < ND) { fprintf(stderr, "need %d devices, have %d\n", ND, nd); return 3; }
    int V_kern = !strcmp(var, "kern"), V_kern0 = !strcmp(var, "kern0"), V_masync = !strcmp(var, "masync"), V_masyncm = !strcmp(var, "masyncm"), V_memcpy = !strcmp(var, "memcpy"),
        V_pull = !strcmp(var, "pull"), V_pull_late = !strcmp(var, "pull_late"), V_cpu = !strcmp(var, "cpustore"), V_none = !strcmp(var, "none");
    if (!(V_kern || V_kern0 || V_masync || V_masyncm || V_memcpy || V_pull || V_pull_late || V_cpu || V_none)) { fprintf(stderr, "unknown variant %s\n", var); return 2; }
    int use_malloc = V_masyncm || V_cpu;
    T0 = now();
    printf("k8b_probe %s: %d x %zu GiB chunks per APU (%s), %d seed chunks of %zu GiB, nomap %d, reg_late %d, fill %d, %d omp threads\n", var, g_chunks, g_C >> 30, g_map_par ? "parallel" : "one at a time", nseed, SB >> 30, nomap, reg_late, dofill, omp_get_max_threads());
    /* device memory: the mapper's VA range per APU, and the seed destination per APU (mapped now, before the window) */
    size_t per_dev = (size_t)((nseed + ND - 1) / ND) * SB; uint64_t *sdst[ND]; hipStream_t s[ND]; unsigned long long *dsum[ND]; pullctl *pc[ND];
    double t_setup = now();
    for (int d = 0; d < ND; d++) {
        CK(hipSetDevice(d)); g_map[d].d = d;
        CK(hipMemAddressReserve((void **)&g_map[d].va, (size_t)g_chunks * g_C, g_C, 0, 0));
        CK(hipStreamCreateWithFlags(&g_map[d].st, hipStreamNonBlocking));
        CK(hipStreamCreateWithFlags(&s[d], hipStreamNonBlocking));
        if (use_malloc) CK(hipMalloc((void **)&sdst[d], per_dev));
        else { CK(hipMemAddressReserve((void **)&sdst[d], per_dev, g_C, 0, 0)); for (size_t off = 0; off < per_dev; off += g_C) vmm_map_chunk(d, (char *)sdst[d] + off, g_C, 0, 0, 0); }
        CK(hipMemsetAsync(sdst[d], 0, per_dev, s[d])); CK(hipStreamSynchronize(s[d]));
        CK(hipMalloc((void **)&dsum[d], 8)); CK(hipMemset(dsum[d], 0, 8));
        CK(hipHostMalloc((void **)&pc[d], sizeof(pullctl), hipHostMallocCoherent)); memset(pc[d], 0, sizeof(pullctl));
    }
    CK(hipSetDevice(0));
    double t_reg = 0; uint64_t *buf[2]; const uint64_t *dsrc[2];
    for (int b = 0; b < 2; b++) buf[b] = (uint64_t *)hstage_alloc(SB, !reg_late, &t_reg);
    if (!reg_late) for (int b = 0; b < 2; b++) { void *p = 0; if (hipHostGetDevicePointer(&p, buf[b], 0) != hipSuccess || !p) p = buf[b]; dsrc[b] = (const uint64_t *)p; }
    tl("setup done (%.2f s; host buffers registered %.2f s)", now() - t_setup, t_reg);
    if (V_pull) { for (int d = 0; d < ND; d++) { CK(hipSetDevice(d)); k_pull<<<PULL_BLOCKS, 256, 0, s[d]>>>(pc[d]); CK(hipGetLastError()); } CK(hipSetDevice(0)); tl("pull kernels launched"); }
    /* the mappers */
    pthread_t th[ND];
    if (!nomap) for (int d = 0; d < ND; d++) pthread_create(&th[d], 0, mapper, &g_map[d]);
    double t_win = now(); T0 = t_win;
    pthread_mutex_lock(&g_go_mx); g_go = 1; pthread_cond_broadcast(&g_go_cv); pthread_mutex_unlock(&g_go_mx);
    tl("window starts: %s", nomap ? "no mappers" : "mappers released");
    if (reg_late) { double t = now(); for (int b = 0; b < 2; b++) CK(hipHostRegister(buf[b], SB, hipHostRegisterDefault)); t_reg = now() - t;
        for (int b = 0; b < 2; b++) { void *p = 0; if (hipHostGetDevicePointer(&p, buf[b], 0) != hipSuccess || !p) p = buf[b]; dsrc[b] = (const uint64_t *)p; }
        tl("host buffers registered under the mappers: %.2f s", t_reg); }
    if (V_pull_late) { double t = now(); for (int d = 0; d < ND; d++) { CK(hipSetDevice(d)); k_pull<<<PULL_BLOCKS, 256, 0, s[d]>>>(pc[d]); CK(hipGetLastError()); } CK(hipSetDevice(0)); tl("pull kernels launched under the mappers: %.2f s", now() - t); V_pull = 1; }
    /* the seed thread */
    double t_issue = 0, t_issue_max = 0, t_wait = 0, t_fill = 0, t_sd = 0, t_gp = 0, t_ln = 0; unsigned long long expect[ND] = { 0, 0, 0, 0 }; size_t n = SB / 8; int bad = 0;
    unsigned long long seqc[ND] = { 0, 0, 0, 0 };
    for (int c = 0; c < nseed && !V_none; c++) {
        int b = c & 1, d = c % ND; uint64_t *dst = sdst[d] + (size_t)(c / ND) * n;
        double tf = now(); expect[d] += dofill ? fill(buf[b], n, (unsigned)c) : 0; double tf1 = now() - tf; t_fill += tf1;
        double ti = now();
        if (V_kern) {
            int cur; double a = now(); CK(hipGetDevice(&cur)); CK(hipSetDevice(d)); double bq = now(); t_sd += bq - a;
            void *p = 0; if (hipHostGetDevicePointer(&p, buf[b], 0) != hipSuccess || !p) p = buf[b]; double cq = now(); t_gp += cq - bq;
            unsigned blocks = (unsigned)((n + 255) / 256); if (blocks > 228 * 16) blocks = 228 * 16;
            k_copy_h2d<<<blocks, 256, 0, s[d]>>>(dst, (const uint64_t *)p, n); CK(hipGetLastError()); double dq = now(); t_ln += dq - cq;
            a = now(); CK(hipSetDevice(cur)); t_sd += now() - a;
        } else if (V_kern0) {
            unsigned blocks = (unsigned)((n + 255) / 256); if (blocks > 228 * 16) blocks = 228 * 16;
            k_copy_h2d<<<blocks, 256, 0, s[d]>>>(dst, dsrc[b], n); CK(hipGetLastError());
        } else if (V_masync || V_masyncm) {
            CK(hipMemcpyAsync(dst, buf[b], SB, hipMemcpyHostToDevice, s[d]));
        } else if (V_memcpy) {
            CK(hipMemcpy(dst, buf[b], SB, hipMemcpyHostToDevice));
        } else if (V_pull) {
            pullctl *q = pc[d]; __atomic_store_n((unsigned long long *)&q->dst, (unsigned long long)dst, __ATOMIC_RELAXED); __atomic_store_n((unsigned long long *)&q->src, (unsigned long long)dsrc[b], __ATOMIC_RELAXED); __atomic_store_n(&q->n, (unsigned long long)n, __ATOMIC_RELAXED);
            __atomic_store_n(&q->seq, ++seqc[d], __ATOMIC_RELEASE);
        } else if (V_cpu) {
            size_t nt = omp_get_max_threads();
#pragma omp parallel for schedule(static) num_threads(nt)
            for (size_t i = 0; i < n; i += (1u << 20)) { size_t m = n - i < (1u << 20) ? n - i : (1u << 20); memcpy(dst + i, buf[b] + i, m * 8); }
        }
        double ti1 = now() - ti; t_issue += ti1; if (ti1 > t_issue_max) t_issue_max = ti1;
        double tw = now();
        if (V_pull) { unsigned long long want = seqc[d] * PULL_BLOCKS; double dl = now() + 120; while (__atomic_load_n(&pc[d]->done, __ATOMIC_ACQUIRE) < want) { if (now() > dl) { tl("FAIL: pull chunk %d not done after 120 s (done %llu of %llu)", c, pc[d]->done, want); bad = 1; break; } usleep(50); } if (bad) break; }
        else if (!V_cpu) CK(hipStreamSynchronize(s[d]));
        double tw1 = now() - tw; t_wait += tw1;
        tl("seed chunk %d -> APU%d: fill %.2f, issue %.3f, wait %.3f", c, d, tf1, ti1, tw1);
    }
    double t_seed_end = now() - T0;
    if (V_pull) { for (int d = 0; d < ND; d++) __atomic_store_n(&pc[d]->quit, 1ull, __ATOMIC_RELEASE);
        double dl = now() + 30; int left = ND; while (left && now() < dl) { left = 0; for (int d = 0; d < ND; d++) if (__atomic_load_n(&pc[d]->exited, __ATOMIC_ACQUIRE) < PULL_BLOCKS) left++; usleep(100); }
        tl("pull kernels exited (%d APUs still inside)", left); if (left) bad = 1; }
    if (!nomap) for (int d = 0; d < ND; d++) pthread_join(th[d], 0);
    double t_map_end = 0; for (int d = 0; d < ND; d++) if (g_map[d].t_end > t_map_end) t_map_end = g_map[d].t_end;
    tl("window ends: mappers %.2f s, seeds ended at %.2f s", nomap ? 0 : t_map_end, t_seed_end);
    /* verify */
    for (int d = 0; d < ND; d++) {
        CK(hipSetDevice(d)); CK(hipStreamSynchronize(s[d])); k_sum<<<912, 256, 0, s[d]>>>(sdst[d], per_dev / 8, dsum[d]); CK(hipStreamSynchronize(s[d]));
        unsigned long long got; CK(hipMemcpy(&got, dsum[d], 8, hipMemcpyDeviceToHost));
        if (got != expect[d]) { printf("VERIFY FAILED APU%d: sum %llx expected %llx\n", d, got, expect[d]); bad = 1; }
    }
    double mt = 0; for (int d = 0; d < ND; d++) mt += g_map[d].t_total;
    printf("RESULT %s%s%s%s: mappers %.2f s (%.1f GiB/s; per-APU create %.2f map %.2f access %.2f zero %.2f queued %.2f, mean) | seeds: %d chunks, issue %.3f s (max %.3f; setdev %.3f getptr %.3f launch %.3f), wait %.3f s, fill %.2f s, ended %.2f s | register %.2f s | %s\n",
           var, nomap ? " NOMAP" : "", g_map_par ? " MAP_PAR" : "", reg_late ? " REG_LATE" : "", nomap ? 0 : t_map_end, nomap ? 0 : (double)g_chunks * ND * (g_C >> 30) / t_map_end,
           mt == 0 ? 0 : (g_map[0].t_cr + g_map[1].t_cr + g_map[2].t_cr + g_map[3].t_cr) / ND, (g_map[0].t_mp + g_map[1].t_mp + g_map[2].t_mp + g_map[3].t_mp) / ND,
           (g_map[0].t_ac + g_map[1].t_ac + g_map[2].t_ac + g_map[3].t_ac) / ND, (g_map[0].t_ms + g_map[1].t_ms + g_map[2].t_ms + g_map[3].t_ms) / ND, (g_map[0].t_q + g_map[1].t_q + g_map[2].t_q + g_map[3].t_q) / ND,
           V_none ? 0 : nseed, t_issue, t_issue_max, t_sd, t_gp, t_ln, t_wait, t_fill, t_seed_end, t_reg, bad ? "VERIFY FAILED" : "VERIFY OK");
    return bad ? 1 : 0;
}

/* comm_copy.c - N7D (F2b): a plain GPU copy kernel for copies that touch the SHMEM proxies' host-registered pool (results/N7C.md).
 * hipMemcpyAsync to or from host-registered memory runs on the SDMA engine (about 21 GB/s on MI300A); a kernel runs at the memory's
 * speed.  Used by comm_shmem.c's COPY under COMM_SHMEM_PX_KCOPY=1; HSA_ENABLE_SDMA=0 had the same effect for every copy of the process. */
#include "comm.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#define CC_THREADS 256
#define CC_MAX_BLOCKS 456            /* two waves of the 228 CUs; the grid-stride loop covers the rest */
template <typename T> __global__ void __launch_bounds__(CC_THREADS) k_ccopy(T *d, const T *s, size_t n)
{
    size_t i = (size_t)blockIdx.x * CC_THREADS + threadIdx.x, st = (size_t)gridDim.x * CC_THREADS;
    for (; i + 3 * st < n; i += 4 * st) { T a = s[i], b = s[i + st], c = s[i + 2 * st], e = s[i + 3 * st]; d[i] = a; d[i + st] = b; d[i + 2 * st] = c; d[i + 3 * st] = e; }
    for (; i < n; i += st) d[i] = s[i];
}
__global__ void k_ccopy_tail(unsigned char *d, const unsigned char *s, size_t n) { for (size_t i = threadIdx.x; i < n; i += blockDim.x) d[i] = s[i]; }
template <typename T> static void launch(void *d, const void *s, size_t n, hipStream_t st)   /* n elements of T */
{
    size_t blocks = (n + CC_THREADS - 1) / CC_THREADS; if (blocks > CC_MAX_BLOCKS) blocks = CC_MAX_BLOCKS;
    hipLaunchKernelGGL(k_ccopy<T>, dim3((unsigned)blocks), dim3(CC_THREADS), 0, st, (T *)d, (const T *)s, n);
}
static void kt_copy(void *d, const void *s, size_t n, hipStream_t st);
/* d <- s, n bytes, on stream st; the same ordering as hipMemcpyAsync.  Misaligned (not 8 bytes) or tiny copies go to hipMemcpyAsync. */
hipError_t comm_copy_async(void *d, const void *s, size_t n, hipStream_t st)
{
    uintptr_t a = (uintptr_t)d | (uintptr_t)s;
    if (n < 4096 || (a & 7)) return hipMemcpyAsync(d, s, n, hipMemcpyDefault, st);
    struct comm_kt kt; comm_ktime_begin(&kt, KT_CCOPY, d, s, n, st);
    kt_copy(d, s, n, st);
    comm_ktime_end(&kt, st);
    return hipGetLastError();
}
static void kt_copy(void *d, const void *s, size_t n, hipStream_t st)
{
    uintptr_t a = (uintptr_t)d | (uintptr_t)s;
    if (!(a & 15)) { size_t m = n / 16; launch<uint4>(d, s, m, st); if (n & 15) hipLaunchKernelGGL(k_ccopy_tail, dim3(1), dim3(64), 0, st, (unsigned char *)d + m * 16, (const unsigned char *)s + m * 16, n & 15); }
    else { size_t m = n / 8; launch<uint64_t>(d, s, m, st); if (n & 7) hipLaunchKernelGGL(k_ccopy_tail, dim3(1), dim3(64), 0, st, (unsigned char *)d + m * 8, (const unsigned char *)s + m * 8, n & 7); }
}

/* ---- KT: in-place kernel timing (COMM_KTIME=1).  A token = a start event recorded before the launch; comm_ktime_end_ records the stop event and queues the
 * pair on the current device's pending list.  Finished pairs are folded into the totals (hipEventQuery, never a wait) at every later call; the rest at exit.
 * Only the device's own threads touch a device's list (one APU thread per device), the totals are under one mutex. */
int g_comm_ktime = 0;
#define KT_NDEV 8
#define KT_MAXP 4096
struct kt_pend { hipEvent_t a, b; int k, sk, dk; size_t bytes; };
struct kt_dev { struct kt_pend p[KT_MAXP]; int head, n; hipEvent_t freev[2 * KT_MAXP]; int nfree; pthread_mutex_t mx; };
static struct kt_dev *g_ktd[KT_NDEV];
static pthread_mutex_t g_ktmx = PTHREAD_MUTEX_INITIALIZER;
static double g_ktms[3][3][3]; static long g_ktn[3][3][3]; static double g_ktb[3][3][3];     /* [kernel][src kind][dst kind] */
static int (*g_ktpool)(const void *);
static int g_ktinit;
void comm_ktime_set_pool(int (*in_pool)(const void *)) { g_ktpool = in_pool; }
static const char *kt_kn[3] = { "k_btrans", "k_bcopy", "k_ccopy" }, *kt_mk[3] = { "dev", "host", "pool" };
static int g_ktdone;
void comm_ktime_report(void)                                                                     /* once per process (atexit, or earlier from ecalc.c / comm_shmem_finalize) */
{
    if (!g_comm_ktime) return;
    pthread_mutex_lock(&g_ktmx); int done = g_ktdone; g_ktdone = 1; pthread_mutex_unlock(&g_ktmx); if (done) return;
    const char *sp = getenv("SLURM_PROCID"); long pe = sp ? atol(sp) : (long)getpid();
    for (int d = 0; d < KT_NDEV; d++) if (g_ktd[d]) {                                          /* fold the rest: the process is ending, waiting is fine */
        hipSetDevice(d);
        while (g_ktd[d]->n) { struct kt_pend *q = &g_ktd[d]->p[g_ktd[d]->head]; float ms; hipEventSynchronize(q->b);
            if (hipEventElapsedTime(&ms, q->a, q->b) == hipSuccess) { g_ktms[q->k][q->sk][q->dk] += ms; g_ktn[q->k][q->sk][q->dk]++; g_ktb[q->k][q->sk][q->dk] += (double)q->bytes; }
            g_ktd[d]->head = (g_ktd[d]->head + 1) % KT_MAXP; g_ktd[d]->n--; } }
    double tms = 0, tb = 0; long tn = 0;
    for (int k = 0; k < 3; k++) for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) if (g_ktn[k][a][b]) {
        printf("ktime pe %ld kernel %s src %s dst %s n %ld ms %.3f bytes %.0f gbps %.1f\n", pe, kt_kn[k], kt_mk[a], kt_mk[b], g_ktn[k][a][b], g_ktms[k][a][b], g_ktb[k][a][b],
               g_ktms[k][a][b] > 0 ? g_ktb[k][a][b] / (g_ktms[k][a][b] * 1e6) : 0.0);
        tn += g_ktn[k][a][b]; tms += g_ktms[k][a][b]; tb += g_ktb[k][a][b]; }
    printf("ktime pe %ld total n %ld ms %.3f bytes %.0f\n", pe, tn, tms, tb); fflush(stdout);
}
static void kt_fold(struct kt_dev *v, int all)
{
    while (v->n) {
        struct kt_pend *q = &v->p[v->head];
        if (hipEventQuery(q->b) != hipSuccess) { (void)hipGetLastError(); break; }
        float ms; if (hipEventElapsedTime(&ms, q->a, q->b) == hipSuccess) {
            pthread_mutex_lock(&g_ktmx); g_ktms[q->k][q->sk][q->dk] += ms; g_ktn[q->k][q->sk][q->dk]++; g_ktb[q->k][q->sk][q->dk] += (double)q->bytes; pthread_mutex_unlock(&g_ktmx); }
        v->freev[v->nfree++] = q->a; v->freev[v->nfree++] = q->b;
        v->head = (v->head + 1) % KT_MAXP; v->n--;
    }
    (void)all;
}
static int kt_kind(const void *p)
{
    if (g_ktpool && g_ktpool(p)) return 2;
    hipPointerAttribute_t a;
    if (hipPointerGetAttributes(&a, p) != hipSuccess) { (void)hipGetLastError(); return 1; }
    return a.type == hipMemoryTypeDevice ? 0 : 1;
}
static __attribute__((constructor)) void kt_ctor(void) { const char *e = getenv("COMM_KTIME"); g_comm_ktime = e && atoi(e) > 0; }
static struct kt_dev *kt_get(int *dv)
{
    int d = 0; if (hipGetDevice(&d) != hipSuccess || d < 0 || d >= KT_NDEV) { (void)hipGetLastError(); return 0; }
    *dv = d;
    pthread_mutex_lock(&g_ktmx);
    if (!g_ktinit) { g_ktinit = 1; atexit(comm_ktime_report); }                                           /* registered after the HIP runtime's own hook: runs before it */
    if (!g_ktd[d]) { g_ktd[d] = (struct kt_dev *)calloc(1, sizeof(struct kt_dev)); }
    pthread_mutex_unlock(&g_ktmx);
    return g_ktd[d];
}
void comm_ktime_begin_(struct comm_kt *t, int kern, const void *dst, const void *src, size_t bytes, hipStream_t st)
{
    int d; struct kt_dev *v = kt_get(&d); if (!v) return;
    pthread_mutex_lock(&v->mx);
    kt_fold(v, 0);
    if (v->n >= KT_MAXP) { pthread_mutex_unlock(&v->mx); return; }                                                                  /* list full: this launch is not timed */
    hipEvent_t a;
    if (v->nfree >= 2) { a = v->freev[--v->nfree]; } else if (hipEventCreate(&a) != hipSuccess) { (void)hipGetLastError(); pthread_mutex_unlock(&v->mx); return; }
    t->a = a; t->k = kern; t->sk = kt_kind(src); t->dk = kt_kind(dst); t->bytes = bytes; t->on = 1;
    hipEventRecord(a, st);
    pthread_mutex_unlock(&v->mx);
}
void comm_ktime_end_(struct comm_kt *t, hipStream_t st)
{
    int d; struct kt_dev *v = kt_get(&d); if (!v) return;
    hipEvent_t b;
    pthread_mutex_lock(&v->mx);
    if (v->nfree >= 1) b = v->freev[--v->nfree]; else if (hipEventCreate(&b) != hipSuccess) { (void)hipGetLastError(); pthread_mutex_unlock(&v->mx); return; }
    hipEventRecord(b, st);
    struct kt_pend *q = &v->p[(v->head + v->n) % KT_MAXP]; q->a = t->a; q->b = b; q->k = t->k; q->sk = t->sk; q->dk = t->dk; q->bytes = t->bytes; v->n++;
    pthread_mutex_unlock(&v->mx);
}

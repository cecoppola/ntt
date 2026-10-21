/* t_xshmem_ofi_smoke.c -- smoke test of the libfabric backend xshmem_ofi.c through xshmem.h only (brief B2).  Mode dc, SPLIT form
 * (putmem_nbi, ctx_quiet, signal_set).  Build: make XSHMEM=ofi OFI=1 tests/t_xshmem_ofi_smoke (aac7 login node).  Run: srun, one PE per process.
 * Phases (barrier between them): 1 ring, each PE -> (me+1) % n: 8 B and 16 MiB, data checked by pattern when the signal is seen;
 * 2 all-to-all of 1 MiB (every PE to every PE including itself), one signal word per source;  3 putmem_signal_nbi (arm, blocks inside) 64 KiB ring.
 * Lanes: contexts are created right after init (XSHMEM_LANES, default 1) and used round-robin by phase 2.  Exit code 0 = all checks passed.
 * Loopback mode (XSHMEM_LOOPBACK_PES = K > 1, one process per node): the process hosts K PEs, one thread per PE; the barriers are a thread barrier plus one
 * xshmem_barrier_all by thread 0; heap_alloc / heap_free run from the main thread in PE order; all pairs run (including PEs of one process, over the NIC). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <hip/hip_runtime.h>
#include "xshmem.h"

#define MB ((size_t)1 << 20)
static __thread int bad;      /* per PE thread in loopback mode */
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static uint8_t pat(int src, size_t i, unsigned seq) { return (uint8_t)(src * 131u + i * 7u + (i >> 8) * 13u + seq * 29u); }
#define HK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP error %s at %d\n", hipGetErrorString(e_), __LINE__); exit(3); } } while (0)

static void fill(void *dev, size_t n, int src, unsigned seq)
{
    uint8_t *h = (uint8_t *)malloc(n);
    for (size_t i = 0; i < n; i++) h[i] = pat(src, i, seq);
    HK(hipMemcpy(dev, h, n, hipMemcpyHostToDevice)); free(h);
}
static void check(const void *dev, size_t n, int src, unsigned seq, const char *what)
{
    uint8_t *h = (uint8_t *)malloc(n); size_t nb = 0, first = 0;
    HK(hipMemcpy(h, dev, n, hipMemcpyDeviceToHost));
    for (size_t i = 0; i < n; i++) if (h[i] != pat(src, i, seq)) { if (!nb) first = i; nb++; }
    if (nb) { bad++; fprintf(stderr, "FAIL pe %d %s from %d: %zu of %zu bytes wrong (first at %zu)\n", xshmem_my_pe(), what, src, nb, n, first); }
    free(h);
}

/* the bytes after the n written ones must be untouched: expect the earlier content (seq 0 = zero heap) */
static void check_tail(const void *dev, size_t from, size_t total, int src, unsigned seq, const char *what)
{
    uint8_t *h = (uint8_t *)malloc(total - from); size_t nb = 0;
    HK(hipMemcpy(h, (const char *)dev + from, total - from, hipMemcpyDeviceToHost));
    for (size_t i = from; i < total; i++) if (h[i - from] != (seq ? pat(src, i, seq) : 0)) nb++;
    if (nb) { bad++; fprintf(stderr, "FAIL pe %d %s: %zu of %zu bytes after the payload were modified\n", xshmem_my_pe(), what, nb, total - from); }
    free(h);
}

static int K = 1;
static pthread_barrier_t pb;
static void sync_all(int l)      /* xshmem_barrier_all is once per process: thread 0 calls it between two thread barriers */
{
    if (K == 1) { xshmem_barrier_all(); return; }
    pthread_barrier_wait(&pb);
    if (l == 0) xshmem_barrier_all();
    pthread_barrier_wait(&pb);
}
typedef struct { int l, nl, rc; xshmem_ctx_t lane[16]; char *D; uint64_t *C; } pe_arg;

static void *run_pe(void *vp)
{
    pe_arg *a = (pe_arg *)vp; int l = a->l, nl = a->nl; xshmem_ctx_t *lane = a->lane; char *D = a->D; uint64_t *C = a->C;
    xshmem_topo_t tp;
    XSHMEM_SELECT(l);
    int me = xshmem_my_pe(), np = xshmem_n_pes(); xshmem_topo(&tp);
    if (K > 1) HK(hipSetDevice(tp.gpu < 0 ? 0 : tp.gpu));
    char *sbuf = D, *rring = D + 16 * MB + 2 * MB, *rall = rring + 16 * MB + 2 * MB;   /* send 16(+2) MiB, ring recv 16(+2), a2a recv np MiB; send of a2a reuses sbuf[0..1 MiB] */
    int next = (me + 1) % np, prev = (me + np - 1) % np; double t0, ring_gbs = 0, a2a_gbs = 0;
    sync_all(l);
    /* phase 1: ring, 8 B then 16 MiB, same word signals (seq 1, 2) */
    size_t sizes[2] = { 8, 16 * MB };
    for (int k = 0; k < 2; k++) {
        size_t n = sizes[k]; unsigned seq = 1 + k;
        fill(sbuf, n, me, seq); t0 = now();
        xshmem_ctx_putmem_nbi(lane[0], rring, sbuf, n, next);
        xshmem_ctx_quiet(lane[0]);
        xshmem_ctx_signal_set(lane[0], &C[0], seq, next);
        xshmem_ctx_quiet(lane[0]);
        xshmem_signal_wait_until(&C[0], XSHMEM_CMP_GE, seq);
        double dt = now() - t0; if (k == 1) ring_gbs = (double)n / dt / 1e9;
        check(rring, n, prev, seq, k ? "ring 16 MiB" : "ring 8 B");
        if (k == 0) check_tail(rring, n, 16 * MB, prev, 0, "ring 8 B tail");
        sync_all(l);
    }
    /* phase 2: all-to-all 1 MiB; word C[1+src] on the receiver is written only by src */
    fill(sbuf, MB, me, 7); t0 = now();
    for (int q = 0; q < np; q++) { int pe = (me + q) % np; xshmem_ctx_putmem_nbi(lane[q % nl], rall + (size_t)me * MB, sbuf, MB, pe); }
    for (int k = 0; k < nl; k++) xshmem_ctx_quiet(lane[k]);
    for (int q = 0; q < np; q++) { int pe = (me + q) % np; xshmem_ctx_signal_set(lane[q % nl], &C[1 + me], 7, pe); }
    for (int k = 0; k < nl; k++) xshmem_ctx_quiet(lane[k]);
    for (int q = 0; q < np; q++) {
        while (!xshmem_signal_test(&C[1 + q], XSHMEM_CMP_GE, 7)) ;
        check(rall + (size_t)q * MB, MB, q, 7, "a2a 1 MiB");
    }
    { double dt = now() - t0; a2a_gbs = (double)np * MB / dt / 1e9; }
    sync_all(l);
    /* phase 3: putmem_signal_nbi arm, 64 KiB ring, signal word C[1+np] */
    { size_t n = 64 * 1024; fill(sbuf, n, me, 9);
      xshmem_ctx_putmem_signal_nbi(lane[0], rring, sbuf, n, &C[1 + np], 9, XSHMEM_SIGNAL_SET, next);
      xshmem_ctx_quiet(lane[0]);
      xshmem_signal_wait_until(&C[1 + np], XSHMEM_CMP_GE, 9);
      check(rring, n, prev, 9, "putmem_signal_nbi 64 KiB");
      check_tail(rring, n, 16 * MB, prev, 2, "putmem_signal_nbi tail (the 16 MiB of seq 2)"); }
    sync_all(l);
    /* verdict: one line per PE (the process-level counters are summed over its PEs in loopback mode) */
    xshmem_ofi_cnt_t c; xshmem_ofi_counters(&c);
    printf("XSMOKE pe %d/%d node %d gpu %d nics %d lanes %d: %s | ring16MiB %.2f GB/s a2a1MiB %.2f GB/s (per PE, single sample) | writes %llu quiets %llu spins %llu | K %d\n",
           me, np, tp.node, tp.gpu, tp.nics, nl, bad ? "FAIL" : "PASS", ring_gbs, a2a_gbs, (unsigned long long)c.writes, (unsigned long long)c.quiets, (unsigned long long)c.wait_spins, K);
    fflush(stdout);
    a->rc = bad != 0;
    sync_all(l);
    return NULL;
}

int main(void)
{
    int prov = 0; xshmem_topo_t tp; pe_arg arg[8]; pthread_t th[8];
    xshmem_init_thread(XSHMEM_THREAD_MULTIPLE, &prov);
    K = xshmem_ofi_loopback_pes(); if (K < 1 || K > 8) { fprintf(stderr, "smoke: K = %d\n", K); return 2; }
    memset(arg, 0, sizeof arg);
    for (int l = 0; l < K; l++) {                       /* contexts: lane order right after init, per PE */
        XSHMEM_SELECT(l); xshmem_topo(&tp);
        arg[l].l = l; arg[l].nl = tp.nlanes < 1 ? 1 : tp.nlanes;
        for (int k = 0; k < arg[l].nl; k++) if (xshmem_ctx_create(XSHMEM_CTX_SERIALIZED, &arg[l].lane[k])) { fprintf(stderr, "ctx_create %d failed\n", k); return 2; }
    }
    int np = xshmem_n_pes();
    size_t dsz = (size_t)(20 + 16 + np + 2) * MB, csz = 4 * MB;
    for (int l = 0; l < K; l++) { XSHMEM_SELECT(l); arg[l].D = (char *)xshmem_heap_alloc(dsz, XSHMEM_SPACE_DATA); }
    for (int l = 0; l < K; l++) { XSHMEM_SELECT(l); arg[l].C = (uint64_t *)xshmem_heap_alloc(csz, XSHMEM_SPACE_CTRL); }
    XSHMEM_SELECT(0);
    int rc = 0;
    if (K == 1) { run_pe(&arg[0]); rc = arg[0].rc; }
    else {
        pthread_barrier_init(&pb, NULL, (unsigned)K);
        for (int l = 0; l < K; l++) pthread_create(&th[l], NULL, run_pe, &arg[l]);
        for (int l = 0; l < K; l++) { pthread_join(th[l], NULL); rc |= arg[l].rc; }
    }
    XSHMEM_SELECT(0);
    for (int l = 0; l < K; l++) { XSHMEM_SELECT(l); xshmem_heap_free(arg[l].C); }
    for (int l = 0; l < K; l++) { XSHMEM_SELECT(l); xshmem_heap_free(arg[l].D); }
    xshmem_finalize();
    return rc;
}

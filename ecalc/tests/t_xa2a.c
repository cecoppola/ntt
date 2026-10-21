/* t_xa2a.c -- transport-only all-to-all probe of the libfabric backend through xshmem.h (TARGET1 diagnosis of the per-GPU-process mode).
 * Same code, two layouts, same PE count, same bytes, same PE -> GPU -> NIC map (pe_geo):
 *   (A) one process per node hosting K PEs, one thread per PE:   XSHMEM_LOOPBACK_PES=K, srun --ntasks-per-node=1
 *   (B) K processes per node, one PE each:                        srun --ntasks-per-node=K
 * Each rep: barrier, every PE puts one slab of B bytes to every other PE (rotated order (me+q)%np, on-node peers over the NIC, as in t_xshmem),
 * quiet, one signal per target, quiet, wait for the np-1 incoming signals.  Sizes 64 KiB .. XA_MAXMB (default 64) in steps of 4, reps XA_REPS (5) after
 * one warm-up.  First and last 16 B of every received slab are checked after the last rep.  Output (one per PE and size):
 *   XA pe <me>/<np> node <n> gpu <g> K <K> | <bytes> B | med_ms <m> min_ms <x> | GB/s_per_PE <r> (received, (np-1) slabs / median)
 *   CTRSUM <bytes> node <n> | miss= restricted_pkts= unrestricted_pkts= flush_reqs=   (node leader, cxi telemetry delta over the reps)
 *   XA_RESULT PASS|FAIL per PE.  Build: make XSHMEM=ofi OFI=1 tests/t_xa2a (aac7 login node). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <dirent.h>
#include <regex.h>
#include <pthread.h>
#include <hip/hip_runtime.h>
#include "xshmem.h"

#define MB ((size_t)1 << 20)
#define MAXCT 6000
#define HK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP error %s at %d\n", hipGetErrorString(e_), __LINE__); exit(3); } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static int K = 1;
static pthread_barrier_t pb;
static void sync_all(int l) { if (K == 1) { xshmem_barrier_all(); return; } pthread_barrier_wait(&pb); if (l == 0) xshmem_barrier_all(); pthread_barrier_wait(&pb); }

typedef struct { char name[72]; int nic; unsigned long long v; } ctr_t;
static regex_t ctre; static int ctre_ok;
static int ct_snap(ctr_t *out)
{
    int n = 0; char dir[160];
    for (int i = 0; i < 16; i++) {
        snprintf(dir, sizeof dir, "/sys/class/cxi/cxi%d/device/telemetry", i); DIR *d = opendir(dir); if (!d) continue;
        struct dirent *e; while ((e = readdir(d)) && n < MAXCT) {
            if (e->d_name[0] == '.' || regexec(&ctre, e->d_name, 0, 0, 0)) continue;
            char p[320]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); FILE *f = fopen(p, "r"); if (!f) continue;
            char b[64] = { 0 }; if (fgets(b, sizeof b, f)) { out[n].v = strtoull(b, 0, 10); snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name); out[n].nic = i; n++; }
            fclose(f); }
        closedir(d);
    }
    return n;
}
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
typedef struct { int l, nl, rc; xshmem_ctx_t lane[16]; char *D; uint64_t *C; } pe_arg;

static void *run_pe(void *vp)
{
    pe_arg *a = (pe_arg *)vp; int l = a->l, nl = a->nl; xshmem_ctx_t *lane = a->lane; uint64_t *C = a->C; xshmem_topo_t tp;
    XSHMEM_SELECT(l);
    int me = xshmem_my_pe(), np = xshmem_n_pes(), bad = 0; xshmem_topo(&tp);
    if (K > 1) HK(hipSetDevice(tp.gpu < 0 ? 0 : tp.gpu));
    int reps = getenv("XA_REPS") ? atoi(getenv("XA_REPS")) : 5; if (reps < 1) reps = 1; if (reps > 32) reps = 32;
    size_t maxb = (size_t)(getenv("XA_MAXMB") ? atol(getenv("XA_MAXMB")) : 64) * MB;
    char *sb = a->D, *rb = a->D + (size_t)np * maxb;
    int ppn = tp.pes_per_node > 0 ? tp.pes_per_node : K, leader = (me % ppn == 0);   /* first PE of its node: reads the node's cxi counters */
    ctr_t *c0 = NULL, *c1 = NULL; int n0 = 0;
    if (leader) { ctre_ok = !regcomp(&ctre, "(^|_)atu(_|$)|page_size|odp|^pi_.*rtrgt|restricted_pkt|dmawr_flush_reqs|atu_flush|pct_req_(un)?ordered|oxe_ioi_pkts_(un)?ordered|miss|ats_", REG_EXTENDED | REG_NOSUB);
        c0 = (ctr_t *)calloc(MAXCT, sizeof *c0); c1 = (ctr_t *)calloc(MAXCT, sizeof *c1); }
    uint64_t seq = 0; uint64_t hdr[2]; uint64_t got[2];
    for (size_t b = 64u << 10; b <= maxb; b *= 4) {
        for (int q = 0; q < np; q++) {            /* header and trailer words of slab (me -> q) */
            hdr[0] = ((uint64_t)me << 32) ^ (uint64_t)q ^ b; hdr[1] = ~hdr[0];
            HK(hipMemcpy(sb + (size_t)q * maxb, hdr, 16, hipMemcpyHostToDevice)); HK(hipMemcpy(sb + (size_t)q * maxb + b - 16, hdr, 16, hipMemcpyHostToDevice));
        }
        double tt[34]; int nr = reps + 1;          /* rep 0 = warm-up */
        for (int r = 0; r < nr; r++) {
            if (r == 1) { sync_all(l); if (leader && ctre_ok) n0 = ct_snap(c0); }
            sync_all(l); double t0 = now(); seq++;
            for (int q = 1; q < np; q++) { int pe = (me + q) % np; xshmem_ctx_putmem_nbi(lane[q % nl], rb + (size_t)me * maxb, sb + (size_t)pe * maxb, b, pe); }
            for (int k = 0; k < nl; k++) xshmem_ctx_quiet(lane[k]);
            for (int q = 1; q < np; q++) { int pe = (me + q) % np; xshmem_ctx_signal_set(lane[q % nl], &C[me], seq, pe); }
            for (int k = 0; k < nl; k++) xshmem_ctx_quiet(lane[k]);
            for (int q = 1; q < np; q++) { int s = (me + q) % np; while (!xshmem_signal_test(&C[s], XSHMEM_CMP_GE, seq)) ; }
            tt[r] = now() - t0;
        }
        sync_all(l);
        if (leader && ctre_ok) {
            int n1 = ct_snap(c1); long s[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < n1 && i < n0; i++) {
                if (strcmp(c0[i].name, c1[i].name) || c0[i].nic != c1[i].nic) continue;
                long long d = (long long)(c1[i].v - c0[i].v); if (!d) continue;
                if (strstr(c1[i].name, "miss")) s[0] += d;
                if (strstr(c1[i].name, "unrestricted")) s[2] += d; else if (strstr(c1[i].name, "restricted")) s[1] += d;
                if (strstr(c1[i].name, "flush")) s[3] += d;
            }
            printf("CTRSUM %zu node %d | miss=%ld restricted_pkts=%ld unrestricted_pkts=%ld flush_reqs=%ld (this node, all NICs, %d reps)\n", b, tp.node, s[0], s[1], s[2], s[3], reps);
        }
        for (int q = 0; q < np; q++) {            /* verify the last rep */
            if (q == me) continue;
            HK(hipMemcpy(got, rb + (size_t)q * maxb, 16, hipMemcpyDeviceToHost));
            hdr[0] = ((uint64_t)q << 32) ^ (uint64_t)me ^ b; hdr[1] = ~hdr[0];
            if (got[0] != hdr[0] || got[1] != hdr[1]) bad++;
            HK(hipMemcpy(got, rb + (size_t)q * maxb + b - 16, 16, hipMemcpyDeviceToHost));
            if (got[0] != hdr[0] || got[1] != hdr[1]) bad++;
        }
        double *v = tt + 1; qsort(v, reps, sizeof *v, cmp_d);
        double med = v[reps / 2], mn = v[0];
        printf("XA pe %d/%d node %d gpu %d K %d | %zu B | med_ms %.3f min_ms %.3f | GB/s_per_PE %.2f\n", me, np, tp.node, tp.gpu, K, b, med * 1e3, mn * 1e3, (double)b * (np - 1) / med / 1e9);
        fflush(stdout);
        sync_all(l);
    }
    printf("XA_RESULT pe %d/%d %s (bad slabs %d)\n", me, np, bad ? "FAIL" : "PASS", bad); fflush(stdout);
    a->rc = bad != 0; free(c0); free(c1);
    sync_all(l);
    return NULL;
}

int main(void)
{
    int prov = 0; xshmem_topo_t tp; pe_arg arg[8]; pthread_t th[8];
    xshmem_init_thread(XSHMEM_THREAD_MULTIPLE, &prov);
    K = xshmem_ofi_loopback_pes(); if (K < 1 || K > 8) { fprintf(stderr, "t_xa2a: K = %d\n", K); return 2; }
    memset(arg, 0, sizeof arg);
    for (int l = 0; l < K; l++) {
        XSHMEM_SELECT(l); xshmem_topo(&tp);
        arg[l].l = l; arg[l].nl = tp.nlanes < 1 ? 1 : tp.nlanes;
        for (int k = 0; k < arg[l].nl; k++) if (xshmem_ctx_create(XSHMEM_CTX_SERIALIZED, &arg[l].lane[k])) { fprintf(stderr, "ctx_create %d failed\n", k); return 2; }
    }
    int np = xshmem_n_pes(); size_t maxb = (size_t)(getenv("XA_MAXMB") ? atol(getenv("XA_MAXMB")) : 64) * MB;
    for (int l = 0; l < K; l++) { XSHMEM_SELECT(l); arg[l].D = (char *)xshmem_heap_alloc(2 * (size_t)np * maxb + 4 * MB, XSHMEM_SPACE_DATA); }
    for (int l = 0; l < K; l++) { XSHMEM_SELECT(l); arg[l].C = (uint64_t *)xshmem_heap_alloc(MB, XSHMEM_SPACE_CTRL); }
    XSHMEM_SELECT(0);
    for (int l = 0; l < K; l++) if (!arg[l].D || !arg[l].C) { fprintf(stderr, "t_xa2a: heap_alloc failed\n"); return 2; }
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

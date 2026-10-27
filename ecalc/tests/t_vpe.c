/* t_vpe.c - PE identity and the PE communicators in the one-process-per-GPU layout (ECALC_PE_PER_GPU=1; vpe.h, mn.c, comm_xshmem.c).  (The thread-mode parts were removed 2026-10-23, Q86.)
 *
 *   t_vpe [all|probe|coll|host] [--ppg] | t_vpe info|die|badlayout      (--ppg is accepted and implied: every run is process mode)
 *     host    no GPU, no network: identity arithmetic (vpe_set_nodes / lanes), the log prefix; in forked children vpe_sub out of range (fatal, rc 3) and vpe_sub NULL for a PE outside the run
 *     probe   the sub-communicator probe (D1): on every PE, the aligned groups of 2 and 4 PEs of a node and the contiguous runs of 3, 6, 12 PEs: barrier,
 *             allreduce_max, allgather_host, alltoallv of random counts on each; prints T_VPE_PROBE PASS|FAIL per process
 *     all     host + probe + the flat communicator (alltoall, alltoallv of random counts incl. zeros, allreduce_max, allgather_host, barrier) + the
 *             tree's group schedule for the PE count against mn_groups_parse; prints T_VPE_RESULT PASS|FAIL per process
 *     coll    the small collectives only (XSHMEM_COLL=flat|tree, whichever the environment sets: run both): on the flat communicator and on runs of 2, 3, 5, 6, 7 PEs (any n, not only
 *             powers of two) XT_COLL_ITERS (200) rounds of random back-to-back barrier / allreduce_max / allgather_host (random block sizes, 8 B to 4 KiB), every result checked; then a
 *             timing line per collective on the flat communicator (microseconds per call, XT_COLL_REPS = 500 calls); prints T_VPE_RESULT PASS|FAIL per process, T_VPE_COLL_TIME on PE 0
 *   ONE PROCESS PER PE: launched as 4 tasks per node (mnrun.sh with ECALC_PE_PER_GPU=1: COMM_TRANSPORT=xshmem, XSHMEM_LOOPBACK_PES=1; make XSHMEM=ofi), 1, 2, 3 nodes = 4, 8, 12 PEs.
 *   all / probe: identity (vpe_idx = the local id, lanes = tasks per node, device, prefix, leaders), the node barrier (a laggard PE holds the others), the node group exercised, then the flat
 *   communicator and sub-communicator parts (one T_VPE_RESULT / T_VPE_PROBE line per PE: 4 per node).  Without COMM_TRANSPORT=xshmem the communicator parts are skipped (SKIP line).
 *     info       mn_init, then one PPG_INFO line per PE: launch ids, visible devices, the chosen device, NUMA node, launch cpuset and cpuset now, cxi NIC, the process's VRAM on every
 *                GPU from kfd sysfs (a second GPU with memory = a context there); the launch/binding probe
 *     die        PE 2 (or the last PE) exits 7 after mn_init while the others wait in a barrier: the step must end nonzero (srun --kill-on-bad-exit), nobody hangs
 *     badlayout  the PE of local id 1 believes it is local 2: mn_init must fatal "launch layout mismatch" (rc 3) there and the step ends (the identity cross-check) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sched.h>
#include <dirent.h>
#include <time.h>
#include <hip/hip_runtime.h>
#include "../vpe.h"
#include "../mn.h"
#include "../comm.h"
#include "../mem.h"

int mn_groups_parse(int size, int *out, int max);
comm *comm_xshmem_create_flat(int pe_start, int n, int id) __attribute__((weak));
/* (comm.h declares these only under XSHMEM_ENGINE) */
int comm_xshmem_burst(int nsig, int batch, double *t_issue, double *t_quiet) __attribute__((weak));   /* comm_xshmem.c: a burst of signals to the other PEs (t_vpe hs) */
static int g_fail, g_checks, g_skip_comm, g_ppg;
#define CHK(c, ...) do { __atomic_fetch_add(&g_checks, 1, __ATOMIC_RELAXED); if (!(c)) { __atomic_fetch_add(&g_fail, 1, __ATOMIC_RELAXED); \
    printf("  FAILED %s:%d  %s  pe %d: ", __FILE__, __LINE__, #c, vpe_pe()); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define HIPC(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { printf("HIP %s at %d\n", hipGetErrorString(e_), __LINE__); exit(3); } } while (0)

/* run fn in a forked child with stdout+stderr captured: returns the exit code, the text in buf (the fatal path _exit()s, so nothing of the parent runs) */
static int in_child(void (*fn)(void *), void *arg, char *buf, size_t n)
{
    int pf[2]; fflush(stdout); fflush(stderr);
    if (pipe(pf)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (!pid) { dup2(pf[1], 1); dup2(pf[1], 2); close(pf[0]); close(pf[1]); fn(arg); fflush(stdout); fflush(stderr); _exit(0); }
    close(pf[1]);
    size_t k = 0; ssize_t r; while (k + 1 < n && (r = read(pf[0], buf + k, n - 1 - k)) > 0) k += (size_t)r;
    buf[k] = 0; close(pf[0]);
    int st = 0; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
static void sub_range_body(void *a) { (void)a; (void)vpe_sub(0, vpe_npe() + 1); }
static void sub_slot_body(void *a) { (void)a; (void)vpe_sub_slot(0, 2, 16); }

/* ---- host part ---- */
static void host_part(void)
{
    CHK(vpe_on(), "forced on");
    int L = vpe_lanes(); CHK(L >= 1 && L <= VPE_MAX, "lanes %d", L);
    vpe_set_nodes(2, 3);
    CHK(vpe_node() == 2 && vpe_nnodes() == 3 && vpe_npe() == 3 * L && vpe_pe() == 2 * L + vpe_idx(), "pe %d npe %d", vpe_pe(), vpe_npe());
    CHK(vpe_ppg() && vpe_idx() >= 0 && vpe_idx() < L && ec_log_prefix()[0] == 'p', "prefix outside vpe_run '%s'", ec_log_prefix());
    CHK(vpe_root() == 0 && vpe_node_leader() == (vpe_idx() == 0) && vpe_proc_leader(), "root/leader on the main thread of node 2");
    vpe_set_nodes(0, 1);
    if (g_ppg && vpe_early_device() >= 0) { printf("t_vpe: forked fatal checks skipped (a launched process has initialised HIP: fork is unsafe; `t_vpe host --ppg` on a login node runs them)\n"); }
    else {   /* vpe_sub argument checks (before any communicator exists), in children: fatal rc 3 + text; a PE outside the run gets NULL */
        char b[512]; int rc = in_child(sub_range_body, 0, b, sizeof b);
        CHK(rc == 3 && strstr(b, "outside the"), "vpe_sub out of range: rc %d text '%s'", rc, b);
        rc = in_child(sub_slot_body, 0, b, sizeof b);
        CHK(rc == 3 && strstr(b, "outside the"), "vpe_sub slot 16: rc %d text '%s'", rc, b);
        vpe_set_nodes(1, 2);                             /* PE >= 4 (L = 4): outside the run [0, 2) */
        CHK(vpe_sub(0, 2) == 0, "vpe_sub NULL for a PE outside the run");
        vpe_set_nodes(0, 1);
    }
    printf("t_vpe: host part: %d lanes, identity, prefix: done\n", L);
}

/* ---- communicator part ---- */
static size_t cntw(int s, int t) { return (size_t)((s * 5 + t * 3 + (s ^ t)) % 7) * 37; }   /* words from PE s to PE t: 0..222, zeros included */
static uint64_t wv(int s, int t, size_t j) { return ((uint64_t)s << 40) | ((uint64_t)t << 20) | j; }
/* the five collectives on communicator c, whose rank 0 is PE g0 */
static void exercise(comm *c, int g0, const char *tag)
{
    int n = comm_size(c), r = comm_rank(c), pe = vpe_pe();
    CHK(r == pe - g0 && n >= 1, "%s: rank %d size %d (pe %d g0 %d)", tag, r, n, pe, g0);
    comm_barrier(c);
    size_t mx = comm_allreduce_max(c, (size_t)pe + 1); CHK(mx == (size_t)(g0 + n), "%s: allreduce_max %zu want %d", tag, mx, g0 + n);
    uint64_t mine[2] = { (uint64_t)pe * 7 + 3, (uint64_t)pe ^ 0x55 }, *all = (uint64_t *)calloc((size_t)n * 2, 8);
    comm_allgather_host(c, mine, all, 16);
    for (int i = 0; i < n; i++) CHK(all[2 * i] == (uint64_t)(g0 + i) * 7 + 3 && all[2 * i + 1] == (uint64_t)((g0 + i) ^ 0x55), "%s: allgather member %d", tag, i);
    free(all);
    size_t *sc = (size_t *)calloc((size_t)n * 4, sizeof(size_t)), *sd = sc + n, *rc = sc + 2 * n, *rd = sc + 3 * n;
    for (int t = 0; t < n; t++) { sc[t] = 8 * cntw(pe, g0 + t); rc[t] = 8 * cntw(g0 + t, pe); }
    size_t ts = comm_prefix(sc, sd, n), tr = comm_prefix(rc, rd, n);
    uint64_t *hs = (uint64_t *)calloc(ts / 8 + 1, 8), *hr = (uint64_t *)calloc(tr / 8 + 1, 8), *ds, *dr;
    for (int t = 0; t < n; t++) for (size_t j = 0; j < sc[t] / 8; j++) hs[sd[t] / 8 + j] = wv(pe, g0 + t, j);
    HIPC(hipMalloc(&ds, ts + 8)); HIPC(hipMalloc(&dr, tr + 8)); HIPC(hipMemcpy(ds, hs, ts, hipMemcpyHostToDevice));
    hipStream_t st; HIPC(hipStreamCreate(&st));
    comm_alltoallv(c, ds, sc, sd, dr, rc, rd, st); comm_wait(c);
    HIPC(hipMemcpy(hr, dr, tr, hipMemcpyDeviceToHost));
    size_t bad = 0; for (int s = 0; s < n; s++) for (size_t j = 0; j < rc[s] / 8; j++) if (hr[rd[s] / 8 + j] != wv(g0 + s, pe, j)) bad++;
    CHK(bad == 0, "%s: alltoallv %zu words wrong (of %zu)", tag, bad, tr / 8);
    /* equal slabs: 64 words per pair */
    size_t sl = 64; uint64_t *es = (uint64_t *)calloc(sl * n, 8), *er = (uint64_t *)calloc(sl * n, 8), *des, *der;
    for (int t = 0; t < n; t++) for (size_t j = 0; j < sl; j++) es[t * sl + j] = wv(pe, g0 + t, j);
    HIPC(hipMalloc(&des, sl * n * 8)); HIPC(hipMalloc(&der, sl * n * 8)); HIPC(hipMemcpy(des, es, sl * n * 8, hipMemcpyHostToDevice));
    comm_alltoall(c, des, der, sl * 8, st); comm_wait(c);
    HIPC(hipMemcpy(er, der, sl * n * 8, hipMemcpyDeviceToHost));
    bad = 0; for (int s = 0; s < n; s++) for (size_t j = 0; j < sl; j++) if (er[s * sl + j] != wv(g0 + s, pe, j)) bad++;
    CHK(bad == 0, "%s: alltoall %zu words wrong", tag, bad);
    {   /* a large alltoallv (1000 to 9900 words per pair, zeros excluded): several chunks per pair when the engine runs in rounds (XSHMEM_ROUND_MB=0.01 -> 4 KiB chunks) */
        uint64_t *bs_, *br_; size_t *bc = (size_t *)calloc((size_t)n * 4, sizeof(size_t)), *bd = bc + n, *rc2 = bc + 2 * n, *rd2 = bc + 3 * n;
        for (int t = 0; t < n; t++) { bc[t] = 8 * (1000 + 40 * cntw(pe, g0 + t)); rc2[t] = 8 * (1000 + 40 * cntw(g0 + t, pe)); }
        size_t bts = comm_prefix(bc, bd, n), btr = comm_prefix(rc2, rd2, n);
        uint64_t *h1 = (uint64_t *)calloc(bts / 8 + 1, 8), *h2 = (uint64_t *)calloc(btr / 8 + 1, 8);
        for (int t = 0; t < n; t++) for (size_t j = 0; j < bc[t] / 8; j++) h1[bd[t] / 8 + j] = wv(pe, g0 + t, j);
        HIPC(hipMalloc(&bs_, bts + 8)); HIPC(hipMalloc(&br_, btr + 8)); HIPC(hipMemcpy(bs_, h1, bts, hipMemcpyHostToDevice));
        comm_alltoallv(c, bs_, bc, bd, br_, rc2, rd2, st); comm_wait(c);
        HIPC(hipMemcpy(h2, br_, btr, hipMemcpyDeviceToHost));
        size_t bad2 = 0; for (int s_ = 0; s_ < n; s_++) for (size_t j = 0; j < rc2[s_] / 8; j++) if (h2[rd2[s_] / 8 + j] != wv(g0 + s_, pe, j)) bad2++;
        CHK(bad2 == 0, "%s: large alltoallv %zu words wrong (of %zu)", tag, bad2, btr / 8);
        HIPC(hipFree(bs_)); HIPC(hipFree(br_)); free(bc); free(h1); free(h2);
    }
    comm_barrier(c);
    HIPC(hipFree(ds)); HIPC(hipFree(dr)); HIPC(hipFree(des)); HIPC(hipFree(der)); HIPC(hipStreamDestroy(st));
    free(sc); free(hs); free(hr); free(es); free(er);
}
static int g_mode_probe, g_mode_all, g_mode_coll, g_mode_hs;
static uint64_t g_rng;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }   /* xorshift64: the same stream on every member of a communicator */
static uint64_t cv(int pe, int it) { uint64_t x = ((uint64_t)pe + 1) * 0x9E3779B97F4A7C15ull ^ ((uint64_t)it * 0xD1B54A32D192ED03ull + 7); x ^= x >> 29; return x >> 3; }
/* random back-to-back small collectives on c (rank 0 = PE g0), identical op sequence on all members (seeded by g0, n) */
static void coll_check(comm *c, int g0, const char *tag, int iters)
{
    int n = comm_size(c), pe = vpe_pe(); g_rng = 0x1234567ull + (uint64_t)g0 * 977 + (uint64_t)n * 31;
    char *sb = (char *)malloc(4096), *rb = (char *)malloc((size_t)n * 4096);
    for (int it = 0; it < iters; it++) {
        int op = (int)(rnd() % 3); size_t bytes = 8 * (1 + rnd() % 512); if (rnd() % 8 == 0) bytes = 4096; if (rnd() % 8 == 0) bytes = 8;
        if (op == 0) { if (pe == g0 + (int)(rnd() % (uint64_t)n)) usleep(50); comm_barrier(c); }   /* a laggard member: the others must wait for it */
        else if (op == 1) {
            size_t want = 0; for (int i = 0; i < n; i++) if (cv(g0 + i, it) > want) want = (size_t)cv(g0 + i, it);
            size_t got = comm_allreduce_max(c, (size_t)cv(pe, it)); CHK(got == want, "%s it %d: max %zu want %zu", tag, it, got, want);
        } else {
            for (size_t j = 0; j < bytes; j++) sb[j] = (char)(cv(pe, it) >> (j % 7));
            memset(rb, 0xEE, (size_t)n * bytes); comm_allgather_host(c, sb, rb, bytes);
            size_t bad = 0; for (int i = 0; i < n; i++) for (size_t j = 0; j < bytes; j++) if (rb[(size_t)i * bytes + j] != (char)(cv(g0 + i, it) >> (j % 7))) bad++;
            CHK(bad == 0, "%s it %d: allgather %zu bytes (of %zu) wrong, block %zu B", tag, it, bad, (size_t)n * bytes, bytes);
        }
    }
    comm_barrier(c); free(sb); free(rb);
}
static double tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static void coll_body(void *unused)
{
    (void)unused;
    int npe = vpe_npe(), pe = vpe_pe(); const char *em = getenv("XSHMEM_COLL"); if (!em || !*em) em = "flat";
    int iters = getenv("XT_COLL_ITERS") ? atoi(getenv("XT_COLL_ITERS")) : 200, reps = getenv("XT_COLL_REPS") ? atoi(getenv("XT_COLL_REPS")) : 500;
    comm *flat = vpe_comm(); CHK(flat != 0, "no flat communicator"); if (!flat) return;
    coll_check(flat, 0, "flat comm", iters);
    static const int sizes[] = { 2, 3, 5, 6, 7 };
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        int g = sizes[i]; if (g > npe) continue;
        int g0 = pe / g * g, gg = g < npe - g0 ? g : npe - g0; comm *c = vpe_sub(g0, gg); CHK(c != 0, "no run [%d,%d)", g0, g0 + gg); if (!c) continue;
        char tag[64]; snprintf(tag, sizeof tag, "run [%d,%d)", g0, g0 + gg); coll_check(c, g0, tag, iters);
    }
    comm_barrier(flat);
    uint64_t blk[2] = { (uint64_t)pe, 1 }, *all = (uint64_t *)calloc((size_t)npe * 2, 8); double t0, us[3];
    for (int k = 0; k < 3; k++) {
        comm_barrier(flat); t0 = tnow();
        for (int i = 0; i < reps; i++) { if (k == 0) comm_barrier(flat); else if (k == 1) (void)comm_allreduce_max(flat, (size_t)i + (size_t)pe); else comm_allgather_host(flat, blk, all, 16); }
        us[k] = 1e6 * (tnow() - t0) / (reps > 0 ? reps : 1);
    }
    free(all);
    if (pe == 0) printf("T_VPE_COLL_TIME XSHMEM_COLL=%s n %d reps %d: barrier %.1f us, allreduce_max %.1f us, allgather_host 16 B %.1f us (per call, PE 0)\n", em, npe, reps, us[0], us[1], us[2]);
}
/* ---- hs (handshake stress): first calls on many new communicators, and per-PE signal bursts ----
 * Part 1: for every communicator size g in 2..8 (aligned powers of two on XT_HS_SLOTS slots (default 12, at most 13), 3 5 6 7 on 4 slots: at most 52 communicators per PE, the limit is 64) the first
 * allgather_host of a NEW communicator (all members enter together after a flat barrier; the first call = the handshake of comm_xshmem.c, flat or tree by XSHMEM_COLL), then XT_HS_REPS (20)
 * steady calls; every result is checked.  PE 0 prints per size the number of communicators, the first-call time (mean and max) and the steady time.
 * Part 2: a burst of N 8-byte signals from every PE to the other PEs in rotated order (comm_xshmem_burst), N in 64 256 1024 2303 4608 (up to XT_HS_BURST_MAX): one unpaced burst and one paced
 * (XT_HS_BATCH signals per quiet, 64): max issue time and final quiet time over the PEs.  One process per PE, at most 8 PEs on one node (vpe.h VPE_MAX): MNRUN_PPG_PPN=8 XSHMEM_PES_PER_GPU=2. */
static void hs_body(void *unused)
{
    (void)unused;
    int npe = vpe_npe(), pe = vpe_pe(); const char *em = getenv("XSHMEM_COLL"); if (!em || !*em) em = "flat";
    int reps = getenv("XT_HS_REPS") ? atoi(getenv("XT_HS_REPS")) : 20, slots = getenv("XT_HS_SLOTS") ? atoi(getenv("XT_HS_SLOTS")) : 12;
    int bmax = getenv("XT_HS_BURST_MAX") ? atoi(getenv("XT_HS_BURST_MAX")) : 4608, hb = getenv("XT_HS_BATCH") ? atoi(getenv("XT_HS_BATCH")) : 64;
    if (slots > 13) slots = 13;   /* (slot 14: the skew case below, 15: the node group) */
    if (slots < 1) slots = 1;
    comm *flat = vpe_comm(); CHK(flat != 0, "no flat communicator"); if (!flat) return;
    double first_sum[9] = { 0 }, first_max[9] = { 0 }, steady_sum[9] = { 0 }; int ncomm[9] = { 0 };
    for (int pass = 0; pass < 2; pass++)                       /* pass 0: aligned powers of two, pass 1: the others */
        for (int g = 2; g <= 8 && g <= npe; g++) {
            int pow2 = (g & (g - 1)) == 0; if (pow2 != (pass == 0)) continue;
            int ns = pow2 ? slots : 4;
            for (int slot = 0; slot < ns; slot++) {
                int g0 = pe / g * g, gg = g < npe - g0 ? g : npe - g0;
                if (g0 == 0 && gg == npe && slot == 0) continue;   /* (that run is the flat communicator) */
                comm_barrier(flat);
                if (gg < g) continue;   /* a short last run would share an id with the runs of its own size (after the barrier: every PE entered it) */
                comm *c = vpe_sub_slot(g0, gg, slot);
                CHK(c != 0, "no communicator [%d,%d) slot %d", g0, g0 + gg, slot); if (!c) continue;
                int n = comm_size(c), me = comm_rank(c); uint64_t mine[2] = { (uint64_t)pe * 3 + 1, (uint64_t)slot }, all[16];
                double t1 = tnow(); comm_allgather_host(c, mine, all, 16); double t2 = tnow();
                for (int i = 0; i < n; i++) CHK(all[2 * i] == (uint64_t)(g0 + i) * 3 + 1 && all[2 * i + 1] == (uint64_t)slot, "hs first call: g %d slot %d member %d of %d (me %d)", g, slot, i, n, me);
                for (int r = 0; r < reps; r++) {
                    mine[1] = (uint64_t)r + 100; comm_allgather_host(c, mine, all, 16);
                    for (int i = 0; i < n; i++) CHK(all[2 * i + 1] == (uint64_t)r + 100, "hs steady call %d: g %d slot %d member %d", r, g, slot, i);
                }
                double t3 = tnow(), fc = t2 - t1;
                first_sum[g] += fc; if (fc > first_max[g]) first_max[g] = fc; steady_sum[g] += (t3 - t2) / (reps > 0 ? reps : 1); ncomm[g]++;
            }
        }
    comm_barrier(flat);
    for (int g = 2; g <= 8; g++) {
        size_t cnt = comm_allreduce_max(flat, (size_t)ncomm[g]); if (!cnt) continue;
        size_t mx = comm_allreduce_max(flat, (size_t)(first_max[g] * 1e6)), sm = comm_allreduce_max(flat, ncomm[g] ? (size_t)(first_sum[g] / ncomm[g] * 1e6) : 0), st = comm_allreduce_max(flat, ncomm[g] ? (size_t)(steady_sum[g] / ncomm[g] * 1e6) : 0);
        if (pe == 0) printf("T_VPE_HS_FIRST XSHMEM_COLL=%s npe %d size %d: %d communicators, first call mean (worst PE) %zu us, max %zu us; steady call mean (worst PE) %zu us\n", em, npe, g, ncomm[g], sm, mx, st);
    }
    if (npe >= 2) {   /* COMM_SEQ_CHECK: a legitimate barrier + allreduce_max, then a skewed FIRST allgather on a fresh communicator must pass (no false divergence) */
        comm *c = (npe - pe / 2 * 2 >= 2) ? vpe_sub_slot(pe / 2 * 2, 2, 14) : 0;
        if (c && comm_size(c) == 2) {
            comm_barrier(c); (void)comm_allreduce_max(c, 3);
            if (comm_rank(c) == 1) usleep(300000);
            uint64_t m2[2] = { (uint64_t)pe, 7 }, a2[4]; comm_allgather_host(c, m2, a2, 16);
            CHK(a2[0] == (uint64_t)(pe / 2 * 2) && a2[2] == (uint64_t)(pe / 2 * 2 + 1), "skewed first allgather after a barrier");
        }
        comm_barrier(flat);
        if (pe == 0) printf("T_VPE_HS_SKEW: barrier, allreduce_max, 0.3 s skew, first allgather on a pair: ok\n");
    }
    if (!comm_xshmem_burst) { if (pe == 0) printf("T_VPE_HS_BURST: skipped (no comm_xshmem_burst: not the xshmem engine)\n"); return; }
    static const int ns_[] = { 64, 256, 1024, 2303, 4608 };
    for (unsigned i = 0; i < sizeof ns_ / sizeof *ns_; i++) for (int paced = 0; paced < 2; paced++) {
        int N = ns_[i]; if (N > bmax) continue;
        comm_barrier(flat); double ti = 0, tq = 0; int rc = comm_xshmem_burst(N, paced ? hb : 0, &ti, &tq);
        CHK(rc == 0, "burst of %d: rc %d", N, rc);
        comm_barrier(flat);
        size_t mi = comm_allreduce_max(flat, (size_t)(ti * 1e6)), mq = comm_allreduce_max(flat, (size_t)(tq * 1e6));
        if (pe == 0) printf("T_VPE_HS_BURST XSHMEM_COLL=%s npe %d: %d signals per PE, %s: issue %.1f ms, final quiet %.1f ms (max over the PEs, %.2f us per signal issued)\n", em, npe, N, paced ? "paced (quiet per XT_HS_BATCH)" : "one burst", mi / 1e3, mq / 1e3, (double)mi / N);
    }
}
/* seqdiv / seqdiv2 (COMM_SEQ_CHECK=1): PEs 0 and 1 call different collectives on their run [0, 2): seqdiv = barrier vs allreduce_max, seqdiv2 = allgather_host (first call) vs barrier.
 * The step must end nonzero with "COMM_SEQ_CHECK" fatal text from the diverging PE(s); no "passed the diverging collective" line may appear. */
static int g_seq2;
static void seqdiv_body(void *unused)
{
    (void)unused;
    comm *flat = vpe_comm(); comm_barrier(flat);
    comm *c = vpe_sub(0, 2); if (!c) { usleep(3000000); return; }
    uint64_t a[2] = { 1, 2 }, b[4];
    if (vpe_pe() == 0) { if (g_seq2) comm_allgather_host(c, a, b, 16); else comm_barrier(c); }
    else { if (g_seq2) comm_barrier(c); else (void)comm_allreduce_max(c, 5); }
    printf("t_vpe seqdiv: pe %d passed the diverging collective (COMM_SEQ_CHECK=1 must prevent this)\n", vpe_pe());
}

/* the partition of the PEs into runs of g (the last one cut): this PE's run, exercised */
static void partition(int g, const char *what)
{
    int npe = vpe_npe(), pe = vpe_pe(), g0 = pe / g * g, gg = g < npe - g0 ? g : npe - g0;
    comm *c = vpe_sub(g0, gg);
    CHK(c != 0, "%s: no communicator for [%d, %d)", what, g0, g0 + gg);
    if (!c) return;
    char tag[64]; snprintf(tag, sizeof tag, "%s [%d,%d)", what, g0, g0 + gg);
    exercise(c, g0, tag);
    if (vpe_node_leader() && g0 == 0) printf("t_vpe: partition by %d (%s): this PE's run [%d, %d) exercised\n", g, what, g0, g0 + gg);
}
static void comm_body(void *unused)
{
    (void)unused;
    int npe = vpe_npe(), L = vpe_lanes();
    comm *flat = vpe_comm();
    CHK(flat && comm_rank(flat) == vpe_pe() && comm_size(flat) == npe, "flat comm rank %d size %d", flat ? comm_rank(flat) : -1, flat ? comm_size(flat) : -1);
    if (!flat) return;
    if (g_mode_all) { exercise(flat, 0, "flat"); if (vpe_root()) printf("t_vpe: flat communicator over %d PEs exercised\n", npe); }
    /* the sub-communicator probe: 2 and 4 PEs of one node (aligned), 3, 6, 12 contiguous runs */
    static const int sizes[] = { 2, 4, 3, 6, 8, 12 };
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        int g = sizes[i]; if (g > npe) continue;
        if (g <= L && (L % g) == 0) partition(g, "node subgroup"); else partition(g, "run");
    }
    /* (the forked duplicate-id check was removed: in the multi-threaded, libfabric-initialised process the forked child did not reach the
       "used twice" fatal (9q8: rc 0, no text); the fatal is the atomic X.used check in comm_xshmem.c, covered by reading) */
    /* the tree's group schedule for this PE count (mn_groups_parse in PE units), every level's group built and exercised */
    if (g_mode_all) {
        int gs[32]; int nl = mn_groups_parse(npe, gs, 31);
        static const int e4[] = { 2, 4 }, e8[] = { 2, 4, 8 }, e12[] = { 2, 4, 12 };
        const int *want = npe == 4 ? e4 : npe == 8 ? e8 : npe == 12 ? e12 : 0; int nw = npe == 4 ? 2 : 3;
        if (want) { CHK(nl == nw, "schedule levels %d want %d", nl, nw); for (int l = 0; l < nl && l < nw; l++) CHK(gs[l] == want[l], "level %d: %d want %d", l + 1, gs[l], want[l]); }
        for (int l = 0; l < nl; l++) {
            int Gl = gs[l], pe = vpe_pe(), g0 = pe / Gl * Gl, gg = Gl < npe - g0 ? Gl : npe - g0;
            comm *c = vpe_sub(g0, gg); CHK(c != 0, "level %d group [%d,%d)", l + 1, g0, g0 + gg);
            if (c) { char tag[48]; snprintf(tag, sizeof tag, "schedule level %d", l + 1); exercise(c, g0, tag); }
        }
        if (vpe_root()) { printf("t_vpe: group schedule for %d PEs:", npe); for (int l = 0; l < nl; l++) printf(" %d", gs[l]); printf("\n"); }
    }
}

/* ---- process mode (S2a) ---- */
static const char *cpustr(const cpu_set_t *m, char *b, size_t n)       /* "0-47,96-143" */
{
    size_t k = 0; b[0] = 0;
    for (int c = 0; c < CPU_SETSIZE && k + 16 < n; ) {
        if (!CPU_ISSET(c, m)) { c++; continue; }
        int e = c; while (e + 1 < CPU_SETSIZE && CPU_ISSET(e + 1, m)) e++;
        k += (size_t)snprintf(b + k, n - k, "%s%d-%d", k ? "," : "", c, e); c = e + 1;
    }
    return b;
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }
static void ppg_ident(void *unused)
{
    (void)unused;
    const char *ll = getenv("SLURM_LOCALID"), *np = getenv("SLURM_NTASKS_PER_NODE");
    int L = vpe_lanes(), dev = -1; HIPC(hipGetDevice(&dev));
    CHK(vpe_ppg() && vpe_in_run(), "ppg %d in_run %d", vpe_ppg(), vpe_in_run());
    CHK(!ll || vpe_idx() == atoi(ll), "vpe_idx %d, SLURM_LOCALID %s", vpe_idx(), ll ? ll : "-");
    CHK(!np || L == atoi(np), "lanes %d, SLURM_NTASKS_PER_NODE %s", L, np ? np : "-");
    CHK(vpe_lane() == vpe_idx() && vpe_pe() == L * vpe_node() + vpe_idx() && vpe_npe() == L * vpe_nnodes(), "identity pe %d npe %d", vpe_pe(), vpe_npe());
    CHK(dev == vpe_device(), "current device %d, vpe_device %d", dev, vpe_device());
    CHK(vpe_node_leader() == (vpe_idx() == 0) && vpe_proc_leader() && vpe_root() == (vpe_pe() == 0), "leaders");
    char want[64]; snprintf(want, sizeof want, "pe %d (node %d lane %d): ", vpe_pe(), vpe_node(), vpe_idx());
    CHK(!strcmp(ec_log_prefix(), want), "prefix '%s' want '%s'", ec_log_prefix(), want);
    CHK(vpe_comm() && comm_rank(vpe_comm()) == vpe_pe(), "flat comm rank");
    /* the node barrier: local 0 arrives 0.4 s late, every PE of the node leaves after that */
    double t0 = now_s(); if (vpe_idx() == 0) usleep(400000);
    vpe_node_barrier();
    double dt = now_s() - t0; CHK(dt > 0.35, "node barrier returned after %.3f s (local 0 arrives at 0.4 s)", dt);
    /* repeated barriers: every PE passes 50, and the node group is a communicator of exactly this node's PEs */
    for (int i = 0; i < 50; i++) vpe_node_barrier();
    int g = L, al = (g & (g - 1)) == 0; comm *nc = vpe_sub_slot(g * vpe_node(), g, al ? VPE_NODE_SLOT : 3);
    CHK(nc && comm_size(nc) == g && comm_rank(nc) == vpe_idx(), "node group size %d rank %d", nc ? comm_size(nc) : -1, nc ? comm_rank(nc) : -1);
}
static void ppg_node_group(void *unused)
{
    (void)unused;
    int g = vpe_lanes(), al = (g & (g - 1)) == 0; comm *nc = vpe_sub_slot(g * vpe_node(), g, al ? VPE_NODE_SLOT : 3);
    if (nc) exercise(nc, g * vpe_node(), "node group");
}
static void kfd_vram(char *b, size_t n)      /* the process's VRAM per GPU (kfd sysfs; empty when unreadable) */
{
    char dir[96]; snprintf(dir, sizeof dir, "/sys/class/kfd/kfd/proc/%d", (int)getpid()); b[0] = 0; size_t k = 0;
    DIR *d = opendir(dir); if (!d) { snprintf(b, n, "(no kfd proc dir)"); return; }
    struct dirent *e;
    while ((e = readdir(d))) if (!strncmp(e->d_name, "vram_", 5)) {
        char f[160]; snprintf(f, sizeof f, "%s/%s", dir, e->d_name); FILE *fp = fopen(f, "r"); unsigned long long v = 0;
        if (fp) { if (fscanf(fp, "%llu", &v) != 1) v = 0; fclose(fp); }
        if (k + 40 < n) k += (size_t)snprintf(b + k, n - k, "%s%s=%lluMiB", k ? " " : "", e->d_name + 5, v >> 20);
    }
    closedir(d);
}
static long kb_of(const char *file, const char *key)    /* "VmRSS:   123 kB" -> 123 */
{
    FILE *f = fopen(file, "r"); char l[256]; long v = -1; size_t n = strlen(key);
    if (f) { while (fgets(l, sizeof l, f)) if (!strncmp(l, key, n)) { v = atol(l + n); break; } fclose(f); }
    return v;
}
static char g_launch_mask[256];
static void ppg_info(void *unused)
{
    (void)unused;
    int nd = 0, dev = -1; HIPC(hipGetDeviceCount(&nd)); HIPC(hipGetDevice(&dev));
    void *p; HIPC(hipMalloc(&p, (size_t)64 << 20)); HIPC(hipMemset(p, 1, (size_t)64 << 20)); HIPC(hipDeviceSynchronize());   /* make the context real */
    cpu_set_t m; char cb[256], kv[512], host[64] = ""; gethostname(host, sizeof host - 1); sched_getaffinity(0, sizeof m, &m); cpustr(&m, cb, sizeof cb); kfd_vram(kv, sizeof kv);
    const char *hv = getenv("HIP_VISIBLE_DEVICES"), *rv = getenv("ROCR_VISIBLE_DEVICES");
    printf("PPG_INFO pe %d npe %d node %d local %d procid %s host %s | visible %d (HIP_VISIBLE %s ROCR_VISIBLE %s) device %d numa(device) %d | cpus launch '%s' now '%s' (%d) | kfd vram: %s | rss %ld MiB hwm %ld MiB threads %ld memavail %ld GiB\n",
           vpe_pe(), vpe_npe(), vpe_node(), vpe_idx(), getenv("SLURM_PROCID") ? getenv("SLURM_PROCID") : "-", host, nd, hv ? hv : "-", rv ? rv : "-", dev,
           mem_numa_node_of_device(dev), g_launch_mask, cb, CPU_COUNT(&m), kv,
           kb_of("/proc/self/status", "VmRSS:") >> 10, kb_of("/proc/self/status", "VmHWM:") >> 10, kb_of("/proc/self/status", "Threads:"), kb_of("/proc/meminfo", "MemAvailable:") >> 20);
    HIPC(hipFree(p));
}
/* die: PE 2 (the last when fewer) exits 7; the others sit in a barrier on the flat communicator (they hang unless the launcher ends the step) */
static void die_body(void *unused)
{
    (void)unused;
    comm *f = vpe_comm(); comm_barrier(f);               /* all are up */
    int victim = vpe_npe() > 2 ? 2 : vpe_npe() - 1;
    if (vpe_pe() == victim) { printf("t_vpe die: pe %d exits 7 now\n", victim); fflush(stdout); _exit(7); }
    usleep(200000); comm_barrier(f);                     /* never completes */
    printf("t_vpe die: pe %d passed the barrier that must not complete\n", vpe_pe());
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 && strncmp(argv[1], "--", 2) ? argv[1] : "all";
    g_ppg = 1;                                           /* process mode only (the thread mode was removed); --ppg is accepted */
    g_mode_probe = !strcmp(mode, "probe"); g_mode_all = !strcmp(mode, "all"); g_mode_coll = !strcmp(mode, "coll"); g_mode_hs = !strcmp(mode, "hs"); g_seq2 = !strcmp(mode, "seqdiv2");
    if (g_ppg) {                                         /* S2a: this process is ONE PE (srun, SLURM_LOCALID); without srun (host on a login node): local 0 of 4 */
        cpu_set_t m; sched_getaffinity(0, sizeof m, &m); cpustr(&m, g_launch_mask, sizeof g_launch_mask);
        setenv("ECALC_PE_PER_GPU", "1", 1);
        if (!strcmp(mode, "badlayout") && getenv("SLURM_LOCALID") && atoi(getenv("SLURM_LOCALID")) == 1) setenv("SLURM_LOCALID", "2", 1);
        if (!getenv("SLURM_LOCALID")) { setenv("ECALC_VPE_LANES", "4", 0); vpe_set_ppg(0); } else vpe_ppg_init();
    }
    if (!g_mode_all && !g_mode_probe && !g_mode_coll && !g_mode_hs && strcmp(mode, "seqdiv") && strcmp(mode, "seqdiv2") && strcmp(mode, "host") && strcmp(mode, "info") && strcmp(mode, "die") && strcmp(mode, "badlayout")) { printf("usage: t_vpe [all|probe|coll|hs|host] [--ppg] | t_vpe info|die|badlayout|seqdiv|seqdiv2\n"); return 2; }
    vpe_set_forced(1);
    host_part();
    if (!strcmp(mode, "host")) { printf("\nt_vpe host: %s (%d checks)\n", g_fail ? "FAILED" : "OK", g_checks); return g_fail != 0; }
    const char *tr = getenv("COMM_TRANSPORT");
    g_skip_comm = !tr || strcmp(tr, "xshmem");
    int npe = 0;
    if (g_skip_comm) printf("t_vpe: SKIP the communicator parts (COMM_TRANSPORT=xshmem, XSHMEM_LOOPBACK_PES=1 and make XSHMEM=ofi needed)\n");
    else {
        npe = mn_init();
        printf("t_vpe: node %d of %d: mn_init -> %d PEs (one process per PE)\n", vpe_node(), vpe_nnodes(), npe);
        CHK(npe == vpe_npe() && mn_size() == npe, "npe %d vs %d", npe, vpe_npe());
        if (g_ppg && !strcmp(mode, "info")) vpe_run(ppg_info, 0);
        else if (g_ppg && !strcmp(mode, "die")) vpe_run(die_body, 0);
        else if (g_ppg && !strcmp(mode, "badlayout")) { vpe_run(die_body, 0); }   /* (mn_init fataled on the PE with the wrong id; the others wait here until the step is killed) */
        else {
            if (g_mode_coll) vpe_run(coll_body, 0);
            else if (g_mode_hs) vpe_run(hs_body, 0);
            else if (!strcmp(mode, "seqdiv") || g_seq2) vpe_run(seqdiv_body, 0);
            else {
            if (g_ppg) { vpe_run(ppg_ident, 0); vpe_run(ppg_node_group, 0); }
            vpe_run(comm_body, 0); }
        }
        mn_finalize();
    }
    const char *tag = g_mode_probe ? "T_VPE_PROBE" : "T_VPE_RESULT";
    if (g_ppg) printf("%s %s pe %d node %d of %d npe %d checks %d failures %d%s\n", tag, g_fail ? "FAIL" : "PASS", vpe_pe(), vpe_node(), vpe_nnodes(), npe, g_checks, g_fail, g_skip_comm ? " (comm parts skipped)" : "");
    else printf("%s %s node %d of %d npe %d checks %d failures %d%s\n", tag, g_fail ? "FAIL" : "PASS", vpe_node(), vpe_nnodes(), npe, g_checks, g_fail, g_skip_comm ? " (comm parts skipped)" : "");
    return g_fail != 0;
}

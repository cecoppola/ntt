/* ecalc - e to d decimal digits on one MI300A node (decimal limbs of 10^18; the former binary-limb pipeline with the
 * radix conversion todec is archived: archive:PLAN §28.5, archive:binary_pipeline/, git tag pre-28.5-binary).
 *
 *   bs    P, Q = binary splitting of sum 1/k!, N = min{m : lgamma(m+1)/ln10 >= d+50}
 *   10dP  A = 10^d (P + Q): a limb shift and one small multiply
 *   dm    X, R = divmod(A, Q) by Newton reciprocal + Barrett
 *   T1    residue identities mod eight 62-bit primes
 *   out   the digits are the limbs of X: "2" followed by d fractional digits, streamed to the file
 *   T2    digit windows vs known values; digits == X mod q
 *
 * Usage: ecalc <digits> [outfile]      env: POOL_LOG (31), RNS_CRT_LAYOUT (0), ECALC_VERBOSE (1)
 * Prints META/RESULT lines (phase seconds, VmHWM) as the benchmarks do.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                                    /* CPU_COUNT (the per-process background team, ECALC_PE_PER_GPU) */
#endif
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "bigint.h"
#include "rns_mul.h"
#include "newton.h"
#include "binsplit.h"
#include "verify.h"
#include "mem.h"
#include "dbig.h"
#include "mn.h"
#include "mn_out.h"
#include "mn_plan.h"                                 /* MN_PLAN_ONLY */
#include "memsample.h"                               /* E1, E12 */
#include "spill.h"                                   /* E3 (the output file under ECALC_ODIRECT) */
#include "fatal.h"                                   /* ec_quit, EC_RC_BUDGET */
#include "omp_team.h"                              /* A1: ECALC_OMP_TEAM */
#include "vpe.h"                                   /* PE identity (ECALC_PE_PER_GPU=1, off by default) */
#include "segv_trace.h"                               /* s18w task 2: ECALC_SEGV_TRACE=1 (default 0) */
extern "C" size_t comm_layered_vbytes(int dev);      /* B7ACCT (comm_layered.c) */
#include <pthread.h>
#include <sys/stat.h>                                /* the recheck's .top directory test in process mode */
#include <semaphore.h>
#include <omp.h>
#include "ntt.h"
#include "config_line.h"   /* L5: the print-only `config:` line */
#include <math.h>

static void meta_line(const char *bench)
{
    char host[128] = "?", date[64]; time_t t = time(0);
    gethostname(host, sizeof host); strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", localtime(&t));
    printf("META bench=%s host=%s date=%s pool_log=%d stg=%d pw_fuse=%d body=%d base=%s\n", bench, host, date, rns_pool_log(), ntt_stg, ntt_pw_fuse, 1, "10^18");
}
#define RESULT(name, unit, v) printf("RESULT ecalc %s %s %.6g\n", name, unit, (double)(v))

/* ---- overlap of disjoint work (RESULTS.md 68: 128.8 vs 112.1 s against sequential).  Background CPU work runs in pthreads with a bounded OpenMP team while the GPUs run the tiers; each
 * joins where its result is first needed. ---- */
static int g_bg_threads = 48;
struct pq_bg { unsigned long N, a0, b1; uint64_t p[T1_NQ], qq[T1_NQ]; pthread_t th; int started, joined; double t; };   /* [a0, b1): this node's term range (M5: the recurrence is rank-local; size 1: [1, N+1)) */
static void *pq_bg_run(void *a) { struct pq_bg *b = (struct pq_bg *)a; double t0 = mem_now(); ec_team_set(g_bg_threads);
    for (int i = 0; i < T1_NQ; i++) vf_pq_range_mod(b->a0, b->b1, t1_q[i], &b->p[i], &b->qq[i]); b->t = mem_now() - t0;
    return 0; }
static void pq_bg_start(void *a) { struct pq_bg *b = (struct pq_bg *)a; if (b->started) return; pthread_create(&b->th, 0, pq_bg_run, b); b->started = 1; }   /* O2: after the seeds */

/* X's residues and the digits -- formatted in chunks and written as they are formatted
 * (mn_out.c), no whole digit string on the host -- in the background during the low product, from the hook that
 * gets X on the host before it */
struct x_bg { bigint *X; dbig *Xd; unsigned long d, d_out; const char *outfile; uint64_t Xres[T1_NQ]; int verbose; pthread_t th; int started; double t_res; mn_out o; sem_t res_ready; size_t t2_defer; };   /* res_ready: X's residues are in (T1 needs only those; the writer runs on).  Xd: X on the device -- the writer and the residues read it there; no host X */
static mn_out_src x_bg_src(const struct x_bg *b) { mn_out_src s; memset(&s, 0, sizeof s); s.dev = b->Xd; s.cnt = b->Xd->n; return s; }
static void x_bg_writer(struct x_bg *b)                /* the chunked writer over X (host or device; also the redo after a correction) */
{
    mn_out_src src = x_bg_src(b);
    memset(&b->o, 0, sizeof b->o); b->o.d = b->d; b->o.d_out = b->d_out; b->o.outfile = b->outfile; b->o.rank = 0; b->o.size = 1; b->o.verbose = b->verbose; b->o.t2_defer = b->t2_defer;
    mn_out_run(&b->o, &src);
}
static void *x_bg_run(void *a)
{
    struct x_bg *b = (struct x_bg *)a; ec_team_set(g_bg_threads);
    double t0 = mem_now();
    { mn_out_src src = x_bg_src(b); mn_out_res_share(&src, b->Xres); }   /* the device kernel on the device X (B1), else the host Horner */
    b->t_res = mem_now() - t0; sem_post(&b->res_ready);
    x_bg_writer(b);
    return 0;
}
static void x_bg_hook(bigint *X, void *a) { struct x_bg *b = (struct x_bg *)a; b->X = X; sem_init(&b->res_ready, 0, 0); pthread_create(&b->th, 0, x_bg_run, b); b->started = 1; }
/* ---- the writer on X_hi after DKM's step 1.  The division forms no X0: X_hi (final after step 1:
 * X's limbs >= s) comes to the X_hi hook, which starts the size-1 writer on it -- one file in two ranges (mn_out's split): the limbs >= s from
 * X_hi, then, at the gate, the limbs < s from X_lo (the division's device X, newton_db_x_dev).  The X_lo hook opens the gate before step 2's low
 * product unless X_lo0 >= B^s (or MN_OUT_DKM_HI_TEST_CARRY=1): then the division adds the corrections to X_lo in place and main opens it after
 * the division.  At size > 1 the same with two part files per rank (mn_out_early_hi_start / mn_out_early_lo_release) ---- */
static int dkm_hi_test_carry(void) { const char *e = getenv("MN_OUT_DKM_HI_TEST_CARRY"); return e && atoi(e) != 0; }
struct x_hi { int on, released; size_t s; dbig Xh; mn_out_src src_lo, src_hi; sem_t go; uint64_t res_hi[T1_NQ]; double t_gate, t_hi; };
static struct x_hi g_xhi;
static void x_bg_gate(void *a, mn_out *o)             /* the writer at s: X_hi's range is formatted and handed off; X_lo's after the release */
{
    struct x_bg *b = (struct x_bg *)a; (void)o;
    double t0 = mem_now(); g_xhi.t_hi = t0 - g_xhi.t_hi; sem_wait(&g_xhi.go); g_xhi.t_gate = mem_now() - t0;
    t0 = mem_now();
    uint64_t rl[T1_NQ]; mn_out_res_share(&g_xhi.src_lo, rl);   /* X mod q = X_hi B^s + X_lo */
    for (int i = 0; i < T1_NQ; i++) b->Xres[i] = vf_add_mod(vf_shift_res(g_xhi.res_hi[i], g_xhi.s, t1_q[i]), rl[i], t1_q[i]);
    b->t_res += mem_now() - t0; sem_post(&b->res_ready);
}
static void *x_bg_run_hi(void *a)
{
    struct x_bg *b = (struct x_bg *)a; ec_team_set(g_bg_threads);
    double t0 = mem_now(); g_xhi.t_hi = t0;
    mn_out_res_share(&g_xhi.src_hi, g_xhi.res_hi);
    b->t_res = mem_now() - t0;
    memset(&b->o, 0, sizeof b->o); b->o.d = b->d; b->o.d_out = b->d_out; b->o.outfile = b->outfile; b->o.rank = 0; b->o.size = 1; b->o.verbose = b->verbose; b->o.t2_defer = b->t2_defer;
    b->o.split = g_xhi.s; b->o.src_hi = &g_xhi.src_hi; b->o.at_split = x_bg_gate; b->o.split_arg = b;
    mn_out_run(&b->o, &g_xhi.src_lo);
    return 0;
}
static int x_hi_hook(dbig *Xh, size_t s, void *a)    /* newton_db_xhi_hook: take X_hi, start the writer on it */
{
    struct x_bg *b = (struct x_bg *)a;
    g_xhi.on = 1; g_xhi.released = 0; g_xhi.s = s; g_xhi.Xh = *Xh; db_init(Xh); sem_init(&g_xhi.go, 0, 0);
    memset(&g_xhi.src_hi, 0, sizeof g_xhi.src_hi); g_xhi.src_hi.dev = &g_xhi.Xh; g_xhi.src_hi.lo = s; g_xhi.src_hi.cnt = g_xhi.Xh.n;
    memset(&g_xhi.src_lo, 0, sizeof g_xhi.src_lo); g_xhi.src_lo.dev = b->Xd; g_xhi.src_lo.lo = 0; g_xhi.src_lo.cnt = s;   /* (X_lo arrives in *b->Xd at the X_lo hook; cnt set at the release) */
    sp_dev_sync_all();
    sem_init(&b->res_ready, 0, 0); pthread_create(&b->th, 0, x_bg_run_hi, b); b->started = 1;
    printf("      MN_OUT_DKM_HI: the writer started on X_hi's %zu limbs (the file's digits above limb %zu) after the division's step 1\n", g_xhi.Xh.n, s);
    return 1;
}
static void x_hi_release(struct x_bg *b)
{
    g_xhi.src_lo.cnt = b->Xd->n < g_xhi.s ? b->Xd->n : g_xhi.s;
    sp_dev_sync_all(); g_xhi.released = 1; sem_post(&g_xhi.go);
}
static int x_lo_hook(dbig *Xl, size_t s, int carry, void *a)   /* newton_db_xlo_hook: X_lo0 in *b->Xd (= Xl); released unless X_lo0 >= B^s */
{
    struct x_bg *b = (struct x_bg *)a; (void)Xl; (void)s;
    if (carry || dkm_hi_test_carry()) { printf("      MN_OUT_DKM_HI: X_lo0 %s: the corrections go into X_lo first, its digits after the division\n", carry ? ">= B^s" : "treated as >= B^s (MN_OUT_DKM_HI_TEST_CARRY)"); return 0; }
    x_hi_release(b); return 1;
}

/* ---- the output stage (archive:PLAN §19, M5 + C1).  Every node: T1 with the P, Q recurrence over its own
 * term range joined across the nodes (P = P_A Q_B + P_B, Q = Q_A Q_B in node order) and the residues of its share of
 * X (device kernel, placed at the share's offset and summed over the nodes); its share of the digits formatted in
 * chunks, residue-checked (Horner over the chunks and the nodes), T2-windowed and written to its part file as it
 * goes; node 0 the summary.  Size 1: the same code over the device or host X (the writer ran in the background from the
 * hook).  Size > 1: this node's share of the distributed division's sharded X. */
struct out_ctx {
    unsigned long N, d, d_out; const char *outfile; int verbose, size, rank;
    dbig *Xd;                                      /* size 1 in the device flow: X on the device (never on the host) */
    mdb *Xm;                                       /* B1, size > 1 in the distributed division: X sharded over the nodes (A-div's mdb); this node's share is read in place */
    uint64_t Pres[T1_NQ], Qres[T1_NQ], Rres[T1_NQ];   /* the residues of P, Q, R: every node's own from the sharded kernels (Xm); size 1: the single node's */
    struct pq_bg *pqb; struct x_bg *xb; int ncorr;   /* the recurrence thread; the size-1 writer thread; corrections to X after the hook */
    int defer; long dx; size_t zone;                 /* X's corrections (dx) not applied -- the output patches the tail; zone: the deferred T2 windows */
    mn_out_early *early; double t_early;             /* MN_OUT_EARLY -- the part file started in the division (mn_out.h) */
    double t00, t_init, t_bs, t_10dp, t_dm;
    int hi, hi_rel; mdb Xh; size_t s;                 /* Phase 15 EW (MN_OUT_DKM_HI) at size > 1: X_hi taken at the division's hook (Xm is then X_lo), the release done */
};
static const char *who(void) { return vpe_on() ? "pe" : "node"; }   /* the log word of a rank: the PE in process mode, else the node */
static void node_pfx(const struct out_ctx *c) { if (c->size > 1) printf("mn: %s %d: ", who(), c->rank); }
/* process mode: the division's hooks are process-wide function pointers set once before the flow (newton.h has one slot each); each finds the flow's out_ctx here, not through the arg */
static struct out_ctx * g_ocp;
static struct out_ctx *oc_of(void *a) { return vpe_on() ? g_ocp : (struct out_ctx *)a; }
/* MN_OUT_EARLY=1 -- the division's hook (newton_mn_x_hook) starts this rank's part file on X before the low product */
static void mn_early_hook(mdb *X, void *a)
{
    struct out_ctx *c = oc_of(a);
    c->early = mn_out_early_start(X, c->d, c->d_out, c->outfile, c->rank, c->size, c->verbose >= 2, mn_comm(0), newton_x_defer ? c->zone : 0);   /* (K's deferred tail windows) */
}
/* Phase 15 EW (MN_OUT_DKM_HI) at size > 1: the X_hi hook (every rank; collective) takes X_hi and starts its part file; the X_lo hook releases the
 * writer onto X_lo0's share (collective) unless X_lo0 >= B^s (then main releases it after the division, on the corrected X_lo) */
static int mn_early_hi_hook(mdb *Xh, size_t s, void *a)
{
    struct out_ctx *c = oc_of(a);
    if (s < 4 * (size_t)c->size || Xh->n < 4 * (size_t)c->size) {   /* (tiny: a layer's part could be empty -- the division goes on as without the switch; the same on every rank) */
        if (c->rank == 0) printf("mn: MN_OUT_DKM_HI: X_hi %zu limbs, s %zu: too small for two part layers over %d %s, not used\n", Xh->n, s, c->size, vpe_on() ? "PEs" : "nodes"); return 0; }
    c->hi = 1; c->hi_rel = 0; c->s = s; c->Xh = *Xh; memset(Xh, 0, sizeof *Xh);
    c->early = mn_out_early_hi_start(&c->Xh, s, c->d, c->d_out, c->outfile, c->rank, c->size, c->verbose >= 2, mn_comm(0), c->zone);
    if (c->rank == 0) printf("mn: MN_OUT_DKM_HI: the X_hi part files (parts 0..%d of %d) started after the division's step 1 (X_hi %zu limbs from limb %zu)\n", c->size - 1, 2 * c->size, c->Xh.n, s);
    return 1;
}
static int mn_early_lo_hook(mdb *Xl, size_t s, int carry, void *a)
{
    struct out_ctx *c = oc_of(a); (void)s;
    if (carry || dkm_hi_test_carry()) { if (c->rank == 0) printf("mn: MN_OUT_DKM_HI: X_lo0 %s: the corrections go into X_lo first, its part files after the division\n", carry ? ">= B^s" : "treated as >= B^s (MN_OUT_DKM_HI_TEST_CARRY)"); return 0; }
    mn_out_early_lo_release(c->early, Xl, mn_comm(0)); c->hi_rel = 1; return 1;
}
static int out_stage(struct out_ctx *c)
{
    comm *cm = mn_comm(0); int multi = c->size > 1;
    double t = mem_now();
    /* T1 (a): P, Q mod q over this node's terms, joined over the nodes */
    uint64_t pr[T1_NQ], qr[T1_NQ], Pg[T1_NQ], Qg[T1_NQ];
    if (c->pqb->started) { if (!c->pqb->joined) pthread_join(c->pqb->th, 0); c->pqb->joined = 1; memcpy(pr, c->pqb->p, sizeof pr); memcpy(qr, c->pqb->qq, sizeof qr); }
    else for (int i = 0; i < T1_NQ; i++) vf_pq_range_mod(c->pqb->a0, c->pqb->b1, t1_q[i], &pr[i], &qr[i]);
    mn_out_pq_combine(cm, pr, qr, Pg, Qg);
    /* the share of X this node holds: the device X (size 1, B1) or this node's share of the sharded X (size > 1: the distributed
     * division's mdb, read in place) */
    mn_out_src src; memset(&src, 0, sizeof src); size_t xn = multi ? c->Xm->n : c->Xd->n;
    if (!multi) { src.dev = c->Xd; src.cnt = c->Xd->n; }
    else {
        size_t lo, hi; mdb_share(c->Xm, c->rank, &lo, &hi);
        src.dev = &c->Xm->sh; src.lo = lo; src.cnt = hi - lo; if (src.cnt > c->Xm->sh.n) src.cnt = c->Xm->sh.n;   /* (the share's dbig is never normalised; shorter = zeros above) */
        node_pfx(c); printf("X%s share [%zu, %zu) of %zu limbs on the device (the distributed division's)\n", c->hi ? "_lo" : "", lo, hi, xn);
    }
    /* T1 (b): the residues of P, Q, R: at size > 1 every node has them (newton_mn_divmod: each share's db_mod_qs
     * scaled by B^lo and summed over the group) */
    /* T1 (c): X's residues: the share's, placed at its offset, summed over the nodes (size 1: the background thread's) */
    uint64_t xs[T1_NQ], Xres[T1_NQ]; int xres_bg = !multi && c->xb->started && (!c->ncorr || c->defer), joined = 0;   /* (deferred corrections -- T1 never waits for the writer) */
    if (xres_bg) { sem_wait(&c->xb->res_ready); memcpy(xs, c->xb->Xres, sizeof xs); }   /* the writer runs on: the file write is not on the timed path (as before, when the write came after `total`) */
    else if (!multi && c->xb->started) { pthread_join(c->xb->th, 0); joined = 1; mn_out_res_share(&src, xs); }
    else mn_out_res_share(&src, xs);
    if (c->defer && c->dx && src.lo == 0)             /* the uncorrected X's residues + dx (the node holding limb 0) */
        for (int i = 0; i < T1_NQ; i++) { uint64_t q = t1_q[i], a = (uint64_t)(c->dx < 0 ? -c->dx : c->dx) % q; xs[i] = vf_add_mod(xs[i], c->dx < 0 ? (a ? q - a : 0) : a, q); }
    mn_out_res_combine(cm, xs, src.lo, Xres);
    mn_out_src shs; memset(&shs, 0, sizeof shs);     /* MN_OUT_DKM_HI, size > 1: X = X_hi B^s + X_lo -- X_hi's share at its offset added */
    if (multi && c->hi) {
        size_t lo, hi; mdb_share(&c->Xh, c->rank, &lo, &hi);
        shs.dev = &c->Xh.sh; shs.lo = c->s + lo; shs.cnt = hi > lo ? hi - lo : 0; if (shs.cnt > c->Xh.sh.n) shs.cnt = c->Xh.sh.n;
        uint64_t xh[T1_NQ], Xh[T1_NQ]; mn_out_res_share(&shs, xh); mn_out_res_combine(cm, xh, shs.lo, Xh);
        for (int i = 0; i < T1_NQ; i++) Xres[i] = vf_add_mod(Xres[i], Xh[i], t1_q[i]);
    }
    bigint none; bi_init(&none);
    int bad1 = tier1_res_pq(c->N, c->d, c->Pres, c->Qres, &none, &none, Pg, Qg, Xres, c->Rres, c->verbose >= 2);
    double t_t1 = mem_now() - t;
    node_pfx(c); printf("T1    %8.2f s   %s%s\n", t_t1, bad1 ? "FAILED" : "ok: T(P+Q) == XQ + R and P, Q mod q for 8 primes", c->pqb->started ? (multi ? " (P, Q recurrence over this node's terms, overlapped with bs; joined over the nodes)" : " (P, Q recurrence overlapped with bs)") : "");
    if (c->pqb->started) { node_pfx(c); printf("      overlapped with bs: P, Q mod q recurrence %.2f s\n", c->pqb->t); }
    if (!multi) RESULT("T1", "s", t_t1);
    node_pfx(c); printf("      VmRSS %.1f GB before dc\n", mem_vmrss() / 1e9);

    /* size 1: the timed run ends here, as before (the digits' formatting is overlapped with the low product, the file write came
     * after `total`; now the writer streams both and is joined below -- on a slow file system the join, not the compute, is the wait) */
    double total = 0, phases = 0;
    if (!multi) {
        total = mem_now() - c->t00; phases = c->t_bs + c->t_10dp + c->t_dm + t_t1;
        printf("total %8.2f s   (bs %.1f + 10dP %.1f + dm %.1f + T1 %.1f + dc %.1f + T2 %.1f = %.1f; init %.1f; other %.1f); VmHWM %.1f GB\n",
               total, c->t_bs, c->t_10dp, c->t_dm, t_t1, 0.0, 0.0, phases, c->t_init, total - phases - c->t_init, mem_vmhwm() / 1e9);
        RESULT("total", "s", total); RESULT("phases", "s", phases); RESULT("other", "s", total - phases - c->t_init);
        RESULT("vmhwm", "GB", mem_vmhwm() / 1e9);
        printf("ref A22 (4e10): 285.7 = bs 112.2 + 10dP 12.6 + dm 46.8 + T1 ~3 + dc 110.3\n");
    }
    /* the digits: the node's share of X in chunks -> its part file; residue and windows per chunk */
    t = mem_now();
    mn_out ow, *o = &ow; memset(&ow, 0, sizeof ow);
    mn_out owh, *oh = 0; memset(&owh, 0, sizeof owh);  /* the X_hi part (finished in the early thread) */
    if (!multi && c->xb->started) {
        o = &c->xb->o; if (!joined) pthread_join(c->xb->th, 0);
        if (g_xhi.on) printf("      MN_OUT_DKM_HI: X_hi's range formatted and handed to the writer in %.2f s from the division's step 1; the writer then waited %.2f s for X_lo\n", g_xhi.t_hi, g_xhi.t_gate);
        if (c->ncorr && !c->defer) {                  /* X changed after the hook: the digits from the final X, the file rewritten */
            printf("      X corrected after the formatting started: redoing the digits\n");
            mn_out_finish(o); x_bg_writer(c->xb);
        }
    } else if (c->early) {                            /* the part file streamed during the low product */
        double tj = mem_now(); o = mn_out_early_join(c->early, &c->t_early);
        node_pfx(c); printf("      %s started in the division (MN_OUT_EARLY%s): its writer ran %.2f s, joined after %.2f s here%s\n", c->outfile ? (c->hi ? "the part files" : "the part file") : "the residues (no output file)", c->hi ? ", MN_OUT_DKM_HI: X_hi's after step 1, X_lo's at the release" : "", c->t_early, mem_now() - tj, c->ncorr && !c->defer ? "; X corrected after it started: redoing the part file" : "");
        oh = mn_out_early_hi(c->early);
        if (c->ncorr && !c->defer) {   /* (with ECALC_CORR_PATCH=2 the corrections are deferred to the tail patch below) */ mn_out_finish(o); mn_out_boundaries(o, &src, cm); mn_out_run(o, &src); }   /* (every rank: the corrections are the group's) */
    } else {
        o->d = c->d; o->d_out = c->d_out; o->outfile = c->outfile; o->rank = c->rank; o->size = c->size; o->verbose = c->verbose >= 2;
        if (c->defer) o->t2_defer = c->zone;
        mn_out_boundaries(o, &src, cm);
        mn_out_run(o, &src);
    }
    mn_out_fix fx; memset(&fx, 0, sizeof fx); int badp = 0;
    if (c->defer) {                                   /* the file complete, then the tail the corrections reach patched (collective at size > 1) */
        mn_out_finish(o); badp = mn_out_tail_fix(o, &src, cm, c->dx, &fx);
        node_pfx(c); printf("      patch: X %+ld after the formatting started (ECALC_CORR_PATCH): %s; %d deferred windows; %.3f s%s\n", c->dx,
                            c->dx ? "" : "nothing to patch", fx.nwin, fx.t, badp ? "  FAILED" : "");
        if (c->dx) { node_pfx(c); printf("      patch: digits [%zu, %lu] change (%zu low limbs read); this node: %zu bytes in %d part file%s\n", fx.kp, c->d, fx.w, fx.bytes, fx.parts, fx.parts == 1 ? "" : "s"); }
    }
    uint64_t Dres[T1_NQ];
    if (oh) { const mn_out *os[2] = { oh, o }; mn_out_digit_res_layers(os, 2, cm, Dres); } else mn_out_digit_res(o, cm, Dres);   /* X_hi's parts, then X_lo's */
    if (c->defer) for (int i = 0; i < T1_NQ; i++) Dres[i] = vf_add_mod(Dres[i], fx.dres_adj[i], t1_q[i]);   /* the patched tail's new - old (the bytes read back) */
    if (oh) { o->bad2 += oh->bad2; o->nwin += oh->nwin; }   /* (both parts' windows) */
    int bad2 = o->bad2, bad3 = tier1_digits_cmp(Dres, Xres, c->verbose >= 2) || badp;
    double t_dc = mem_now() - t;
    if (!multi) { db_free(c->Xd); if (g_xhi.on) { db_free(&g_xhi.Xh); sem_destroy(&g_xhi.go); g_xhi.on = 0; } }   /* (X_hi) */
    if (c->Xm) db_free(&c->Xm->sh);
    node_pfx(c); printf("dc    %8.2f s   digits [%zu, %zu) of %lu formatted from %zu decimal limbs in %d chunks%s (residues %.2f, format %.2f, digit residue %.2f, T2 %.2f, fetch %.2f, waiting for the writer %.2f)\n",
                        t_dc, o->k0, o->k1, c->d + 1, src.cnt, o->nchunks, !multi && c->xb->started ? " (streamed with the low product; joined after total)" : "", !multi && c->xb->started ? c->xb->t_res : 0.0, o->t_fmt, o->t_res, o->t_t2, o->t_fetch, o->t_wait);
    if (!multi) RESULT("dc", "s", t_dc);
    node_pfx(c); printf("T2    %8.2f s   windows %s (%d checked%s), digits == X mod q %s%s\n", 0.0, bad2 ? "FAILED" : "ok", o->nwin, multi ? " on this node" : "", bad3 ? "FAILED" : "ok", !multi && c->xb->started ? " (overlapped)" : "");
    if (!multi) RESULT("T2", "s", 0.0);
    node_pfx(c); printf("digits: %s...%s%s\n", oh ? oh->first : o->first, o->last, multi ? (oh ? " (this node's two ranges: X_hi's, X_lo's)" : " (this node's range)") : "");
    if (multi && c->rank == 0) {
        total = mem_now() - c->t00; phases = c->t_bs + c->t_10dp + c->t_dm + t_t1 + t_dc;
        printf("total %8.2f s   (bs %.1f + 10dP %.1f + dm %.1f + T1 %.1f + dc %.1f + T2 %.1f = %.1f; init %.1f; other %.1f); VmHWM %.1f GB\n",
               total, c->t_bs, c->t_10dp, c->t_dm, t_t1, t_dc, 0.0, phases, c->t_init, total - phases - c->t_init, mem_vmhwm() / 1e9);
        RESULT("total", "s", total); RESULT("phases", "s", phases); RESULT("other", "s", total - phases - c->t_init);
        RESULT("vmhwm", "GB", mem_vmhwm() / 1e9);
        printf("ref A22 (4e10): 285.7 = bs 112.2 + 10dP 12.6 + dm 46.8 + T1 ~3 + dc 110.3\n");
    }
    t = mem_now(); mn_out_finish(o);                  /* the last chunk's write */
    if (oh) { owh = *oh; oh = &owh; }                 /* (kept before the early state goes) */
    if (c->early) { ow = *o; o = &ow; mn_out_early_free(c->early); c->early = 0; }   /* the results kept, the early writer's state freed */
    if (c->hi) { db_free(&c->Xh.sh); memset(&c->Xh, 0, sizeof c->Xh); c->hi = 0; }   /* X_hi's share (the patch read it) */
    if (c->outfile) { node_pfx(c); if (multi && oh) printf("wrote %s.part%04d and .part%04d (%.2f + %.2f GB; write %.2f + %.2f s in the writer thread, %.2f s after the checks)\n", c->outfile, c->size - 1 - c->rank, 2 * c->size - 1 - c->rank, oh->bytes / 1e9, o->bytes / 1e9, oh->t_write, o->t_write, mem_now() - t);
                      else if (multi) printf("wrote %s.part%04d (%.2f GB; write %.2f s in the writer thread, %.2f s after the checks)\n", c->outfile, c->size - 1 - c->rank, o->bytes / 1e9, o->t_write, mem_now() - t);
                      else printf("wrote %s (%.2f GB; write %.2f s in the writer thread, %.2f s after the checks)\n", c->outfile, o->bytes / 1e9, o->t_write, mem_now() - t); }
    int fail = bad1 || bad2 || bad3;
    if (c->outfile && c->rank == 0) mn_out_sidecar_write(c->outfile, c->N, c->d, c->d_out, c->size, Xres, c->Rres, c->Pres, c->Qres, o->tail, o->ntail);   /* <outfile>.t1 for ECALC_RECHECK */
    node_pfx(c); printf("%s\n", fail ? "VERIFY FAILED" : "VERIFY OK");
    if (multi) { int any = mn_out_allreduce_or(cm, fail); if (c->rank == 0) printf("mn: all %d %s: %s\n", c->size, vpe_on() ? "PEs" : "nodes", any ? "VERIFY FAILED" : "VERIFY OK"); fail = any; }
    return fail;
}
/* the top-level P, Q of a size-1 run as a tree set (level 0) for ECALC_RECHECK (mn_out.h), written by a background
 * thread in the device flow.  The four region files carry P's quarter then Q's (binsplit.c tree_io), so P is written under the
 * reciprocal and Q under the division.  the driver releases P before S = P + Q
 * overwrites it, and Q only after the output stage when the writer still needs it (joining before Q's block goes, at
 * the end of the division, waited 113 s at 0.31 GB/s); ECALC_CKPT_TOP=2 budgets both releases by the measured disk rate
 * (ECALC_CKPT_TOP_SLACK seconds, default 1): a set the disk cannot finish in time is dropped instead of waited for. */
static void binsplit_seeds_begin_v(void *a) { binsplit_seeds_begin((unsigned long)(uintptr_t)a); }
/* ECALC_BUDGET_CHECK=1 -- before the run computes (after init's allocations and
 * mem_report("init")), every rank models its peak and the ranks decide together whether any node is over its budget; if one
 * is, every rank stops here (rc EC_RC_BUDGET = 8) with the figures, instead of one node dying mid-run.
 *   rank peak = max(device bytes in use now [the layout just mapped], the model's plane pools + arena [binsplit_node_bytes at
 *               this run's cap: covers a VMM arena still mapping in the background]) + max(the model's host terms [runtime,
 *               checkpoint staging, seed buffers, + 6 GB of comm at size > 1], VmRSS now);
 *   to come   = rank peak - (device in use now + VmRSS now): what the rank still has to take from the node;
 *   node over = the sum of its ranks' peaks > ECALC_NODE_GB (default 480), or the sum of their "to come" > MemAvailable
 *               (/proc/meminfo, the smallest reading of the node's ranks); a rank is also over when its peak exceeds its
 *               share of the node budget (ECALC_NODE_GB / ranks on the node).
 * The ranks allgather their figures (the host name's hash, the node's ranks found by it) and all apply the same rule to the
 * same numbers: the collective verdict is a max-reduce of "over" over the communicator.  Default on (see budget_check
 * below); ECALC_BUDGET_CHECK=0 turns it off. */
static uint64_t host_hash(void) { char h[256] = ""; gethostname(h, sizeof h - 1); uint64_t x = 1469598103934665603ull; for (const char *p = h; *p; p++) x = (x ^ (unsigned char)*p) * 1099511628211ull; return x; }
static void budget_check(unsigned long N, int verbose)
{
    const char *sw = getenv("ECALC_BUDGET_CHECK"); if (sw && !atoi(sw)) return;   /* default on; =0 off */
    int me = mn_rank(), sz = mn_size(); if (sz < 1) sz = 1;
    int L = vpe_on() ? vpe_lanes() : 1; const int gn = vpe_on() ? vpe_nnodes() : sz;   /* process mode: the model and the process figures are per NODE; every PE of a node reports 1/L of them, so a node's PEs sum to the node and the rule below is unchanged */
    double node_gb = getenv("ECALC_NODE_GB") ? atof(getenv("ECALC_NODE_GB")) : 480.0;
    int cap = rns_pool_log() >= 31 ? 1 : 0;
    size_t pb = 0, ab = 0, hb = 0; binsplit_node_bytes(N, gn, cap, ec_np_init(), &pb, &ab, &hb);
    if (vpe_ppg()) { const size_t pp = (size_t)binsplit_ppn_proc(); pb /= pp; ab /= pp; hb /= pp; L = 1; }   /* (S2c's PPN_PROC: the host model already counts a runtime and a comm pool per process, so the per-process share is node / ppn_proc, the planner's own term) */   /* S2 (one process per PE): the model is per node, so each PE takes 1/ppn of it; the measured device bytes and VmRSS are this process's own (its device, its runtime): already one PE's, not divided again */
    size_t dev = mem_report_dev_total(), rss = mem_vmrss();
    size_t cres = binsplit_comm_resident_bytes(N, gn); if (vpe_ppg()) cres /= (size_t)binsplit_ppn_proc();   /* the device pools exist already (the transport opened before rns_init) and are in the model's host term: not still to come (plq2: 2 n 1e10 Q2 was refused for 62 GB of them) */
    size_t dpk = (dev > pb + ab ? dev : pb + ab), hpk = rss > hb ? rss : hb, peak = dpk + hpk, now = dev + rss + cres;
    struct meminfo mi; memset(&mi, 0, sizeof mi); mem_meminfo(&mi);
    const int K = 6;
    uint64_t v[6] = { host_hash(), peak / L, dpk / L, hpk / L, peak > now ? (peak - now) / L : 0, mi.avail };
    uint64_t *all = (uint64_t *)calloc((size_t)sz * K, 8);
    if (sz > 1) mn_allgather(mn_comm(0), v, K, all); else memcpy(all, v, sizeof v);
    int nover = 0, first = -1; char *over = (char *)calloc(sz, 1);
    for (int r = 0; r < sz; r++) {
        const uint64_t *a = all + (size_t)r * K; int nr = 0; uint64_t sum = 0, come = 0, avail = (uint64_t)-1;
        for (int q = 0; q < sz; q++) { const uint64_t *b = all + (size_t)q * K; if (b[0] != a[0]) continue; nr++; sum += b[1]; come += b[4]; if (b[5] < avail) avail = b[5]; }
        double share = node_gb * 1e9 / nr;
        int why = (double)sum > node_gb * 1e9 ? 1 : (avail && come > avail) ? 2 : (double)a[1] > share ? 3 : 0;
        over[r] = (char)why; if (why) { nover++; if (first < 0) first = r; }
        if (r == me) rns_dist_cache_budget(share - (double)a[1], nr);   /* this rank's room and its node's ranks for the transform-cache fit rule (rns_dist.c cache_avail) */
        if (why || (verbose >= 2 && me == 0))
            if (me == 0 || r == me)
                printf("budget: rank %d%s: peak %.1f GB (device %.1f, host %.1f), still to take %.1f GB | node of %d rank%s: peaks %.1f of %.0f GB, to take %.1f of MemAvailable %.1f GB | rank share %.1f GB%s\n",
                       r, why ? " OVER" : "", a[1] * 1e-9, a[2] * 1e-9, a[3] * 1e-9, a[4] * 1e-9, nr, nr > 1 ? "s" : "", sum * 1e-9, node_gb, come * 1e-9, avail * 1e-9, share * 1e-9,
                       why == 1 ? " -- the node's peaks exceed ECALC_NODE_GB" : why == 2 ? " -- the node cannot supply what its ranks still need" : why == 3 ? " -- the rank exceeds its share" : "");
    }
    if (nover) {
        const uint64_t *a = all + (size_t)first * K;
        fprintf(stderr, "ecalc: BUDGET CHECK FAILED (ECALC_BUDGET_CHECK=1) on rank %d of %d: %d rank%s over, the first rank %d (peak %.1f GB: device %.1f + host %.1f; %s); every rank stops before compute, rc %d\n",
                me, sz, nover, nover > 1 ? "s" : "", first, a[1] * 1e-9, a[2] * 1e-9, a[3] * 1e-9,
                over[first] == 1 ? "its node's peaks exceed ECALC_NODE_GB" : over[first] == 2 ? "its node's MemAvailable is short" : "over its share of the node budget", EC_RC_BUDGET);
        fflush(stdout); fflush(stderr);
        if (sz > 1) mn_barrier();                    /* every rank has printed before any leaves (the launcher kills the rest at the first exit) */
        ec_quit(EC_RC_BUDGET);
    }
    uint64_t mx = 0; for (int r = 0; r < sz; r++) if (all[(size_t)r * K + 1] > mx) mx = all[(size_t)r * K + 1];
    if (me == 0) printf("budget: ECALC_BUDGET_CHECK ok: %d rank%s, largest rank peak %.1f GB, node budget %.0f GB\n", sz, sz > 1 ? "s" : "", mx * 1e-9, node_gb);
    free(all); free(over);
}
/* the plane cap as one switch.  ECALC_PLANE_CAP = 2^30 | 2^31 (also 30, 31) sets the two knobs that make a cap: POOL_LOG (the
 * plane pools' base, the batch and mdev tiers' 2^pool_log) and DIST_LOGN_TEST (the single-node dist tier's 2^k cap = pool_log;
 * without it a 2^31 product would grow a 2^30 pool inside a phase and abort).  ECALC_PLANE_CAP=run takes 2^30 when the run's largest product (the division's 2 k + 8 limbs, binsplit_div_product_limbs) fits one mn plane at that cap, else 2^31 (the start-up gain of 2^30
 * without a size the 2^30 plane was not measured at; not with ECALC_PE_PER_GPU).  ECALC_PLANE_CAP=fit takes the larger cap whose
 * node total (binsplit_node_bytes: the plane pools at the prime count + the arena + the host init constants) fits ECALC_NODE_GB
 * (default 480, the safe budget).  Unset: `run` (the default; 2^31 with ECALC_PE_PER_GPU, `fit` with ECALC_PE_PER_GPU=1 and XSHMEM_PES_PER_GPU > 1), unless POOL_LOG or DIST_LOGN_TEST is given; =off: the knobs as
 * given.  A knob set in the environment that disagrees with the cap is refused.  BS_LAYOUT_ONLY is served here, before
 * rns_init, so the layout report needs no device.  The cap index c is pool_log - 30 (bs_cap_name). */
static int g_layout_refused;                                         /* BS_LAYOUT_ONLY's prime-set check refused a point */
static void layout_refused_exit(void) { fflush(stdout); _exit(EC_RC_FATAL); }
static int plane_cap_switch(int pool_log, unsigned long N, int verbose)
{
    const char *e = getenv("ECALC_PLANE_CAP"); int c = -1;
    int dflt = 0;                                                          /* the cap came from the default, not the environment (the verbose line says so) */
    if (!e) { if (!getenv("POOL_LOG") && !getenv("DIST_LOGN_TEST")) {   /* default `run` (2^30 or 2^31 by the run's size), unless a knob it sets is given */
            const char *pg = getenv("ECALC_PE_PER_GPU"), *pp = getenv("XSHMEM_PES_PER_GPU");
            e = pg && atoi(pg) == 1 && pp && atoi(pp) > 1 ? "fit" : "run"; dflt = 1;   /* 2 PEs share one GPU: every PE's plane pool is a full cap, so take the larger cap whose node total fits ECALC_NODE_GB (the plan then models the shared device) */
        } }
    else if (!strcmp(e, "off")) e = NULL;                                  /* ECALC_PLANE_CAP=off: POOL_LOG and DIST_LOGN_TEST as given */
    if (e && *e) {
        static const char *nm[2][3] = { { "2^30", "30", "1073741824" }, { "2^31", "31", "2147483648" } };
        for (int i = 0; i < 2 && c < 0; i++) for (int j = 0; j < 3; j++) if (!strcmp(e, nm[i][j])) { c = i; break; }
        if (c < 0 && !strcmp(e, "fit")) {
            int sz = getenv("COMM_SIZE") ? atoi(getenv("COMM_SIZE")) : 1; if (sz < 1) sz = 1;
            double budget = (getenv("ECALC_NODE_GB") ? atof(getenv("ECALC_NODE_GB")) : 480.0) * 1e9;
            for (c = 1; c > 0; c--) if ((double)binsplit_node_bytes(N, sz, c, ec_np_init(), 0, 0, 0) <= budget) break;
            if (verbose) printf("ECALC_PLANE_CAP=fit: plane cap %s (node %.1f GB of %.0f at %d primes)\n", bs_cap_name[c], binsplit_node_bytes(N, sz, c, ec_np_init(), 0, 0, 0) * 1e-9, budget * 1e-9, ec_np_init());
        }
        if (c < 0 && !strcmp(e, "run")) {                  /* 2^30 when its plane holds the run's largest product (as the C that would refuse or abort reads it), else 2^31 */
            int sz = getenv("COMM_SIZE") ? atoi(getenv("COMM_SIZE")) : 1; if (sz < 1) sz = 1;
            int ppg = vpe_on();
            size_t need = binsplit_div_product_limbs(N); int pl0 = rns_pool_log(), lc;
            rns_preinit_pool_log(30); lc = ppg ? 0 : rns_mul_dist_mn_logcap_g(sz); rns_preinit_pool_log(pl0);   /* the mn tier's plane cap at pool_log 30 for this group size (before rns_init, as `fit`) */
            c = !ppg && need <= ((size_t)1 << lc) ? 0 : 1;   /* ECALC_PE_PER_GPU: not modelled here, 2^31 */
            if (verbose) printf("ECALC_PLANE_CAP=run%s: plane cap %s (largest product %zu limbs %s 2^%d, the 2^30 cap's plane over %d node%s%s)\n", dflt ? " (default)" : "", bs_cap_name[c], need, c ? ">" : "<=", ppg ? 0 : lc, sz, sz > 1 ? "s" : "", ppg ? "; ECALC_PE_PER_GPU set: not modelled, 2^31" : "");
        }
        if (c < 0) { fprintf(stderr, "ECALC_PLANE_CAP=%s: one of 2^30, 2^31 (or 30, 31), fit or run\n", e); exit(2); }
        char pl[8]; snprintf(pl, sizeof pl, "%d", 30 + c);
        const char *knob[2] = { "POOL_LOG", "DIST_LOGN_TEST" };
        for (int k = 0; k < 2; k++) {
            const char *o = getenv(knob[k]);
            if (o && *o && strcmp(o, "auto") && atoi(o) != atoi(pl)) { fprintf(stderr, "ECALC_PLANE_CAP=%s sets %s=%s; the environment has %s=%s\n", e, knob[k], pl, knob[k], o); exit(2); }
            setenv(knob[k], pl, 1);
        }
        { char ci[4]; snprintf(ci, sizeof ci, "%d", c); setenv("ECALC_PLANE_CAP_IDX", ci, 1); }
        pool_log = 30 + c;
        if (verbose) printf("ECALC_PLANE_CAP: %s points (POOL_LOG=%s DIST_LOGN_TEST=%s)\n", bs_cap_name[c], pl, pl);
    }
    if (getenv("BS_LAYOUT_ONLY")) {
        /* each point D:g (D digits per node) with g > 1 checked first against the prime set (mn_plan_check: the plan of
         * D g total digits; one `plan check` / `plan REFUSED` line), and a refusal turns the layout's exit status into 3 */
        char *dup = strdup(getenv("BS_LAYOUT_ONLY"));
        for (char *t = strtok(dup, ","); t; t = strtok(NULL, ",")) {
            char *c = strchr(t, ':'); int g = c ? atoi(c + 1) : 1; double D = atof(t);
            if (g > 1 && D > 0) { unsigned long dt = (unsigned long)llround(D * g); dt = (dt + 17) / 18 * 18;
                                  binsplit_layout_comm_size(g);   /* MS: the point's g as COMM_SIZE, so rns_pool0_np (P24's decision) sees it */
                                  if (mn_plan_check(dt, e_terms(dt), g, pool_log)) g_layout_refused = 1;
                                  binsplit_layout_comm_size(0); }
        }
        free(dup);
        if (g_layout_refused) atexit(layout_refused_exit);           /* binsplit_layout_only ends with exit(0) */
        rns_preinit_pool_log(pool_log); binsplit_pregrow(N);         /* prints the layout lines and exits */
    }
    return pool_log;
}
/* ---- ECALC_PE_PER_GPU: the flow of one PE, one process per GPU (vpe.h) ---------------------------------------------------------------------
 * Every PE process runs the same flow as a node-process does today, with mn_size() = the PE count (>= 2: the size-1 flow stays for the off path), mn_rank() = the
 * PE, bs_a0 / bs_b1 = its term range, its own recurrence thread (pq_bg_run) and writer (mn_out_early); the output parts are two per PE (D3: X_hi's part npe-1-pe,
 * X_lo's 2 npe-1-pe), the .top set per PE (binsplit.c).  Process-wide things are done once in main, before the threads (the division's hooks, newton_x_defer,
 * bs_ckpt_dir, the thread counts) or by the node leader (node-level reports); everything the flow keeps is a local of the flow function. */
#define MEMREP(x) do { if (vpe_proc_leader()) mem_report(x); } while (0)   /* every PE reports its own process (device pools, host items) */
static int g_flow_rc;
static int flow_rc_all(void) { return g_flow_rc; }   /* this process's slot (the only one written) */
struct flow_ctx { unsigned long N, d, d_out; const char *outfile; int verbose, pool_log, mn_size; double t00, t_ri, t_pre, t_mn, t_st, t_age; };   /* t_*: ECALC_INIT_TL=1 (print only): plan/fit, mn_init, mn_selftest, the process's age at main */
static int ckpt_top_mode(void)
{
    return getenv("ECALC_CKPT_TOP") ? atoi(getenv("ECALC_CKPT_TOP"))
         : (getenv("ECALC_CHECKPOINT") && atoi(getenv("ECALC_CHECKPOINT")) ? 2 : 0);   /* off for record runs; ECALC_CHECKPOINT=1 = the budgeted top set (=2); ECALC_CKPT_TOP overrides both */
}
static void vpe_set_range(unsigned long N)            /* this PE's terms: [1 + N pe / npe, 1 + N (pe + 1) / npe) */
{
    unsigned __int128 nn = N; int pe = vpe_pe(), npe = vpe_npe();
    bs_a0 = 1 + (unsigned long)(nn * pe / npe); bs_b1 = 1 + (unsigned long)(nn * (pe + 1) / npe);
}
static int env_vpe_bg_threads(void)                    /* the background team of one PE (recurrence, writer) = CPUs / PEs per node (192 / 4 = 48 on aac7, where the off path's 48 is per node) */
{
    int n = 0;
    if (n < 1 && vpe_ppg()) {                           /* S2: this process's own CPUs: its affinity mask (vpe_pe_affinity restricted it to the PE's NUMA node), not the node's */
        cpu_set_t m; CPU_ZERO(&m); n = sched_getaffinity(0, sizeof m, &m) == 0 ? CPU_COUNT(&m) : 0;
        if (n < 1) n = omp_get_num_procs();
    }
    if (n < 1) { n = omp_get_num_procs() / vpe_lanes(); if (n < 1) n = 1; }
    return n;
}
/* the division's hooks, set once (the slots in newton.h are process-wide; a hook finds the flow's out_ctx through oc_of) */
static void mn_hooks_set(struct out_ctx *oc, const char *outfile)
{
    /* MN_OUT_EARLY_ALWAYS=1 (B3b, default off): the hook also without an output file: the writer then computes the digit residues only (no file is opened) while the division runs */
    if ((outfile || (getenv("MN_OUT_EARLY_ALWAYS") && atoi(getenv("MN_OUT_EARLY_ALWAYS")))) && (getenv("MN_OUT_EARLY") ? atoi(getenv("MN_OUT_EARLY")) : 1)) {   /* default 1 */ newton_mn_x_hook = mn_early_hook; newton_mn_x_arg = oc; }
    /* the writer on X_hi after DKM's step 1 (MN_OUT_DKM_HI, always on since 2026-10-12; the =0 fallback was removed 2026-10-23, Q87): needs the early writer and the deferred corrections */
    if (newton_mn_x_hook && newton_x_defer) { newton_mn_xhi_hook = mn_early_hi_hook; newton_mn_xlo_hook = mn_early_lo_hook; }
}
/* the end of the flow of one rank / PE (the sequences are the off path's, byte for byte; the process form shuts down the device pools once) */
static void flow_end(int f, int rank0)
{
    g_flow_rc = f;
    mn_ckpt_top_finish();                             /* (size > 1, node 0: the top tree set's writer) */
    if (vpe_on()) {
        vpe_node_barrier();                           /* every lane is past its output stage */
        if (vpe_proc_leader()) { db_release_pools(); mem_report_host_item(MEM_HOST_X, 0); mem_report("end"); mem_report_summary(); }   /* S2: each process releases its own pools */   /* B1: X's block (device / sharded) was in use until here */
        vpe_node_barrier();
        mn_barrier(); mn_finalize();                  /* (collective: every PE; rns_shutdown once, in main) */
    } else if (rank0) {
        db_release_pools();                           /* B1: X's block (device / sharded) was in use until here */
        mem_report_host_item(MEM_HOST_X, 0); mem_report("end"); mem_report_summary();
        mn_barrier(); mn_finalize();
        rns_shutdown();
    } else { db_release_pools(); rns_shutdown(); mem_report("released"); mem_report_summary(); mn_barrier(); mn_finalize(); }   /* M5 + A-mem: its device memory goes before the final barrier (after the top set's writer) */
}
static void seeds_begin_thunk(void *a) { vpe_set_range((unsigned long)(uintptr_t)a); binsplit_seeds_begin((unsigned long)(uintptr_t)a); }
static void seeds_begin_vpe(void *a) { vpe_run(seeds_begin_thunk, a); }   /* I2 in process mode: the PE's seeds and region pools, inside rns_init (binsplit's per-thread state) */
static void flow_run(void *a);
static void recheck_run(void *a)                       /* ECALC_RECHECK in process mode: one PE's recheck (its parts, its top set, its term range) */
{
    struct flow_ctx *fc = (struct flow_ctx *)a; vpe_set_range(fc->N);
    int f = mn_out_recheck(fc->N, fc->d, fc->d_out, fc->outfile, mn_comm(0), mn_rank(), fc->mn_size, bs_a0, bs_b1 ? bs_b1 : fc->N + 1, fc->verbose);
    g_flow_rc = f; mn_barrier(); mn_finalize();
}
/* ECALC_INIT_TL=1: seconds since this process started: /proc/self/stat field 22 (start time in clock ticks since boot) against CLOCK_BOOTTIME (10 ms resolution); -1 when unreadable */
static double ec_proc_age_s(void)
{
    FILE *f = fopen("/proc/self/stat", "r"); if (!f) return -1;
    char b[2048]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    char *p = strrchr(b, ')'), *sv = 0, *tok; if (!p) return -1;
    unsigned long long st = 0; int fld = 2, ok = 0;                 /* the token after ") " is field 3 */
    for (tok = strtok_r(p + 1, " ", &sv); tok; tok = strtok_r(0, " ", &sv)) if (++fld == 22) { st = strtoull(tok, 0, 10); ok = 1; break; }
    struct timespec ts; long tk = sysconf(_SC_CLK_TCK);
    if (!ok || tk <= 0 || clock_gettime(CLOCK_BOOTTIME, &ts)) return -1;
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec - (double)st / (double)tk;
}
int main(int argc, char **argv)
{
    const double tl_age = ec_proc_age_s();                   /* ECALC_INIT_TL=1: the process's age at main (read before anything else runs) */
    cfg_snapshot();                                   /* L5: record the registered switches set in the environment, before ecalc sets any itself (print-only) */
    /* MN_PLAN_ONLY=<total digits>:<g> -- the plan of a run of that size on g node-processes (mn_plan.c): every
     * large product's grid, pieces and planes from the code's own decisions, no device, no communicator; exits after the knobs
     * below are set (the plane-cap switch, the pools' sizes), before rns_init.  The digits (4.4e13 or 44000000000000) replace
     * argv[1], which may be omitted; COMM_SIZE is set to g for the switch's `fit` (nothing else reads it before the exit) */
    const char *plan = getenv("MN_PLAN_ONLY"); int plan_g = 1; unsigned long plan_d = 0;
    if (plan && *plan) {
        char *e; double dd = strtod(plan, &e); plan_d = (unsigned long)llround(dd); plan_g = *e == ':' ? atoi(e + 1) : 1;
        if (dd < 1 || plan_g < 1 || (*e && *e != ':')) { fprintf(stderr, "MN_PLAN_ONLY=%s: <total digits>:<node-processes>, e.g. 4.4e13:576\n", plan); return 2; }
        char gb[16]; snprintf(gb, sizeof gb, "%d", plan_g); setenv("COMM_SIZE", gb, 1); setenv("COMM_RANK", "0", 1);
    } else plan = 0;
    if (argc < 2 && !plan) { fprintf(stderr, "usage: ecalc <digits> [outfile]\n"); return 2; }
    ec_segv_trace_install();                          /* s18w task 2: after COMM_RANK is known (the environment above, or the
                                                          MN_PLAN_ONLY branch's setenv); no-op unless ECALC_SEGV_TRACE=1 */
    unsigned long d_out = plan ? plan_d : strtoul(argv[1], 0, 10), d = d_out;   /* d: the digits computed; in decimal rounded up to a multiple of 18 (the requested digits are a prefix: floor(floor(10^d' e) / 10^(d'-d)) = floor(10^d e)); d_out: written and windowed */
    const char *outfile = argc > 2 && !plan ? argv[2] : 0;
    int verbose = getenv("ECALC_VERBOSE") ? atoi(getenv("ECALC_VERBOSE")) : 1;
    int pool_log = getenv("POOL_LOG") ? atoi(getenv("POOL_LOG")) : 31;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!getenv("ECALC_NP")) { const char *cs = getenv("COMM_SIZE"); setenv("ECALC_NP", cs && atoi(cs) > 1 ? "auto" : "3", 1); }   /* three primes for decimal limbs (RESULTS 78: -17 % wall, -25.8 GB); D18 (results/LAUNCH_DEFAULTS.md §DEF): auto at size > 1, so every multi-node product runs P24 (crt.c ec_np_planes) */
    d = ((d_out + 17) / 18) * 18;
    rns_np2_set_digits((double)d);                    /* MN_NONPOW2_CAP auto: on from MN_NONPOW2_MIN digits per node-process (rns_mul.c rns_np2cap) */
    printf("== ecalc: e to %lu digits (decimal limbs, base 10^18)%s ==\n", d_out, d != d_out ? " [computed to the next multiple of 18]" : "");
    meta_line("ecalc");
    { const char *es = getenv("COMM_SIZE"), *er = getenv("COMM_RANK"), *nc = getenv("MN_NONPOW2_CAP"); int sz = es ? atoi(es) : 1;
      if (verbose >= 1 && (!er || atoi(er) == 0) && sz >= 3 && (sz & (sz - 1))) {
          if (nc && *nc && strcmp(nc, "auto")) printf("MN_NONPOW2_CAP=%s: %s\n", nc, rns_np2cap() ? "on" : "off");
          else printf("MN_NONPOW2_CAP auto: %s (%.3g digits per node %s %.3g)\n", rns_np2cap() ? "on" : "off", (double)d / sz, rns_np2cap() ? ">=" : "<", rns_np2_min()); } }
    double t00 = mem_now(), t;
    unsigned long N = e_terms(d);
    { const char *es = getenv("COMM_SIZE"), *er = getenv("COMM_RANK"); int sz = es ? atoi(es) : 1, rk = er ? atoi(er) : 0;   /* M2: this process's term range (mn_init below checks the rest) */
      if (sz > 1 && !vpe_on()) { unsigned __int128 nn = N; bs_a0 = 1 + (unsigned long)(nn * rk / sz); bs_b1 = 1 + (unsigned long)(nn * (rk + 1) / sz); } }
    rns_after_staging_hook = vpe_on() ? seeds_begin_vpe : (void (*)(void *))binsplit_seeds_begin_v; rns_hook_arg = (void *)N;   /* I2: the seeds during the pool allocations */
    pool_log = plane_cap_switch(pool_log, N, verbose);   /* ECALC_PLANE_CAP (`run` by default; exits for BS_LAYOUT_ONLY) */
    struct bs_fit fit; int fit_g = getenv("COMM_SIZE") ? atoi(getenv("COMM_SIZE")) : 1; if (fit_g < 1) fit_g = 1;
    { int pl0 = rns_pool_log(); rns_preinit_pool_log(pool_log); binsplit_fit(N, fit_g, &fit); rns_preinit_pool_log(pl0); }   /* FIT (D26): the memory fit at rank 0's layout; decides DM_MN_WRAP when unset (every rank alike) */
    if (!plan && verbose && (!getenv("COMM_RANK") || atoi(getenv("COMM_RANK")) == 0)) binsplit_fit_print(&fit, (double)d_out, fit_g, "memory fit: ");
    if (!plan && !fit.fits && (!getenv("COMM_RANK") || atoi(getenv("COMM_RANK")) == 0))
        fprintf(stderr, "ecalc: WARNING: this run does not fit the modelled memory (device %.2f / %.2f GB, node %.2f / %.0f GB); launchers refuse it (mnrun.sh, tools/plan_values.py --fit)\n",
                fit.device * 1e-9, fit.limit * 1e-9, fit.node * 1e-9, fit.budget * 1e-9);
    { char eff[360]; const char *np = getenv("ECALC_NP"), *pc = getenv("ECALC_PLANE_CAP_IDX"), *cs = getenv("COMM_SIZE");   /* L5: the resolved values (print-only) */
      snprintf(eff, sizeof eff, "pool_log:%d,np:%s,base:%s,verbose:%d,plane_cap:%s,comm_size:%s,dm_mn_wrap:%d(%s)", pool_log, np ? np : "-", "10^18", verbose, pc ? pc : "-", cs ? cs : "1", newton_dm_wrap(), binsplit_dm_wrap_src());
      cfg_print(eff); }
    { /* step 3: in the decimal device flow the pinned staging only serves the seeds (and checkpoints): size it to them.
                                                                                 * the seeds stream through their own two 2 GiB buffers (binsplit.c) -- the staging is the checkpoints' 1 GiB chunk per APU */
      rns_staging_bytes_req = 1u << 30;   /* no cap at 2^pool_log limbs -- at 8e10 the seed stage of a region is 19.5 GB > 16 GiB and the run aborted at the seeds ("seed region larger than the staging buffer") */
      if (vpe_ppg() && rns_batch_split_mode() == 1) rns_staging_bytes_req = (size_t)2 << 30;   /* RNS_BATCH_SPLIT=1: the staging holds the piece products' temporaries (rns_mul.c): one 2^28-limb piece = 2 GiB (default 1 GiB, 2^27) */
      /* plane pool 1 at the dist tier's 3 q + 16 limbs (the host mdev tier, which needs the full 2^pool_log, is not used in this flow) */
      rns_pool1_bytes_req = rns_pool1_default_bytes(pool_log); }
    if (plan) { int f = mn_plan_run(d, N, plan_g, pool_log); binsplit_shmem_pool_rule(N, plan_g, 1);
                binsplit_fit_print(&fit, (double)d_out, plan_g, "plan fit    ");   /* FIT (D26): the memory fit; a run that does not fit is refused (the last plan line) */
                if (vpe_ppg()) {   /* S2fix: ECALC_PE_PER_GPU=1 has no striped tier (rns_mul_batch dies there): every batch-tier product of a PE's leaf must fit its one device's plane pool */
                    int pl0 = rns_pool_log(); rns_preinit_pool_log(pool_log); size_t nc = 0, pl_limbs = rns_plane_limbs(); rns_preinit_pool_log(pl0); int lg = binsplit_ppg_batch_logl(N, plan_g, &nc), np = ec_np_init();
                    int ok = !lg || (size_t)np * ((size_t)1 << lg) <= pl_limbs;
                    printf("plan fit    batch tier (ECALC_PE_PER_GPU=1, PE 0's leaf of %d nodes x %d PEs): the largest product %zu limbs = 2^%d points x %d primes = %.3g limbs vs the plane pool's %.3g (cap 2^%d): %s\n",
                           plan_g, vpe_lanes(), nc, lg, np, (double)np * ((size_t)1 << lg), (double)pl_limbs, pool_log, ok ? "local tier OK" : "the striped tier would be needed (it needs all 4 devices)");
                    if (!ok && rns_batch_split_mode() == 1) {   /* RNS_BATCH_SPLIT=1: oversized levels are cut into pieces on the one device (rns_mul.c); no memory beyond the staging and the pools, so the fit lines stand */
                        size_t lv[40]; int nl = binsplit_ppg_batch_levels(N, plan_g, lv, 40), bad = 0;
                        rns_preinit_pool_log(pool_log); size_t Lp = rns_batch_split_lmax(); rns_preinit_pool_log(pl0);
                        for (int l = 0; l < nl; l++) {
                            int lgl = 0; while (((size_t)1 << lgl) < lv[l]) lgl++;
                            if ((size_t)np * ((size_t)1 << lgl) <= pl_limbs) continue;
                            int ka = 0, kb = 0;
                            if (!rns_batch_split_shape(lv[l] / 2, lv[l] / 2, 2, Lp, &ka, &kb)) { bad = 1; printf("plan fit    batch tier split: level %d (%zu limbs): no grid of at most 1024 pieces within 2^%d limbs\n", l, lv[l], (Lp ? 63 - __builtin_clzll(Lp) : 0)); continue; }
                            size_t sa = (lv[l] / 2 + ka - 1) / ka, sb = (lv[l] / 2 + kb - 1) / kb; int lgp = 0; while (((size_t)1 << lgp) < sa + sb) lgp++;
                            printf("plan fit    batch tier split (RNS_BATCH_SPLIT=1): level %d product %zu limbs = %d x %d pieces of %zu x %zu limbs (2^%d points; piece limit 2^%d = the pools and the %.3g-limb staging); transform points x%.2f; temporaries in the staging, no extra memory: OK\n",
                                   l, lv[l], ka, kb, sa, sb, lgp, (Lp ? 63 - __builtin_clzll(Lp) : 0), (double)(rns_staging_bytes_req ? rns_staging_bytes_req / 8 : (size_t)1 << pool_log),
                                   (double)kb * (1.0 + 4.0 * ka) * (double)((size_t)1 << lgp) / (5.0 * (double)((size_t)1 << lgl)));
                        }
                        if (!bad) ok = 1;
                    }
                    if (!ok) { printf("plan REFUSED %.4g digits g %d: ECALC_PE_PER_GPU=1 cannot run the leaf's largest batch product (2^%d points x %d primes > the plane pool %.3g limbs at ECALC_PLANE_CAP=2^%d; the striped tier needs all 4 devices): %sa PE's leaf must stay within 2^29 points (3 primes) -- more nodes or fewer digits (about 1.0e11 at 3 nodes) -- or run without ECALC_PE_PER_GPU\n",
                                      (double)d_out, plan_g, lg, np, (double)pl_limbs, pool_log, pool_log < 31 && np * ((size_t)1 << lg) <= ((size_t)3 << 29) ? "use ECALC_PLANE_CAP=2^31, and " : ""); if (!f) f = EC_RC_BUDGET; } }
                if (!fit.fits) { printf("plan REFUSED %.4g digits g %d: the memory does not fit (device %.2f GB vs ECALC_DEVICE_GB %.2f, node %.2f GB vs ECALC_NODE_GB %.0f): see the plan fit lines\n",
                                        (double)d_out, plan_g, fit.device * 1e-9, fit.limit * 1e-9, fit.node * 1e-9, fit.budget * 1e-9); if (!f) f = EC_RC_BUDGET; }
                fflush(stdout); _exit(f); }   /* + the SHMEM pool line */   /* MN_PLAN_ONLY (no device was touched; _exit: no atexit reports) */
    if (getenv("ECALC_RECHECK") && atoi(getenv("ECALC_RECHECK"))) {   /* the standalone recheck of a finished run (mn_out.h) -- no pools, no computation */
        int sz = mn_init(); bs_ckpt_dir = getenv("BS_CKPT_DIR"); if (bs_ckpt_dir && !*bs_ckpt_dir) bs_ckpt_dir = 0;
        if (vpe_on()) {                                   /* every PE rechecks its own parts and its own top set (the check counts PEs) */
            if (sz < 2) ec_fatal(EC_RC_FATAL, "ecalc: ECALC_PE_PER_GPU=1 needs at least 2 PEs (%d): run on 2+ nodes or more tasks per node\n", sz);
            struct flow_ctx fc; memset(&fc, 0, sizeof fc); fc.N = N; fc.d = d; fc.d_out = d_out; fc.outfile = outfile; fc.verbose = verbose; fc.mn_size = sz;
            if (!bs_ckpt_dir) { const char *dd = mn_out_ckpt_default(outfile); struct stat st; if (dd && stat(dd, &st) == 0 && S_ISDIR(st.st_mode)) bs_ckpt_dir = dd; }
            vpe_run(recheck_run, &fc);
            int f = flow_rc_all();
            rns_shutdown(); return f;
        }
        int f = mn_out_recheck(N, d, d_out, outfile, mn_comm(0), mn_rank(), sz, bs_a0, bs_b1 ? bs_b1 : N + 1, verbose);
        mn_barrier(); mn_finalize(); return f;
    }
    /* the transport is opened (the pool rule, mn_init: shmem_init_thread, the symmetric
     * pool, the four meshes) BEFORE rns_init -- i.e. before the VMM arenas and plane pools are mapped and before the seed thread
     * runs its kernels and DMAs (binsplit_seeds_begin inside rns_init).  Cray OpenSHMEMX's shmem_init_thread segfaulted in task 1
     * in 3 of 23 launches of 10^10 at 2 nodes with 45 GB of VMM arenas and the seeds already live (archive:results_merged_2026-10-24/AAC7_PORT.md §A16 step 7).  The
     * pool rule needs only N and the size (MN_PLAN_ONLY runs it with no device); mn_init needs the pool's size from it and HIP
     * (streams, hipHostRegister), nothing of rns_init.  The self-tests stay after rns_init. */
    int mn_size_ = 1; double t_pre = 0, t_mn = 0, t_st = 0;
    {
        binsplit_shmem_pool_rule(N, getenv("COMM_SIZE") ? atoi(getenv("COMM_SIZE")) : 1, 0);   /* the pool from the model */
        t_pre = mem_now() - t00;                      /* ECALC_INIT_TL=1: everything between the clock's start and the transport (terms, plane cap, memory fit, pool rule) */
        t_mn = mem_now(); mn_size_ = mn_init(); t_mn = mem_now() - t_mn;   /* a node-process among COMM_SIZE; the meshes are opened here */
        if (verbose >= 2 && mn_size_ > 1) printf("      init: the transport opened before rns_init (%.2f s)\n", t_mn);
    }
    mem_sampler_start();                            /* ECALC_MEM_SAMPLE=<seconds> */
    double t_ri = mem_now(); rns_init(pool_log); t_ri = mem_now() - t_ri;
    t_st = mem_now();
    if (mn_size_ > 1 && !mn_selftest(11, 11, verbose >= 2)) { printf("VERIFY FAILED\n"); return 1; }
    t_st = mem_now() - t_st;
    int mn_dist = mn_size_ > 1;                      /* M3: the top levels as distributed products */
    if (mn_dist && !mn_selftest_layered(11, 11, verbose >= 2)) { printf("VERIFY FAILED\n"); return 1; }
    if (getenv("ECALC_BG_THREADS")) g_bg_threads = atoi(getenv("ECALC_BG_THREADS"));
    if (vpe_on()) {                                  /* the process-wide settings of the PE flow, once, before the flow */
        if (mn_size_ < 2) ec_fatal(EC_RC_FATAL, "ecalc: ECALC_PE_PER_GPU=1 needs at least 2 PEs (%d): run on 2+ nodes or more tasks per node\n", mn_size_);
        g_bg_threads = env_vpe_bg_threads();
        if (ec_team_on()) { ec_team_share = vpe_lanes(); if (ec_team() < g_bg_threads) g_bg_threads = ec_team(); }   /* A1: with =mask a PE's teams are its share of the node (or OMP_NUM_THREADS), bg included */
        mn_out_bg_threads(g_bg_threads);
        if (vpe_ppg()) printf("ppg: %s: background team %d threads (this process's affinity mask of %d CPUs; OMP max %d), device %d\n", ec_log_prefix(), g_bg_threads, g_bg_threads, omp_get_max_threads(), vpe_device());   /* S2: one line per process (the mask is the PE's own) */
        if (!getenv("COMM_RANK") || atoi(getenv("COMM_RANK")) == 0)
            printf("vpe: %d PEs = %d per node x %d node%s; background team %d threads per PE (%d CPUs / %d PEs per node; OMP max %d); two part files per PE\n",
                   vpe_npe(), vpe_lanes(), vpe_nnodes(), vpe_nnodes() > 1 ? "s" : "", g_bg_threads, omp_get_num_procs(), vpe_lanes(), omp_get_max_threads());
        bs_ckpt_dir = getenv("BS_CKPT_DIR"); if (bs_ckpt_dir && !*bs_ckpt_dir) bs_ckpt_dir = 0;
        if (ckpt_top_mode() && !bs_ckpt_dir && outfile) bs_ckpt_dir = mn_out_ckpt_default(outfile);   /* the one .top directory of all PEs (names are per PE, binsplit.c) */
        { int cp = getenv("ECALC_CORR_PATCH") ? atoi(getenv("ECALC_CORR_PATCH")) : 2; newton_x_defer = cp >= 2; }   /* (as the flow's size > 1 rule) */
        mn_hooks_set(0, outfile);
    }
    if (ec_team_on() && (!getenv("COMM_RANK") || atoi(getenv("COMM_RANK")) == 0))   /* A1: ECALC_OMP_TEAM=mask (nothing printed with it off) */
        printf("omp team: ECALC_OMP_TEAM=mask (rule: %s): affinity mask %d CPUs, omp_get_max_threads %d, seed threads %d, bs team %d, residue team %d, bg threads %d\n",
               ec_team_rule(), ec_team_mask_count(), omp_get_max_threads(), bs_seed_team_size(), bs_team_size(), ec_team(), g_bg_threads);
    struct flow_ctx fc; memset(&fc, 0, sizeof fc);
    fc.N = N; fc.d = d; fc.d_out = d_out; fc.outfile = outfile; fc.verbose = verbose; fc.pool_log = pool_log; fc.mn_size = mn_size_; fc.t00 = t00; fc.t_ri = t_ri; fc.t_pre = t_pre; fc.t_mn = t_mn; fc.t_st = t_st; fc.t_age = tl_age;
    vpe_run(flow_run, &fc);                          /* off: once on this thread (the off path's flow, as before); process mode: this process's PE */
    int rc = flow_rc_all();
    if (vpe_on()) rns_shutdown();                    /* (off: the flow's end did it) */
    return rc;
}
/* the flow of one rank (off) or one PE (ECALC_PE_PER_GPU): bs, the reciprocal and division, the output stage */
static void flow_run(void *a)
{
    struct flow_ctx *fc = (struct flow_ctx *)a;
    const unsigned long N = fc->N, d = fc->d, d_out = fc->d_out; const char *outfile = fc->outfile;
    const int verbose = fc->verbose, pool_log = fc->pool_log, mn_size_ = fc->mn_size, mn_dist = mn_size_ > 1; const double t00 = fc->t00, t_ri = fc->t_ri;
    double t;
    if (vpe_on()) vpe_set_range(N);                  /* this PE's term range [a0, b1) of N over the PEs */
    if (mn_size_ > 1) printf("mn: %s %d computes terms [%lu, %lu) of %lu\n", who(), mn_rank(), bs_a0, bs_b1, N);
    double t_pg = mem_now(); binsplit_pregrow(N); t_pg = mem_now() - t_pg;   /* WP3: region pools at init, like the device pools */
    if (getenv("ECALC_INIT_ONLY") && atoi(getenv("ECALC_INIT_ONLY"))) { vpe_node_barrier(); MEMREP("init"); if (vpe_node_leader()) printf("ECALC_INIT_ONLY: the pools and the arena of %lu digits are mapped (%.1f s); stopping\n", d, mem_now() - t00); fflush(stdout); _exit(0); }   /* the ceiling probe (the allocation edge in seconds) */
    double t_init = mem_now() - t00;
    { const char *tl = getenv("ECALC_INIT_TL"); int tv = tl ? atoi(tl) : 0;   /* ECALC_INIT_TL=1 (print only): the init figure as its parts, which sum to it; the engine's own steps are the `tl ... xshmem:` lines.  =2: every node prints */
      if (tv > 0 && (mn_rank() == 0 || (tv >= 2 && vpe_node_leader()))) {
          printf("tl          %s %d: process age at main %.2f s (/proc starttime against boot time, 10 ms; it precedes the init clock)\n", who(), mn_rank(), fc->t_age);
          printf("tl          %s %d: init %.2f s = plan/fit/pool rule before the transport %.2f + mn_init %.2f + rns_init %.2f + mn_selftest %.2f + region pools %.2f + other %.2f (other: seeds, sampler, term ranges)\n",
                 who(), mn_rank(), t_init, fc->t_pre, fc->t_mn, t_ri, fc->t_st, t_pg, t_init - fc->t_pre - fc->t_mn - t_ri - fc->t_st - t_pg); }
      else if (tv > 0 && fc->t_age > 30.0) printf("tl          %s %d: LATE START: process age at main %.2f s\n", who(), mn_rank(), fc->t_age); }
    if (verbose >= 2 && vpe_node_leader()) printf("      init: rns_init %.2f s, region pools %.2f s, the rest %.2f s\n", t_ri, t_pg, t_init - t_ri - t_pg);   /* A-mem */
    if (vpe_node_leader()) RESULT("init", "s", t_init);   /* one per node (a global figure) */
    if (vpe_proc_leader()) {                         /* S2: each PE's own process line */
    printf("      VmRSS %.1f GB after init (staging %.1f GB pinned + device regions %.0f GB); init %.1f s\n", mem_vmrss() / 1e9, rns_staging_bytes() * 4 / 1e9, mem_dev_pool_bytes() / 1e9, t_init); }
    if (getenv("MEM_REPORT_DEVS") && mn_size_ > 1 && vpe_proc_leader()) {   /* B7ACCT: the layout's terms outside "in use" on the per-APU lines (a report only) */
        const char *eo = getenv("COMM_OFI_POOL_MB"); size_t ofi = eo ? (size_t)atol(eo) << 20 : 0;
        mem_report_outside(0, ofi, comm_layered_vbytes);   /* X2: the v-slots are in the comm pool (ofi) */ }
    MEMREP("init");                               /* device and host bytes by category at each phase boundary (process mode: every PE) */
    budget_check(N, verbose);                     /* ECALC_BUDGET_CHECK=1 -- every rank stops here when any node is over */
    bs_verbose = verbose >= 2;
    if (!vpe_on()) {                                  /* (PE layout: set once in main: one process-wide value) */
    bs_ckpt_dir = getenv("BS_CKPT_DIR");                                             /* the top-level set's directory (ECALC_CKPT_TOP) */
    if (bs_ckpt_dir && !*bs_ckpt_dir) bs_ckpt_dir = 0; }
    int ovl = mn_size_ == 1;                       /* the overlapped flow (device top levels, device dm): size 1; size > 1 runs M3's distributed flow */
    bigint P, Q, X;
    bi_init(&P); bi_init(&Q); bi_init(&X);
    dbig Xdev; db_init(&Xdev); mdb Xm; memset(&Xm, 0, sizeof Xm);   /* B1: X on the device (size 1) / sharded (the distributed division) until the output stage has written it */
    struct pq_bg pqb; memset(&pqb, 0, sizeof pqb); pqb.N = N; pqb.a0 = bs_a0; pqb.b1 = bs_b1 ? bs_b1 : N + 1;   /* M5: the T1 recurrence over this node's terms */
    struct x_bg xb; memset(&xb, 0, sizeof xb); xb.d = d; xb.d_out = d_out; xb.outfile = outfile; xb.verbose = verbose >= 2;
    struct out_ctx oc; memset(&oc, 0, sizeof oc); oc.N = N; oc.d = d; oc.d_out = d_out; oc.outfile = outfile; oc.verbose = verbose; oc.size = mn_size_; oc.rank = mn_rank(); g_ocp = &oc;
    { int cp = getenv("ECALC_CORR_PATCH") ? atoi(getenv("ECALC_CORR_PATCH")) : 2;   /* default 2 */   /* 1 = size 1 (the writer runs before the corrections), 2 = also size > 1 */
      if (!vpe_on()) newton_x_defer = (ovl && cp >= 1) || (mn_size_ > 1 && cp >= 2);   /* (PE layout: set in main) */
      oc.zone = getenv("ECALC_CORR_PATCH_ZONE") ? strtoul(getenv("ECALC_CORR_PATCH_ZONE"), 0, 10) : 4096;
      if (newton_x_defer) xb.t2_defer = oc.zone; }
    oc.pqb = &pqb; oc.xb = &xb; oc.t00 = t00; oc.t_init = t_init;
    if (mn_dist) bs_keep_dev = 1;                     /* M3: the leaf's P_r, Q_r stay on the device when the top leaf level ran there */
    if (ovl) { bs_after_seeds_hook = pq_bg_start; bs_hook_arg = &pqb; bs_keep_dev = 1; }
    else if (mn_size_ > 1) { bs_after_seeds_hook = pq_bg_start; bs_hook_arg = &pqb; }   /* M5: every node runs its range's recurrence during its leaf levels */


    comm_wst_set_phase(WST_BS);   /* D3: MN_WAIT_STATS phase */
    t = mem_now(); binsplit_e(&P, &Q, N); double t_bs = mem_now() - t;
    if (getenv("ECALC_TEST_DIE_PE") && atoi(getenv("ECALC_TEST_DIE_PE")) == vpe_pe() && vpe_on())   /* test hook (S2 kill test; off unless set): this PE dies at a fixed phase, after bs, with its peers about to enter the tree's collectives */
        ec_fatal(EC_RC_FATAL, "ecalc: ECALC_TEST_DIE_PE=%d: this PE stops here on purpose (kill test)\n", vpe_pe());
    /* the top-level P, Q on disk for ECALC_RECHECK -- default on above 10^10 digits with an outfile
     * (ECALC_CKPT_TOP=0: off; =1: on at any size), into BS_CKPT_DIR or, unset, <outfile>.top.  Size 1: the level-0 tree set,
     * written in the background by bs_ckpt_bg_* (binsplit.c); size > 1: mn_tree's top tree set.  No other checkpoint exists
     * (no restart inside a run: the user's decision D11, 2026-10-12) */
    /* the default is OFF.  Agent W measured the 35.6 GB top set as hidden (0-1 s of wall) on disks
     * writing at 1.13-1.59 GB/s, but on aac6's slower path it runs at 0.31 GB/s: 113 s of background write that the
     * division waits for (dm 28 -> 113 s, the 4e10 wall 81 -> 164 s, regression job 20964).  The set is what makes
     * ECALC_RECHECK possible for a finished run, so it stays one switch away: ECALC_CKPT_TOP=1. */
    int ckpt_top = ckpt_top_mode();                 /* off for record runs; ECALC_CHECKPOINT=1 (development and testing) = the budgeted top set (=2); ECALC_CKPT_TOP overrides both */
    double ckpt_slack = getenv("ECALC_CKPT_TOP_SLACK") ? atof(getenv("ECALC_CKPT_TOP_SLACK")) : 1.0;   /* ECALC_CKPT_TOP=2's budget per release */
    bs_ckpt_bg *topbg = 0; int topq_held = 0;       /* the size-1 top set's writer; Q held past the division for it */
    /* the reciprocal and the division over the sharded P, Q at size > 1.  The residues of P, Q, R come from the sharded kernels;
     * X stays sharded and every node writes its part (A-out) */
    int mn_dm = mn_dist;
    uint64_t Pres[T1_NQ], Qres[T1_NQ], Rres[T1_NQ];
    double t_recip = 0, t_10dp = 0, t_dm = 0; size_t mn_pn = 0, mn_qn = 0, mn_xn = 0;
    if (mn_dist) {                                  /* M3: the top log2(size) levels as distributed products over node groups */
        double tg = mem_now(); dbig Pl, Ql; db_init(&Pl); db_init(&Ql);
        if (bs_Pd.n) { Pl = bs_Pd; Ql = bs_Qd; memset(&bs_Pd, 0, sizeof bs_Pd); memset(&bs_Qd, 0, sizeof bs_Qd); }
        else { db_from_bi(&Pl, &P); db_from_bi(&Ql, &Q); }
        printf("mn: %s %d leaf P %zu limbs, Q %zu limbs (%s)\n", who(), mn_rank(), Pl.n, Ql.n, P.n ? "host, copied in" : "device");
        if (ckpt_top && !bs_ckpt_dir && outfile) { bs_ckpt_dir = mn_out_ckpt_default(outfile); printf("mn: %s %d: the top tree set -> %s (ECALC_CKPT_TOP)\n", who(), mn_rank(), bs_ckpt_dir); }
        mn_ckpt_bg_mode = !ckpt_top ? 0 : ckpt_top == 2 ? 2 : 1; mn_ckpt_slack = ckpt_slack;   /* the top tree set in the background */
        mdb Pm, Qm; mn_tree(&Pm, &Qm, &Pl, &Ql);
        double tt = mem_now();
        {                                           /* M4: the division over shares; X stays sharded (A-out) */
            printf("mn: %s %d: tree levels %.2f s: P %zu limbs, Q %zu limbs\n", who(), mn_rank(), tt - tg, Pm.n, Qm.n);
            t_bs += tt - tg; mn_pn = Pm.n; mn_qn = Qm.n;
            MEMREP("tree");                     /* the tree's slabs and shares should show under the pool's donated bytes, not hipMalloc */
            P.n = Q.n = 0; binsplit_free_pools();
            if (vpe_proc_leader()) { memset(&newton_st, 0, sizeof newton_st); memset(&rns_st, 0, sizeof rns_st); }   /* (each process resets its own) */
            int L = 0; while ((1 << L) < mn_size_) L++;
            mn_group *G = mn_group_at(L);
            double td = mem_now(); comm_wst_set_phase(WST_DM);   /* D3 */
            if (!vpe_on()) mn_hooks_set(&oc, outfile);   /* (PE layout: the hooks were set in main and stay: they find the PE's oc by g_ocp) */
            newton_mn_divmod(&Xm, &Pm, &Qm, (d + 17) / 18, G, t1_q, T1_NQ, Pres, Qres, Rres, &t_recip);
            if (!vpe_on()) { newton_mn_x_hook = 0; newton_mn_xhi_hook = 0; newton_mn_xlo_hook = 0; }
            if (oc.hi && !oc.hi_rel) { mn_out_early_lo_release(oc.early, &Xm, mn_comm(0)); oc.hi_rel = 1; }   /* X_lo0 >= B^s -- X_lo now corrected (every rank: collective) */
            t_dm = mem_now() - td; mn_xn = Xm.n;
            if (vpe_proc_leader()) mn_wait_stats_print(); comm_wst_set_phase(WST_OTHER);   /* D3: one line per process (S1: per node), then the rest of the run is 'other' */
            newton_db_free_scratch(); oc.Xm = &Xm;   /* B1 (H): X stays sharded; the output stage reads this node's share in place (the block pool is released after it) */
            memcpy(oc.Pres, Pres, sizeof Pres); memcpy(oc.Qres, Qres, sizeof Qres); memcpy(oc.Rres, Rres, sizeof Rres);   /* every node's own residues (the sharded kernels) -- the non-zero ranks go to the output stage from here */
            oc.ncorr = (int)(newton_st.down_corr + newton_st.up_corr); oc.t_bs = t_bs; oc.t_dm = t_dm;
            oc.defer = newton_x_defer; oc.dx = newton_x_dx;
            printf("mn: %s %d: dm over %d %s %.2f s (reciprocal %.2f), X %zu limbs, sharded%s\n", who(), mn_rank(), mn_size_, vpe_on() ? "PEs" : "nodes", t_dm, t_recip, mn_xn, mn_rank() ? "; to the output stage" : "");
            if (mn_rank() != 0) { int f = out_stage(&oc); flow_end(f, 0); return; }   /* M5 + A-mem: every node writes its part of X and checks its residues; its device memory goes before the final barrier (after the top set's writer) */
        }
    }
    if (!mn_dm && !bs_Pd.n) { ec_fatal(EC_RC_FATAL, "ecalc: the top level left no device P, Q\n"); }   /* size 1: binsplit hands P, Q over as device numbers whatever tier ran the top level */
    if (mn_size_ == 1 && ckpt_top) {                    /* the top-level P, Q as a tree set (level 0) for ECALC_RECHECK at size 1 */
        if (!bs_ckpt_dir) bs_ckpt_dir = mn_out_ckpt_default(outfile);
        if (!bs_ckpt_dir) printf("      checkpoint: ECALC_CKPT_TOP needs BS_CKPT_DIR or an outfile -- the top-level set is not written\n");
        else {                                          /* in the background (P under the reciprocal, Q under the division and the output stage) */
            uint64_t desc[10] = { bs_Pd.n, bs_Pd.n, 0, 1, bs_Pd.n, bs_Qd.n, bs_Qd.n, 0, 1, bs_Qd.n };
            topbg = bs_ckpt_bg_start(0, N, desc, &bs_Pd, &bs_Qd, ckpt_top == 2, ckpt_slack);
        }
    }
    printf("bs    %8.2f s   N %lu, P %zu limbs, Q %zu limbs (seeds %.1f school %.1f batch %.1f mdev %.1f; pool %.1f GB; dev pools %.1f GB)   VmRSS %.1f GB, VmHWM %.1f GB\n",
           t_bs, N, mn_dm ? mn_pn : bs_Pd.n, mn_dm ? mn_qn : bs_Qd.n, bs_st.t_seed, bs_st.t_school, bs_st.t_batch, bs_st.t_mdev, bs_st.peak_pool_limbs * 8e-9, mem_dev_pool_bytes() / 1e9, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
    RESULT("bs", "s", t_bs);
    if (bs_st.n_grow) printf("      bs: region pools grew %d times inside the phase (%.1f GB of hipMalloc)\n", bs_st.n_grow, bs_st.grow_bytes / 1e9);
    MEMREP("bs");
    binsplit_free_pools();

    size_t dl = (d + 17) / 18;                        /* limbs of 10^d */
    if (mn_dm) {                                      /* M4: the dm phase ran over the nodes above; the phase lines for the summary */
        printf("recip %8.2f s   (over %d %s; %zu iterations)\n", t_recip, mn_size_, vpe_on() ? "PEs" : "nodes", newton_st.iters);
        printf("10dP  %8.2f s   (S = P + Q over shares, inside dm)\n", 0.0); RESULT("10dP", "s", 0.0);
        printf("dm    %8.2f s   X %zu limbs (recip %.1f s; corrections %zu/%zu; over %d %s)   VmRSS %.1f GB, VmHWM %.1f GB\n",
               t_dm, mn_xn, t_recip, newton_st.down_corr, newton_st.up_corr, mn_size_, vpe_on() ? "PEs" : "nodes", mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
        RESULT("dm", "s", t_dm);
    } else {
    /* dm part 1: the reciprocal of Q first (memory peak).  I3: P, Q stay on the device -- residues by kernel, S = P + Q in place,
     * A = S B^dl implicit */
    t = mem_now();
    memset(&newton_st, 0, sizeof newton_st); memset(&rns_st, 0, sizeof rns_st);
    rns_release_staging();                            /* nothing between here and dm needs the staging */
    double tr = mem_now();
    db_mod_qs(&bs_Pd, t1_q, T1_NQ, Pres); db_mod_qs(&bs_Qd, t1_q, T1_NQ, Qres);
    { size_t don = 0; for (int dv = 0; dv < 4; dv++) don += rns_dpool_donate_tail(dv, 1, rns_pool1_default_bytes(pool_log)); if (verbose >= 2) printf("      I3: plane pool tails donated: %.1f GB\n", don / 1e9); }   /* the dist tier uses 3 q (+16 limbs) of pool 1; nothing to donate when pool 1 is sized to that (C4) */
    double t_res3 = mem_now() - tr;
    size_t k_mu = bs_Pd.n + 1 + dl - bs_Qd.n + 1;    /* S has at most one limb more than P */
    newton_db_Qd = &bs_Qd; newton_db_mu_host = 0;
    /* (with the tail layout t1's quarter lands in the arena's reserved tail and the pool never grows; the line below still reports any in-phase growth) */
    size_t hm0 = 0; for (int dv = 0; dv < 4; dv++) hm0 += db_pool_hipmalloc_bytes(dv);
    { bigint MU; bi_init(&MU); newton_db_recip(&MU, &Q, k_mu); bi_free(&MU); }
    size_t hm1 = 0; for (int dv = 0; dv < 4; dv++) hm1 += db_pool_hipmalloc_bytes(dv);
    if (hm1 > hm0) printf("      recip: the block pool grew by %.1f GB of hipMalloc inside the phase\n", (hm1 - hm0) / 1e9);   /* what C3's sizing missed */
    t_recip = mem_now() - t;
    printf("recip %8.2f s   mu %zu limbs (%zu iterations)   VmRSS %.1f GB, VmHWM %.1f GB\n", t_recip, k_mu + 1, newton_st.iters, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
    mem_report("recip");

    t = mem_now();
    bs_ckpt_bg_release(topbg, 0);                     /* the writer is past P (or, budgeted, has dropped the set) before P is overwritten */
    db_add(&bs_Pd, &bs_Pd, &bs_Qd);                   /* S = P + Q on the device; A = S B^dl is never formed */
    t_10dp = mem_now() - t;
    printf("      I3: P, Q residues by kernel %.2f s (before the reciprocal), S = P + Q on device %.2f s; no host A\n", t_res3, t_10dp);
    printf("10dP  %8.2f s   T %zu limbs, A %zu limbs   VmRSS %.1f GB, VmHWM %.1f GB\n", t_10dp, dl, bs_Pd.n + dl, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
    RESULT("10dP", "s", t_10dp);

    t = mem_now();
    memset(&rns_st, 0, sizeof rns_st);
    newton_db_x_hook = x_bg_hook; newton_db_x_arg = &xb;   /* O4: the hook starts the residues and the chunked writer (M5) on X before the low product */
    newton_db_x_dev = &Xdev; xb.Xd = &Xdev; oc.Xd = &Xdev;   /* X never leaves the device -- the writer and the residues read it there */
    if (newton_x_defer) { newton_db_xhi_hook = x_hi_hook; newton_db_xlo_hook = x_lo_hook; }   /* the writer on X_hi after DKM's step 1 (needs the deferred corrections) */
    newton_db_divmod_shifted(&X, &bs_Pd, dl, &bs_Qd, t1_q, T1_NQ, Rres); db_free(&bs_Pd);
    newton_db_x_hook = 0; newton_db_Qd = 0; newton_db_x_dev = 0; newton_db_xhi_hook = 0; newton_db_xlo_hook = 0;
    if (g_xhi.on && !g_xhi.released) x_hi_release(&xb);   /* X_lo0 >= B^s -- X_lo now corrected in place */
    if (topbg && !bs_ckpt_bg_done(topbg, 1)) topq_held = 1;   /* the writer still reads Q: its block is held until after the output stage */
    else db_free(&bs_Qd);
    mem_report("division");                           /* the pool at the end of the division, before it is released (its growth inside the phase and the peak) */
    newton_db_free_scratch();     /* (X lives in the block pool until the output stage has written it: released after out_stage) */
    t_dm = mem_now() - t + t_recip;
    printf("dm    %8.2f s   X %zu limbs (recip %.1f s; corrections %zu/%zu)%s   VmRSS %.1f GB, VmHWM %.1f GB\n",
           t_dm, g_xhi.on ? g_xhi.s + g_xhi.Xh.n : Xdev.n, t_recip, newton_st.down_corr, newton_st.up_corr, g_xhi.on ? "; X on the device as X_hi B^s + X_lo (MN_OUT_DKM_HI)" : "; X on the device", mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
    mem_report("dm");
    RESULT("dm", "s", t_dm);
    }

    /* the output stage (M5 + C1) -- T1 from the residues, the digits streamed to the file in chunks, T2 */
    memcpy(oc.Pres, Pres, sizeof Pres); memcpy(oc.Qres, Qres, sizeof Qres); memcpy(oc.Rres, Rres, sizeof Rres);
    oc.ncorr = (int)(newton_st.down_corr + newton_st.up_corr); oc.t_bs = t_bs; oc.t_10dp = t_10dp; oc.t_dm = t_dm;
    if (mn_size_ == 1) { oc.defer = newton_x_defer && xb.started; oc.dx = oc.defer ? newton_x_dx : 0; }   /* the size-1 writer started before the corrections (size > 1: set after newton_mn_divmod) */
    bi_free(&Q);
    int fail = out_stage(&oc);
    if (topbg) {                                      /* the top set's writer -- Q released (held so far if it was still needed), then joined */
        bs_ckpt_bg_release(topbg, 1); if (topq_held) db_free(&bs_Qd);
        double tw = 0, tq = 0; bs_ckpt_bg_join(topbg, "      ", bs_ckpt_dir, &tw, &tq); topbg = 0;
        RESULT("ckpt_top", "s", tw); RESULT("ckpt_top_wait", "s", tq);
    }
    flow_end(fail, 1);
}

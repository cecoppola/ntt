/* ecalc - e to d decimal digits on one MI300A node, the paper's pipeline.
 *
 *   bs    P, Q = binary splitting of sum 1/k!, N = min{m : lgamma(m+1)/ln10 >= d+50}
 *   10dP  T = 10^d (5^d by squaring, shifted), A = T (P + Q)
 *   dm    X, R = divmod(A, Q) by Newton reciprocal + Barrett
 *   T1    residue identities mod eight 62-bit primes
 *   dc    digits = todec(X), "2" followed by d fractional digits
 *   T2    digit windows vs known values; digits == X mod q
 *
 * Usage: ecalc <digits> [outfile]      env: POOL_LOG (31), NTT_B16_STG (7),
 *        PW_FUSE (14), RNS_CRT_LAYOUT (0), ECALC_VERBOSE (1)
 * Prints META/RESULT lines (phase seconds, VmHWM) as the benchmarks do.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "bigint.h"
#include "rns_mul.h"
#include "newton.h"
#include "binsplit.h"
#include "todec.h"
#include "verify.h"
#include "mem.h"
#include "dbig.h"
#include "mn.h"
#include "mn_out.h"
#include <pthread.h>
#include <semaphore.h>
#include <omp.h>
#include "ntt.h"
#include <math.h>

static void meta_line(const char *bench)
{
    char host[128] = "?", date[64]; time_t t = time(0);
    gethostname(host, sizeof host); strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", localtime(&t));
    printf("META bench=%s host=%s date=%s pool_log=%d stg=%d pw_fuse=%d body=%d engine=%d base=%s\n", bench, host, date, rns_pool_log(), ntt_stg, ntt_pw_fuse, ntt_b16_body, rns_engine, bi_decimal ? "10^18" : "2^64");
}
#define RESULT(name, unit, v) printf("RESULT ecalc %s %s %.6g\n", name, unit, (double)(v))

/* T = 10^d: decimal limbs: B^(d/18) x 10^(d%18); binary: 5^d by powering, then << d */
static void pow10_big(bigint *T, unsigned long d)
{
    if (bi_decimal) { bi_set_base_pow(T, d / 18); if (d % 18) bi_mul_pow10(T, T, (unsigned)(d % 18)); return; }
    bigint five, t2; bi_init(&five); bi_init(&t2);
    bi_set_u64(&five, 1);
    int top = 63; while (top > 0 && !((d >> top) & 1)) top--;
    for (int b = top; b >= 0; b--) {
        if (b != top) { rns_mul(&t2, &five, &five); bigint sw = five; five = t2; t2 = sw; }
        if ((d >> b) & 1) bi_mul_u64(&five, &five, 5);
    }
    bi_shl(T, &five, d);
    bi_free(&five); bi_free(&t2);
}


/* ---- Phase 8 (PLAN.md 18): overlap of disjoint work, ECALC_OVERLAP=1.  Background CPU work runs in pthreads with
 * a bounded OpenMP team while the GPUs run the tiers; each joins where its result is first needed. ---- */
static int g_overlap = 1, g_bg_threads = 48;   /* ECALC_OVERLAP=0: the sequential flow (RESULTS.md 68: 128.8 vs 112.1 s) */   /* ECALC_OVERLAP_COPY=1: P, Q copied out inside the background thread (the DMA then contends with the reciprocal); 0: before it */
struct pq_bg { unsigned long N, a0, b1; uint64_t p[T1_NQ], qq[T1_NQ]; pthread_t th; int started; double t, t_grow; size_t grow; };   /* [a0, b1): this node's term range (M5: the recurrence is rank-local; size 1: [1, N+1)) */
static void *pq_bg_run(void *a) { struct pq_bg *b = (struct pq_bg *)a; double t0 = mem_now(); omp_set_num_threads(g_bg_threads);
    for (int i = 0; i < T1_NQ; i++) vf_pq_range_mod(b->a0, b->b1, t1_q[i], &b->p[i], &b->qq[i]); b->t = mem_now() - t0;
    t0 = mem_now();
    if (b->grow) for (int dv = 0; dv < 4; dv++) db_pregrow(dv, b->grow);   /* the dm phase's block pool beyond the donated regions, allocated while the GPUs run the levels */
    b->t_grow = mem_now() - t0;
    return 0; }
static void pq_bg_start(void *a) { struct pq_bg *b = (struct pq_bg *)a; if (b->started) return; pthread_create(&b->th, 0, pq_bg_run, b); b->started = 1; }   /* O2: after the seeds */

/* O4 (Phase 9 A-out, M5 + C1): X's residues and the digits -- formatted in chunks and written as they are formatted
 * (mn_out.c), no whole digit string on the host -- in the background during the low product, from the hook that
 * gets X on the host before it */
struct x_bg { bigint *X; unsigned long d, d_out; const char *outfile; uint64_t Xres[T1_NQ]; int verbose; pthread_t th; int started; double t_res; mn_out o; sem_t res_ready; };   /* res_ready: X's residues are in (T1 needs only those; the writer runs on) */
static void x_bg_writer(struct x_bg *b)                /* the chunked writer over the host X (also the redo after a correction) */
{
    mn_out_src src = { b->X->l, 0, 0, b->X->n };
    memset(&b->o, 0, sizeof b->o); b->o.d = b->d; b->o.d_out = b->d_out; b->o.outfile = b->outfile; b->o.rank = 0; b->o.size = 1; b->o.verbose = b->verbose;
    mn_out_run(&b->o, &src);
}
static void *x_bg_run(void *a)
{
    struct x_bg *b = (struct x_bg *)a; omp_set_num_threads(g_bg_threads);
    double t0 = mem_now();
    for (int i = 0; i < T1_NQ; i++) b->Xres[i] = vf_limbs_mod(b->X->l, b->X->n, t1_q[i]);
    b->t_res = mem_now() - t0; sem_post(&b->res_ready);
    x_bg_writer(b);
    return 0;
}
static void x_bg_hook(bigint *X, void *a) { struct x_bg *b = (struct x_bg *)a; b->X = X; sem_init(&b->res_ready, 0, 0); pthread_create(&b->th, 0, x_bg_run, b); b->started = 1; }

/* ---- the output stage (Phase 9 A-out: PLAN.md 19, M5 + C1).  Every node: T1 with the P, Q recurrence over its own
 * term range joined across the nodes (P = P_A Q_B + P_B, Q = Q_A Q_B in node order) and the residues of its share of
 * X (device kernel, placed at the share's offset and summed over the nodes); its share of the digits formatted in
 * chunks, residue-checked (Horner over the chunks and the nodes), T2-windowed and written to its part file as it
 * goes; node 0 the summary.  Size 1: the same code over the host X (the writer ran in the background from the hook).
 * Until A-div lands, X exists on node 0's host after its division and is scattered here as a stand-in for the
 * sharded X of the distributed division (the HOOK comments below say what plugs in). */
struct out_ctx {
    unsigned long N, d, d_out; const char *outfile; int verbose, size, rank;
    bigint *X;                                     /* size 1 / node 0: X on the host after the division (empty elsewhere) */
    uint64_t Pres[T1_NQ], Qres[T1_NQ], Rres[T1_NQ];   /* node 0's residues of P, Q, R (stand-in: broadcast; A-div: per share + combine) */
    struct pq_bg *pqb; struct x_bg *xb; int ncorr;   /* the recurrence thread; the size-1 writer thread; corrections to X after the hook */
    double t00, t_init, t_bs, t_10dp, t_dm;
};
static void node_pfx(const struct out_ctx *c) { if (c->size > 1) printf("mn: node %d: ", c->rank); }
static int out_stage(struct out_ctx *c)
{
    comm *cm = mn_comm(0); int multi = c->size > 1;
    double t = mem_now();
    /* T1 (a): P, Q mod q over this node's terms, joined over the nodes */
    uint64_t pr[T1_NQ], qr[T1_NQ], Pg[T1_NQ], Qg[T1_NQ];
    if (c->pqb->started) { pthread_join(c->pqb->th, 0); memcpy(pr, c->pqb->p, sizeof pr); memcpy(qr, c->pqb->qq, sizeof qr); }
    else for (int i = 0; i < T1_NQ; i++) vf_pq_range_mod(c->pqb->a0, c->pqb->b1, t1_q[i], &pr[i], &qr[i]);
    mn_out_pq_combine(cm, pr, qr, Pg, Qg);
    /* the share of X this node holds */
    mn_out_src src; memset(&src, 0, sizeof src); dbig xsh; db_init(&xsh); size_t xn = c->X->n;
    if (!multi) { src.host = c->X->l; src.lo = 0; src.cnt = c->X->n; }
    else {
        /* HOOK A-div: with the distributed division X is an mdb over all nodes: src.dev = &Xm.sh, mdb_share(&Xm, rank, &lo, &hi),
         * src.lo = lo, src.cnt = hi - lo, xn = Xm.n -- and the stand-in scatter below goes */
        double ts = mem_now(); size_t lo, cnt;
        mn_out_scatter_standin(cm, c->X->l, c->X->n, &xsh, &lo, &cnt, &xn);
        src.dev = &xsh; src.lo = lo; src.cnt = cnt;
        if (c->rank == 0) { free(c->X->l); c->X->l = 0; c->X->n = c->X->cap = 0; }
        node_pfx(c); printf("X share [%zu, %zu) of %zu limbs on the device (stand-in scatter from node 0: %.2f s)\n", lo, lo + cnt, xn, mem_now() - ts);
    }
    /* T1 (b): the residues of P, Q, R.  HOOK A-div: from the mdb shares -- db_mod_qs(&Pm.sh) etc. with sh.n = the share length,
     * then mn_out_res_combine(cm, res, lo, out) for each; the stand-in broadcasts node 0's */
    if (multi) { mn_out_bcast_u64(cm, c->Pres, T1_NQ, 0); mn_out_bcast_u64(cm, c->Qres, T1_NQ, 0); mn_out_bcast_u64(cm, c->Rres, T1_NQ, 0); }
    /* T1 (c): X's residues: the share's, placed at its offset, summed over the nodes (size 1: the background thread's) */
    uint64_t xs[T1_NQ], Xres[T1_NQ]; int xres_bg = !multi && c->xb->started && !c->ncorr, joined = 0;
    if (xres_bg) { sem_wait(&c->xb->res_ready); memcpy(xs, c->xb->Xres, sizeof xs); }   /* the writer runs on: the file write is not on the timed path (as before, when the write came after `total`) */
    else if (!multi && c->xb->started) { pthread_join(c->xb->th, 0); joined = 1; mn_out_res_share(&src, xs); }
    else mn_out_res_share(&src, xs);
    mn_out_res_combine(cm, xs, src.lo, Xres);
    bigint none; bi_init(&none);
    int bad1 = tier1_res_pq(c->N, c->d, c->Pres, c->Qres, &none, &none, Pg, Qg, Xres, c->Rres, c->verbose >= 2);
    double t_t1 = mem_now() - t;
    node_pfx(c); printf("T1    %8.2f s   %s%s\n", t_t1, bad1 ? "FAILED" : "ok: T(P+Q) == XQ + R and P, Q mod q for 8 primes", c->pqb->started ? (multi ? " (P, Q recurrence over this node's terms, overlapped with bs; joined over the nodes)" : " (P, Q recurrence overlapped with bs)") : "");
    if (c->pqb->started) { node_pfx(c); printf("      overlapped with bs: P, Q mod q recurrence %.2f s, block pool pregrown by %.0f GB in %.2f s\n", c->pqb->t, 4.0 * c->pqb->grow / 1e9, c->pqb->t_grow); }
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
        printf("paper A22 (4e10): 285.7 = bs 112.2 + 10dP 12.6 + dm 46.8 + T1 ~3 + dc 110.3\n");
    }
    /* the digits: the node's share of X in chunks -> its part file; residue and windows per chunk */
    t = mem_now();
    mn_out ow, *o = &ow; memset(&ow, 0, sizeof ow);
    if (!multi && c->xb->started) {
        o = &c->xb->o; if (!joined) pthread_join(c->xb->th, 0);
        if (c->ncorr) {                               /* X changed after the hook: the digits from the final X, the file rewritten */
            printf("      X corrected after the formatting started: redoing the digits\n");
            mn_out_finish(o); x_bg_writer(c->xb);
        }
    } else {
        o->d = c->d; o->d_out = c->d_out; o->outfile = c->outfile; o->rank = c->rank; o->size = c->size; o->verbose = c->verbose >= 2;
        mn_out_boundaries(o, &src, cm);
        mn_out_run(o, &src);
    }
    uint64_t Dres[T1_NQ]; mn_out_digit_res(o, cm, Dres);
    int bad2 = o->bad2, bad3 = tier1_digits_cmp(Dres, Xres, c->verbose >= 2);
    double t_dc = mem_now() - t;
    if (!multi) { free(c->X->l); c->X->l = 0; c->X->n = c->X->cap = 0; }
    db_free(&xsh);
    node_pfx(c); printf("dc    %8.2f s   digits [%zu, %zu) of %lu formatted from %zu decimal limbs in %d chunks%s (residues %.2f, format %.2f, digit residue %.2f, T2 %.2f, fetch %.2f, waiting for the writer %.2f)\n",
                        t_dc, o->k0, o->k1, c->d + 1, src.cnt, o->nchunks, !multi && c->xb->started ? " (streamed with the low product; joined after total)" : "", !multi && c->xb->started ? c->xb->t_res : 0.0, o->t_fmt, o->t_res, o->t_t2, o->t_fetch, o->t_wait);
    if (!multi) RESULT("dc", "s", t_dc);
    node_pfx(c); printf("T2    %8.2f s   windows %s (%d checked%s), digits == X mod q %s%s\n", 0.0, bad2 ? "FAILED" : "ok", o->nwin, multi ? " on this node" : "", bad3 ? "FAILED" : "ok", !multi && c->xb->started ? " (overlapped)" : "");
    if (!multi) RESULT("T2", "s", 0.0);
    node_pfx(c); printf("digits: %s...%s%s\n", o->first, o->last, multi ? " (this node's range)" : "");
    if (multi && c->rank == 0) {
        total = mem_now() - c->t00; phases = c->t_bs + c->t_10dp + c->t_dm + t_t1 + t_dc;
        printf("total %8.2f s   (bs %.1f + 10dP %.1f + dm %.1f + T1 %.1f + dc %.1f + T2 %.1f = %.1f; init %.1f; other %.1f); VmHWM %.1f GB\n",
               total, c->t_bs, c->t_10dp, c->t_dm, t_t1, t_dc, 0.0, phases, c->t_init, total - phases - c->t_init, mem_vmhwm() / 1e9);
        RESULT("total", "s", total); RESULT("phases", "s", phases); RESULT("other", "s", total - phases - c->t_init);
        RESULT("vmhwm", "GB", mem_vmhwm() / 1e9);
        printf("paper A22 (4e10): 285.7 = bs 112.2 + 10dP 12.6 + dm 46.8 + T1 ~3 + dc 110.3\n");
    }
    t = mem_now(); mn_out_finish(o);                  /* the last chunk's write */
    if (c->outfile) { node_pfx(c); if (multi) printf("wrote %s.part%04d (%.2f GB; write %.2f s in the writer thread, %.2f s after the checks)\n", c->outfile, c->size - 1 - c->rank, o->bytes / 1e9, o->t_write, mem_now() - t);
                      else printf("wrote %s (%.2f GB; write %.2f s in the writer thread, %.2f s after the checks)\n", c->outfile, o->bytes / 1e9, o->t_write, mem_now() - t); }
    int fail = bad1 || bad2 || bad3;
    node_pfx(c); printf("%s\n", fail ? "VERIFY FAILED" : "VERIFY OK");
    if (multi) { int any = mn_out_allreduce_or(cm, fail); if (c->rank == 0) printf("mn: all %d nodes: %s\n", c->size, any ? "VERIFY FAILED" : "VERIFY OK"); fail = any; }
    return fail;
}
static void binsplit_seeds_begin_v(void *a) { binsplit_seeds_begin((unsigned long)(uintptr_t)a); }
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: ecalc <digits> [outfile]\n"); return 2; }
    unsigned long d_out = strtoul(argv[1], 0, 10), d = d_out;   /* d: the digits computed; in decimal rounded up to a multiple of 18 (the requested digits are a prefix: floor(floor(10^d' e) / 10^(d'-d)) = floor(10^d e)); d_out: written and windowed */
    const char *outfile = argc > 2 ? argv[2] : 0;
    int verbose = getenv("ECALC_VERBOSE") ? atoi(getenv("ECALC_VERBOSE")) : 1;
    int pool_log = getenv("POOL_LOG") ? atoi(getenv("POOL_LOG")) : 31;
    if (getenv("NTT_B16_STG")) ntt_stg = atoi(getenv("NTT_B16_STG"));
    if (getenv("PW_FUSE")) ntt_pw_fuse = atoi(getenv("PW_FUSE"));
    if (getenv("NTT_B16_BODY")) ntt_b16_body = atoi(getenv("NTT_B16_BODY"));
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!getenv("LIMB_BASE")) bi_set_decimal(1);      /* the decimal base is the pipeline's default (RESULTS.md 63, 67); LIMB_BASE=2 reproduces the paper's binary limbs */
    bi_env_base();
    if (bi_decimal) d = ((d_out + 17) / 18) * 18;
    printf("== ecalc: e to %lu digits%s%s ==\n", d_out, bi_decimal ? " (decimal limbs, base 10^18)" : " (binary limbs)", d != d_out ? " [computed to the next multiple of 18]" : "");
    meta_line("ecalc");
    double t00 = mem_now(), t;
    unsigned long N = e_terms(d);
    { const char *es = getenv("COMM_SIZE"), *er = getenv("COMM_RANK"); int sz = es ? atoi(es) : 1, rk = er ? atoi(er) : 0;   /* M2: this process's term range (mn_init below checks the rest) */
      if (sz > 1) { unsigned __int128 nn = N; bs_a0 = 1 + (unsigned long)(nn * rk / sz); bs_b1 = 1 + (unsigned long)(nn * (rk + 1) / sz); } }
    int ovl_env = getenv("ECALC_OVERLAP") ? atoi(getenv("ECALC_OVERLAP")) : 1;
    if (ovl_env && !getenv("BS_RESTART")) { rns_after_staging_hook = (void (*)(void *))binsplit_seeds_begin_v; rns_hook_arg = (void *)N; }   /* I2: the seeds during the pool allocations */
    if (getenv("BS_REGION_SLACK")) bs_region_slack = atoi(getenv("BS_REGION_SLACK"));
    { int stg = getenv("ECALC_STAGING") ? atoi(getenv("ECALC_STAGING")) : 1;   /* step 3: in the decimal device flow the pinned staging only serves the seeds (and checkpoints): size it to them */
      int devflow = bi_decimal && (getenv("NEWTON_DEVICE") ? atoi(getenv("NEWTON_DEVICE")) : 1) && (getenv("BS_DEV_MDEV") ? atoi(getenv("BS_DEV_MDEV")) : 1) && (getenv("BS_DEVICE_POOLS") ? atoi(getenv("BS_DEVICE_POOLS")) : 1);
      int host_combine = getenv("MN_COMBINE") && !strcmp(getenv("MN_COMBINE"), "host");   /* M2's host combine multiplies on the host mdev tier: it keeps the paper's staging and pools */
      if (stg && devflow && !host_combine) { size_t need = binsplit_seed_stage_bytes(N) + (64u << 20); need = (need + (1u << 30) - 1) & ~(size_t)((1u << 30) - 1); if (need < (2u << 30)) need = 2u << 30; if (need < ((size_t)8 << pool_log)) rns_staging_bytes_req = need; }
      /* Phase 9 C4 (A-mem): plane pool 1 at the dist tier's 3 q + 16 limbs (the host mdev tier, which needs the full 2^pool_log, is not used in this flow) */
      if (devflow && !host_combine) rns_pool1_bytes_req = rns_pool1_default_bytes(pool_log); }
    double t_ri = mem_now(); rns_init(pool_log); t_ri = mem_now() - t_ri;
    int mn_size_ = mn_init();                       /* Phase 8 M1: a node-process among COMM_SIZE; the meshes are opened here */
    if (mn_size_ > 1 && !mn_selftest(11, 11, verbose >= 2)) { printf("VERIFY FAILED\n"); return 1; }
    int mn_dist = mn_size_ > 1 && !(getenv("MN_COMBINE") && !strcmp(getenv("MN_COMBINE"), "host"));   /* M3: the top levels as distributed products (MN_COMBINE=host: M2's combine on node 0) */
    if (mn_dist && !mn_selftest_layered(11, 11, verbose >= 2)) { printf("VERIFY FAILED\n"); return 1; }
    if (mn_size_ > 1) printf("mn: node %d computes terms [%lu, %lu) of %lu\n", mn_rank(), bs_a0, bs_b1, N);
    double t_pg = mem_now(); binsplit_pregrow(N); t_pg = mem_now() - t_pg;   /* WP3: region pools at init, like the device pools */
    double t_init = mem_now() - t00;
    if (verbose >= 2) printf("      init: rns_init %.2f s, region pools %.2f s, the rest %.2f s\n", t_ri, t_pg, t_init - t_ri - t_pg);   /* A-mem */
    RESULT("init", "s", t_init);
    printf("      VmRSS %.1f GB after init (staging %.1f GB pinned + device regions %.0f GB); init %.1f s\n", mem_vmrss() / 1e9, rns_staging_bytes() * 4 / 1e9, mem_dev_pool_bytes() / 1e9, t_init);
    mem_report("init");                           /* Phase 9 M9 (A-mem): device and host bytes by category at each phase boundary */
    bs_verbose = dec_verbose = verbose >= 2;
    bs_donate_pools = getenv("NEWTON_DEVICE") ? atoi(getenv("NEWTON_DEVICE")) : 1;   /* WP5: the bs regions become the dm phase's blocks (default since RESULTS.md 62b) */
    bs_ckpt_dir = getenv("BS_CKPT_DIR");                                             /* WP7: per-level checkpoints of bs, and restart */
    if (bs_ckpt_dir && !*bs_ckpt_dir) bs_ckpt_dir = 0;
    if (getenv("BS_CKPT_EVERY")) bs_ckpt_every = atoi(getenv("BS_CKPT_EVERY"));
    if (getenv("BS_CKPT_MIN_LEVEL")) bs_ckpt_min_level = atoi(getenv("BS_CKPT_MIN_LEVEL"));
    bs_restart = getenv("BS_RESTART") ? atoi(getenv("BS_RESTART")) : 0;
    g_overlap = getenv("ECALC_OVERLAP") ? atoi(getenv("ECALC_OVERLAP")) : 1;
    if (getenv("ECALC_BG_THREADS")) g_bg_threads = atoi(getenv("ECALC_BG_THREADS"));
    int newton_dev = getenv("NEWTON_DEVICE") ? atoi(getenv("NEWTON_DEVICE")) : 1;   /* WP5: the reciprocal and division on device-resident numbers (default) */
    int ovl = g_overlap && bi_decimal && newton_dev && bs_dev_mdev && mn_size_ == 1;   /* the overlapped flow needs the device top levels and the device dm; single-node until M3 */
    bigint P, Q, T, A, X, R, S;
    bi_init(&P); bi_init(&Q); bi_init(&T); bi_init(&A); bi_init(&X); bi_init(&R); bi_init(&S);
    struct pq_bg pqb; memset(&pqb, 0, sizeof pqb); pqb.N = N; pqb.a0 = bs_a0; pqb.b1 = bs_b1 ? bs_b1 : N + 1;   /* M5: the T1 recurrence over this node's terms */
    struct x_bg xb; memset(&xb, 0, sizeof xb); xb.d = d; xb.d_out = d_out; xb.outfile = outfile; xb.verbose = verbose >= 2;
    struct out_ctx oc; memset(&oc, 0, sizeof oc); oc.N = N; oc.d = d; oc.d_out = d_out; oc.outfile = outfile; oc.verbose = verbose; oc.size = mn_size_; oc.rank = mn_rank();
    oc.X = &X; oc.pqb = &pqb; oc.xb = &xb; oc.t00 = t00; oc.t_init = t_init;
    if (mn_dist) bs_keep_dev = 1;                     /* M3: the leaf's P_r, Q_r stay on the device when the top leaf level ran there */
    if (ovl) { bs_after_seeds_hook = pq_bg_start; bs_hook_arg = &pqb; bs_keep_dev = 1;
               pqb.grow = getenv("ECALC_POOL_GROW_GB") ? (size_t)(atof(getenv("ECALC_POOL_GROW_GB")) * 1e9) : 0; }   /* per device; off: hipMalloc in the background stalls the GPU levels (RESULTS.md 70) */
    else if (mn_size_ > 1) { bs_after_seeds_hook = pq_bg_start; bs_hook_arg = &pqb; }   /* M5: every node runs its range's recurrence during its leaf levels */


    t = mem_now(); binsplit_e(&P, &Q, N); double t_bs = mem_now() - t;
    /* Phase 9 M4 (A-div): the reciprocal and the division over the sharded P, Q (default; MN_DM=host: M3's gather to node 0
     * and the single-node division there).  The residues of P, Q, R come from the sharded kernels; X is gathered to node 0
     * for the existing output (A-out writes it per node) */
    int mn_dm = mn_dist && bi_decimal && !(getenv("MN_DM") && !strcmp(getenv("MN_DM"), "host"));
    uint64_t Pres[T1_NQ], Qres[T1_NQ], Rres[T1_NQ]; int rres_ok = 0;
    double t_recip = 0, t_10dp = 0, t_dm = 0; size_t mn_pn = 0, mn_qn = 0, mn_xn = 0;
    if (mn_dist) {                                  /* M3: the top log2(size) levels as distributed products over node groups */
        double tg = mem_now(); dbig Pl, Ql; db_init(&Pl); db_init(&Ql);
        if (bs_Pd.n) { Pl = bs_Pd; Ql = bs_Qd; memset(&bs_Pd, 0, sizeof bs_Pd); memset(&bs_Qd, 0, sizeof bs_Qd); }
        else { db_from_bi(&Pl, &P); db_from_bi(&Ql, &Q); }
        printf("mn: node %d leaf P %zu limbs, Q %zu limbs (%s)\n", mn_rank(), Pl.n, Ql.n, P.n ? "host, copied in" : "device");
        mdb Pm, Qm; mn_tree(&Pm, &Qm, &Pl, &Ql);
        double tt = mem_now();
        if (mn_dm) {                                /* M4: the division over shares; X gathered to node 0 until A-out */
            printf("mn: node %d: tree levels %.2f s: P %zu limbs, Q %zu limbs\n", mn_rank(), tt - tg, Pm.n, Qm.n);
            t_bs += tt - tg; mn_pn = Pm.n; mn_qn = Qm.n;
            mem_report("tree");                     /* Phase 10 B6 (agent M): the tree's slabs and shares should show under the pool's donated bytes, not hipMalloc */
            P.n = Q.n = 0; binsplit_free_pools();
            memset(&newton_st, 0, sizeof newton_st); memset(&rns_st, 0, sizeof rns_st);
            int L = 0; while ((1 << L) < mn_size_) L++;
            mn_group *G = mn_group_at(L);
            double td = mem_now(); mdb Xm; memset(&Xm, 0, sizeof Xm);
            newton_mn_divmod(&Xm, &Pm, &Qm, (d + 17) / 18, G, t1_q, T1_NQ, Pres, Qres, Rres, &t_recip);
            t_dm = mem_now() - td; rres_ok = 1; mn_xn = Xm.n;
            double tx = mem_now();
            mn_gather_host(&X, &Xm); db_free(&Xm.sh);
            newton_db_free_scratch(); db_release_pools(); rns_free_scratch();
            printf("mn: node %d: dm over %d nodes %.2f s (reciprocal %.2f), X %zu limbs, gathered to node 0 in %.2f s%s\n", mn_rank(), mn_size_, t_dm, t_recip, mn_xn, mem_now() - tx, mn_rank() ? "; done" : "");
            if (mn_rank() != 0) { int f = out_stage(&oc); db_release_pools(); rns_shutdown(); mem_report("released"); mem_report_summary(); mn_barrier(); mn_finalize(); return f; }   /* M5 + A-mem: every node writes its part of X and checks its residues; its device memory goes before the final barrier */
        } else {
        mn_gather_host(&P, &Pm); mn_gather_host(&Q, &Qm); db_free(&Pm.sh); db_free(&Qm.sh);
        printf("mn: node %d: tree levels %.2f s, gather to node 0 %.2f s%s\n", mn_rank(), tt - tg, mem_now() - tt, mn_rank() ? "; done" : "");
        mem_report("tree");
        if (mn_rank() != 0) { int f = out_stage(&oc); db_release_pools(); rns_shutdown(); mem_report("released"); mem_report_summary(); mn_barrier(); mn_finalize(); return f; }   /* M5 + A-mem: the part file and the residues, then the device memory goes before the final barrier */
        printf("mn: node 0: P %zu limbs, Q %zu limbs\n", P.n, Q.n);
        t_bs += mem_now() - tg;
        }
    } else if (mn_size_ > 1) {                      /* M2: node 0 gathers P_r, Q_r (host, over the thread-0 mesh) and combines them in order; the other nodes are done */
        comm *c = mn_comm(0); double tg = mem_now();
        if (mn_rank() != 0) {
            uint64_t n2[2] = { P.n, Q.n }; comm_send(c, 0, n2, 16); comm_send(c, 0, P.l, P.n * 8); comm_send(c, 0, Q.l, Q.n * 8);
            printf("mn: node %d sent P (%zu limbs), Q (%zu limbs) to node 0 in %.2f s\n", mn_rank(), P.n, Q.n, mem_now() - tg);
            bi_free(&P); bi_free(&Q);
            int f = out_stage(&oc); db_release_pools(); rns_shutdown(); mem_report("released"); mem_report_summary(); mn_barrier(); mn_finalize(); return f;
        }
        bigint Pr, Qr, tt; bi_init(&Pr); bi_init(&Qr); bi_init(&tt);
        for (int r = 1; r < mn_size_; r++) {
            uint64_t n2[2]; comm_recv(c, r, n2, 16);
            bi_reserve(&Pr, n2[0] + 1); bi_reserve(&Qr, n2[1] + 1); Pr.n = n2[0]; Qr.n = n2[1];
            comm_recv(c, r, Pr.l, Pr.n * 8); comm_recv(c, r, Qr.l, Qr.n * 8);
            rns_mul(&tt, &P, &Qr); bi_add(&P, &tt, &Pr);        /* P = P Q_r + P_r,  Q = Q Q_r */
            rns_mul(&tt, &Q, &Qr); bi_copy(&Q, &tt);
        }
        bi_free(&Pr); bi_free(&Qr); bi_free(&tt);
        printf("mn: node 0 combined %d ranges in %.2f s: P %zu limbs, Q %zu limbs\n", mn_size_, mem_now() - tg, P.n, Q.n);
        t_bs += mem_now() - tg;
    }
    if (getenv("ECALC_STOP_AFTER_BS")) { printf("bs    %8.2f s   (seeds %.1f school %.1f batch %.1f mdev %.1f)\n", t_bs, bs_st.t_seed, bs_st.t_school, bs_st.t_batch, bs_st.t_mdev); return 0; }
    int ovl3 = ovl && bs_Pd.n;                       /* the top level left P, Q on device (it does when it ran on the device tier); otherwise the host flow */
    if (ovl3) { P.n = bs_Pd.n; Q.n = bs_Qd.n; }      /* sizes for the line below; the limbs come off the device in the background */
    printf("bs    %8.2f s   N %lu, P %zu limbs, Q %zu limbs (seeds %.1f school %.1f batch %.1f mdev %.1f; pool %.1f GB; dev pools %.1f GB)   VmRSS %.1f GB, VmHWM %.1f GB\n",
           t_bs, N, mn_dm ? mn_pn : P.n, mn_dm ? mn_qn : Q.n, bs_st.t_seed, bs_st.t_school, bs_st.t_batch, bs_st.t_mdev, bs_st.peak_pool_limbs * 8e-9, mem_dev_pool_bytes() / 1e9, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
    RESULT("bs", "s", t_bs);
    if (bs_st.n_grow) printf("      bs: region pools grew %d times inside the phase (%.1f GB of hipMalloc)\n", bs_st.n_grow, bs_st.grow_bytes / 1e9);
    mem_report("bs");
    if (bs_st.n_ckpt || bs_st.restart_level) {
        printf("      bs checkpoints: %d written, %.2f GB, %.2f s (%.2f s each); restart from level %d in %.2f s\n",
               bs_st.n_ckpt, bs_st.ckpt_bytes * 1e-9, bs_st.t_ckpt, bs_st.n_ckpt ? bs_st.t_ckpt / bs_st.n_ckpt : 0.0, bs_st.restart_level, bs_st.t_restart);
        RESULT("bs_ckpt", "s", bs_st.t_ckpt); RESULT("bs_ckpt_bytes", "GB", bs_st.ckpt_bytes * 1e-9);
    }
    { size_t hc = 0; uint64_t *hp = binsplit_take_hpool(&hc);   /* the bs host pool (already faulted) becomes A's buffer */
      size_t need = bi_decimal ? (d + 17) / 18 + P.n + 4 : 0;
      if (hp && need && hc >= need) { A.l = hp; A.cap = hc; A.n = 0; } else if (hp) free(hp); }
    binsplit_free_pools();

    /* residues of P and Q for T1 now, so P can go as soon as S = P + Q exists */
    if (!ovl3 && !mn_dm) for (int i = 0; i < T1_NQ; i++) { Pres[i] = vf_limbs_mod(P.l, P.n, t1_q[i]); Qres[i] = vf_limbs_mod(Q.l, Q.n, t1_q[i]); }

    bigint MU; bi_init(&MU);
    char *digits = 0;                                 /* the binary path's whole digit string (todec); decimal streams its digits (M5) */
    size_t dl = bi_decimal ? (d + 17) / 18 : (size_t)ceil(d * log2(10.0) / 64.0);   /* limbs of 10^d */
    if (mn_dm) {                                      /* M4: the dm phase ran over the nodes above; the phase lines for the summary */
        printf("recip %8.2f s   (over %d nodes; %zu iterations)\n", t_recip, mn_size_, newton_st.iters);
        printf("10dP  %8.2f s   (S = P + Q over shares, inside dm)\n", 0.0); RESULT("10dP", "s", 0.0);
        printf("dm    %8.2f s   X %zu limbs (recip %.1f s; corrections %zu/%zu; over %d nodes)   VmRSS %.1f GB, VmHWM %.1f GB\n",
               t_dm, X.n, t_recip, newton_st.down_corr, newton_st.up_corr, mn_size_, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
        RESULT("dm", "s", t_dm);
    } else {
    /* dm part 1: the reciprocal of Q first, while A does not exist yet (memory peak) */
    t = mem_now();
    memset(&newton_st, 0, sizeof newton_st); memset(&rns_st, 0, sizeof rns_st);
    size_t na_est = 2 * Q.n + dl - Q.n + 2, k_mu = na_est - Q.n + 1;
    newton_db_free_inputs = newton_dev;               /* Q lives on device from here; its host copy goes */
    if (newton_dev && bi_decimal) rns_release_staging();   /* decimal: nothing between here and dm needs the staging */
    double t_res3 = 0;
    if (ovl3) {                                       /* I3: P, Q stay on the device -- residues by kernel, S = P + Q in place, A = S B^dl implicit */
        double tr = mem_now();
        db_mod_qs(&bs_Pd, t1_q, T1_NQ, Pres); db_mod_qs(&bs_Qd, t1_q, T1_NQ, Qres);
        { size_t don = 0; for (int dv = 0; dv < 4; dv++) don += rns_dpool_donate_tail(dv, 1, rns_pool1_default_bytes(pool_log)); if (verbose >= 2) printf("      I3: plane pool tails donated: %.1f GB\n", don / 1e9); }   /* the dist tier uses 3 q (+16 limbs) of pool 1; nothing to donate when pool 1 is sized to that (C4) */
        t_res3 = mem_now() - tr;
        na_est = bs_Pd.n + 1 + dl; k_mu = na_est - bs_Qd.n + 1;   /* S has at most one limb more than P */
        P.n = Q.n = 0;
        newton_db_Qd = &bs_Qd; newton_db_mu_host = 0;
        if (getenv("ECALC_DM_POOL") && atoi(getenv("ECALC_DM_POOL"))) {   /* C3 (A-div): the block pool sized to the reciprocal's scratch once, before the phase, instead of growing by hipMalloc block by block inside it (RESULTS.md 71: 6-7e10) */
            double tp = mem_now(); size_t nq_ = bs_Qd.n, tcap = (nq_ + k_mu > 2 * k_mu ? nq_ + k_mu : 2 * k_mu) + 8;
            size_t need = ((2 * (k_mu + 4) + tcap + ((size_t)1 << 31) + 8 + 4 * 4096) / 4) * 8 + ((size_t)1 << 30), grown = 0;   /* per device: r, r2, t1, the grid's piece temporary; Q and S are live already */
            for (int dv = 0; dv < 4; dv++) { size_t fr = db_pool_free_bytes(dv); if (need > fr) { db_pregrow(dv, need - fr); grown += need - fr; } }
            printf("      C3: block pool sized to the reciprocal's scratch (%.1f GB per device): grown by %.1f GB in %.2f s\n", need / 1e9, grown / 1e9, mem_now() - tp);
        }
        newton_db_recip(&MU, &Q, k_mu);
    } else if (newton_dev) newton_db_recip(&MU, &Q, k_mu); else newton_recip(&MU, &Q, k_mu);
    newton_free_scratch(); rns_free_scratch();
    t_recip = mem_now() - t;
    printf("recip %8.2f s   mu %zu limbs (%zu iterations, %zu mdev)   VmRSS %.1f GB, VmHWM %.1f GB\n", t_recip, ovl3 ? k_mu + 1 : MU.n, newton_st.iters, rns_st.n_mdev, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
    mem_report("recip");

    t = mem_now();
    if (ovl3) {
        db_add(&bs_Pd, &bs_Pd, &bs_Qd);               /* S = P + Q on the device; A = S B^dl is never formed */
        t_10dp = mem_now() - t;
        printf("      I3: P, Q residues by kernel %.2f s (before the reciprocal), S = P + Q on device %.2f s; no host A\n", t_res3, t_10dp);
        T.n = dl; A.n = bs_Pd.n + dl;
    } else {
    bi_add(&S, &P, &Q);
    bi_free(&P);
    memset(&rns_st, 0, sizeof rns_st);
    if (bi_decimal) {                           /* WP2: 10^d (P+Q) is a limb shift and one small multiply */
        bi_shl_limbs(&A, &S, d / 18);
        if (d % 18) bi_mul_pow10(&A, &A, (unsigned)(d % 18));
        bi_set_u64(&T, 0); T.n = (d + 17) / 18;   /* for the size print only */
    } else {
        pow10_big(&T, d);
        rns_mul(&A, &T, &S);
    }
    t_10dp = mem_now() - t;
    }
    printf("10dP  %8.2f s   T %zu limbs, A %zu limbs (%zu mdev, %zu splits)   VmRSS %.1f GB, VmHWM %.1f GB\n", t_10dp, T.n, A.n, rns_st.n_mdev, rns_st.n_split, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
    RESULT("10dP", "s", t_10dp);
    if (ovl3) { T.n = A.n = 0; }
    bi_free(&S); bi_free(&T); rns_free_scratch();

    t = mem_now();
    memset(&rns_st, 0, sizeof rns_st);
    /* (binary keeps the staging: dc needs it and its memory fits; decimal released it before the reciprocal) */
    if (ovl) { newton_db_x_hook = x_bg_hook; newton_db_x_arg = &xb; }   /* O4: the hook starts the residues and the chunked writer (M5) with X on the host */
    if (ovl3) { newton_db_divmod_shifted(&X, &bs_Pd, dl, &bs_Qd, t1_q, T1_NQ, Rres); rres_ok = 1; db_free(&bs_Pd); }
    else if (newton_dev) newton_db_divmod(&X, &R, &A, &Q, &MU); else newton_divmod(&X, &R, &A, &Q, &MU);
    newton_db_x_hook = 0; newton_db_Qd = 0;
    if (ovl3) db_free(&bs_Qd);
    bi_free(&MU); newton_free_scratch(); newton_db_free_scratch(); db_release_pools(); rns_free_scratch();
    t_dm = mem_now() - t + t_recip;
    printf("dm    %8.2f s   X %zu limbs, R %zu limbs (recip %.1f s; corrections %zu/%zu; %zu mdev)   VmRSS %.1f GB, VmHWM %.1f GB\n",
           t_dm, X.n, R.n, t_recip, newton_st.down_corr, newton_st.up_corr, rns_st.n_mdev, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
    mem_report_host_item(MEM_HOST_X, X.cap * 8); mem_report_host_item(MEM_HOST_DIGITS, digits ? d + 2 : 0); mem_report("dm");
    RESULT("dm", "s", t_dm);
    }

    if (!bi_decimal) {                                /* the binary-limb path (LIMB_BASE=2): the radix conversion produces the whole string; unchanged */
        t = mem_now();
        if (ovl && pqb.started) pthread_join(pqb.th, 0);
        int bad1 = tier1_res_pq(N, d, Pres, Qres, &X, &R, ovl && pqb.started ? pqb.p : 0, ovl && pqb.started ? pqb.qq : 0, 0, rres_ok ? Rres : 0, verbose >= 2);
        double t_t1 = mem_now() - t;
        printf("T1    %8.2f s   %s\n", t_t1, bad1 ? "FAILED" : "ok: T(P+Q) == XQ + R and P, Q mod q for 8 primes");
        RESULT("T1", "s", t_t1);
        bi_free(&A); bi_free(&R); bi_free(&Q);
        uint64_t Xres[T1_NQ];
        for (int i = 0; i < T1_NQ; i++) Xres[i] = vf_limbs_mod(X.l, X.n, t1_q[i]);
        t = mem_now();
        todec_free_input = 1;
        todec(0, &X, d + 1);                   /* allocates the 40 GB digit string only at the LEAF step */
        digits = todec_out;
        double t_dc = mem_now() - t;
        printf("dc    %8.2f s   %zu leaf pieces, %d levels (prewarm %.1f top %.1f mid %.1f deep %.1f leaf %.1f)\n",
               t_dc, dec_st.pieces, dec_st.levels, dec_st.t_prewarm, dec_st.t_top, dec_st.t_mid, dec_st.t_deep, dec_st.t_leaf);
        RESULT("dc", "s", t_dc);
        printf("      VmHWM %.1f GB after dc\n", mem_vmhwm() / 1e9);
        t = mem_now();
        int bad2 = tier2(digits, d_out + 1, verbose >= 2);
        int bad3 = tier1_digits_res(digits, d + 1, Xres, verbose >= 2);
        double t_t2 = mem_now() - t;
        printf("T2    %8.2f s   windows %s, digits == X mod q %s\n", t_t2, bad2 ? "FAILED" : "ok", bad3 ? "FAILED" : "ok");
        RESULT("T2", "s", t_t2);
        printf("digits: %.62s...%.20s\n", digits, digits + d_out + 1 - 20);
        double total = mem_now() - t00, phases = t_bs + t_10dp + t_dm + t_t1 + t_dc + t_t2;
        printf("total %8.2f s   (bs %.1f + 10dP %.1f + dm %.1f + T1 %.1f + dc %.1f + T2 %.1f = %.1f; init %.1f; other %.1f); VmHWM %.1f GB\n",
               total, t_bs, t_10dp, t_dm, t_t1, t_dc, t_t2, phases, t_init, total - phases - t_init, mem_vmhwm() / 1e9);
        RESULT("total", "s", total); RESULT("phases", "s", phases); RESULT("other", "s", total - phases - t_init);
        RESULT("vmhwm", "GB", mem_vmhwm() / 1e9);
        printf("paper A22 (4e10): 285.7 = bs 112.2 + 10dP 12.6 + dm 46.8 + T1 ~3 + dc 110.3\n");
        if (outfile) {
            FILE *f = fopen(outfile, "w");
            if (f) { fputc(digits[0], f); fputc('.', f); fwrite(digits + 1, 1, d_out, f); fputc('\n', f); fclose(f); printf("wrote %s\n", outfile); }
        }
        int fail = bad1 || bad2 || bad3;
        printf("%s\n", fail ? "VERIFY FAILED" : "VERIFY OK");
        mem_hreg_free(digits);
        mn_barrier(); mn_finalize();
        rns_shutdown();
        return fail;
    }
    /* decimal: the output stage (M5 + C1) -- T1 from the residues, the digits streamed to the file in chunks, T2 */
    if (!rres_ok) for (int i = 0; i < T1_NQ; i++) Rres[i] = vf_limbs_mod(R.l, R.n, t1_q[i]);   /* the host flow's R (the device flow's came from the kernel) */
    memcpy(oc.Pres, Pres, sizeof Pres); memcpy(oc.Qres, Qres, sizeof Qres); memcpy(oc.Rres, Rres, sizeof Rres);
    oc.ncorr = (int)(newton_st.down_corr + newton_st.up_corr); oc.t_bs = t_bs; oc.t_10dp = t_10dp; oc.t_dm = t_dm;
    bi_free(&A); bi_free(&R); bi_free(&Q);
    int fail = out_stage(&oc);
    mem_report_host_item(MEM_HOST_X, 0); mem_report("end"); mem_report_summary();
    mn_barrier(); mn_finalize();
    rns_shutdown();
    return fail;
}

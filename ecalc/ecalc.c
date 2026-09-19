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
struct pq_bg { unsigned long N; uint64_t p[T1_NQ], qq[T1_NQ]; pthread_t th; int started; double t, t_grow; size_t grow; };
static void *pq_bg_run(void *a) { struct pq_bg *b = (struct pq_bg *)a; double t0 = mem_now(); omp_set_num_threads(g_bg_threads);
    for (int i = 0; i < T1_NQ; i++) vf_pq_mod(b->N, t1_q[i], &b->p[i], &b->qq[i]); b->t = mem_now() - t0;
    t0 = mem_now();
    if (b->grow) for (int dv = 0; dv < 4; dv++) db_pregrow(dv, b->grow);   /* the dm phase's block pool beyond the donated regions, allocated while the GPUs run the levels */
    b->t_grow = mem_now() - t0;
    return 0; }
static void pq_bg_start(void *a) { struct pq_bg *b = (struct pq_bg *)a; if (b->started) return; pthread_create(&b->th, 0, pq_bg_run, b); b->started = 1; }   /* O2: after the seeds */

struct x_bg { bigint *X; unsigned long d, d_out; char *digits; uint64_t Xres[T1_NQ]; int bad2, bad3, verbose; pthread_t th; int started; double t_res, t_fmt, t_t2; };
static void digits_format(char *digits, const bigint *X, unsigned long d)   /* the limbs are the digits; X < 10^(d+1) has ceil((d+1)/18) limbs */
{
    size_t nl = (d + 1 + 17) / 18, pad = nl * 18 - (d + 1);     /* leading zeros to drop */
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < nl; i++) {
        char buf[19]; uint64_t v = i < X->n ? X->l[i] : 0;
        snprintf(buf, sizeof buf, "%018llu", (unsigned long long)v);
        size_t pos = (nl - 1 - i) * 18;                      /* padded position of this limb's first digit */
        for (int c = 0; c < 18; c++) if (pos + c >= pad) digits[pos + c - pad] = buf[c];
    }
    digits[d + 1] = 0;
}
static void *x_bg_run(void *a)                        /* O4: X's residues, the digits, T2 and the digit residue -- during the low product */
{
    struct x_bg *b = (struct x_bg *)a; omp_set_num_threads(g_bg_threads);
    double t0 = mem_now();
    for (int i = 0; i < T1_NQ; i++) b->Xres[i] = vf_limbs_mod(b->X->l, b->X->n, t1_q[i]);
    double t1 = mem_now();
    digits_format(b->digits, b->X, b->d);
    double t2 = mem_now();
    b->bad2 = tier2(b->digits, b->d_out + 1, b->verbose);
    b->bad3 = tier1_digits_res(b->digits, b->d + 1, b->Xres, b->verbose);
    b->t_res = t1 - t0; b->t_fmt = t2 - t1; b->t_t2 = mem_now() - t2;
    return 0;
}
static void x_bg_hook(bigint *X, void *a) { struct x_bg *b = (struct x_bg *)a; b->X = X; pthread_create(&b->th, 0, x_bg_run, b); b->started = 1; }

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
    rns_init(pool_log);
    int mn_size_ = mn_init();                       /* Phase 8 M1: a node-process among COMM_SIZE; the meshes are opened here */
    if (mn_size_ > 1 && !mn_selftest(11, 11, verbose >= 2)) { printf("VERIFY FAILED\n"); return 1; }
    if (mn_size_ > 1) printf("mn: node %d computes terms [%lu, %lu) of %lu\n", mn_rank(), bs_a0, bs_b1, N);
    binsplit_pregrow(N);                          /* WP3: region pools at init, like the device pools */
    double t_init = mem_now() - t00;
    RESULT("init", "s", t_init);
    printf("      VmRSS %.1f GB after init (staging 64 GB pinned + device pools %.0f GB incl. bs regions); init %.1f s\n", mem_vmrss() / 1e9, mem_dev_pool_bytes() / 1e9, t_init);
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
    struct pq_bg pqb; memset(&pqb, 0, sizeof pqb); pqb.N = N;
    if (ovl) { bs_after_seeds_hook = pq_bg_start; bs_hook_arg = &pqb; bs_keep_dev = 1;
               pqb.grow = getenv("ECALC_POOL_GROW_GB") ? (size_t)(atof(getenv("ECALC_POOL_GROW_GB")) * 1e9) : 0; }   /* per device; off: hipMalloc in the background stalls the GPU levels (RESULTS.md 70) */


    t = mem_now(); binsplit_e(&P, &Q, N); double t_bs = mem_now() - t;
    if (mn_size_ > 1) {                             /* M2: node 0 gathers P_r, Q_r (host, over the thread-0 mesh) and combines them in order; the other nodes are done */
        comm *c = mn_comm(0); double tg = mem_now();
        if (mn_rank() != 0) {
            uint64_t n2[2] = { P.n, Q.n }; comm_send(c, 0, n2, 16); comm_send(c, 0, P.l, P.n * 8); comm_send(c, 0, Q.l, Q.n * 8);
            printf("mn: node %d sent P (%zu limbs), Q (%zu limbs) to node 0 in %.2f s; waiting\n", mn_rank(), P.n, Q.n, mem_now() - tg);
            mn_barrier(); mn_finalize(); rns_shutdown(); return 0;
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
           t_bs, N, P.n, Q.n, bs_st.t_seed, bs_st.t_school, bs_st.t_batch, bs_st.t_mdev, bs_st.peak_pool_limbs * 8e-9, mem_dev_pool_bytes() / 1e9, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
    RESULT("bs", "s", t_bs);
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
    uint64_t Pres[T1_NQ], Qres[T1_NQ], Rres[T1_NQ]; int rres_ok = 0;
    if (!ovl3) for (int i = 0; i < T1_NQ; i++) { Pres[i] = vf_limbs_mod(P.l, P.n, t1_q[i]); Qres[i] = vf_limbs_mod(Q.l, Q.n, t1_q[i]); }

    /* dm part 1: the reciprocal of Q first, while A does not exist yet (memory peak) */
    t = mem_now();
    memset(&newton_st, 0, sizeof newton_st); memset(&rns_st, 0, sizeof rns_st);
    bigint MU; bi_init(&MU);
    size_t dl = bi_decimal ? (d + 17) / 18 : (size_t)ceil(d * log2(10.0) / 64.0);   /* limbs of 10^d */
    size_t na_est = 2 * Q.n + dl - Q.n + 2, k_mu = na_est - Q.n + 1;
    newton_db_free_inputs = newton_dev;               /* Q lives on device from here; its host copy goes */
    if (newton_dev && bi_decimal) rns_release_staging();   /* decimal: nothing between here and dm needs the staging */
    double t_10dp = 0, t_res3 = 0;
    if (ovl3) {                                       /* I3: P, Q stay on the device -- residues by kernel, S = P + Q in place, A = S B^dl implicit */
        double tr = mem_now();
        db_mod_qs(&bs_Pd, t1_q, T1_NQ, Pres); db_mod_qs(&bs_Qd, t1_q, T1_NQ, Qres);
        { size_t don = 0; for (int dv = 0; dv < 4; dv++) don += rns_dpool_donate_tail(dv, 1, (size_t)3 * ((size_t)8 << (pool_log - 2))); if (verbose >= 2) printf("      I3: plane pool tails donated: %.1f GB\n", don / 1e9); }   /* the dist tier uses 3 q of pool 1 */
        t_res3 = mem_now() - tr;
        na_est = bs_Pd.n + 1 + dl; k_mu = na_est - bs_Qd.n + 1;   /* S has at most one limb more than P */
        P.n = Q.n = 0;
        newton_db_Qd = &bs_Qd; newton_db_mu_host = 0;
        newton_db_recip(&MU, &Q, k_mu);
    } else if (newton_dev) newton_db_recip(&MU, &Q, k_mu); else newton_recip(&MU, &Q, k_mu);
    newton_free_scratch(); rns_free_scratch();
    double t_recip = mem_now() - t;
    printf("recip %8.2f s   mu %zu limbs (%zu iterations, %zu mdev)   VmRSS %.1f GB, VmHWM %.1f GB\n", t_recip, ovl3 ? k_mu + 1 : MU.n, newton_st.iters, rns_st.n_mdev, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);

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
    struct x_bg xb; memset(&xb, 0, sizeof xb); xb.d = d; xb.d_out = d_out; xb.verbose = verbose >= 2;
    char *digits = 0; int digits_reg = 1;
    if (ovl) {                                        /* O4: the digit buffer now (plain pages; the formatting thread touches them), the hook starts the formatting */
        digits_reg = 0;
        if (posix_memalign((void **)&digits, 2u << 20, d + 2)) { fprintf(stderr, "digits: %lu bytes\n", d + 2); return 1; }
        xb.digits = digits; newton_db_x_hook = x_bg_hook; newton_db_x_arg = &xb;
    }
    if (ovl3) { newton_db_divmod_shifted(&X, &bs_Pd, dl, &bs_Qd, t1_q, T1_NQ, Rres); rres_ok = 1; db_free(&bs_Pd); }
    else if (newton_dev) newton_db_divmod(&X, &R, &A, &Q, &MU); else newton_divmod(&X, &R, &A, &Q, &MU);
    newton_db_x_hook = 0; newton_db_Qd = 0;
    if (ovl3) db_free(&bs_Qd);
    bi_free(&MU); newton_free_scratch(); newton_db_free_scratch(); db_release_pools(); rns_free_scratch();
    double t_dm = mem_now() - t + t_recip;
    printf("dm    %8.2f s   X %zu limbs, R %zu limbs (recip %.1f s; corrections %zu/%zu; %zu mdev)   VmRSS %.1f GB, VmHWM %.1f GB\n",
           t_dm, X.n, R.n, t_recip, newton_st.down_corr, newton_st.up_corr, rns_st.n_mdev, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
    RESULT("dm", "s", t_dm);

    t = mem_now();
    if (ovl && pqb.started) pthread_join(pqb.th, 0);
    if (ovl && xb.started) pthread_join(xb.th, 0);   /* its X residues serve T1 (if X was corrected afterwards they are redone below and T1 recomputes) */
    int xres_ok = ovl && xb.started && !(newton_st.down_corr + newton_st.up_corr);
    int bad1 = tier1_res_pq(N, d, Pres, Qres, &X, &R, ovl && pqb.started ? pqb.p : 0, ovl && pqb.started ? pqb.qq : 0, xres_ok ? xb.Xres : 0, rres_ok ? Rres : 0, verbose >= 2);
    double t_t1 = mem_now() - t;
    printf("T1    %8.2f s   %s%s\n", t_t1, bad1 ? "FAILED" : "ok: T(P+Q) == XQ + R and P, Q mod q for 8 primes", ovl && pqb.started ? " (P, Q recurrence overlapped with bs)" : "");
    if (ovl && pqb.started) printf("      overlapped with bs: P, Q mod q recurrence %.2f s, block pool pregrown by %.0f GB in %.2f s\n", pqb.t, 4.0 * pqb.grow / 1e9, pqb.t_grow);
    RESULT("T1", "s", t_t1);
    bi_free(&A); bi_free(&R); bi_free(&Q);
    printf("      VmRSS %.1f GB before dc\n", mem_vmrss() / 1e9);

    /* X's residues now, so X can go as soon as dc has copied it into its level pool */
    uint64_t Xres[T1_NQ];
    double t_dc, t_t2; int bad2, bad3;
    if (ovl && xb.started) {                          /* O4: the formatting, T2 and the digit residue ran during the low product */
        t = mem_now();
        if (newton_st.down_corr + newton_st.up_corr) {   /* X changed after the hook: redo from the final X */
            printf("      X corrected after the formatting started: redoing the digits\n");
            for (int i = 0; i < T1_NQ; i++) xb.Xres[i] = vf_limbs_mod(X.l, X.n, t1_q[i]);
            digits_format(digits, &X, d);
            xb.bad2 = tier2(digits, d_out + 1, verbose >= 2); xb.bad3 = tier1_digits_res(digits, d + 1, xb.Xres, verbose >= 2);
        }
        free(X.l); X.l = 0; X.n = X.cap = 0;
        t_dc = mem_now() - t; t_t2 = 0; bad2 = xb.bad2; bad3 = xb.bad3;
        printf("dc    %8.2f s   digits formatted from %zu decimal limbs (overlapped with the low product: residues %.2f, format %.2f, T2 + digit residue %.2f)\n", t_dc, (d + 1 + 17) / 18, xb.t_res, xb.t_fmt, xb.t_t2);
        RESULT("dc", "s", t_dc);
        printf("T2    %8.2f s   windows %s, digits == X mod q %s (overlapped)\n", t_t2, bad2 ? "FAILED" : "ok", bad3 ? "FAILED" : "ok");
        RESULT("T2", "s", t_t2);
    } else {
    for (int i = 0; i < T1_NQ; i++) Xres[i] = vf_limbs_mod(X.l, X.n, t1_q[i]);
    t = mem_now();
    if (bi_decimal) {                          /* WP2: the limbs are the digits; X < 10^(d+1) has ceil((d+1)/18) limbs */
        digits = (char *)mem_hreg_alloc(d + 2);
        digits_format(digits, &X, d);
        free(X.l); X.l = 0; X.n = X.cap = 0;
    } else {
        todec_free_input = 1;
        todec(0, &X, d + 1);                   /* allocates the 40 GB digit string only at the LEAF step */
        digits = todec_out;
    }
    t_dc = mem_now() - t;
    if (bi_decimal) printf("dc    %8.2f s   digits formatted from %zu decimal limbs (no conversion)\n", t_dc, (d + 1 + 17) / 18);
    else printf("dc    %8.2f s   %zu leaf pieces, %d levels (prewarm %.1f top %.1f mid %.1f deep %.1f leaf %.1f)\n",
           t_dc, dec_st.pieces, dec_st.levels, dec_st.t_prewarm, dec_st.t_top, dec_st.t_mid, dec_st.t_deep, dec_st.t_leaf);
    RESULT("dc", "s", t_dc);
    printf("      VmHWM %.1f GB after dc\n", mem_vmhwm() / 1e9);

    t = mem_now();
    bad2 = tier2(digits, d_out + 1, verbose >= 2);
    bad3 = tier1_digits_res(digits, d + 1, Xres, verbose >= 2);
    t_t2 = mem_now() - t;
    printf("T2    %8.2f s   windows %s, digits == X mod q %s\n", t_t2, bad2 ? "FAILED" : "ok", bad3 ? "FAILED" : "ok");
    RESULT("T2", "s", t_t2);
    }
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
    if (digits_reg) mem_hreg_free(digits); else free(digits);
    mn_barrier(); mn_finalize();
    rns_shutdown();
    return fail;
}

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

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: ecalc <digits> [outfile]\n"); return 2; }
    unsigned long d = strtoul(argv[1], 0, 10);
    const char *outfile = argc > 2 ? argv[2] : 0;
    int verbose = getenv("ECALC_VERBOSE") ? atoi(getenv("ECALC_VERBOSE")) : 1;
    int pool_log = getenv("POOL_LOG") ? atoi(getenv("POOL_LOG")) : 31;
    if (getenv("NTT_B16_STG")) ntt_stg = atoi(getenv("NTT_B16_STG"));
    if (getenv("PW_FUSE")) ntt_pw_fuse = atoi(getenv("PW_FUSE"));
    if (getenv("NTT_B16_BODY")) ntt_b16_body = atoi(getenv("NTT_B16_BODY"));
    setvbuf(stdout, NULL, _IOLBF, 0);
    bi_env_base();
    printf("== ecalc: e to %lu digits%s ==\n", d, bi_decimal ? " (decimal limbs, base 10^18)" : "");
    meta_line("ecalc");
    double t00 = mem_now(), t;
    rns_init(pool_log);
    RESULT("init", "s", mem_now() - t00);
    printf("      VmRSS %.1f GB after init (staging 64 GB pinned + device pools 128 GB)\n", mem_vmrss() / 1e9);
    bs_verbose = dec_verbose = verbose >= 2;

    unsigned long N = e_terms(d);
    bigint P, Q, T, A, X, R, S;
    bi_init(&P); bi_init(&Q); bi_init(&T); bi_init(&A); bi_init(&X); bi_init(&R); bi_init(&S);

    t = mem_now(); binsplit_e(&P, &Q, N); double t_bs = mem_now() - t;
    printf("bs    %8.2f s   N %lu, P %zu limbs, Q %zu limbs (seeds %.1f school %.1f batch %.1f mdev %.1f; pool %.1f GB)\n",
           t_bs, N, P.n, Q.n, bs_st.t_seed, bs_st.t_school, bs_st.t_batch, bs_st.t_mdev, bs_st.peak_pool_limbs * 8e-9);
    RESULT("bs", "s", t_bs);
    binsplit_free_pools();

    /* residues of P and Q for T1 now, so P can go as soon as S = P + Q exists */
    uint64_t Pres[T1_NQ], Qres[T1_NQ];
    for (int i = 0; i < T1_NQ; i++) { Pres[i] = vf_limbs_mod(P.l, P.n, t1_q[i]); Qres[i] = vf_limbs_mod(Q.l, Q.n, t1_q[i]); }

    /* dm part 1: the reciprocal of Q first, while A does not exist yet (memory peak) */
    t = mem_now();
    memset(&newton_st, 0, sizeof newton_st); memset(&rns_st, 0, sizeof rns_st);
    bigint MU; bi_init(&MU);
    size_t dl = bi_decimal ? (d + 17) / 18 : (size_t)ceil(d * log2(10.0) / 64.0);   /* limbs of 10^d */
    size_t na_est = 2 * Q.n + dl - Q.n + 2, k_mu = na_est - Q.n + 1;
    newton_recip(&MU, &Q, k_mu);
    newton_free_scratch(); rns_free_scratch();
    double t_recip = mem_now() - t;
    printf("recip %8.2f s   mu %zu limbs (%zu iterations, %zu mdev)   VmRSS %.1f GB, VmHWM %.1f GB\n", t_recip, MU.n, newton_st.iters, rns_st.n_mdev, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);

    t = mem_now();
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
    double t_10dp = mem_now() - t;
    printf("10dP  %8.2f s   T %zu limbs, A %zu limbs (%zu mdev, %zu splits)   VmRSS %.1f GB, VmHWM %.1f GB\n", t_10dp, T.n, A.n, rns_st.n_mdev, rns_st.n_split, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
    RESULT("10dP", "s", t_10dp);
    bi_free(&S); bi_free(&T); rns_free_scratch();

    t = mem_now();
    memset(&rns_st, 0, sizeof rns_st);
    newton_divmod(&X, &R, &A, &Q, &MU);
    bi_free(&MU); newton_free_scratch(); rns_free_scratch();
    double t_dm = mem_now() - t + t_recip;
    printf("dm    %8.2f s   X %zu limbs, R %zu limbs (recip %.1f s; corrections %zu/%zu; %zu mdev)   VmRSS %.1f GB, VmHWM %.1f GB\n",
           t_dm, X.n, R.n, t_recip, newton_st.down_corr, newton_st.up_corr, rns_st.n_mdev, mem_vmrss() / 1e9, mem_vmhwm() / 1e9);
    RESULT("dm", "s", t_dm);

    t = mem_now();
    int bad1 = tier1_res(N, d, Pres, Qres, &X, &R, verbose >= 2);
    double t_t1 = mem_now() - t;
    printf("T1    %8.2f s   %s\n", t_t1, bad1 ? "FAILED" : "ok: T(P+Q) == XQ + R and P, Q mod q for 8 primes");
    RESULT("T1", "s", t_t1);
    bi_free(&A); bi_free(&R); bi_free(&Q);
    printf("      VmRSS %.1f GB before dc\n", mem_vmrss() / 1e9);

    /* X's residues now, so X can go as soon as dc has copied it into its level pool */
    uint64_t Xres[T1_NQ];
    for (int i = 0; i < T1_NQ; i++) Xres[i] = vf_limbs_mod(X.l, X.n, t1_q[i]);
    t = mem_now();
    char *digits;
    if (bi_decimal) {                          /* WP2: the limbs are the digits; X < 10^(d+1) has ceil((d+1)/18) limbs */
        digits = (char *)mem_hreg_alloc(d + 2);
        size_t nl = (d + 1 + 17) / 18, pad = nl * 18 - (d + 1);     /* leading zeros to drop */
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < nl; i++) {
            char buf[19]; uint64_t v = i < X.n ? X.l[i] : 0;
            snprintf(buf, sizeof buf, "%018llu", (unsigned long long)v);
            size_t pos = (nl - 1 - i) * 18;                      /* padded position of this limb's first digit */
            for (int c = 0; c < 18; c++) if (pos + c >= pad) digits[pos + c - pad] = buf[c];
        }
        digits[d + 1] = 0;
        free(X.l); X.l = 0; X.n = X.cap = 0;
    } else {
        todec_free_input = 1;
        todec(0, &X, d + 1);                   /* allocates the 40 GB digit string only at the LEAF step */
        digits = todec_out;
    }
    double t_dc = mem_now() - t;
    if (bi_decimal) printf("dc    %8.2f s   digits formatted from %zu decimal limbs (no conversion)\n", t_dc, (d + 1 + 17) / 18);
    else printf("dc    %8.2f s   %zu leaf pieces, %d levels (prewarm %.1f top %.1f mid %.1f deep %.1f leaf %.1f)\n",
           t_dc, dec_st.pieces, dec_st.levels, dec_st.t_prewarm, dec_st.t_top, dec_st.t_mid, dec_st.t_deep, dec_st.t_leaf);
    RESULT("dc", "s", t_dc);
    printf("      VmHWM %.1f GB after dc\n", mem_vmhwm() / 1e9);

    t = mem_now();
    int bad2 = tier2(digits, d + 1, verbose >= 2);
    int bad3 = tier1_digits_res(digits, d + 1, Xres, verbose >= 2);
    double t_t2 = mem_now() - t;
    printf("T2    %8.2f s   windows %s, digits == X mod q %s\n", t_t2, bad2 ? "FAILED" : "ok", bad3 ? "FAILED" : "ok");
    RESULT("T2", "s", t_t2);
    printf("digits: %.62s...%.20s\n", digits, digits + d + 1 - 20);

    double total = mem_now() - t00;
    printf("total %8.2f s   (bs %.1f + 10dP %.1f + dm %.1f + T1 %.1f + dc %.1f + T2 %.1f); VmHWM %.1f GB\n",
           total, t_bs, t_10dp, t_dm, t_t1, t_dc, t_t2, mem_vmhwm() / 1e9);
    RESULT("total", "s", total);
    RESULT("vmhwm", "GB", mem_vmhwm() / 1e9);
    printf("paper A22 (4e10): 285.7 = bs 112.2 + 10dP 12.6 + dm 46.8 + T1 ~3 + dc 110.3\n");

    if (outfile) {
        FILE *f = fopen(outfile, "w");
        if (f) { fputc(digits[0], f); fputc('.', f); fwrite(digits + 1, 1, d, f); fputc('\n', f); fclose(f); printf("wrote %s\n", outfile); }
    }
    int fail = bad1 || bad2 || bad3;
    printf("%s\n", fail ? "VERIFY FAILED" : "VERIFY OK");
    mem_hreg_free(digits);
    rns_shutdown();
    return fail;
}

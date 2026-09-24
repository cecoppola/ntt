/* mn_plan.c - Phase 13d L (PLAN.md 32, row L): MN_PLAN_ONLY=<total digits>:<g> -- the large products of a run, as the code
 * would form them, printed without a device or a communicator (a login-node tool; ecalc.c calls mn_plan_run before rns_init,
 * after the plane-cap switch has set its knobs, and exits).
 *
 * What decides is the code's own (mn_plan.h): mn_groups_parse (the tree's schedule), rns_dist_mn_plan / rns_dist_db_plan (mn_grid's
 * and mul_grid's choice of one plane or the grid, split_grid_cap / split_grid with b_fits, and the cuts' skip rule), newton_chain_next
 * and newton_mn_chain_start (the reciprocal's chain), newton_mn_x1_level (the group a reciprocal step runs on).  What the plan
 * predicts is the operand sizes, which a run only knows once it has the numbers:
 *   - Q(a, b) = a (a+1) ... (b-1) over the term range [a, b) (binsplit.c span): log10 = (lgamma(b) - lgamma(a)) / ln 10;
 *     P(a, b) = Q(a, b) f(a, b), f = sum_{m=a}^{b-1} 1 / (a (a+1) ... m) (the span's recurrence: P/Q = (1 + P'/Q') / a),
 *     so f(1, N+1) = e - 1 and f ~ 1/a above; limbs = floor(log10 / 18) + 1 (decimal limbs).  In long double: exact but for
 *     a value within ~1e-5 digits of a limb boundary.
 *   - the reciprocal's iterate r has j + 1 limbs at the step to target j (every step ends with r' shifted to jn + 1 limbs);
 *     d = |B^(2j) - Q_t r >> ...| has D_EST(j) limbs (the error of r at j + 1 limbs: about B^j -- data-dependent, see D_EST);
 *     every step converges without a repeat (a repeat, rare, forms the step's two products once more).
 *   - S = P + Q has the limbs of Q e; X = floor(S B^dl / Q) has dl + 1 limbs (X ~ e B^dl).
 * Output: one line per product, `plan <phase> ...: ` followed by the line the run prints for it under RNS_VERBOSE=1 (dist_mn
 * node 0 / dist_db, up to the timing fields), then the piece (limbs), its transform length and the planes a node holds; per tree
 * level the pieces of node 0's group (what node 0 prints) and the range over all the level's groups (the high term ranges have
 * larger P, Q: up to ~35 % more limbs per node at 576); a summary with the pieces per phase in mn_model.py's categories
 * (run(...)['pieces'] = the distributed tree levels + the reciprocal's sharded steps + the division's two products, every
 * product counted as its formed pieces, a one-plane product as 1). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mn_plan.h"
#include "mdb.h"
#include "bigint.h"
#include "binsplit.h"
#include "rns_mul.h"
#include "modarith.h"

/* ---- the sizes (the only prediction) ---------------------------------------------------------------------------------- */
static long double g_dpl = 18.0L;                                 /* digits per limb (decimal limbs; binary: 64 log10 2) */
static long double lg10_q(unsigned long a, unsigned long b) { return b <= a + 1 ? (b == a + 1 ? log10l((long double)a) : 0.0L) : (lgammal((long double)b) - lgammal((long double)a)) / logl(10.0L); }
static long double lg10_f(unsigned long a, unsigned long b)       /* log10 of P / Q over [a, b) */
{
    long double t = 1.0L, f = 0.0L;
    for (unsigned long m = a; m < b && m < a + 64; m++) { t /= (long double)m; f += t; if (t < f * 1e-30L) break; }
    return log10l(f);
}
static size_t limbs_of(long double lg) { return lg < 0 ? 1 : (size_t)floorl(lg / g_dpl) + 1; }
struct pq { unsigned long a, b; size_t pn, qn; long double lq; };
static struct pq pq_of(unsigned long a, unsigned long b) { struct pq x; x.a = a; x.b = b; x.lq = lg10_q(a, b); x.qn = limbs_of(x.lq); x.pn = limbs_of(x.lq + lg10_f(a, b)); return x; }
/* node r's terms (ecalc.c: bs_a0 = 1 + N r / size) */
static unsigned long term0(unsigned long N, int r, int size) { unsigned __int128 nn = N; return 1 + (unsigned long)(nn * (unsigned)r / (unsigned)size); }
/* d = |B^(2j) - u| at the step to target j: r carries j + 1 limbs, so its error is about one unit of its last limb -- d ~ B^j.
 * The run's value is data-dependent (validated against real runs: results/L13d.md); it sets only the second product's
 * second operand, whose grid it can move only at a boundary */
static size_t D_EST(size_t j) { const char *e = getenv("MN_PLAN_DEXTRA"); long x = e ? atol(e) : 0; return (size_t)((long)j + x > 1 ? (long)j + x : 1); }

/* ---- the products ------------------------------------------------------------------------------------------------------ */
enum { PH_LEAF, PH_TREE, PH_RCHAIN, PH_RECIP, PH_DIV, PH_N };
static const char *ph_name[PH_N] = { "leaf", "tree", "recip1", "recip", "div" };
static long g_pieces[PH_N], g_prods[PH_N], g_grids[PH_N];
static int g_quiet;                                               /* MN_PLAN_QUIET=1: the summary lines only (the sweep) */
static void show_mn(int ph, const char *what, size_t na, size_t nb, int g, int has_x, size_t lowcut, size_t w, int print, struct rns_grid_plan *out)
{
    struct rns_grid_plan p; rns_dist_mn_plan(na, nb, g, has_x, lowcut, w, &p);
    size_t N = na + nb + (has_x ? 1 : 0); int trunc = w < N;
    if (ph >= 0) { g_pieces[ph] += p.formed; g_prods[ph]++; if (!p.one) g_grids[ph]++; }
    if (print && !g_quiet)
        printf("plan %-6s %s: dist_mn node 0: %zu x %zu limbs over %d x 4 ranks (cap 2^%d): %d x %d pieces, %d formed, %d skipped%s%s | piece %zu + %zu limbs, 2^%d points (%d x %d), planes %.2f GB per node\n",
               ph_name[ph < 0 ? PH_TREE : ph], what, na, nb, g, p.logcap, p.ka, p.kb, p.formed, p.skipped, trunc ? " (low product)" : "", lowcut ? " (low cut)" : "",
               p.pa, p.pb, (int)log2((double)p.pts), p.logR, p.logC, p.plane_bytes * 1e-9);
    if (out) *out = p;
}
static void show_db(int ph, const char *what, size_t na, size_t nb, size_t lowcut, size_t w)
{
    struct rns_grid_plan p; rns_dist_db_plan(na, nb, lowcut, w, &p);
    g_pieces[ph] += p.formed; g_prods[ph]++; if (!p.one) g_grids[ph]++;
    if (g_quiet) return;
    char pts[32]; if (p.pts & (p.pts - 1)) snprintf(pts, sizeof pts, "3*2^%d", (int)log2((double)(p.pts / 3))); else snprintf(pts, sizeof pts, "2^%d", (int)log2((double)p.pts));
    if (p.one) printf("plan %-6s %s:    dist_db %zu limbs | one plane of %s points (%s form), planes %.2f GB per node\n", ph_name[ph], what, na + nb, pts, p.form_b ? "B" : "C", p.plane_bytes * 1e-9);
    else printf("plan %-6s %s:    dist_db %zu x %zu limbs: %d x %d pieces of %zu + %zu, %d formed, %d skipped%s%s | %s points per piece (%s form), planes %.2f GB per node\n", ph_name[ph], what, na, nb, p.ka, p.kb, p.pa, p.pb, p.formed, p.skipped,
                lowcut ? " (low cut)" : "", w != (size_t)-1 ? " (low product)" : "", pts, p.form_b ? "B" : "C", p.plane_bytes * 1e-9);
}

/* ---- the leaf tree's device levels (binsplit.c's level loop: spans of bs_seed_terms, pairs combined, the mdev tier on the device
 * above 2^bs_mdev_logl: P = P_a Q_b + P_b, Q = Q_a Q_b as two dist_db products per pair) -------------------------------------- */
static void plan_leaf(unsigned long a0, unsigned long b1)
{
    /* the tier knobs read as binsplit.c reads them (seed_limbs, binsplit_e): BS_SEED_TERMS 256, BS_SCHOOL_NL 0, BS_MDEV_LOGL
     * RNS_BATCH_LOGL_MAX, BS_DEV_MDEV 1, BS_DEVICE_POOLS 1 */
    unsigned long S = getenv("BS_SEED_TERMS") ? strtoul(getenv("BS_SEED_TERMS"), 0, 10) : 256;
    size_t school = getenv("BS_SCHOOL_NL") ? (size_t)atol(getenv("BS_SCHOOL_NL")) : 0;
    int mlog = getenv("BS_MDEV_LOGL") ? atoi(getenv("BS_MDEV_LOGL")) : RNS_BATCH_LOGL_MAX;
    int devm = (getenv("BS_DEV_MDEV") ? atoi(getenv("BS_DEV_MDEV")) : 1) && (getenv("BS_DEVICE_POOLS") ? atoi(getenv("BS_DEVICE_POOLS")) : 1);
    unsigned long nspan = (b1 - a0 + S - 1) / S;
    int lvl = 0, nbatch = 0; size_t n = nspan;
    for (int l = 0; n > 1; l++) {
        size_t npairs = n / 2, m = (size_t)1 << l, max_nl = 0;
        /* node i of level l covers spans [i 2^l, (i+1) 2^l) (the last cut at nspan) */
        #define NODE_RANGE(i, lo, hi) do { unsigned long s0 = (unsigned long)((i) * m), s1 = (unsigned long)(((i) + 1) * m); if (s1 > nspan) s1 = nspan; lo = a0 + s0 * S; hi = a0 + s1 * S; if (hi > b1) hi = b1; } while (0)
        for (size_t i = 0; i < n; i++) { unsigned long lo, hi; NODE_RANGE(i, lo, hi); struct pq x = pq_of(lo, hi); if (x.pn > max_nl) max_nl = x.pn; if (x.qn > max_nl) max_nl = x.qn; }
        int mdev = max_nl > school && 2 * max_nl + 1 > ((size_t)1 << mlog);
        if (!mdev || !devm) { nbatch++; n = npairs + (n & 1); continue; }
        lvl++;
        for (size_t i = 0; i < npairs; i++) {
            unsigned long al, ah, bl, bh; NODE_RANGE(2 * i, al, ah); NODE_RANGE(2 * i + 1, bl, bh);
            struct pq A = pq_of(al, ah), B = pq_of(bl, bh); char w[96];
            snprintf(w, sizeof w, "level %d pair %zu of %zu P", l + 1, i, npairs); show_db(PH_LEAF, w, A.pn, B.qn, 0, (size_t)-1);
            snprintf(w, sizeof w, "level %d pair %zu of %zu Q", l + 1, i, npairs); show_db(PH_LEAF, w, A.qn, B.qn, 0, (size_t)-1);
        }
        #undef NODE_RANGE
        n = npairs + (n & 1);
    }
    if (!g_quiet) printf("plan leaf   terms [%lu, %lu): %lu spans of %lu, %d levels below the device tier (batch/school: no grids), %d device (mdev) levels\n", a0, b1, nspan, S, nbatch, lvl);
}

/* ---- the distributed tree (mn.c mn_tree / tree_level / tree_level_k) --------------------------------------------------------- */
struct lvl_sum { long pieces0, pmin, pmax; int gmax0; };
static long plan_group(int l, unsigned long N, int size, int g0, int g, int gp, int print)
{
    int nch = (g + gp - 1) / gp; long before = g_pieces[PH_TREE];
    int ph = print ? PH_TREE : -1;                                /* the other groups: counted here, not in the summary */
    long cnt = 0; struct rns_grid_plan p;
    #define CH(i) pq_of(term0(N, g0 + (i) * gp, size), term0(N, (g0 + ((i) + 1) * gp < g0 + g ? g0 + ((i) + 1) * gp : g0 + g), size))
    struct pq run = CH(nch - 1);
    for (int i = nch - 2; i >= 0; i--) {                          /* Horner from the top child (tree_level: nch = 2, the same pair) */
        struct pq c = CH(i); char w[96];
        snprintf(w, sizeof w, "level %d [%d, %d) combine %d of %d P", l, g0, g0 + g, nch - 1 - i, nch - 1);
        show_mn(ph, w, c.pn, run.qn, g, 1, 0, (size_t)-1, print, &p); cnt += p.formed;
        snprintf(w, sizeof w, "level %d [%d, %d) combine %d of %d Q", l, g0, g0 + g, nch - 1 - i, nch - 1);
        show_mn(ph, w, c.qn, run.qn, g, 0, 0, (size_t)-1, print, &p); cnt += p.formed;
        run = pq_of(c.a, run.b);
    }
    #undef CH
    (void)before;
    return cnt;
}
static void plan_tree(unsigned long N, int size, long *level_max_total)
{
    int gs[64]; int L = mn_groups_parse(size, gs, 63);
    int captest = getenv("MN_TREE_LOGN_TEST") ? atoi(getenv("MN_TREE_LOGN_TEST")) : 0;
    if (captest) rns_dist_plan_cap_test(captest);
    *level_max_total = 0;
    for (int l = 1; l <= L; l++) {
        int Gl = gs[l - 1], Gp = l > 1 ? gs[l - 2] : 1;
        long p0 = -1, pmin = -1, pmax = -1; int gmax = 0, ngroups = 0;
        for (int g0 = 0; g0 < size; g0 += Gl) {
            int g = Gl < size - g0 ? Gl : size - g0, nch = (g + Gp - 1) / Gp;
            if (nch < 2) continue;                                /* one child: carried up unchanged */
            long c = plan_group(l, N, size, g0, g, Gp, g0 == 0);
            if (g0 == 0) p0 = c;
            if (pmin < 0 || c < pmin) pmin = c;
            if (c > pmax) { pmax = c; gmax = g0; }
            ngroups++;
        }
        if (p0 < 0) continue;
        *level_max_total += pmax;
        int nch = (Gl < size ? Gl : size) / Gp + ((Gl < size ? Gl : size) % Gp != 0);
        if (!g_quiet) printf("plan tree   level %d: groups of %d (%d-way, children of %d): node 0's group %ld pieces; over the level's %d groups %ld .. %ld (the most: [%d, %d))\n",
                             l, Gl, nch, Gp, p0, ngroups, pmin, pmax, gmax, gmax + (Gl < size - gmax ? Gl : size - gmax));
    }
    if (captest) rns_dist_plan_cap_test(0);
}

/* ---- the reciprocal and the division ------------------------------------------------------------------------------------ */
/* size > 1: newton_db.c recip_mn (the single-node chain on every node to kp, then the sharded steps, each on the X1 group) */
static void plan_recip_mn(size_t nq, size_t k, int size)
{
    size_t kp = newton_mn_chain_start(k), T = 2 * kp + 2 < nq ? 2 * kp + 2 : nq;
    /* the single-node part: recip_db2 on the top T limbs of Q, from the seed's j = 2 */
    size_t j = 2; char w[96];
    while (j < kp) { size_t jn = newton_chain_next(j, kp), take = 2 * j + 2 < T ? 2 * j + 2 : T;
        snprintf(w, sizeof w, "chain j %zu -> %zu Q_t r", j, jn); show_db(PH_RCHAIN, w, take, j + 1, 0, (size_t)-1);
        snprintf(w, sizeof w, "chain j %zu -> %zu r d", j, jn); show_db(PH_RCHAIN, w, j + 1, D_EST(j), 0, (size_t)-1);
        j = jn; }
    if (!g_quiet) printf("plan recip  recip(mn): the single-node chain to %zu limbs, then the sharded steps to %zu over %d nodes\n", kp, k, size);
    j = kp;
    while (j < k) {
        size_t jn = newton_chain_next(j, k), take = 2 * j + 2 < nq ? 2 * j + 2 : nq;
        int Ln = newton_mn_x1_level(take, j + 1, size, size, 0), gs = Ln ? 1 << Ln : size;
        snprintf(w, sizeof w, "j %zu -> %zu Q_t r", j, jn);
        if (take == nq && gs == size) show_mn(PH_RECIP, w, nq, j + 1, gs, 0, 0, (size_t)-1, 1, 0);   /* Q itself (no cache hold: RNS_DIST_CACHE_HOLD off) */
        else show_mn(PH_RECIP, w, take, j + 1, gs, 0, 0, (size_t)-1, 1, 0);
        snprintf(w, sizeof w, "j %zu -> %zu r d", j, jn);
        show_mn(PH_RECIP, w, j + 1, D_EST(j), gs, 0, 0, (size_t)-1, 1, 0);
        j = jn;
    }
}
/* size 1: newton_db.c recip_db2 on the whole Q (the prewarm's k = k_mu) */
static void plan_recip_db(size_t nq, size_t k)
{
    size_t j = 2; char w[96];
    while (j < k) { size_t jn = newton_chain_next(j, k), take = 2 * j + 2 < nq ? 2 * j + 2 : nq;
        snprintf(w, sizeof w, "j %zu -> %zu Q_t r", j, jn); show_db(PH_RECIP, w, take, j + 1, 0, (size_t)-1);
        snprintf(w, sizeof w, "j %zu -> %zu r d", j, jn); show_db(PH_RECIP, w, j + 1, D_EST(j), 0, (size_t)-1);
        j = jn; }
}

int mn_plan_run(unsigned long d, unsigned long N, int size, int pool_log)
{
    g_quiet = getenv("MN_PLAN_QUIET") && atoi(getenv("MN_PLAN_QUIET"));
    if (!bi_decimal) g_dpl = 64.0L * log10l(2.0L);
    int np = ec_np_init();
    size_t p0, p1; rns_plane_pool_bytes(pool_log, rns_planes_3q30 > 0, np, &p0, &p1);          /* rns_init's pools (the device flow) */
    if (rns_pool1_bytes_req) p1 = rns_pool1_bytes_req;
    if (getenv("RNS_POOL1_GB")) p1 = (size_t)(atof(getenv("RNS_POOL1_GB")) * 1e9);
    rns_dist_plan_pools(p0, p1);
    struct pq all = pq_of(1, N + 1);
    size_t nq = all.qn, pn = all.pn, dl = bi_decimal ? (d + 17) / 18 : (size_t)ceil(d * log2(10.0) / 64.0);
    int gs[64]; int L = mn_groups_parse(size, gs, 63);
    printf("== MN_PLAN_ONLY: e to %lu digits on %d node-process%s: N %lu terms, P %zu limbs, Q %zu limbs (predicted), dl %zu; pool_log %d, %d primes, pools %.2f + %.2f GiB per APU; MN_GROUPS",
           d, size, size > 1 ? "es" : "", N, pn, nq, dl, pool_log, np, p0 / 1073741824.0, p1 / 1073741824.0);
    for (int l = 0; l < L; l++) printf("%c%d", l ? ',' : ' ', gs[l]);
    printf("%s ==\n", L ? "" : " - (one node)");

    /* the leaf (node 0's range) */
    plan_leaf(1, size > 1 ? term0(N, 1, size) : N + 1);
    long tree_max = 0;
    size_t k_mu, sn;
    if (size > 1) {
        plan_tree(N, size, &tree_max);
        /* newton_mn_divmod: k from S's largest possible length, the reciprocal, S = P + Q, the two cut products (X1 groups) */
        k_mu = pn + 1 + dl - nq + 1;
        plan_recip_mn(nq, k_mu, size);
        sn = limbs_of(all.lq + log10l(expl(1.0L)));               /* S = P + Q = Q e (P / Q = e - 1 over [1, N+1)) */
        size_t na = sn + dl, k = na - nq + 1, w = nq + 2, sh = nq - 1 - dl, ahn = sn - sh, xn = dl + 1;
        int La = newton_mn_x1_level(ahn, k + 1, size, size, 2 * (ahn + k + 1)), Lx = newton_mn_x1_level(xn, nq, size, size, 2 * (xn + nq));
        show_mn(PH_DIV, "A_h mu", ahn, k + 1, La ? 1 << La : size, 0, k + 1, (size_t)-1, 1, 0);
        show_mn(PH_DIV, "X Q mod B^w", xn < w ? xn : w, nq < w ? nq : w, Lx ? 1 << Lx : size, 0, 0, w, 1, 0);
    } else {
        /* ecalc.c's device flow (ovl3): k_mu from P's device length, the prewarm reciprocal, S = P + Q, newton_db_divmod_shifted */
        k_mu = pn + 1 + dl - nq + 1;
        plan_recip_db(nq, k_mu);
        sn = limbs_of(all.lq + log10l(expl(1.0L)));
        size_t na = sn + dl, k = na - nq + 1, w = nq + 2, sh = nq - 1 - dl, ahn = sn - sh, xn = dl + 1;
        show_db(PH_DIV, "A_h mu", ahn, k + 1, k + 1, (size_t)-1);
        show_db(PH_DIV, "X Q mod B^w", xn < w ? xn : w, nq < w ? nq : w, 0, w);
    }
    long tot = g_pieces[PH_TREE] + g_pieces[PH_RECIP] + g_pieces[PH_DIV];
    printf("plan summary %.4g digits g %d: pieces tree %ld recip %ld div %ld total %ld | tree with each level's largest group %ld, total %ld | grids tree %ld recip %ld div %ld | leaf dist_db %ld (%ld grids) recip single-node chain %ld\n",
           (double)d, size, g_pieces[PH_TREE], g_pieces[PH_RECIP], g_pieces[PH_DIV], tot, size > 1 ? tree_max : 0, (size > 1 ? tree_max : 0) + g_pieces[PH_RECIP] + g_pieces[PH_DIV],
           g_grids[PH_TREE], g_grids[PH_RECIP], g_grids[PH_DIV], g_pieces[PH_LEAF], g_grids[PH_LEAF], g_pieces[PH_RCHAIN]);
    if (!g_quiet) printf("plan note   pieces = products formed (a one-plane product is 1 piece), in mn_model.py's categories (run()['pieces'] = tree + recip + div);"
                         " size 1: the dist tier's (the model counts none there)\n");
    return 0;
}

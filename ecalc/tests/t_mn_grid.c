/* t_mn_grid - the multi-node product tier over shares (Phase 9 A3): products of sharded random numbers against the
 * host product (rns_mul, GMP-checked in t_mul), in shapes that force one plane, 1 x 2, 2 x 3 and 3 x 1 grids of piece
 * products, with the added operand X, as low products (pieces above a window skipped), with operand views, operands
 * sharded over subgroups (the tree's layout), and the shifted distributed add on its own.  DIST_LOGN_TEST=24 caps the
 * plane at 2^24 points per node, so the shapes are small multiples of the cap 2^(24 + floor(log2 g)).  Run under
 * mnrun.sh with any number of node-processes (Phase 11 L: 2, 3, 5, 6, 9 -- a group of any size balances the transform
 * over all its nodes; DIST_GEN=1 forces the general map at a power-of-two size too):
 * SLURM_JOB_ID=<id> ./mnrun.sh 3 ./tests/t_mn_grid [scale] [pool_log]  (scale: the shapes' unit as a fraction of the
 * cap, default 1; 0.25 for a quick run).  Every node checks its own share and the length. */
#include "harness.h"
#include <hip/hip_runtime.h>
#include "../dbig.h"
#include "../mdb.h"
#include "../mn.h"
#include "../rns_mul.h"
#include <string.h>
static rng_t rg = { 4242 };
static int g_me, g_size;
static void rnd_bi(bigint *a, size_t n, int kind) { bi_reserve(a, n ? n : 1); gen_limbs(a->l, n, kind, &rg); a->n = n; bi_norm(a); }
/* the number a sharded over nodes [g0, g0+g) with basis N >= a->n: this node's share (zeros above a->n) */
static void make_mdb(mdb *m, const bigint *a, size_t N, int g0, int g)
{
    if (m->sh.cap) db_free(&m->sh);
    memset(m, 0, sizeof *m); db_init(&m->sh); m->n = a->n; m->N = N; m->g0 = g0; m->g = g;
    size_t lo, hi; mdb_share(m, g_me, &lo, &hi);
    if (hi > lo) {
        bigint v; bi_init(&v); bi_reserve(&v, hi - lo); v.n = hi - lo;
        for (size_t i = 0; i < hi - lo; i++) v.l[i] = lo + i < a->n ? a->l[lo + i] : 0;
        db_from_bi(&m->sh, &v); m->sh.n = hi - lo; bi_free(&v);
    }
}
/* this node's share of C against the host number r: the length, the limbs, and zeros above r->n (the adds rely on them) */
static int check(const mdb *C, const bigint *r, const char *what)
{
    int ok = C->n == r->n; size_t lo, hi; mdb_share(C, g_me, &lo, &hi);
    if (!ok) printf("  node %d %s: n %zu vs %zu\n", g_me, what, C->n, r->n);
    if (hi > lo) {
        if (C->sh.n != hi - lo) { printf("  node %d %s: share n %zu vs %zu\n", g_me, what, C->sh.n, hi - lo); return 0; }
        bigint h; bi_init(&h); db_to_bi(&h, &C->sh); size_t bad = 0, first = 0;
        for (size_t i = 0; i < hi - lo; i++) { uint64_t want = lo + i < r->n ? r->l[lo + i] : 0; if (h.l[i] != want) { if (!bad) first = lo + i; bad++; } }
        if (bad) { printf("  node %d %s: %zu of %zu limbs of the share [%zu, %zu) differ (first at %zu)\n", g_me, what, bad, hi - lo, lo, hi, first); ok = 0; }
        bi_free(&h);
    }
    return ok;
}
static void ref_low(bigint *rl, const bigint *r, size_t w) { bi_reserve(rl, r->n ? r->n : 1); size_t n = r->n < w ? r->n : w; memcpy(rl->l, r->l, n * 8); rl->n = n; bi_norm(rl); }
int main(int argc, char **argv)
{
    double scale = argc > 1 ? atof(argv[1]) : 1.0;
    harness_meta("t_mn_grid"); bi_env_base(); rns_init(argc > 2 ? atoi(argv[2]) : 31);
    g_size = mn_init(); g_me = mn_rank();
    if (g_size < 2) { printf("t_mn_grid: needs COMM_SIZE >= 2 (mnrun.sh)\n"); return 2; }
    int L = 0; while ((1 << L) < g_size) L++;
    mn_group *G = mn_group_at(L);
    setenv("DIST_LOGN_TEST", "24", 1);
    int logcap = rns_mul_dist_mn_logcap(G);
    size_t cap = (size_t)1 << logcap, u = (size_t)(cap * scale);
    printf("t_mn_grid: node %d of %d, group [%d, %d) (%d x 4 ranks%s), plane cap 2^%d, unit %zu limbs\n", g_me, g_size, G->g0, G->g0 + G->g, G->g, (G->g & (G->g - 1)) || getenv("DIST_GEN") ? ", the general map" : "", logcap, u);
    bigint a, b, x, r, t, rl; bi_init(&a); bi_init(&b); bi_init(&x); bi_init(&r); bi_init(&t); bi_init(&rl);
    mdb A, B, X, C; memset(&A, 0, sizeof A); memset(&B, 0, sizeof B); memset(&X, 0, sizeof X); memset(&C, 0, sizeof C);
    /* shapes as fractions of the cap: one plane; 2 pieces; the 4e10 shape (2 x 3); 3 x 1; 2 x 2-ish */
    struct { double fa, fb; const char *name; } sh[] = { {0.30, 0.25, "one plane"}, {0.60, 0.50, "2 pieces"}, {1.03, 1.03, "2x3"}, {1.50, 0.40, "3x1"}, {0.90, 0.90, "subgroups"} };
    int half = g_size / 2 ? g_size / 2 : 1;
    for (size_t si = 0; si < sizeof sh / sizeof *sh; si++) for (int kind = 0; kind < 2; kind++) {
        size_t na = (size_t)(u * sh[si].fa), nb = (size_t)(u * sh[si].fb); if (na < 1000) na = 1000; if (nb < 1000) nb = 1000;
        int sub = si == 4;                                   /* A over the lower half of the nodes, B and X over the rest (the tree's layout) */
        rnd_bi(&a, na, kind); rnd_bi(&b, nb, kind); rnd_bi(&x, nb - 3, kind);
        make_mdb(&A, &a, na + 5, sub ? 0 : 0, sub ? half : g_size);
        make_mdb(&B, &b, nb + 2, sub ? half : 0, sub ? g_size - half : g_size);
        make_mdb(&X, &x, nb, sub ? half : 0, sub ? g_size - half : g_size);
        double t0 = now();
        rns_mul_dist_mn(&C, &A, &B, 0, G); rns_mul(&r, &a, &b);
        VERIFY(check(&C, &r, "product"), "%s %zu x %zu %s", sh[si].name, na, nb, gen_name[kind]);
        if (g_me == 0 && kind == 0) printf("   %s %zu x %zu: %.2f s\n", sh[si].name, na, nb, now() - t0);
        rns_mul_dist_mn(&C, &A, &B, &X, G); bi_add(&t, &r, &x);
        VERIFY(check(&C, &t, "product + x"), "%s %zu x %zu + x %s", sh[si].name, na, nb, gen_name[kind]);
        for (int wv = 0; wv < 3; wv++) {                      /* low products: pieces above w skipped, the result truncated (A5: in basis w) */
            size_t w = wv == 0 ? na + 2 : wv == 1 ? na / 2 + 1 : r.n - 1;
            rns_mul_low_mn(&C, &A, &B, G, w); ref_low(&rl, &r, w);
            VERIFY(check(&C, &rl, "low") && C.N <= w, "%s low %zu x %zu w %zu %s (basis %zu)", sh[si].name, na, nb, w, gen_name[kind], C.N);
        }
        {   /* Phase 10 A5: the low cut (the A_h mu product over shares): the pieces whose limbs end at or below the cut are skipped;
             * the reference is the product minus exactly those pieces (the grid from rns_mul_dist_mn_shape), with and without a high cut */
            int ka, kb; size_t an = a.n, bn = b.n; rns_mul_dist_mn_shape(an, bn, G, &ka, &kb);   /* (the normalised lengths: the grid is formed on them) */
            size_t pa = (an + ka - 1) / ka, pb = (bn + kb - 1) / kb;
            size_t cuts[] = { (na + nb) / 2, nb / 2 + 1, na + nb };
            for (int ci = 0; ci < 3; ci++) for (int hc = 0; hc < 2; hc++) {
                size_t cut = cuts[ci], w = hc ? r.n - 1 : (size_t)-1;   /* r.n - 1 >= max(na, nb): the operands' views are whole, the grid the same */
                rns_mul_dist_mn_cut(&C, &A, &B, G, cut, w);
                bi_copy(&t, &r); int skipped = 0;
                for (int j = 0; j < kb; j++) for (int i = 0; i < ka; i++) {
                    size_t oa = (size_t)i * pa, ob = (size_t)j * pb;
                    bigint ha = { a.l + oa, an - oa < pa ? an - oa : pa, 0 }, hb = { b.l + ob, bn - ob < pb ? bn - ob : pb, 0 }; bi_norm(&ha); bi_norm(&hb);
                    if (!ha.n || !hb.n || oa + ob + ha.n + hb.n > cut) continue;
                    rns_mul(&rl, &ha, &hb); bi_shl_limbs(&rl, &rl, oa + ob); bi_sub(&t, &t, &rl); skipped++;
                }
                if (hc) ref_low(&rl, &t, w); else bi_copy(&rl, &t);
                VERIFY(check(&C, &rl, "lowcut") && (!hc || C.N <= w), "%s lowcut %zu%s: %d x %d pieces, %d skipped %s", sh[si].name, cut, hc ? " + highcut" : "", ka, kb, skipped, gen_name[kind]);
            }
        }
        {   /* Phase 10 A1 over shares: B's piece transforms held across two products (the reciprocal's Q_t r and the
             * division's X Q): the product with B pinned, then the low product over the same B pieces (hits), then released */
            setenv("RNS_DIST_CACHE_HOLD", "1", 1);
            int held = rns_dist_cache_hold(1);
            rns_mul_dist_mn(&C, &A, &B, 0, G);
            VERIFY(check(&C, &r, "held product"), "%s held product %zu x %zu %s", sh[si].name, na, nb, gen_name[kind]);
            size_t w = na + nb - 1; rns_mul_low_mn(&C, &A, &B, G, w); ref_low(&rl, &r, w);
            size_t hits = 0, misses = 0; rns_dist_cache_stats(&hits, &misses);
            VERIFY(check(&C, &rl, "low after hold"), "%s low product after the hold w %zu %s (held %d: %zu hits, %zu misses)", sh[si].name, w, gen_name[kind], held, hits, misses);
            rns_dist_cache_hold(0); unsetenv("RNS_DIST_CACHE_HOLD");
        }
        {   /* views: A[na/3, na/3 + na/2) x B[7, ...) */
            size_t oa = na / 3, la = na / 2, ob = 7, lb = nb - 7;
            mdbv av = mdb_view(&A, oa, la, G), bv = mdb_view(&B, ob, lb, G);
            bigint ha = { a.l + oa, la, 0 }, hb = { b.l + ob, lb, 0 }; bi_norm(&ha); bi_norm(&hb);
            VERIFY(av.len == ha.n && bv.len == hb.n, "%s view lengths %zu/%zu vs %zu/%zu", sh[si].name, av.len, bv.len, ha.n, hb.n);
            rns_mul_dist_mn_v(&C, &av, &bv, 0, G, (size_t)-1); rns_mul(&t, &ha, &hb);
            VERIFY(check(&C, &t, "views"), "%s views %zu x %zu %s", sh[si].name, la, lb, gen_name[kind]);
            size_t w = la + 3; rns_mul_dist_mn_v(&C, &av, &bv, 0, G, w); ref_low(&rl, &t, w);
            VERIFY(check(&C, &rl, "views low"), "%s views low w %zu %s", sh[si].name, w, gen_name[kind]);
        }
        {   /* the shifted distributed add on its own: C (the product, basis na + nb + 1) += x << k */
            rns_mul_dist_mn(&C, &A, &B, &X, G); bi_add(&t, &r, &x);
            size_t ks[] = { 0, 7, na / 2, na };
            for (int ki = 0; ki < 4; ki++) {
                mdb_add_shifted(&C, &X, ks[ki], G); bi_shl_limbs(&rl, &x, ks[ki]); bi_add(&t, &t, &rl); mdb_norm(&C, G, (size_t)-1);
                VERIFY(check(&C, &t, "add_shifted"), "%s add_shifted k %zu %s", sh[si].name, ks[ki], gen_name[kind]);
            }
        }
    }
    /* Phase 12 G: the same over a group that does not start at node 0 -- every node's level-1 group [2k, 2k+2) (the tree's lower
     * levels: A on its first node, B and X on its second), the grid forced: the product, + X, and mdb_add_shifted.  (The tree at
     * 10^9 / 4 with its levels gridded gave a wrong P = P_A Q_B + P_B on the group [2, 4) while Q and the group [0, 2) were right.) */
    if (g_size >= 4) {
        mn_group *G1 = mn_group_at(1); int g0 = G1->g0, g1 = G1->g;
        int logcap1 = rns_mul_dist_mn_logcap(G1); size_t u1 = (size_t)(((size_t)1 << logcap1) * scale);
        printf("t_mn_grid: node %d: level-1 group [%d, %d), cap 2^%d, unit %zu limbs\n", g_me, g0, g0 + g1, logcap1, u1);
        for (int kind = 0; kind < 2 && g1 == 2; kind++) for (int si = 0; si < 3; si++) {
            size_t na = (size_t)(u1 * (si == 0 ? 0.6 : si == 1 ? 1.03 : 0.3)), nb = (size_t)(u1 * (si == 0 ? 0.5 : si == 1 ? 1.03 : 0.25));
            rnd_bi(&a, na, kind); rnd_bi(&b, nb, kind); rnd_bi(&x, nb - 3, kind);
            make_mdb(&A, &a, na + 5, g0, 1); make_mdb(&B, &b, nb + 2, g0 + 1, 1); make_mdb(&X, &x, nb, g0 + 1, 1);
            rns_mul_dist_mn(&C, &A, &B, 0, G1); rns_mul(&r, &a, &b);
            VERIFY(check(&C, &r, "subgroup product"), "group [%d, %d) product %zu x %zu %s", g0, g0 + g1, na, nb, gen_name[kind]);
            rns_mul_dist_mn(&C, &A, &B, &X, G1); bi_add(&t, &r, &x);
            VERIFY(check(&C, &t, "subgroup product + x"), "group [%d, %d) product + x %zu x %zu %s", g0, g0 + g1, na, nb, gen_name[kind]);
            rns_mul_dist_mn(&C, &A, &B, 0, G1); bi_copy(&t, &r);
            size_t ks[] = { 0, na / 2 };
            for (int ki = 0; ki < 2; ki++) {
                mdb_add_shifted(&C, &X, ks[ki], G1); bi_shl_limbs(&rl, &x, ks[ki]); bi_add(&t, &t, &rl); mdb_norm(&C, G1, (size_t)-1);
                VERIFY(check(&C, &t, "subgroup add_shifted"), "group [%d, %d) add_shifted k %zu %s", g0, g0 + g1, ks[ki], gen_name[kind]);
            }
        }
    }
    db_free(&A.sh); db_free(&B.sh); db_free(&X.sh); db_free(&C.sh);
    bi_free(&a); bi_free(&b); bi_free(&x); bi_free(&r); bi_free(&t); bi_free(&rl);
    mn_barrier(); mn_finalize();
    return verify_done("t_mn_grid");
}

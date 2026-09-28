/* cx_grid - Phase 15 CX (results/CX15.md): what the multi-node transform cache is worth per grid product.  The target's grid shapes
 * (tests/cx_shapes.py --emit: the 5.1e13 run's distinct grids scaled to this run's cap) are formed over the g node-processes of
 * mnrun.sh with the cache at each slot count of the list, interleaved: for every repetition, every shape, every count -- the same
 * operands, the same node.  rns_dist_cache_test_slots sets the count in-process (the slots are allocated once, at the first product
 * that wants them: RNS_DIST_CACHE_MN of them); RNS_DIST_CACHE_TRACE=1 prints the per-piece lines (DIST_STATS=1 adds the transforms'
 * parts).  Every product's share of C is hashed (sha256): the hashes must agree over the slot counts and the repetitions.
 *
 *   SLURM_JOB_ID=<id> ./mnrun.sh <g> env LIMB_BASE=10 ECALC_NP=4 RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_TRACE=1 ./tests/cx_grid <pool_log> <reps> <slots,..> <shapes file>
 *
 * shapes file: one line per shape, "na nb lowcut highcut tag" (highcut -1: none); tag ending in "_full" = the tree's layout (A over the
 * group's lower half, B over the upper half), otherwise both over the whole group.  Repetition 0 is a warm-up (each shape once, at the first count; printed, marked). */
#include "harness.h"
#include "sha256.h"
#include <hip/hip_runtime.h>
#include "../dbig.h"
#include "../mdb.h"
#include "../mn.h"
#include "../rns_mul.h"
#include <string.h>
extern "C" void rns_dist_cache_test_slots(int n);
extern "C" double rns_dist_cache_alloc_s(void);
static rng_t rg = { 1515 };
static int g_me, g_size;
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
static void share_hash(const mdb *C, char hex[65])
{
    size_t lo, hi; mdb_share(C, g_me, &lo, &hi);
    if (hi <= lo || !C->sh.n) { strcpy(hex, "(empty share)"); return; }
    bigint h; bi_init(&h); db_to_bi(&h, &C->sh);
    sha256_t s; sha256_init(&s); sha256_update(&s, h.l, h.n * 8); sha256_update(&s, &C->n, sizeof C->n);
    unsigned char o[32]; sha256_final(&s, o); for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", o[i]); hex[64] = 0;
    bi_free(&h);
}
int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: cx_grid <pool_log> <reps> <slots,..> <shapes file>\n"); return 2; }
    int pool_log = atoi(argv[1]), reps = atoi(argv[2]);
    int sl[8], ns = 0; { char *t = strdup(argv[3]); for (char *p = strtok(t, ","); p && ns < 8; p = strtok(0, ",")) sl[ns++] = atoi(p); }
    harness_meta("cx_grid"); bi_env_base(); rns_init(pool_log);
    g_size = mn_init(); g_me = mn_rank();
    if (g_size < 2) { printf("cx_grid: needs COMM_SIZE >= 2 (mnrun.sh)\n"); return 2; }
    int L = 0; while ((1 << L) < g_size) L++;
    mn_group *G = mn_group_at(L);
    printf("cx_grid: node %d of %d, plane cap 2^%d, pool_log %d, %d repetitions (+ warm-up), slot counts %s\n", g_me, g_size, rns_mul_dist_mn_logcap(G), pool_log, reps, argv[3]);
    struct shp { size_t na, nb, lo, hi; char tag[64]; } sh[32]; int nsh = 0;
    { FILE *f = fopen(argv[4], "r"); if (!f) { fprintf(stderr, "cx_grid: cannot open %s\n", argv[4]); return 2; }
      long long a, b, c, d; char t[64];
      while (nsh < 32 && fscanf(f, "%lld %lld %lld %lld %63s", &a, &b, &c, &d, t) == 5) { sh[nsh].na = a; sh[nsh].nb = b; sh[nsh].lo = c; sh[nsh].hi = d < 0 ? (size_t)-1 : (size_t)d; strcpy(sh[nsh].tag, t); nsh++; }
      fclose(f); }
    bigint a, b; bi_init(&a); bi_init(&b);
    mdb A[32], B[32], C; memset(A, 0, sizeof A); memset(B, 0, sizeof B); memset(&C, 0, sizeof C);
    int half = g_size / 2;
    for (int s = 0; s < nsh; s++) {                                    /* the operands, once per shape (uniform limbs) */
        bi_reserve(&a, sh[s].na); gen_limbs(a.l, sh[s].na, 0, &rg); a.n = sh[s].na; bi_norm(&a);
        bi_reserve(&b, sh[s].nb); gen_limbs(b.l, sh[s].nb, 0, &rg); b.n = sh[s].nb; bi_norm(&b);
        int sub = strstr(sh[s].tag, "_full") != 0 && g_size >= 2;
        make_mdb(&A[s], &a, a.n + 2, 0, sub ? half : g_size);
        make_mdb(&B[s], &b, b.n + 2, sub ? half : 0, sub ? g_size - half : g_size);
    }
    bi_free(&a); bi_free(&b);
    char ref[32][65]; memset(ref, 0, sizeof ref); int bad = 0;
    { int mx = 0; for (int k = 0; k < ns; k++) if (sl[k] > mx) mx = sl[k];       /* the slots allocated once, at the largest count: one untimed product */
      rns_dist_cache_test_slots(mx); mn_barrier(); double t0 = now();
      if (nsh) rns_mul_dist_mn(&C, &A[0], &B[0], 0, G);
      mn_barrier(); printf("cx_grid node %d: the allocating product (%d slots): %.3f s, the cache's allocation %.2f s\n", g_me, mx, now() - t0, rns_dist_cache_alloc_s()); }
    for (int r = 0; r <= reps; r++) for (int s = 0; s < nsh; s++) for (int k = 0; k < ns; k++) {
        if (r == 0 && k > 0) continue;                                 /* the warm-up: each shape once, at the first count */
        rns_dist_cache_test_slots(sl[k]);
        size_t h0, m0; rns_dist_cache_stats(&h0, &m0);                  /* (resets the counters) */
        mn_barrier(); double t0 = now();
        if (sh[s].lo || sh[s].hi != (size_t)-1) rns_mul_dist_mn_cut(&C, &A[s], &B[s], G, sh[s].lo, sh[s].hi);
        else rns_mul_dist_mn(&C, &A[s], &B[s], 0, G);
        mn_barrier(); double t = now() - t0;
        size_t hits, misses; rns_dist_cache_stats(&hits, &misses);
        char hx[65]; share_hash(&C, hx);
        if (!ref[s][0]) strcpy(ref[s], hx); else if (strcmp(ref[s], hx)) bad++;
        printf("cx_grid node %d: rep %d%s shape %s (%zu x %zu, cut %zu / %zd) slots %d: %.3f s, cache %zu hits %zu misses, C sha256 %.16s %s\n", g_me, r, r ? "" : " (warm-up)", sh[s].tag,
               sh[s].na, sh[s].nb, sh[s].lo, sh[s].hi == (size_t)-1 ? (ssize_t)-1 : (ssize_t)sh[s].hi, sl[k], t, hits, misses, hx, strcmp(ref[s], hx) ? "DIFFERS" : "same");
        fflush(stdout);
    }
    printf("cx_grid node %d: the cache's allocation %.2f s (once, excluded above)\n", g_me, rns_dist_cache_alloc_s());
    VERIFY(bad == 0, "node %d: every product's share identical over the slot counts and repetitions (%d differ)", g_me, bad);
    for (int s = 0; s < nsh; s++) { db_free(&A[s].sh); db_free(&B[s].sh); }
    if (C.sh.cap) db_free(&C.sh);
    mn_barrier(); mn_finalize();
    return verify_done("cx_grid");
}

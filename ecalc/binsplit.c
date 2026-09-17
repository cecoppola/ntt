/* binsplit.c - see binsplit.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "binsplit.h"
#include "rns_mul.h"
#include "mem.h"

bs_stats bs_st;
int bs_seed_terms = 512;
int bs_school_nl = 160;
int bs_verbose = 0;

unsigned long e_terms(unsigned long d)
{
    unsigned long N = 1;
    while (lgamma((double)N + 1.0) / log(10.0) < (double)d + 50.0) N++;
    return N;
}

/* span [a, b) by schoolbook, right to left: P = 1, Q = b-1; prepend k: P = Q + P, Q = k Q */
static void span(bigint *P, bigint *Q, unsigned long a, unsigned long b)
{
    bi_set_u64(P, 1); bi_set_u64(Q, b - 1);
    for (unsigned long k = b - 1; k-- > a;) {
        bi_add(P, P, Q);
        bi_mul_u64(Q, Q, k);
    }
}
void binsplit_ref(bigint *P, bigint *Q, unsigned long a, unsigned long b)
{
    if (b - a <= 64) { span(P, Q, a, b); return; }
    unsigned long m = (a + b) / 2;
    bigint P2, Q2, t; bi_init(&P2); bi_init(&Q2); bi_init(&t);
    binsplit_ref(P, Q, a, m);
    binsplit_ref(&P2, &Q2, m, b);
    bi_mul_school(&t, P, &Q2); bi_add(P, &t, &P2);
    bi_mul_school(&t, Q, &Q2); bi_copy(Q, &t);
    bi_free(&P2); bi_free(&Q2); bi_free(&t);
}

/* a level: nodes as (offset, length) pairs into region pools.  WP3: node i of a
 * level of n nodes lives in region r = NR i / n -- a subtree per region, so a
 * pair (2i, 2i+1) and its parent share a region (up to one boundary pair per
 * level) and a region's products are transformed by its own APU with every
 * read local (RESULTS.md 55).  Region r's pool is device r's memory when
 * bs_regions_on_device (default when there are devices), else registered
 * host memory. */
#define NR 4
struct node { size_t po, pn, qo, qn; int r; };
struct level { uint64_t *pool[NR]; struct node *nd; size_t n; };
#define NODE_P(lv, nd) ((lv).pool[(nd)->r] + (nd)->po)
#define NODE_Q(lv, nd) ((lv).pool[(nd)->r] + (nd)->qo)
int bs_regions_on_device = -1;                       /* BS_DEVICE_POOLS: 1 device pools, 0 host */
static int region_of(size_t i, size_t n) { size_t r = i * NR / n; return (int)(r < NR ? r : NR - 1); }

/* copy limbs out of (or into) region r's pool with the threads of r's node (a lone memcpy from device memory runs at a few GB/s) */
static void region_copy(uint64_t *dst, const uint64_t *src, size_t limbs, int r)
{
    if (limbs < ((size_t)1 << 20)) { memcpy(dst, src, limbs * 8); return; }
#pragma omp parallel
    {
        int rk, cnt = mem_region_threads(&rk), home = mem_thread_home();
        if (home < 0 || r % NR == home % NR) {
            size_t chunk = (limbs + cnt - 1) / cnt, lo = chunk * rk, hi = lo + chunk < limbs ? lo + chunk : limbs;
            if (lo < hi) memcpy(dst + lo, src + lo, (hi - lo) * 8);
        }
    }
}
static uint64_t *g_pool[2][NR]; static size_t g_cap[2][NR];
static uint64_t *pool_get(int which, int r, size_t limbs)
{
    if (g_cap[which][r] < limbs) {
        if (g_pool[which][r]) { if (mem_dev_of(g_pool[which][r]) >= 0) mem_dev_free(g_pool[which][r]); else mem_hreg_free(g_pool[which][r]); }
        size_t cap = limbs + limbs / 8 + 4096;
        int nd = bs_regions_on_device ? mem_device_count() : 0;
        g_pool[which][r] = (uint64_t *)(nd > 0 ? mem_dev_alloc(r % nd, cap * 8) : mem_hreg_alloc(cap * 8));
        g_cap[which][r] = cap;
        if (bs_verbose) printf("bs: level pool %d region %d -> %.2f GB (%s)\n", which, r, cap * 8e-9, nd > 0 ? "device" : "host");
    }
    return g_pool[which][r];
}

static size_t seed_limbs(unsigned long N, size_t *per_out, unsigned long *nspan_out)
{
    unsigned long S = bs_seed_terms, nspan = (N + S - 1) / S;
    size_t per = (S * (size_t)ceil(log2((double)N + 2.0)) + 128) / (bi_decimal ? 59 : 64) + 2;   /* a decimal limb holds 59.8 bits */
    if (per_out) *per_out = per; if (nspan_out) *nspan_out = nspan;
    return 2 * per * nspan;
}
void binsplit_pregrow(unsigned long N)
{
    if (bs_regions_on_device < 0) bs_regions_on_device = getenv("BS_DEVICE_POOLS") ? atoi(getenv("BS_DEVICE_POOLS")) : 1;
    size_t total0 = seed_limbs(N, 0, 0);
    for (int w = 0; w < 2; w++) for (int r = 0; r < NR; r++) pool_get(w, r, total0 / NR + total0 / (NR * 4) + (1 << 20));
}
void binsplit_e(bigint *P, bigint *Q, unsigned long N)
{
    double t0 = mem_now(), t;
    memset(&bs_st, 0, sizeof bs_st);
    unsigned long S = bs_seed_terms, nspan; size_t per;
    /* seed spans: Q(a,b) < b^S, P < S b^S: reserve (S log2(N+1) + 64 + 64) / 64 limbs each */
    seed_limbs(N, &per, &nspan);
    struct level cur, nxt;
    if (bs_regions_on_device < 0) bs_regions_on_device = getenv("BS_DEVICE_POOLS") ? atoi(getenv("BS_DEVICE_POOLS")) : 1;
    cur.n = nspan; cur.nd = (struct node *)malloc(nspan * sizeof *cur.nd);
    size_t r0[NR + 1];                               /* first node of each region at level 0 */
    for (int r = 0; r <= NR; r++) { r0[r] = 0; while (r0[r] < nspan && region_of(r0[r], nspan) < r) r0[r]++; }
    /* region pools sized once from level 0 (the levels' totals stay within a few percent of it;
     * pool_get still grows if a level needs more); binsplit_pregrow does this at init, outside the timed phase */
    size_t total0 = 2 * per * nspan;
    for (int w = 0; w < 2; w++) for (int r = 0; r < NR; r++) pool_get(w, r, total0 / NR + total0 / (NR * 4) + (1 << 20));
    for (int r = 0; r < NR; r++) cur.pool[r] = pool_get(0, r, 2 * per * (r0[r + 1] - r0[r]) + 2);
    if (mem_dev_of(cur.pool[0]) >= 0 && (size_t)rns_pool_log() && ((size_t)8 << rns_pool_log()) < (2 * per * (r0[1] - r0[0]) + 2) * 8) { fprintf(stderr, "bs: seed region larger than the staging buffer\n"); abort(); }
    if (bs_st.peak_pool_limbs < total0) bs_st.peak_pool_limbs = total0;
    t = mem_now();
    /* seeds go to a host staging area per region (small scattered writes into device memory are slow,
     * RESULTS.md 56), then one bulk copy per region */
    uint64_t *stage[NR]; int own_stage = mem_dev_of(cur.pool[0]) < 0;
    for (int r = 0; r < NR; r++) stage[r] = own_stage ? (uint64_t *)malloc((2 * per * (r0[r + 1] - r0[r]) + 2) * 8) : rns_hstage(r % mem_device_count());
#pragma omp parallel
    {
        bigint p, q; bi_init(&p); bi_init(&q);
        int rk, cnt = mem_region_threads(&rk), home = mem_thread_home();   /* region-aware: this node's threads do this region's spans */
        for (int r = 0; r < NR; r++) {
            if (home >= 0 && r % NR != home % NR) continue;               /* (home < 0: threads not pinned, every thread does everything by rank) */
            size_t lo = r0[r], hi = r0[r + 1];
            for (size_t i = lo + (size_t)rk; i < hi; i += (size_t)cnt) {
                unsigned long a = 1 + i * S, b = a + S; if (b > N + 1) b = N + 1;
                span(&p, &q, a, b);
                struct node *nd = &cur.nd[i];
                nd->r = r; nd->po = 2 * per * (i - lo); nd->pn = p.n; nd->qo = nd->po + per; nd->qn = q.n;
                if (p.n > per || q.n > per) { fprintf(stderr, "bs: seed span overflow\n"); abort(); }
                memcpy(stage[r] + nd->po, p.l, p.n * 8); memcpy(stage[r] + nd->qo, q.l, q.n * 8);
            }
        }
        bi_free(&p); bi_free(&q);
    }
    double t_span = mem_now() - t;
    for (int r = 0; r < NR; r++) {
        size_t limbs = 2 * per * (r0[r + 1] - r0[r]);
#pragma omp parallel
        {
            int rk, cnt = mem_region_threads(&rk), home = mem_thread_home();
            if (home < 0 || r % NR == home % NR) {                        /* this node's threads copy their region */
                size_t chunk = (limbs + cnt - 1) / cnt, lo = chunk * rk, hi = lo + chunk < limbs ? lo + chunk : limbs;
                if (lo < hi) memcpy(cur.pool[r] + lo, stage[r] + lo, (hi - lo) * 8);
            }
        }
        if (own_stage) free(stage[r]);
    }
    bs_st.t_seed = mem_now() - t;
    if (bs_verbose) printf("bs: %lu terms, %lu spans of %lu, seeds %.2f s (spans %.2f, copy %.2f)\n", N, nspan, S, bs_st.t_seed, t_span, bs_st.t_seed - t_span);

    int which = 0;
    while (cur.n > 1) {
        t = mem_now();
        size_t npairs = cur.n / 2, odd = cur.n & 1, max_nl = 0;
        for (size_t i = 0; i < cur.n; i++) { if (cur.nd[i].pn > max_nl) max_nl = cur.nd[i].pn; if (cur.nd[i].qn > max_nl) max_nl = cur.nd[i].qn; }
        /* next level layout: P slot n(P1)+n(Q2)+1, Q slot n(Q1)+n(Q2) */
        nxt.n = npairs + odd; nxt.nd = (struct node *)malloc(nxt.n * sizeof *nxt.nd);
        size_t offr[NR] = {0}, off = 0;
        for (size_t i = 0; i < npairs; i++) {
            struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
            o->r = region_of(i, nxt.n);
            o->po = offr[o->r]; offr[o->r] += a->pn + b->qn + 1; o->qo = offr[o->r]; offr[o->r] += a->qn + b->qn;
            o->pn = o->qn = 0;
        }
        if (odd) { struct node *a = &cur.nd[cur.n - 1], *o = &nxt.nd[npairs]; o->r = region_of(npairs, nxt.n); o->po = offr[o->r]; offr[o->r] += a->pn; o->qo = offr[o->r]; offr[o->r] += a->qn; o->pn = a->pn; o->qn = a->qn; }
        which ^= 1;
        for (int r = 0; r < NR; r++) { nxt.pool[r] = pool_get(which, r, offr[r] + 2); off += offr[r]; }
        if (off > bs_st.peak_pool_limbs) bs_st.peak_pool_limbs = off;
        double tl0 = mem_now(), tl1 = 0, tl2 = 0;
        const char *tier; int normed = 0;
        if (max_nl <= (size_t)bs_school_nl) {
            tier = "school"; bs_st.school_levels++;
#pragma omp parallel
            {
                int rk, cnt = mem_region_threads(&rk), home = mem_thread_home();
                for (size_t i = (size_t)rk; i < npairs; i += (size_t)cnt) {
                    struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                    if (home >= 0 && o->r % NR != home % NR) continue;
                    uint64_t *pp = NODE_P(nxt, o), *qq = NODE_Q(nxt, o);
                    limb_mul_school(pp, NODE_P(cur, a), a->pn, NODE_Q(cur, b), b->qn);
                    pp[a->pn + b->qn] = limb_add(pp, pp, a->pn + b->qn, NODE_P(cur, b), b->pn);
                    limb_mul_school(qq, NODE_Q(cur, a), a->qn, NODE_Q(cur, b), b->qn);
                }
            }
        } else if (2 * max_nl + 1 <= ((size_t)1 << RNS_BATCH_LOGL_MAX)) {
            tier = "batch"; bs_st.batch_levels++;
            rns_prod *pr = (rns_prod *)calloc(1, 2 * npairs * sizeof *pr);
            for (size_t i = 0; i < npairs; i++) {
                struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                pr[2 * i].a = NODE_P(cur, a); pr[2 * i].na = a->pn; pr[2 * i].b = NODE_Q(cur, b); pr[2 * i].nb = b->qn; pr[2 * i].c = NODE_P(nxt, o);
                pr[2 * i].x = NODE_P(cur, b); pr[2 * i].nx = b->pn;                 /* P = P1 Q2 + P2 in the CRT */
                pr[2 * i + 1].a = NODE_Q(cur, a); pr[2 * i + 1].na = a->qn; pr[2 * i + 1].b = pr[2 * i].b; pr[2 * i + 1].nb = b->qn; pr[2 * i + 1].c = NODE_Q(nxt, o);
            }
            tl1 = mem_now();
            rns_mul_batch(pr, 2 * npairs);
            tl2 = mem_now();
            normed = 1;
            for (size_t i = 0; i < npairs; i++) {
                if (!pr[2 * i].ncn || !pr[2 * i + 1].ncn) { normed = 0; break; }
                nxt.nd[i].pn = pr[2 * i].ncn; nxt.nd[i].qn = pr[2 * i + 1].ncn;
            }
            free(pr);
        } else {
            tier = "mdev"; bs_st.mdev_levels++;
            bigint A1, A2, B, C1, C2; bi_init(&A1); bi_init(&A2); bi_init(&B); bi_init(&C1); bi_init(&C2);
            for (size_t i = 0; i < npairs; i++) {
                struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                /* views into the pools (no copies) */
                A1.l = NODE_P(cur, a); A1.n = a->pn; A2.l = NODE_Q(cur, a); A2.n = a->qn; B.l = NODE_Q(cur, b); B.n = b->qn;
                A1.cap = A2.cap = B.cap = 0;
                rns_mul_pair(&C1, &A1, &C2, &A2, &B);
                uint64_t *pp = NODE_P(nxt, o), *qq = NODE_Q(nxt, o);
                memcpy(pp, C1.l, C1.n * 8); if (C1.n < a->pn + b->qn + 1) memset(pp + C1.n, 0, (a->pn + b->qn + 1 - C1.n) * 8);
                pp[a->pn + b->qn] = limb_add(pp, pp, a->pn + b->qn, NODE_P(cur, b), b->pn);
                memcpy(qq, C2.l, C2.n * 8); if (C2.n < a->qn + b->qn) memset(qq + C2.n, 0, (a->qn + b->qn - C2.n) * 8);
            }
            A1.l = A2.l = B.l = 0; bi_free(&C1); bi_free(&C2);
        }
        if (normed) {                                   /* lengths came back from the device-local batch path */
        } else if (npairs >= 64) {
#pragma omp parallel
            {
                int rk, cnt = mem_region_threads(&rk), home = mem_thread_home();
                for (size_t i = (size_t)rk; i < npairs; i += (size_t)cnt) {
                    struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                    if (home >= 0 && o->r % NR != home % NR) continue;
                    o->pn = limb_norm(NODE_P(nxt, o), a->pn + b->qn + 1);
                    o->qn = limb_norm(NODE_Q(nxt, o), a->qn + b->qn);
                }
            }
        } else for (size_t i = 0; i < npairs; i++) {
            struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
            o->pn = limb_norm(NODE_P(nxt, o), a->pn + b->qn + 1);
            o->qn = limb_norm(NODE_Q(nxt, o), a->qn + b->qn);
        }
        if (odd) { struct node *a = &cur.nd[cur.n - 1], *o = &nxt.nd[npairs];
                   memcpy(NODE_P(nxt, o), NODE_P(cur, a), a->pn * 8); memcpy(NODE_Q(nxt, o), NODE_Q(cur, a), a->qn * 8); }
        double dt = mem_now() - t, tl3 = mem_now();
        if (bs_verbose && tl2) printf("bs:   layout %.3f  batch %.3f  add+norm %.3f\n", tl1 - t, tl2 - tl1, tl3 - tl2);
        if (!strcmp(tier, "school")) bs_st.t_school += dt; else if (!strcmp(tier, "batch")) bs_st.t_batch += dt; else bs_st.t_mdev += dt;
        bs_st.levels++;
        if (bs_verbose) printf("bs: level %2d %-6s %8zu pairs  max_nl %10zu  pool %6.2f GB  %.2f s  (batch %.2f: scatter %.2f ntt %.2f crt %.2f merge %.2f)\n", bs_st.levels, tier, npairs, max_nl, off * 8e-9, dt, rns_st.tb_total, rns_st.tb_scatter, rns_st.tb_ntt, rns_st.tb_crt, rns_st.tb_merge);
        memset(&rns_st, 0, sizeof rns_st);
        free(cur.nd);
        cur = nxt;
    }
    bi_reserve(P, cur.nd[0].pn); region_copy(P->l, NODE_P(cur, &cur.nd[0]), cur.nd[0].pn, cur.nd[0].r); P->n = cur.nd[0].pn;
    bi_reserve(Q, cur.nd[0].qn); region_copy(Q->l, NODE_Q(cur, &cur.nd[0]), cur.nd[0].qn, cur.nd[0].r); Q->n = cur.nd[0].qn;
    free(cur.nd);
    bs_st.t_total = mem_now() - t0;
}
void binsplit_free_pools(void)
{
    for (int w = 0; w < 2; w++) for (int r = 0; r < NR; r++) {
        if (g_pool[w][r]) { if (mem_dev_of(g_pool[w][r]) >= 0) mem_dev_free(g_pool[w][r]); else mem_hreg_free(g_pool[w][r]); }
        g_pool[w][r] = 0; g_cap[w][r] = 0;
    }
}

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

/* a level: nodes as (offset, length) pairs into a pool */
struct node { size_t po, pn, qo, qn; };
struct level { uint64_t *pool; size_t cap; struct node *nd; size_t n; };

static uint64_t *g_pool[2]; static size_t g_cap[2];
static uint64_t *pool_get(int which, size_t limbs)
{
    if (g_cap[which] < limbs) {
        if (g_pool[which]) mem_hreg_free(g_pool[which]);
        size_t cap = limbs + limbs / 8 + 4096;
        g_pool[which] = (uint64_t *)mem_hreg_alloc(cap * 8);
        g_cap[which] = cap;
        if (bs_verbose) printf("bs: level pool %d -> %.2f GB\n", which, cap * 8e-9);
    }
    if (limbs > bs_st.peak_pool_limbs) bs_st.peak_pool_limbs = limbs;
    return g_pool[which];
}

void binsplit_e(bigint *P, bigint *Q, unsigned long N)
{
    double t0 = mem_now(), t;
    memset(&bs_st, 0, sizeof bs_st);
    unsigned long S = bs_seed_terms, nspan = (N + S - 1) / S;
    /* seed spans: Q(a,b) < b^S, P < S b^S: reserve (S log2(N+1) + 64 + 64) / 64 limbs each */
    size_t per = (S * (size_t)ceil(log2((double)N + 2.0)) + 128) / (bi_decimal ? 59 : 64) + 2;   /* a decimal limb holds 59.8 bits */
    struct level cur, nxt;
    cur.n = nspan; cur.nd = (struct node *)malloc(nspan * sizeof *cur.nd);
    cur.pool = pool_get(0, 2 * per * nspan);
    t = mem_now();
#pragma omp parallel
    {
        bigint p, q; bi_init(&p); bi_init(&q);
#pragma omp for schedule(dynamic, 64)
        for (unsigned long i = 0; i < nspan; i++) {
            unsigned long a = 1 + i * S, b = a + S; if (b > N + 1) b = N + 1;
            span(&p, &q, a, b);
            struct node *nd = &cur.nd[i];
            nd->po = 2 * per * i; nd->pn = p.n; nd->qo = nd->po + per; nd->qn = q.n;
            if (p.n > per || q.n > per) { fprintf(stderr, "bs: seed span overflow\n"); abort(); }
            memcpy(cur.pool + nd->po, p.l, p.n * 8); memcpy(cur.pool + nd->qo, q.l, q.n * 8);
        }
        bi_free(&p); bi_free(&q);
    }
    bs_st.t_seed = mem_now() - t;
    if (bs_verbose) printf("bs: %lu terms, %lu spans of %lu, seeds %.2f s\n", N, nspan, S, bs_st.t_seed);

    int which = 0;
    while (cur.n > 1) {
        t = mem_now();
        size_t npairs = cur.n / 2, odd = cur.n & 1, max_nl = 0;
        for (size_t i = 0; i < cur.n; i++) { if (cur.nd[i].pn > max_nl) max_nl = cur.nd[i].pn; if (cur.nd[i].qn > max_nl) max_nl = cur.nd[i].qn; }
        /* next level layout: P slot n(P1)+n(Q2)+1, Q slot n(Q1)+n(Q2) */
        nxt.n = npairs + odd; nxt.nd = (struct node *)malloc(nxt.n * sizeof *nxt.nd);
        size_t off = 0;
        for (size_t i = 0; i < npairs; i++) {
            struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
            o->po = off; off += a->pn + b->qn + 1; o->qo = off; off += a->qn + b->qn;
            o->pn = o->qn = 0;
        }
        if (odd) { struct node *a = &cur.nd[cur.n - 1], *o = &nxt.nd[npairs]; o->po = off; off += a->pn; o->qo = off; off += a->qn; o->pn = a->pn; o->qn = a->qn; }
        which ^= 1;
        nxt.pool = pool_get(which, off);
        double tl0 = mem_now(), tl1 = 0, tl2 = 0;
        const char *tier;
        if (max_nl <= (size_t)bs_school_nl) {
            tier = "school"; bs_st.school_levels++;
#pragma omp parallel for schedule(dynamic, 16)
            for (size_t i = 0; i < npairs; i++) {
                struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                uint64_t *pp = nxt.pool + o->po, *qq = nxt.pool + o->qo;
                limb_mul_school(pp, cur.pool + a->po, a->pn, cur.pool + b->qo, b->qn);
                pp[a->pn + b->qn] = limb_add(pp, pp, a->pn + b->qn, cur.pool + b->po, b->pn);
                limb_mul_school(qq, cur.pool + a->qo, a->qn, cur.pool + b->qo, b->qn);
            }
        } else if (2 * max_nl + 1 <= ((size_t)1 << RNS_BATCH_LOGL_MAX)) {
            tier = "batch"; bs_st.batch_levels++;
            rns_prod *pr = (rns_prod *)malloc(2 * npairs * sizeof *pr);
            for (size_t i = 0; i < npairs; i++) {
                struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                pr[2 * i].a = cur.pool + a->po; pr[2 * i].na = a->pn; pr[2 * i].b = cur.pool + b->qo; pr[2 * i].nb = b->qn; pr[2 * i].c = nxt.pool + o->po;
                pr[2 * i + 1].a = cur.pool + a->qo; pr[2 * i + 1].na = a->qn; pr[2 * i + 1].b = pr[2 * i].b; pr[2 * i + 1].nb = b->qn; pr[2 * i + 1].c = nxt.pool + o->qo;
            }
            tl1 = mem_now();
            rns_mul_batch(pr, 2 * npairs);
            tl2 = mem_now();
            free(pr);
            if (npairs >= 64) {
#pragma omp parallel for schedule(dynamic, 64)
                for (size_t i = 0; i < npairs; i++) {
                    struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                    uint64_t *pp = nxt.pool + o->po;
                    pp[a->pn + b->qn] = limb_add(pp, pp, a->pn + b->qn, cur.pool + b->po, b->pn);
                }
            } else for (size_t i = 0; i < npairs; i++) {           /* few big pairs: limb_add is parallel inside */
                struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                uint64_t *pp = nxt.pool + o->po;
                pp[a->pn + b->qn] = limb_add(pp, pp, a->pn + b->qn, cur.pool + b->po, b->pn);
            }
        } else {
            tier = "mdev"; bs_st.mdev_levels++;
            bigint A1, A2, B, C1, C2; bi_init(&A1); bi_init(&A2); bi_init(&B); bi_init(&C1); bi_init(&C2);
            for (size_t i = 0; i < npairs; i++) {
                struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                /* views into the pools (no copies) */
                A1.l = cur.pool + a->po; A1.n = a->pn; A2.l = cur.pool + a->qo; A2.n = a->qn; B.l = cur.pool + b->qo; B.n = b->qn;
                A1.cap = A2.cap = B.cap = 0;
                rns_mul_pair(&C1, &A1, &C2, &A2, &B);
                uint64_t *pp = nxt.pool + o->po, *qq = nxt.pool + o->qo;
                memcpy(pp, C1.l, C1.n * 8); if (C1.n < a->pn + b->qn + 1) memset(pp + C1.n, 0, (a->pn + b->qn + 1 - C1.n) * 8);
                pp[a->pn + b->qn] = limb_add(pp, pp, a->pn + b->qn, cur.pool + b->po, b->pn);
                memcpy(qq, C2.l, C2.n * 8); if (C2.n < a->qn + b->qn) memset(qq + C2.n, 0, (a->qn + b->qn - C2.n) * 8);
            }
            A1.l = A2.l = B.l = 0; bi_free(&C1); bi_free(&C2);
        }
#pragma omp parallel for schedule(dynamic, 256)
        for (size_t i = 0; i < npairs; i++) {
            struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
            o->pn = limb_norm(nxt.pool + o->po, a->pn + b->qn + 1);
            o->qn = limb_norm(nxt.pool + o->qo, a->qn + b->qn);
        }
        if (odd) { struct node *a = &cur.nd[cur.n - 1], *o = &nxt.nd[npairs];
                   memcpy(nxt.pool + o->po, cur.pool + a->po, a->pn * 8); memcpy(nxt.pool + o->qo, cur.pool + a->qo, a->qn * 8); }
        double dt = mem_now() - t, tl3 = mem_now();
        if (bs_verbose && tl2) printf("bs:   layout %.3f  batch %.3f  add+norm %.3f\n", tl1 - t, tl2 - tl1, tl3 - tl2);
        if (!strcmp(tier, "school")) bs_st.t_school += dt; else if (!strcmp(tier, "batch")) bs_st.t_batch += dt; else bs_st.t_mdev += dt;
        bs_st.levels++;
        if (bs_verbose) printf("bs: level %2d %-6s %8zu pairs  max_nl %10zu  pool %6.2f GB  %.2f s  (batch %.2f: scatter %.2f ntt %.2f crt %.2f merge %.2f)\n", bs_st.levels, tier, npairs, max_nl, off * 8e-9, dt, rns_st.tb_total, rns_st.tb_scatter, rns_st.tb_ntt, rns_st.tb_crt, rns_st.tb_merge);
        memset(&rns_st, 0, sizeof rns_st);
        free(cur.nd);
        cur = nxt;
    }
    bi_reserve(P, cur.nd[0].pn); memcpy(P->l, cur.pool + cur.nd[0].po, cur.nd[0].pn * 8); P->n = cur.nd[0].pn;
    bi_reserve(Q, cur.nd[0].qn); memcpy(Q->l, cur.pool + cur.nd[0].qo, cur.nd[0].qn * 8); Q->n = cur.nd[0].qn;
    free(cur.nd);
    bs_st.t_total = mem_now() - t0;
}
void binsplit_free_pools(void)
{
    for (int w = 0; w < 2; w++) { if (g_pool[w]) mem_hreg_free(g_pool[w]); g_pool[w] = 0; g_cap[w] = 0; }
}

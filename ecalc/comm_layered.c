/* comm_layered.c - the layered communicator (Phase 8 M3, PLAN.md 17): the rank space of a whole-machine
 * transform is (4 APUs) x (g nodes).  APU d of node r is global rank rho = g d + r (APU-major), so the g
 * slabs bound for the APUs d' of every node are contiguous in the sender's slab buffer.  An all-to-all is
 * two exchanges with a block transpose between them:
 *   1. intra-node (xGMI push, the four APU threads): APU d sends block d' = slabs {g d' + r'} to APU d';
 *      the receive holds [d source][r' dest] blocks
 *   2. transpose to [r'][d]; inter-node over mesh d' (TCP here, RDMA on the target; rank = node): node r
 *      sends block r' = the four slabs (r, 0..3) -> (r', d') to node r'; the receive holds [r][d]
 *   3. transpose to [d][r] = global source rank order.
 * The transposes are 4 g device copies of one slab each; the scratch (one slab buffer) is the caller's
 * (comm_layered_scratch) or hipMalloc'd on first use.
 * M7: two exchanges may be in flight (inflight 2): the second's intra-node stage runs while the first's
 * inter-node stage is on the wire (the inter transport takes one at a time, so posting the second completes
 * the first before its own inter stage starts); each pending exchange has its own slot of the scratch (two
 * slots of 4 g x bytes -- when only one fits, the exchanges serialise).  wait() completes the oldest exchange
 * not yet waited for (one wait per post: the k-th wait guarantees the k-th exchange, whether the transport
 * finished it early or not).
 * allgather: inter first (mesh d gathers my block over the nodes into my slot [d][0..g) of the result --
 * every mesh carries one block per node), then the xGMI all-gather of the g-block slots; no transpose, and the
 * fabric moves 4 x less than intra-first would.
 * B7 alltoallv: the same three stages with per-peer counts.  The node's send counts are all-gathered over the four
 * APU threads (4 x 4 g size_t) so that every stage's receive sizes are known: intra -- APU d sends APU d' the g
 * slabs bound for ranks (d', 0..g-1) as one block (the caller's slabs are used in place when they lie back to back in
 * rank order, else copied into that order); transpose to [r'][d]; inter over mesh d with per-node counts; wait --
 * the [r][d] slabs into the receive buffer at the caller's offsets.  Its scratch (2 x the intra total + the receive
 * total) is the communicator's own; a v-exchange never overlaps an equal-slab one (either kind completes the other). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comm.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#define NA 4
/* ---- Phase 13a X (PLAN.md 29 E9 part 1): the stage timeline of every exchange, COMM_LAYER_STATS=1|2 (default 0: off,
 * nothing recorded, no branch taken beyond the flag test).  Per exchange: a0 entry, [x0, x1] the xGMI stage (the intra
 * post + wait), r1 ready for the fabric (the transpose done), [f0, f1] the fabric stage (the inter post .. its completion),
 * c1 the exchange complete (the transpose back).  f1: mode 1 (passive) = when the inter wait returned in the normal
 * course -- an upper bound on the fabric's completion (the wait is called lazily); mode 2 (exact) = a watcher thread
 * calls the inter wait right after the post and stamps its return (the TCP upload / SHMEM staging then runs at once
 * instead of at the lazy wait: a slightly earlier completion, the values identical).  Folded at destroy / report into
 * per-size buckets; printed at exit (and by comm_layered_stats_report). */
struct lst_rec { double a0, x0, x1, r1, f0, f1, c1, l0, l1; size_t xb, fb; int v; };   /* [l0, l1]: the push kernel's link-active interval (COMM_XGMI_STATS=1) */
extern "C" int comm_xgmi_last_push(int rank, double *t0, double *t1);   /* comm_xgmi.c */
struct lst { struct lst_rec *r; int n, cap; };
static int lst_mode;                                     /* COMM_LAYER_STATS: read at the first create */
static inline double lst_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
struct lay_ex { void *rb; size_t bytes; hipStream_t s; char *tmp; int inter_posted; int rec; pthread_t w; int w_on; double wf1; };   /* one pending exchange (rec.. wf1: the stats record, the watcher, its stamp) */
struct lay_v { void *rb; const size_t *rcnt, *rdsp; hipStream_t s; char *x3; size_t *cnt2; int pend; int rec; pthread_t w; int w_on; double wf1; };   /* the pending v-exchange */
typedef struct { comm *intra, *inter; int d, g, na, dev, minor; char *tmp; size_t tmp_cap; int own_tmp, tmp_sym;   /* tmp_sym: the scratch is the inter transport's symmetric memory (S12) */   /* d: my intra rank; dev: my device; na: intra size; minor: rho = na r + d (else g d + r) */
                 struct lay_ex ex[2]; int head, npend, nlog;                /* npend: physically pending; nlog: posted minus waited */
                 char *vtmp; size_t vcap; struct lay_v v; struct lst st; } lay_priv;
#define PRIV(c) ((lay_priv *)(c)->priv)
static inline int rho_of(const lay_priv *p, int d, int r) { return p->minor ? p->na * r + d : p->g * d + r; }   /* the global rank of (intra d, inter r) */
/* ---- the stats (E9 part 1) ---- */
static int lst_new(lay_priv *p, double a0, int v)
{
    struct lst *L = &p->st;
    if (L->n == L->cap) { L->cap = L->cap ? 2 * L->cap : 1024; L->r = (struct lst_rec *)realloc(L->r, L->cap * sizeof *L->r); }
    struct lst_rec *r = &L->r[L->n]; memset(r, 0, sizeof *r); r->a0 = a0; r->v = v;
    return L->n++;
}
#define LREC(p, i) (&(p)->st.r[(i)])
static void lst_link(lay_priv *p, struct lst_rec *r) { if (!p->minor) comm_xgmi_last_push(p->d, &r->l0, &r->l1); }
/* mode 2: the watcher completes the inter exchange as soon as it lands and stamps the time */
struct lst_w { comm *inter; int dev; double *f1; };
static void *lst_watch(void *a)
{
    struct lst_w *w = (struct lst_w *)a;
    HIP_CHECK(hipSetDevice(w->dev));
    comm_wait(w->inter); *w->f1 = lst_now();
    free(w); return NULL;
}
static void lst_watch_start(lay_priv *p, pthread_t *th, int *on, double *f1)
{
    struct lst_w *w = (struct lst_w *)malloc(sizeof *w); w->inter = p->inter; w->dev = p->dev; w->f1 = f1;
    if (pthread_create(th, NULL, lst_watch, w)) { fprintf(stderr, "comm_layered: pthread_create\n"); exit(1); }
    *on = 1;
}
/* the inter wait of the oldest exchange: the watcher's join (mode 2) or the wait itself; returns the fabric's end stamp */
static double lst_inter_wait(lay_priv *p, pthread_t th, int *on, const double *wf1)
{
    if (*on) { pthread_join(th, NULL); *on = 0; return *wf1; }
    comm_wait(p->inter);
    return lst_mode ? lst_now() : 0;
}
/* the folded totals, per bucket of the fabric bytes one APU sends per exchange (log2) */
#define LB 48
struct lst_tot { double n, nv, span, x, f, both, held, idle, xb, fb, wall0, wall1, xl, bothl, tr; };   /* tr: the block transposes (r1 - x1, c1 - f1) */
static struct lst_tot g_tot[LB], g_all;
static double g_dfb[NA], g_df[NA];                       /* per APU thread (device): fabric bytes and fabric time -- the NIC balance */
static double *g_pool; static char *g_pk; static int g_np, g_npc;   /* the node view's intervals (kind 0 xGMI, 1 fabric, 2 span) */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static lay_priv *g_reg[1024]; static int g_nreg; static int g_node = -1, g_gsz = 0;
static int lst_bucket(size_t b) { int k = 0; while (k < LB - 1 && ((size_t)2 << k) <= b) k++; return k; }
/* the measure of the intersection of two ordered lists of disjoint intervals */
static double lst_isect(const double *a, int na, const double *b, int nb)
{
    double s = 0; int i = 0, j = 0;
    while (i < na && j < nb) {
        double lo = a[2 * i] > b[2 * j] ? a[2 * i] : b[2 * j], hi = a[2 * i + 1] < b[2 * j + 1] ? a[2 * i + 1] : b[2 * j + 1];
        if (hi > lo) s += hi - lo;
        if (a[2 * i + 1] < b[2 * j + 1]) i++; else j++;
    }
    return s;
}
/* fold one communicator's records (caller holds g_mx; the communicator is quiescent).  Per exchange: its own xGMI stage
 * against every fabric stage of the communicator (another exchange's -- the overlap the pipeline buys); the span is the
 * union of the exchanges' lifetimes [x0, c1]; held = post - ready (the one-at-a-time wait); idle = the part of it in
 * which the fabric had finished the previous exchange (what a deeper inter stage could not buy, the lazy completion) */
static void lst_fold(lay_priv *p)
{
    struct lst *L = &p->st; int n = L->n;
    if (!n) return;
    double *X = (double *)malloc(4 * (size_t)n * sizeof(double)), *F = X + 2 * n; int nf = 0;
    for (int k = 0; k < n; k++) { X[2 * k] = L->r[k].x0; X[2 * k + 1] = L->r[k].x1; }
    for (int k = 0; k < n; k++) if (L->r[k].f1 > 0) { F[2 * nf] = L->r[k].f0; F[2 * nf + 1] = L->r[k].f1; nf++; }
    double span_hi = 0, prev_f1 = 0;
    for (int k = 0; k < n; k++) {
        struct lst_rec *r = &L->r[k]; struct lst_tot *t = &g_tot[lst_bucket(r->fb)];
        double both = lst_isect(X + 2 * k, 1, F, nf), lk[2] = { r->l0, r->l1 }, bl = r->l1 > 0 ? lst_isect(lk, 1, F, nf) : 0;
        double s0 = r->x0 > span_hi ? r->x0 : span_hi, sp = r->c1 > s0 ? r->c1 - s0 : 0;
        if (r->c1 > span_hi) span_hi = r->c1;
        double held = r->f0 > r->r1 ? r->f0 - r->r1 : 0, rdy = r->r1 > prev_f1 ? r->r1 : prev_f1, idle = r->f0 > rdy ? r->f0 - rdy : 0;
        if (r->f1 > 0) prev_f1 = r->f1;
        if (p->dev >= 0 && p->dev < NA) { g_dfb[p->dev] += r->fb; g_df[p->dev] += r->f1 > 0 ? r->f1 - r->f0 : 0; }
        for (int a = 0; a < 2; a++) {
            struct lst_tot *u = a ? &g_all : t;
            u->n += 1; u->nv += r->v; u->span += sp; u->x += r->x1 - r->x0; u->f += r->f1 > 0 ? r->f1 - r->f0 : 0; u->both += both;
            u->held += held; u->idle += idle; u->xb += r->xb; u->fb += r->fb; u->xl += r->l1 > 0 ? r->l1 - r->l0 : 0; u->bothl += bl;
            u->tr += (r->r1 > r->x1 ? r->r1 - r->x1 : 0) + (r->c1 > r->f1 && r->f1 > 0 ? r->c1 - r->f1 : 0);
            if (!u->wall0 || r->a0 < u->wall0) u->wall0 = r->a0;
            if (r->c1 > u->wall1) u->wall1 = r->c1;
        }
    }
    /* the node view: every interval into the pool (the four APU threads' fabric stages share the node's NICs) */
    for (int k = 0; k < n; k++) {
        struct lst_rec *r = &L->r[k];
        if (g_np + 4 > g_npc) { g_npc = g_npc ? 2 * g_npc : 4096; g_pool = (double *)realloc(g_pool, 2 * (size_t)g_npc * sizeof(double)); g_pk = (char *)realloc(g_pk, g_npc); }
        g_pool[2 * g_np] = r->x0; g_pool[2 * g_np + 1] = r->x1; g_pk[g_np++] = 0;
        if (r->f1 > 0) { g_pool[2 * g_np] = r->f0; g_pool[2 * g_np + 1] = r->f1; g_pk[g_np++] = 1; }
        g_pool[2 * g_np] = r->x0; g_pool[2 * g_np + 1] = r->c1; g_pk[g_np++] = 2;
        if (r->l1 > 0) { g_pool[2 * g_np] = r->l0; g_pool[2 * g_np + 1] = r->l1; g_pk[g_np++] = 3; }
    }
    free(X); L->n = 0;
}
/* the node view of the pool: the union of each kind (xGMI, fabric, span) over the APU threads, and xGMI n fabric */
static int lst_cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static int lst_union(int kind, double *out)             /* merged, ordered intervals of one kind; returns the count */
{
    int m = 0;
    for (int i = 0; i < g_np; i++) if (g_pk[i] == kind) { out[2 * m] = g_pool[2 * i]; out[2 * m + 1] = g_pool[2 * i + 1]; m++; }
    qsort(out, m, 2 * sizeof(double), lst_cmp);
    int w = 0;
    for (int i = 0; i < m; i++) {
        if (w && out[2 * i] <= out[2 * w - 1]) { if (out[2 * i + 1] > out[2 * w - 1]) out[2 * w - 1] = out[2 * i + 1]; }
        else { out[2 * w] = out[2 * i]; out[2 * w + 1] = out[2 * i + 1]; w++; }
    }
    return w;
}
static double lst_len(const double *a, int n) { double s = 0; for (int i = 0; i < n; i++) s += a[2 * i + 1] - a[2 * i]; return s; }
static void lst_node_print(const char *tag)
{
    if (!g_np) return;
    double *X = (double *)malloc(8 * (size_t)g_np * sizeof(double)), *F = X + 2 * g_np, *S = F + 2 * g_np, *K = S + 2 * g_np;
    int nx = lst_union(0, X), nf = lst_union(1, F), ns = lst_union(2, S), nk = lst_union(3, K);
    double x = lst_len(X, nx), f = lst_len(F, nf), s = lst_len(S, ns), b = lst_isect(X, nx, F, nf), xk = lst_len(K, nk), bk = lst_isect(K, nk, F, nf);
    printf("layer-stats %s node %d NODE VIEW (union over the APU threads): span %.4f s | xGMI %.4f fabric %.4f BOTH %.4f | xGMI-only %.4f fabric-only %.4f neither %.4f | both/xGMI %.1f %%, both/span %.1f %%, fabric busy/span %.1f %%\n",
           tag, g_node, s, x, f, b, x - b, f - b, s - x - f + b, x > 0 ? 100 * b / x : 0, s > 0 ? 100 * b / s : 0, s > 0 ? 100 * f / s : 0);
    if (nk) printf("layer-stats %s node %d NODE VIEW, the xGMI links (the push kernels, stamped): busy %.4f s, BOTH with the fabric %.4f s = %.1f %% of the link time\n", tag, g_node, xk, bk, xk > 0 ? 100 * bk / xk : 0);
    free(X); g_np = 0;
}
static void lst_print(const char *tag)
{
    char host[64] = "?"; gethostname(host, sizeof host); host[sizeof host - 1] = 0;
    struct lst_tot *a = &g_all; double q = NA;              /* per APU thread = the sum over the node's four / 4 */
    if (!a->n) return;
    double mn = a->x < a->f ? a->x : a->f;
    printf("layer-stats %s node %d/%d (%s, %s): %.0f exchanges (%.0f v), per APU thread: span %.4f s | xGMI %.4f fabric %.4f BOTH %.4f | xGMI-only %.4f fabric-only %.4f neither %.4f s\n",
           tag, g_node, g_gsz, host, lst_mode == 2 ? "exact" : "passive", a->n, a->nv, a->span / q, a->x / q, a->f / q, a->both / q,
           (a->x - a->both) / q, (a->f - a->both) / q, (a->span - a->x - a->f + a->both) / q);
    printf("layer-stats %s node %d: both/span %.1f %%, both/xGMI %.1f %%, both/min(xGMI,fabric) %.1f %% | saving vs sequential both/(x+f) %.1f %%, ceiling min/(x+f) %.1f %% | one-at-a-time: held %.4f s, of it fabric idle %.4f s | xGMI %.3f GB at %.1f GB/s, fabric %.3f GB at %.3f GB/s per APU\n",
           tag, g_node, a->span > 0 ? 100 * a->both / a->span : 0, a->x > 0 ? 100 * a->both / a->x : 0, mn > 0 ? 100 * a->both / mn : 0,
           a->x + a->f > 0 ? 100 * a->both / (a->x + a->f) : 0, a->x + a->f > 0 ? 100 * mn / (a->x + a->f) : 0, a->held / q, a->idle / q,
           a->xb / q * 1e-9, a->x > 0 ? a->xb / a->x * 1e-9 : 0, a->fb / q * 1e-9, a->f > 0 ? a->fb / a->f * 1e-9 : 0);
    {   double bmin = 1e300, bmax = 0, tmin = 1e300, tmax = 0;
        for (int d = 0; d < NA; d++) { if (g_dfb[d] < bmin) bmin = g_dfb[d]; if (g_dfb[d] > bmax) bmax = g_dfb[d]; if (g_df[d] < tmin) tmin = g_df[d]; if (g_df[d] > tmax) tmax = g_df[d]; }
        printf("layer-stats %s node %d: block transposes %.4f s per APU (%.3f ms per exchange); the NIC balance over the APU threads: fabric bytes %.4f .. %.4f GB (max/min %.4f), fabric time %.4f .. %.4f s (max/min %.3f)\n",
               tag, g_node, a->tr / q, a->n > 0 ? 1e3 * a->tr / a->n : 0, bmin * 1e-9, bmax * 1e-9, bmin > 0 ? bmax / bmin : 0, tmin, tmax, tmin > 0 ? tmax / tmin : 0); }
    if (a->xl > 0) printf("layer-stats %s node %d: the xGMI stage vs its link time (the push kernels, COMM_XGMI_STATS): stage %.4f s, link-active %.4f s (%.1f %%, %.1f GB/s per APU), BOTH link+fabric %.4f s = %.1f %% of the link time\n",
                          tag, g_node, a->x / q, a->xl / q, a->x > 0 ? 100 * a->xl / a->x : 0, a->xb / a->xl * 1e-9, a->bothl / q, 100 * a->bothl / a->xl);
    for (int k = 0; k < LB; k++) {
        struct lst_tot *t = &g_tot[k]; if (!t->n) continue;
        double m2 = t->x < t->f ? t->x : t->f;
        printf("layer-stats %s node %d:   fabric >= %-8.4g MB/APU/exch: %6.0f exch (%.0f v)  span %.4f xGMI %.4f fabric %.4f both %.4f (both/xGMI %.0f %%, both/min %.0f %%) held %.4f idle %.4f  xGMI %.1f fabric %.3f GB/s/APU\n",
               tag, g_node, k ? (double)((size_t)1 << k) * 1e-6 : 0.0, t->n, t->nv, t->span / q, t->x / q, t->f / q, t->both / q,
               t->x > 0 ? 100 * t->both / t->x : 0, m2 > 0 ? 100 * t->both / m2 : 0, t->held / q, t->idle / q, t->x > 0 ? t->xb / t->x * 1e-9 : 0, t->f > 0 ? t->fb / t->f * 1e-9 : 0);
    }
    fflush(stdout);
}
static void lst_atexit(void) { pthread_mutex_lock(&g_mx); for (int i = 0; i < g_nreg; i++) lst_fold(g_reg[i]); lst_print("(at exit)"); lst_node_print("(at exit)"); pthread_mutex_unlock(&g_mx); }
/* print the totals so far (every layered communicator quiescent) and reset them (tag 0: reset only); a no-op without
 * COMM_LAYER_STATS */
extern "C" void comm_layered_stats_report(const char *tag)
{
    if (!lst_mode) return;
    pthread_mutex_lock(&g_mx);
    for (int i = 0; i < g_nreg; i++) lst_fold(g_reg[i]);
    if (tag) { lst_print(tag); lst_node_print(tag); }    /* tag 0: reset only */
    g_np = 0;
    memset(g_tot, 0, sizeof g_tot); memset(&g_all, 0, sizeof g_all); memset(g_dfb, 0, sizeof g_dfb); memset(g_df, 0, sizeof g_df);
    pthread_mutex_unlock(&g_mx);
}
static void lst_register(lay_priv *p, int node, int g)
{
    static int once;
    pthread_mutex_lock(&g_mx);
    if (!once) { const char *e = getenv("COMM_LAYER_STATS"); lst_mode = e ? atoi(e) : 0; if (lst_mode) atexit(lst_atexit); once = 1; }
    if (lst_mode && g_nreg < 1024) { g_reg[g_nreg++] = p; if (g > g_gsz) { g_gsz = g; g_node = node; } }
    pthread_mutex_unlock(&g_mx);
}
static void lst_unregister(lay_priv *p)
{
    if (!lst_mode) return;
    pthread_mutex_lock(&g_mx);
    lst_fold(p); free(p->st.r); p->st.r = 0;
    for (int i = 0; i < g_nreg; i++) if (g_reg[i] == p) { g_reg[i] = g_reg[--g_nreg]; break; }
    pthread_mutex_unlock(&g_mx);
}
static int y_rank(comm *c) { return c->rank; }
static int y_size(comm *c) { return c->size; }
static void need_tmp(comm *c, size_t bytes)
{
    lay_priv *p = PRIV(c);
    if (p->tmp_cap >= bytes) return;
    if (!p->own_tmp && p->tmp) { fprintf(stderr, "comm_layered: the caller's scratch (%zu B) is smaller than the exchange (%zu B)\n", p->tmp_cap, bytes); exit(1); }
    if (p->tmp) { if (p->tmp_sym) comm_sym_free(p->inter, p->tmp); else HIP_CHECK(hipFree(p->tmp)); }
    HIP_CHECK(hipSetDevice(p->dev));
    /* Phase 12 S (a minimal change): the scratch from the inter transport's symmetric pool where it has one (the SHMEM
     * transport then receives the inter stage straight into it, no staging), else hipMalloc'd as before */
    p->tmp = (char *)comm_sym_alloc(p->inter, bytes); p->tmp_sym = p->tmp != 0;
    if (!p->tmp) HIP_CHECK(hipMalloc((void **)&p->tmp, bytes));
    p->tmp_cap = bytes; p->own_tmp = 1;
}
/* [a][b] blocks of `bytes` -> [b][a]: na x nb blocks */
static void block_transpose(void *dst, const void *src, size_t bytes, int na, int nb, hipStream_t s)
{
    for (int a = 0; a < na; a++) for (int b = 0; b < nb; b++)
        HIP_CHECK(hipMemcpyAsync((char *)dst + ((size_t)b * na + a) * bytes, (const char *)src + ((size_t)a * nb + b) * bytes, bytes, hipMemcpyDefault, s));   /* (S12: Default -- the scratch may be registered host memory) */
}
/* the inter-node stage of the oldest pending exchange (its intra stage and transpose are done) */
static void inter_post(comm *c, struct lay_ex *e)
{
    lay_priv *p = PRIV(c);
    if (lst_mode) LREC(p, e->rec)->f0 = lst_now();
    if (p->minor) comm_alltoall(p->inter, e->tmp, e->rb, e->bytes * p->na, e->s);   /* minor: the transposed scratch out, rb receives [r][d] = rank order */
    else comm_alltoall(p->inter, e->rb, e->tmp, e->bytes * p->na, e->s);
    e->inter_posted = 1;
    if (lst_mode == 2) lst_watch_start(p, &e->w, &e->w_on, &e->wf1);
}
/* complete the oldest pending exchange: tmp slot holds [r][d] after the inter wait; -> [d][r] into rb (major); minor: rb is final */
static void complete_oldest(comm *c)
{
    lay_priv *p = PRIV(c);
    if (!p->npend) return;
    struct lay_ex *e = &p->ex[p->head];
    if (!e->inter_posted) inter_post(c, e);
    double f1 = lst_inter_wait(p, e->w, &e->w_on, &e->wf1);
    HIP_CHECK(hipSetDevice(p->dev));
    if (!p->minor) { block_transpose(e->rb, e->tmp, e->bytes, p->g, p->na, e->s); HIP_CHECK(hipStreamSynchronize(e->s)); }   /* -> [d][r] = source rank order */
    if (lst_mode) { struct lst_rec *r = LREC(p, e->rec); r->f1 = f1; r->c1 = lst_now(); }
    p->head ^= 1; p->npend--;
}
static void complete_v(comm *c);
static void y_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    lay_priv *p = PRIV(c); int g = p->g, na = p->na;
    size_t slot = bytes * (size_t)na * g;
    double a0 = lst_mode ? lst_now() : 0;
    complete_v(c);                                       /* one kind at a time */
    need_tmp(c, (!p->tmp || p->own_tmp) ? 2 * slot : slot);   /* our own scratch holds two slots; the caller's what it is */
    if (p->npend == 2) complete_oldest(c);
    int two = p->tmp_cap >= 2 * slot;                    /* two slots fit: the previous exchange may stay on the wire */
    if (p->npend && !two) complete_oldest(c);
    struct lay_ex *e = &p->ex[(p->head + p->npend) & 1];
    e->rb = rb; e->bytes = bytes; e->s = s; e->tmp = p->tmp + (two ? ((p->head + p->npend) & 1) * slot : 0); e->inter_posted = 0;
    p->npend++; p->nlog++;
    HIP_CHECK(hipSetDevice(p->dev));
    if (lst_mode) {                                      /* this APU's bytes: over xGMI to the other (na - 1) intra peers, over the fabric to the (g - 1) nodes */
        e->rec = lst_new(p, a0, 0); struct lst_rec *r = LREC(p, e->rec);
        r->xb = bytes * (size_t)g * (na - 1); r->fb = bytes * (size_t)na * (g - 1); r->x0 = lst_now();
    }
    if (p->minor) {
        /* minor: the slabs for intra peer d' are {na r' + d'}: gathered into the scratch as [d'][r'] blocks of g slabs; the intra
         * exchange into rb ([d''][r']); the transpose into the scratch ([r'][d'']); the inter exchange sends its blocks of na */
        block_transpose(e->tmp, sb, bytes, g, na, s); HIP_CHECK(hipStreamSynchronize(s));
        comm_alltoall(p->intra, e->tmp, rb, bytes * g, s); comm_wait(p->intra);
        if (lst_mode) LREC(p, e->rec)->x1 = lst_now();
        block_transpose(e->tmp, rb, bytes, na, g, s); HIP_CHECK(hipStreamSynchronize(s));
    } else {
    /* 1: intra-node, blocks of g slabs; the xGMI wait synchronises the stream and the four threads */
    comm_alltoall(p->intra, sb, e->tmp, bytes * g, s);
    comm_wait(p->intra);
    if (lst_mode) { LREC(p, e->rec)->x1 = lst_now(); lst_link(p, LREC(p, e->rec)); }
    /* 2: [d][r'] -> [r'][d] into rb, then the inter-node exchange of blocks of na slabs, received into the slot */
    block_transpose(rb, e->tmp, bytes, na, g, s);
    HIP_CHECK(hipStreamSynchronize(s));
    }
    if (lst_mode) LREC(p, e->rec)->r1 = lst_now();
    if (p->npend == 2) complete_oldest(c);               /* the inter transport takes one exchange at a time */
    inter_post(c, e);
}
/* ---- B7: the unequal exchange ---- */
static void need_vtmp(comm *c, size_t bytes)
{
    lay_priv *p = PRIV(c);
    if (p->vcap >= bytes) return;
    HIP_CHECK(hipSetDevice(p->dev));
    if (p->vtmp) HIP_CHECK(hipFree(p->vtmp));
    HIP_CHECK(hipMalloc((void **)&p->vtmp, bytes)); p->vcap = bytes;
}
/* the count tables of a v-exchange on this APU thread (rank g d + node, size 4 g): T[d'][rho] = APU d' of my node
 * sends rho (all-gathered over the intra communicator); the intra stage's counts sI/rI (per APU), the inter stage's
 * cnt2 (per node; send [0, g), recv [g, 2 g)).  All in bytes. */
struct vtab { size_t *T, *sI, *rI, *sIoff, *rIoff, *cnt2, *dsp2, A, B, S; int contig; };   /* sI.. rIoff: na each (after T) */
static void vtab_build(comm *c, const size_t *scnt, const size_t *sdsp, const size_t *rcnt, struct vtab *t, int host)
{
    lay_priv *p = PRIV(c); int g = p->g, na = p->na, n = na * g, d = p->d;
    t->T = (size_t *)malloc(((size_t)na * n + 4 * (size_t)na) * sizeof(size_t)); t->sI = t->T + (size_t)na * n; t->rI = t->sI + na; t->sIoff = t->rI + na; t->rIoff = t->sIoff + na;
    t->cnt2 = (size_t *)malloc(4 * (size_t)g * sizeof(size_t)); t->dsp2 = t->cnt2 + 2 * g;
    (void)host; comm_allgather_host(p->intra, scnt, t->T, (size_t)n * sizeof(size_t));
    t->A = t->B = t->S = 0; t->contig = !p->minor;      /* minor: the slabs for one intra peer are strided in rank order: always packed */
    for (int dd = 0; dd < na; dd++) {
        size_t si = 0, ri = 0;
        for (int r = 0; r < g; r++) { si += scnt[rho_of(p, dd, r)]; ri += t->T[(size_t)dd * n + rho_of(p, d, r)]; }
        t->sI[dd] = si; t->rI[dd] = ri; t->sIoff[dd] = t->S; t->rIoff[dd] = t->A; t->S += si; t->A += ri;
    }
    for (int rho = 1; rho < n; rho++) if (sdsp[rho] != sdsp[rho - 1] + scnt[rho - 1]) t->contig = 0;
    for (int r = 0; r < g; r++) { size_t sn = 0, rn = 0; for (int dd = 0; dd < na; dd++) { sn += t->T[(size_t)dd * n + rho_of(p, d, r)]; rn += rcnt[rho_of(p, dd, r)]; } t->cnt2[r] = sn; t->cnt2[g + r] = rn; }
    comm_prefix(t->cnt2, t->dsp2, g); t->B = comm_prefix(t->cnt2 + g, t->dsp2 + g, g);
}
static void vtab_free(struct vtab *t) { free(t->T); free(t->cnt2); }
/* a copy on the device stream or the host */
static void vcopy(void *dst, const void *src, size_t n, hipStream_t s, int host)
{
    if (!n) return;
    if (host) memmove(dst, src, n); else HIP_CHECK(hipMemcpyAsync(dst, src, n, hipMemcpyDefault, s));
}
/* the intra stage and the transpose: my slabs -> x1 [d][r'] blocks (the intra exchange) -> x2 [r'][d]; then the inter
 * stage's post.  x1/x2/x3 are the caller's areas (device or host); the intra and inter ops are the transport's of the kind. */
static void v_stages(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, struct vtab *t, char *x0, char *x1, char *x2, char *x3, hipStream_t s, int host, struct lst_rec *st)
{
    lay_priv *p = PRIV(c); int g = p->g, na = p->na, n = na * g, d = p->d;
    const char *src = (const char *)sb; size_t soff0 = sdsp[0];
    if (!t->contig) {                                    /* the slabs into [d'][r'] order (= rank order in the major form), back to back, in x0 */
        size_t o = 0; for (int dd = 0; dd < na; dd++) for (int r = 0; r < g; r++) { int rho = rho_of(p, dd, r); vcopy(x0 + o, (const char *)sb + sdsp[rho], scnt[rho], s, host); o += scnt[rho]; }
        src = x0; soff0 = 0;
    }
    size_t *sdI = (size_t *)malloc(2 * (size_t)na * sizeof(size_t)), *rdI = sdI + na; for (int dd = 0; dd < na; dd++) { sdI[dd] = soff0 + t->sIoff[dd]; rdI[dd] = t->rIoff[dd]; }
    if (st) { st->x0 = lst_now(); for (int dd = 0; dd < na; dd++) if (dd != d) st->xb += t->sI[dd]; }   /* (the stats: the xGMI stage) */
    if (host) comm_alltoallv_host(p->intra, src, t->sI, sdI, x1, t->rI, rdI);
    else { HIP_CHECK(hipStreamSynchronize(s)); comm_alltoallv(p->intra, src, t->sI, sdI, x1, t->rI, rdI, s); comm_wait(p->intra); }
    if (st) { st->x1 = lst_now(); if (!host) lst_link(p, st); }
    free(sdI);
    /* x1: block dd = slabs (dd -> (d, r')) for r' = 0..g-1 back to back; x2: node r' = its na slabs dd = 0..na-1 */
    size_t o2 = 0;
    for (int r = 0; r < g; r++) for (int dd = 0; dd < na; dd++) {
        size_t off = t->rIoff[dd]; for (int rr = 0; rr < r; rr++) off += t->T[(size_t)dd * n + rho_of(p, d, rr)];
        size_t len = t->T[(size_t)dd * n + rho_of(p, d, r)];
        vcopy(x2 + o2, x1 + off, len, s, host); o2 += len;
    }
    if (!host) HIP_CHECK(hipStreamSynchronize(s));
    if (st) { st->r1 = st->f0 = lst_now(); int me = comm_rank(p->inter); for (int r = 0; r < g; r++) if (r != me) st->fb += t->cnt2[r]; }   /* (the fabric stage) */
    if (host) comm_alltoallv_host(p->inter, x2, t->cnt2, t->dsp2, x3, t->cnt2 + g, t->dsp2 + g);
    else comm_alltoallv(p->inter, x2, t->cnt2, t->dsp2, x3, t->cnt2 + g, t->dsp2 + g, s);
}
/* x3 [r][d] -> the receive buffer at the caller's offsets */
static void v_scatter(comm *c, const struct vtab *t, const char *x3, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s, int host)
{
    lay_priv *p = PRIV(c); int g = p->g;
    for (int r = 0; r < g; r++) { size_t off = t->dsp2[g + r]; for (int dd = 0; dd < p->na; dd++) { int rho = rho_of(p, dd, r); vcopy((char *)rb + rdsp[rho], x3 + off, rcnt[rho], s, host); off += rcnt[rho]; } }
}
static void complete_v(comm *c)
{
    lay_priv *p = PRIV(c); struct lay_v *v = &p->v;
    if (!v->pend) return;
    double f1 = lst_inter_wait(p, v->w, &v->w_on, &v->wf1);
    HIP_CHECK(hipSetDevice(p->dev));
    struct vtab t; t.dsp2 = v->cnt2 + 2 * p->g;         /* only dsp2 is needed by the scatter */
    v_scatter(c, &t, v->x3, v->rb, v->rcnt, v->rdsp, v->s, 0);
    HIP_CHECK(hipStreamSynchronize(v->s));
    if (lst_mode) { struct lst_rec *r = LREC(p, v->rec); r->f1 = f1; r->c1 = lst_now(); }
    free(v->cnt2); v->pend = 0;
}
static void y_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    lay_priv *p = PRIV(c);
    double a0 = lst_mode ? lst_now() : 0;
    while (p->npend) complete_oldest(c);                 /* one kind at a time */
    complete_v(c);
    struct vtab t; vtab_build(c, scnt, sdsp, rcnt, &t, 0);
    size_t x0n = t.contig ? 0 : t.S;
    need_vtmp(c, x0n + 2 * t.A + t.B + 4);
    char *x0 = p->vtmp, *x1 = x0 + x0n, *x2 = x1 + t.A, *x3 = x2 + t.A;
    HIP_CHECK(hipSetDevice(p->dev));
    if (lst_mode) p->v.rec = lst_new(p, a0, 1);
    v_stages(c, sb, scnt, sdsp, &t, x0, x1, x2, x3, s, 0, lst_mode ? LREC(p, p->v.rec) : 0);
    if (lst_mode == 2) lst_watch_start(p, &p->v.w, &p->v.w_on, &p->v.wf1);
    p->v.rb = rb; p->v.rcnt = rcnt; p->v.rdsp = rdsp; p->v.s = s; p->v.x3 = x3; p->v.cnt2 = t.cnt2; p->v.pend = 1; p->nlog++;
    free(t.T);
}
static void y_alltoallv_host(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp)
{
    struct vtab t; vtab_build(c, scnt, sdsp, rcnt, &t, 1);
    size_t x0n = t.contig ? 0 : t.S;
    char *x0 = (char *)malloc(x0n + 2 * t.A + t.B + 4), *x1 = x0 + x0n, *x2 = x1 + t.A, *x3 = x2 + t.A;
    v_stages(c, sb, scnt, sdsp, &t, x0, x1, x2, x3, 0, 1, 0);
    v_scatter(c, &t, x3, rb, rcnt, rdsp, 0, 1);
    free(x0); vtab_free(&t);
}
static void y_wait(comm *c)
{
    lay_priv *p = PRIV(c);
    if (!p->nlog) return;
    if (p->nlog <= p->npend + p->v.pend) { if (p->v.pend) complete_v(c); else complete_oldest(c); }   /* else the oldest waited-for exchange was completed early */
    p->nlog--;
}
static void y_barrier(comm *c) { lay_priv *p = PRIV(c); comm_barrier(p->intra); comm_barrier(p->inter); comm_barrier(p->intra); }
static uint64_t y_modq(comm *c, uint64_t v, uint64_t q, uint64_t w) { (void)c; (void)v; (void)q; (void)w; fprintf(stderr, "comm_layered: allreduce_modq not provided\n"); exit(1); }
static size_t y_max(comm *c, size_t v)
{
    lay_priv *p = PRIV(c);
    size_t m = comm_allreduce_max(p->intra, v);          /* the node's max (every APU thread) */
    m = comm_allreduce_max(p->inter, m);                 /* over the nodes, per mesh: every mesh sees the same values */
    return m;
}
/* all-gather in global rank order rho = g d + r: my block over the nodes into slot d of the result, then the
 * four APUs' slots over xGMI (APU d's slot is already in place: the intra op skips the self copy) */
static void y_allgather(comm *c, const void *sb, void *rb, size_t bytes)
{
    lay_priv *p = PRIV(c);
    HIP_CHECK(hipSetDevice(p->dev));
    if (p->minor) {                                      /* the group's na slots are contiguous: intra first, then the group blocks over the inter comm */
        char *slot = (char *)rb + (size_t)p->na * comm_rank(p->inter) * bytes;
        comm_allgather(p->intra, sb, slot, bytes);
        comm_allgather(p->inter, slot, rb, bytes * (size_t)p->na);
        return;
    }
    char *slot = (char *)rb + (size_t)p->d * p->g * bytes;
    comm_allgather(p->inter, sb, slot, bytes);
    comm_allgather(p->intra, slot, rb, bytes * (size_t)p->g);
}
static void y_allgather_host(comm *c, const void *sb, void *rb, size_t bytes)
{
    lay_priv *p = PRIV(c);
    if (p->minor) {
        char *slot = (char *)rb + (size_t)p->na * comm_rank(p->inter) * bytes;
        comm_allgather_host(p->intra, sb, slot, bytes);
        comm_allgather_host(p->inter, slot, rb, bytes * (size_t)p->na);
        return;
    }
    char *slot = (char *)rb + (size_t)p->d * p->g * bytes;
    comm_allgather_host(p->inter, sb, slot, bytes);
    comm_allgather_host(p->intra, slot, rb, bytes * (size_t)p->g);
}
static void y_destroy(comm *c)
{
    lay_priv *p = PRIV(c);
    lst_unregister(p);
    if ((p->own_tmp && p->tmp) || p->vtmp) HIP_CHECK(hipSetDevice(p->dev));
    if (p->own_tmp && p->tmp) { if (p->tmp_sym) comm_sym_free(p->inter, p->tmp); else HIP_CHECK(hipFree(p->tmp)); }
    if (p->vtmp) HIP_CHECK(hipFree(p->vtmp));
    free(p); free(c);
}
/* S12: the callers' symmetric buffers come from the inter transport's pool (the inter stage sends the caller's receive
 * buffer and receives into the scratch; a pool-resident buffer is not staged there) */
static void *y_sym_alloc(comm *c, size_t bytes) { return comm_sym_alloc(PRIV(c)->inter, bytes); }
static void y_sym_free(comm *c, void *p) { comm_sym_free(PRIV(c)->inter, p); }
static const struct comm_ops lay_ops = { y_rank, y_size, y_alltoall, y_wait, y_barrier, y_modq, y_max, y_destroy, 0, 0, y_allgather, y_allgather_host, y_alltoallv, y_alltoallv_host, y_sym_alloc, y_sym_free };
comm *comm_layered_create(comm *intra, comm *inter, int d)
{
    if (comm_size(intra) != NA) { fprintf(stderr, "comm_layered: the intra communicator must have %d ranks\n", NA); exit(1); }
    comm *c = (comm *)calloc(1, sizeof *c); lay_priv *p = (lay_priv *)calloc(1, sizeof *p);
    p->intra = intra; p->inter = inter; p->d = d; p->dev = d; p->na = NA; p->g = comm_size(inter);
    c->ops = &lay_ops; c->priv = p; c->size = NA * p->g; c->rank = p->g * d + comm_rank(inter); c->inflight = 2;
    lst_register(p, comm_rank(inter), p->g);
    return c;
}
/* S: the intra-minor form on device dev: rank = size(intra) x rank(inter) + rank(intra) (the dragonfly's third layer: intra
 * = the nodes of my group, inter = my in-group index's nodes across the groups; rank = node) */
comm *comm_layered_create_minor(comm *intra, comm *inter, int dev)
{
    comm *c = (comm *)calloc(1, sizeof *c); lay_priv *p = (lay_priv *)calloc(1, sizeof *p);
    p->intra = intra; p->inter = inter; p->d = comm_rank(intra); p->dev = dev; p->na = comm_size(intra); p->g = comm_size(inter); p->minor = 1;
    c->ops = &lay_ops; c->priv = p; c->size = p->na * p->g; c->rank = p->na * comm_rank(inter) + p->d; c->inflight = 2;
    lst_register(p, comm_rank(inter), p->g);
    return c;
}
/* a device scratch of at least one slab buffer (size x bytes of the largest exchange; two slab buffers of the
 * pipelined exchanges let two stay in flight); the comm's own is freed */
void comm_layered_scratch(comm *c, void *p, size_t bytes)
{
    lay_priv *v = PRIV(c);
    if (v->npend) { fprintf(stderr, "comm_layered: scratch replaced with an exchange pending\n"); exit(1); }
    if (v->own_tmp && v->tmp) { HIP_CHECK(hipSetDevice(v->dev)); if (v->tmp_sym) comm_sym_free(v->inter, v->tmp); else HIP_CHECK(hipFree(v->tmp)); }
    v->tmp = (char *)p; v->tmp_cap = bytes; v->own_tmp = 0; v->tmp_sym = 0;
}

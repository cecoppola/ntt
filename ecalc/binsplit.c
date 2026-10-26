/* binsplit.c - see binsplit.h */
#include <stdio.h>
#include "fatal.h"
#include "xshmem_round.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "binsplit.h"
#include "rns_mul.h"
#include "mem.h"
#include "dbig.h"
#include "spill.h"
#include "memsample.h"
int ec_np_init(void);                                     /* modarith.h (crt.c): the prime count (the layout report) */
extern int ec_np_auto; int ec_np_planes(int pool_log, int g);   /* modarith.h (crt.c), pool 0's planes (ECALC_NP=auto: 3 or 4 by the run's largest group) */
size_t newton_dkm_h(size_t k);   /* newton.h (newton_db.c), dm_layout's DKM set */
/* the node-process rank/size (the checkpoint names and headers), from mn.c (mn.h needs the HIP headers; this is a C file) */
int mn_rank(void); int mn_size(void);
/* S1d (TARGET1): the PE identity from vpe.h (vpe.h includes comm.h, which needs HIP's headers; this file is plain C).  With ECALC_PE_PER_GPU=1 a PE is one process per GPU;
 * its binary splitting has ONE region (its device, ), its own term range and its own state (one copy per process); switch off: vpe_on() = 0,
 * vpe_idx = 0 and every line below behaves as before. */
void vpe_adopt(void); int vpe_on(void); int vpe_lanes(void); int vpe_pe(void); int vpe_npe(void);
int mn_groups_parse(int size, int *out, int max); size_t rns_mul_dist_mn_scratch(size_t na, size_t nb, int has_x, int g, size_t share_a, size_t share_b, size_t share_c, int *pieces);
size_t rns_mul_dist_mn_stage(size_t na, size_t nb, int g, size_t share_a, size_t share_b, size_t share_c, size_t *qmax);
int rns_mul_dist_mn_logcap_g(int g); int rns_mul_dist_mn_logmin_g(int g);   /* D19 (rns_dist.c): DM_MN_WRAP's plane in dm_layout */
size_t rns_mul_dist_mn_vslot(size_t na, size_t nb, int g);   /* B7ACCT (rns_dist.c) */   /* the tree's schedule and its products' scratch (rns_dist.c / mdb.h, a C++ header) */
/* the layout's environment hand-offs (COMM_*): the values are the same on every call, but setenv itself is not thread-safe */
static pthread_mutex_t g_env_mx = PTHREAD_MUTEX_INITIALIZER;
static int bs_setenv(const char *n, const char *v, int o) { pthread_mutex_lock(&g_env_mx); int r = setenv(n, v, o); pthread_mutex_unlock(&g_env_mx); return r; }
static int bs_unsetenv(const char *n) { pthread_mutex_lock(&g_env_mx); int r = unsetenv(n); pthread_mutex_unlock(&g_env_mx); return r; }
/* the per-process state of binsplit.h (bs_N, bs_st, bs_seed_terms, bs_a0, bs_b1, the hook, the device P and Q)*/
unsigned long bs_N;                       /* M6: the run's N, for the tree sets written by mn.c */
bs_stats bs_st;
int bs_seed_terms = 256;   /* BS_SEED_TERMS: seed span; 256 measured best in both bases (RESULTS.md 58), 512 was the earlier value */
int bs_verbose = 0;
const char *bs_ckpt_dir = 0;                         /* BS_CKPT_DIR: the directory of the top-level set (ECALC_CKPT_TOP); 0 = none */

unsigned long e_terms(unsigned long d)
{
    /* min N with lgamma(N+1)/ln 10 >= d + 50, by bisection (the function is increasing; the
     * linear scan this replaces cost ~45 s at 4e10 -- it was inside the run's wall time) */
    double target = (double)d + 50.0, l10 = log(10.0);
    unsigned long lo = 1, hi = 2;
    while (lgamma((double)hi + 1.0) / l10 < target) hi *= 2;
    while (hi - lo > 1) { unsigned long mid = lo + (hi - lo) / 2; if (lgamma((double)mid + 1.0) / l10 < target) lo = mid; else hi = mid; }
    return lgamma((double)lo + 1.0) / l10 >= target ? lo : hi;
}

/* span [a, b) by schoolbook, right to left: P = 1, Q = b-1; prepend k: P = Q + P, Q = k Q */
static void span(bigint *P, bigint *Q, unsigned long a, unsigned long b)
{
    bi_set_u64(P, 1); bi_set_u64(Q, b - 1);
    for (unsigned long k = b - 1; k-- > a;) bi_span_step(P, Q, k);   /* P += Q; Q *= k fused, the quotient by a precomputed reciprocal */
}
void binsplit_span(bigint *P, bigint *Q, unsigned long a, unsigned long b) { span(P, Q, a, b); }   /* for tests/t_seed (declared there) */
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
 * read local (RESULTS.md 55).  Region r's pool is device r's memory. */
#define NRMAX 8                                       /* array bound: the regions of one process (4 per node; one per PE) */
static int bs_nreg(void) { return vpe_on() ? 1 : 4; }   /* regions of one binary splitting: 4 per node (one per APU), or ONE per PE (its own device) */
#define NR bs_nreg()
/* the planner's per-node counts.  PE layout off: 4 devices per node and the node range is split into 4 regions.  ECALC_PE_PER_GPU=1: L = vpe_lanes() PEs per node (one device each), each
 * holding ONE region over its own range; a node is L identical PEs, so the per-node figures (planes, arena, v-slots, the shares of the dm numbers) are the one PE's times L.
 * bs_nrn() = devices (PEs) per node, bs_lanes() = PEs per node (1 off).  The tree's group schedule stays node-based (MN_GROUPS over the nodes; the groups below a node
 * are not modelled until S1b/S1c's *_pe functions exist) -- with L = 4 the PE model equals the node model up to rounding. */
static int bs_nrn(void) { return vpe_on() ? vpe_lanes() : 4; }
static int bs_lanes(void) { return vpe_on() ? vpe_lanes() : 1; }
/* S2c (ECALC_PE_PER_GPU=1): every PE is its own process, so each of the node's PEs has its own ROCm runtime and its own comm host memory: the planner's host terms 'runtime' and 'comm' are
 * multiplied by the processes per node (mem_model.py PPN_PROC, in lockstep); 1 with the switch off. */
int vpe_ppg(void);
static size_t bs_ppn_proc(void) { return vpe_ppg() ? (size_t)vpe_lanes() : 1; }
int binsplit_ppn_proc(void) { return (int)bs_ppn_proc(); }   /* S2: ecalc.c budget_check divides the node model by the same term, so planner and budget agree */
#define BS_HOST_INIT_BYTES_P() ((size_t)7000000000 * bs_ppn_proc() + ((size_t)8 << 30))   /* BS_HOST_INIT_BYTES with the runtime per process */
static size_t bs_staging(void) { return vpe_on() ? (size_t)vpe_lanes() << (vpe_ppg() && rns_batch_split_mode() == 1 ? 31 : 30) : (size_t)4 << 30; }   /* RNS_BATCH_SPLIT=1 under P: ecalc.c raises the pinned staging to 2 GiB per PE */   /* the checkpoint staging: 1 GiB per APU thread / PE */
static unsigned long bs_pe_b1(unsigned long N, int g)   /* the end of rank 0's (PE 0's) term range [1, b1): b1 = 0 = all terms */
{
    unsigned __int128 G = (unsigned __int128)g * (unsigned)bs_lanes(), nn = N;
    return G > 1 ? 1 + (unsigned long)(nn / G) : 0;
}
struct node { size_t po, pn, qo, qn; int r; dbig *pd, *qd; };   /* pd/qd: the node as device numbers (top levels on the device tier, WP5) */
struct level { uint64_t *pool[NRMAX]; struct node *nd; size_t n; };
#define NODE_P(lv, nd) ((lv).pool[(nd)->r] + (nd)->po)
#define NODE_Q(lv, nd) ((lv).pool[(nd)->r] + (nd)->qo)
int bs_mdev_logl = RNS_BATCH_LOGL_MAX;                /* BS_MDEV_LOGL: products above 2^this go to the mdev tier (lower it to test at 10^9) */
/* the planner's PE terms for mn_plan.c (binsplit.h) */
unsigned long binsplit_pe_b1(unsigned long N, int g) { return bs_pe_b1(N, g); }
int binsplit_nrn(void) { return bs_nrn(); }
int binsplit_lanes(void) { return bs_lanes(); }
/* the region pools, the arenas (this process's one PE) */
static uint64_t *g_pool[2][NRMAX]; static size_t g_cap[2][NRMAX];
int vpe_device(void);
static int reg_dev(int r, int nd) { return vpe_on() ? vpe_device() % nd : r % nd; }   /* the device of region r: the vPE's own APU, or the region's */
/* a node in a region pool as a (non-owning, single-quarter) device number */
static dbig pool_view(const uint64_t *p, size_t n)
{
    dbig v; memset(&v, 0, sizeof v); v.q[0] = (uint64_t *)p; v.n = n; v.qc = (size_t)1 << 40; return v;
}
static dbig node_p(const struct level *lv, const struct node *nd) { return nd->pd ? *nd->pd : pool_view(lv->pool[nd->r] + nd->po, nd->pn); }
static dbig node_q(const struct level *lv, const struct node *nd) { return nd->qd ? *nd->qd : pool_view(lv->pool[nd->r] + nd->qo, nd->qn); }
#include "verify.h"
/* the two parities of region r are the halves of one device allocation (the arena), so that when both
 * are handed to the dbig block pool they coalesce into one extent (dm's largest quarters need contiguous space:
 * two 14 GB halves that cannot merge left the pool falling back to hipMalloc, RESULTS 70).  A pool that outgrows
 * its half is re-allocated on its own (the half stays and is donated too); the arena itself outlives the block
 * pool's use of it (borrowed, never freed by db_release_pools) and goes at rns_shutdown. */
static struct { uint64_t *base; size_t bytes, half, hole, thresh; int dev; int donated; int vmm; } g_arena[NRMAX];   /* vmm: a VMM range from db_vmm_arena_alloc */   /* bytes = 2 half + extra + hole; the hole (the last bytes) is the block pool's reserved tail */
static int in_arena(int r, const uint64_t *p) { return g_arena[r].base && p >= g_arena[r].base && p < g_arena[r].base + g_arena[r].bytes / 8; }
static void arena_half_range(int r, int which, char **p, size_t *bytes)   /* the arena's range that goes with parity `which`: half 1 carries the extra and the hole to the arena's end */
{
    *p = (char *)g_arena[r].base + (size_t)which * g_arena[r].half;
    *bytes = which ? g_arena[r].bytes - g_arena[r].half : g_arena[r].half;
}
static void arena_donated_half(int r, int which, int dev)   /* after a half went to the pool: the second completes the arena; half 1 brings the tail */
{
    if (which == 1 && g_arena[r].hole) db_pool_set_tail(dev, (char *)g_arena[r].base + g_arena[r].bytes - g_arena[r].hole, g_arena[r].hole, g_arena[r].thresh);
    if (++g_arena[r].donated == 2) mem_dev_forget(g_arena[r].base);
}
static void donate_one(int which, int r)             /* pool (which, r) to the block allocator: its arena half joins the other half when that is already there */
{
    uint64_t *p = g_pool[which][r]; size_t cap = g_cap[which][r]; if (!p || mem_dev_of(p) < 0) return;
    int dev = reg_dev(r, mem_device_count());
    if (in_arena(r, p)) { char *hp; size_t hb; arena_half_range(r, which, &hp, &hb); if (g_arena[r].vmm) db_vmm_arena_wait(dev, (size_t)-1); db_donate_adjacent(dev, hp, hb); arena_donated_half(r, which, dev); }
    else {
        db_donate(dev, p, cap * 8); mem_dev_forget(p);
        if (g_arena[r].base) {                       /* this parity outgrew its arena half: the idle half goes too */
            char *hp; size_t hb; arena_half_range(r, which, &hp, &hb); db_donate_adjacent(dev, hp, hb); arena_donated_half(r, which, dev);
        }
    }
    g_pool[which][r] = 0; g_cap[which][r] = 0;
}
static void donate_pools(int which)                  /* the region pools of one parity to the device block allocator */
{
    for (int r = 0; r < NR; r++) donate_one(which, r);
}
void binsplit_release_arenas(void)                   /* after the block pool is done with them (rns_shutdown) */
{
    for (int r = 0; r < NRMAX; r++) {
        __typeof__(g_arena[0]) *a = &g_arena[r]; if (!a->base) continue;
        if (a->vmm) { if (a->donated < 2) mem_dev_forget(a->base); db_vmm_arena_release(a->dev); } else if (a->donated < 2) mem_dev_free(a->base); else mem_dev_free_raw(a->dev, a->base);
        a->base = 0;
    }
}
static int region_of(size_t i, size_t n) { size_t r = i * NR / n; return (int)(r < NR ? r : NR - 1); }
/* the region of output node i of a level of n nodes.  Large levels: region NR i / n -- a subtree per
 * region, a pair and its parent share a region (RESULTS 55).  Small levels (n <= bs_balance_n): the node counts
 * are not multiples of NR (the tree's odd carries), so the subtree rule puts two of five nodes in region 0 (40 %
 * of the level) and its pool grew inside the phase (RESULTS 72); the nodes go round robin instead (i mod NR: at
 * most one node more in a region, the carried odd node -- the small one -- last).  A least-loaded rule with ties
 * to the inputs' region was tried first: its choice depends on the real sizes, so the sizing pass could not
 * predict which region gets the extra node and had to size every region for the largest share (4 x a level at
 * a two-node level).  Any placement is correct: the batch tier computes a product on the device that owns the
 * result and reads the operands where they are (rns_mul_batch_local). */
int bs_balance_n = 16;                               /* BS_BALANCE_N: levels with at most this many nodes are balanced; 0 = never */
static int place_node(size_t i, size_t n, const size_t *offr, int ra, int rb)
{
    (void)offr; (void)ra; (void)rb;
    if (n > (size_t)bs_balance_n) return region_of(i, n);
    return (int)(i % NR);                            /* round robin: at most one node more per region, and the placement depends on the index alone, so the sizing pass (region_need) predicts it exactly */
}

/* the normalised length of a node's limbs in a device pool the host cannot read (a VMM range): the top limbs come
 * over by DMA, 64 at a time from the end (a product's length is within a limb or two of na + nb: one DMA almost always) */
static size_t node_norm(const uint64_t *p, size_t n)
{
    if (!db_pool_vmm_on() || mem_dev_of(p) < 0) return limb_norm(p, n);
    uint64_t t[64];
    while (n) { size_t m = n < 64 ? n : 64; mem_dev_copy(t, p + (n - m), m * 8); size_t k = limb_norm(t, m); if (k) return n - m + k; n -= m; }
    return 0;
}
/* the calling thread's rank and the team's size within its region, and its home region (-1: not pinned).  A PE process's team is its own omp team (it owns its device and its share of the cores) */
static int bs_rt(int *rk, int *home)
{
    if (vpe_on()) { *rk = omp_get_thread_num(); *home = -1; return omp_get_num_threads(); }
    *home = mem_thread_home(); return mem_region_threads(rk);
}
/* the omp thread count of a whole-node team: all cores, or a PE's share of them (the cores of the process divided by its lanes) */
/* (S2c: a PE process has the CPUs of its own mask to itself: no division by the lanes) */
static int bs_nthreads(void) { return omp_get_max_threads(); }
/* copy limbs out of (or into) region r's pool with the threads of r's node (a lone memcpy from device memory runs at a few GB/s) */
static void region_copy(uint64_t *dst, const uint64_t *src, size_t limbs, int r)
{
    if (mem_dev_of(src) >= 0 || mem_dev_of(dst) >= 0) { mem_dev_copy(dst, src, limbs * 8); return; }   /* (first: a VMM arena takes no CPU access) */
    if (limbs < ((size_t)1 << 20)) { memcpy(dst, src, limbs * 8); return; }
#pragma omp parallel num_threads(bs_nthreads())
    {
        int rk, home, cnt = bs_rt(&rk, &home);
        if (home < 0 || r % NR == home % NR) {
            size_t chunk = (limbs + cnt - 1) / cnt, lo = chunk * rk, hi = lo + chunk < limbs ? lo + chunk : limbs;
            if (lo < hi) memcpy(dst + lo, src + lo, (hi - lo) * 8);
        }
    }
}
static __thread int g_in_pregrow;                            /* pool_get inside binsplit_pregrow (the sizing pass) */
static uint64_t *pool_get(int which, int r, size_t limbs)
{
    if (which == 1 && g_arena[r].vmm && !g_in_pregrow && in_arena(r, g_pool[1][r])) db_vmm_arena_wait(g_arena[r].dev, 2 * g_arena[r].half);   /* the parity-1 half is mapped in the background during the seeds (its first use is level 1's outputs; pregrow's own pool_get must not wait, or the mapping runs inside init) */
    if (g_cap[which][r] < limbs) {
        if (mem_pool_guard && g_pool[which][r]) {         /* a region pool growing inside bs -- the layout (binsplit_pregrow) sized it; abort with the accounting unless RNS_POOL_GROW=1 */
            if (rns_pool_grow < 0) rns_pool_grow = getenv("RNS_POOL_GROW") ? atoi(getenv("RNS_POOL_GROW")) : 0;
            if (!rns_pool_grow) { ec_fatal_begin(EC_RC_OOM, "bs: region pool %d of parity %d would grow inside bs, %.2f -> %.2f GB: the regions are sized at init and must not grow (RNS_POOL_GROW=1 allows it)\n", r, which, g_cap[which][r] * 8e-9, limbs * 8e-9); mem_report("GROW"); mem_report_summary(); ec_fatal_end(EC_RC_OOM); }   /* one report per process, then _exit (the four APU threads meet it together) */
        }
        if (g_pool[which][r] && !in_arena(r, g_pool[which][r])) { if (mem_dev_of(g_pool[which][r]) >= 0) mem_dev_free(g_pool[which][r]); else mem_hreg_free(g_pool[which][r]); }
        size_t cap = limbs + limbs / 8 + 4096;
        int nd = mem_device_count();
        g_pool[which][r] = (uint64_t *)(nd > 0 ? mem_dev_alloc(reg_dev(r, nd), cap * 8) : mem_hreg_alloc(cap * 8));
        g_cap[which][r] = cap;
        bs_st.n_grow++; bs_st.grow_bytes += cap * 8;
        if (bs_verbose) printf("bs: level pool %d region %d -> %.2f GB (%s)%s\n", which, r, cap * 8e-9, nd > 0 ? "device" : "host", g_arena[r].base ? " [outgrew its arena half]" : "");
    }
    return g_pool[which][r];
}
/* C4: the two parities of region r from one allocation of 2 cap limbs on the region's device; plus `extra` bytes
 * (the dm phase's need beyond the parities), the last `hole` of them the block pool's reserved tail */
static void arena_get(int r, size_t cap, size_t extra, size_t hole, size_t thresh)
{
    int nd = mem_device_count(); if (g_arena[r].base || g_cap[0][r] >= cap || nd <= 0) return;
    cap = (cap * 8 + ((size_t)2 << 20) - 1) / ((size_t)2 << 20) * ((size_t)2 << 20) / 8;
    extra = (extra + ((size_t)2 << 20) - 1) / ((size_t)2 << 20) * ((size_t)2 << 20); hole = (hole + ((size_t)2 << 20) - 1) / ((size_t)2 << 20) * ((size_t)2 << 20);
    g_arena[r].dev = reg_dev(r, nd); g_arena[r].half = cap * 8; g_arena[r].bytes = 2 * cap * 8 + extra; if (hole > g_arena[r].bytes) hole = g_arena[r].bytes; g_arena[r].hole = hole; g_arena[r].thresh = thresh; g_arena[r].donated = 0;
    if (db_pool_vmm_on()) { g_arena[r].vmm = 1; g_arena[r].base = (uint64_t *)db_vmm_arena_alloc(g_arena[r].dev, g_arena[r].bytes, g_arena[r].half); mem_dev_note(g_arena[r].dev, g_arena[r].base, g_arena[r].bytes); }
    else g_arena[r].base = (uint64_t *)mem_dev_alloc(g_arena[r].dev, g_arena[r].bytes);
    rns_shutdown_hook = binsplit_release_arenas;         /* released at rns_shutdown on every rank, whether or not binsplit_free_pools ran there (A-mem open issue 2) */
    for (int w = 0; w < 2; w++) { g_pool[w][r] = g_arena[r].base + w * cap; g_cap[w][r] = cap; }
    if (bs_verbose) printf("bs: region %d arena %.2f GB on APU %d (two parities of %.2f GB, dm extra %.2f GB, tail %.2f GB)\n", r, g_arena[r].bytes * 1e-9, g_arena[r].dev, cap * 8e-9, extra * 1e-9, hole * 1e-9);
}

/* BS_SEED_FILL=<limbs> (0 = off, the default): the seed span chosen per run as the largest S whose largest span, the last
 * one [bend - S, bend), has at most <limbs> limbs (mn_plan.c's size: floor(log10 Q / digits per limb) + 1).  A node of level l then
 * has at most 2^l <limbs> limbs, so with <limbs> = 128 every batch product from 2^10 points up fills its 2^k length and the batch
 * tier's top level ends at the mdev threshold (2^30); 96 or 192 fill 3 2^k.  S depends on log10 of the run's last term (at 10^11:
 * 128 -> 229, at 4 x 10^10: 239; archive:scripts_2026-10-23/t2_seed_model.py), so no one BS_SEED_TERMS fits every size.  Overrides BS_SEED_TERMS */
/* BS_SEED_FILL=128 by default; BS_SEED_FILL=0 (or a BS_SEED_TERMS in the environment, or a caller
 * that set bs_seed_terms before the first use, as the unit tests do) keeps the fixed span */
static int g_fill_auto = -1;
long bs_seed_fill(void)
{
    const char *e = getenv("BS_SEED_FILL"); if (e) return atol(e);
    if (g_fill_auto < 0) g_fill_auto = !getenv("BS_SEED_TERMS") && bs_seed_terms == 256;
    return g_fill_auto ? 128 : 0;
}
unsigned long bs_seed_terms_for(unsigned long bend)
{
    long fill = bs_seed_fill();
    if (fill <= 0 || bend < 3) return getenv("BS_SEED_TERMS") ? (unsigned long)atol(getenv("BS_SEED_TERMS")) : (unsigned long)bs_seed_terms;
    long double dpl = 18.0L, l10 = logl(10.0L), lb = lgammal((long double)bend);
#define SEED_FILL_LIMBS(S) ((long)floorl((lb - lgammal((long double)(bend - (S)))) / l10 / dpl) + 1)
    unsigned long S = (unsigned long)(fill * dpl / log10l((long double)bend)); if (S < 1) S = 1; if (S > bend - 2) S = bend - 2;
    while (S > 1 && SEED_FILL_LIMBS(S) > fill) S--;
    while (S + 2 < bend && SEED_FILL_LIMBS(S + 1) <= fill) S++;
#undef SEED_FILL_LIMBS
    return S;
}
static size_t seed_limbs(unsigned long N, size_t *per_out, unsigned long *nspan_out)
{
    if (getenv("BS_SEED_TERMS")) bs_seed_terms = atoi(getenv("BS_SEED_TERMS"));
    if (bs_seed_fill() > 0) bs_seed_terms = (int)bs_seed_terms_for(bs_b1 ? bs_b1 : N + 1);
    if (getenv("BS_MDEV_LOGL")) bs_mdev_logl = atoi(getenv("BS_MDEV_LOGL"));
    unsigned long S = bs_seed_terms, nterms = bs_b1 ? bs_b1 - bs_a0 : N, nspan = (nterms + S - 1) / S;
    size_t per = (S * (size_t)ceil(log2((double)N + 2.0)) + 128) / 59 + 2;   /* a decimal limb holds 59.8 bits */
    if (per_out) *per_out = per;
    if (nspan_out) *nspan_out = nspan;
    return 2 * per * nspan;
}
/* what each region must hold, by laying out every level of the tree in advance with the seed bound
 * `per` for every span (level l's node i covers spans [i 2^l, (i+1) 2^l) of the nspan, P and Q slots as the level
 * loop lays them out, regions by the same place_node), over the levels that live in the region pools: with the
 * device top levels, the batch levels (the tier switch taken at 0.9 x the bound, so a level whose real sizes fall
 * just below the threshold is still counted); with host regions or the host mdev tier, every level.  The bound
 * is ~11 % above the real sizes (RESULTS 72: level 1 = 0.895 of total0) and pool_get adds 1/8: the margin.  This
 * replaces the flat total0 / NR (1 + 1/4), which held 1.4 x the live data and still grew at the five-node level. */
/* the tier prediction of the sizing pass.  The old rule took a level as mdev when 0.9 x the bound
 * (per: ceil(log2(N + 2)) bits per term of the WHOLE series) exceeded the batch tier's limit; the real sizes of a node whose
 * terms are the smallest of the g ranges (rank 0 at size > 1 holds [1, N/g)) are 0.88 of that bound (10^11 on 4 nodes:
 * level 23's inputs 5.31e8 limbs against the bound 6.04e8), so the rule skipped a level the run then computed in the batch
 * tier, and its outputs did not fit the pool (rank 0: parity 1's pool 0, 10.87 -> 16.15 GB, rc 6).  BS_POOL_RULE=1 (the
 * default) decides from a LOWER BOUND on the level's largest Q -- the last full node's product over its own terms,
 * lgamma(b) - lgamma(a) in limbs, the measure bs_seed_terms_for uses -- so a level is skipped only when even that bound is
 * over the limit (the run then takes it through the mdev tier for sure); the bound is 0.97-0.99 of the real size, so at
 * most one more level than before is counted, and only where the old rule would have aborted the run or came within 3 %
 * of it.  mem_model.py region_need follows it. */
static size_t level_q_lower(unsigned long a0, unsigned long nterms, unsigned long S, size_t m_full, unsigned long nspan)
{
    unsigned long nfull = nspan / m_full; if (!nfull) return 0;
    unsigned long t0 = a0 + (nfull - 1) * m_full * S, t1 = t0 + m_full * S; if (t1 > a0 + nterms) t1 = a0 + nterms;
    long double dpl = 18.0L;
    return (size_t)floorl((lgammal((long double)t1) - lgammal((long double)t0)) / logl(10.0L) / dpl);
}
static void region_need(unsigned long N, size_t need[NRMAX])
{
    size_t per; unsigned long nspan; seed_limbs(N, &per, &nspan);
    unsigned long S = bs_seed_terms, a0 = bs_b1 ? bs_a0 : 1, nterms = bs_b1 ? bs_b1 - bs_a0 : N;   /* this node's own terms */
    /* FIT: the last two results kept (a pure function of these inputs; ~0.18 s at the target's share, and the fit lays out two arms) */
    struct rn_memo { unsigned long N, a0, nterms, S, nspan; size_t per; int logl, bal; size_t need[NRMAX]; int ok; };
    static __thread struct rn_memo memo[2]; static __thread int next;
    for (int m = 0; m < 2; m++) if (memo[m].ok && memo[m].N == N && memo[m].a0 == a0 && memo[m].nterms == nterms && memo[m].S == S && memo[m].nspan == nspan
                                    && memo[m].per == per && memo[m].logl == bs_mdev_logl && memo[m].bal == bs_balance_n) { memcpy(need, memo[m].need, sizeof memo[m].need); return; }
    struct rn_memo *mm = &memo[next]; next ^= 1; mm->ok = 0;
    for (int r = 0; r < NR; r++) need[r] = 0;
    { size_t r0[NRMAX + 1]; for (int r = 0; r <= NR; r++) { r0[r] = 0; while (r0[r] < nspan && region_of(r0[r], nspan) < r) r0[r]++; }
      for (int r = 0; r < NR; r++) need[r] = 2 * per * (r0[r + 1] - r0[r]) + 2; }
    int *cur_r = 0, *nxt_r = 0;
    for (int l = 0; ; l++) {
        size_t n_in = (nspan + ((size_t)1 << l) - 1) >> l; if (n_in <= 1) break;
        size_t m_full = (size_t)1 << l;
        int mdev_level = 2 * level_q_lower(a0, nterms, S, m_full, nspan) + 1 > ((size_t)1 << bs_mdev_logl);   /* from the lower bound (a miss would abort) */
        if (mdev_level) break;                                                    /* the mdev levels use device numbers */
        size_t npairs = n_in / 2, odd = n_in & 1, n = npairs + odd, offr[NRMAX] = {0};
        if (n <= (size_t)bs_balance_n) nxt_r = (int *)malloc(n * sizeof *nxt_r);
        for (size_t i = 0; i < n; i++) {
            size_t ma = (2 * i) * m_full < nspan ? (nspan - 2 * i * m_full < m_full ? nspan - 2 * i * m_full : m_full) : 0;
            size_t mb = (2 * i + 1) * m_full < nspan ? (nspan - (2 * i + 1) * m_full < m_full ? nspan - (2 * i + 1) * m_full : m_full) : 0;
            size_t pa = ma * per + l, qa = ma * per, pb = mb * per + l, qb = mb * per;
            int ra = cur_r ? cur_r[2 * i] : region_of(2 * i, n_in), rb = odd && i == npairs ? -1 : cur_r ? cur_r[2 * i + 1] : region_of(2 * i + 1, n_in);
            int r = place_node(i, n, offr, ra, rb);
            if (odd && i == npairs) offr[r] += pa + qa; else offr[r] += pa + qb + 1 + qa + qb;
            (void)pb;
            if (nxt_r) nxt_r[i] = r;
        }
        for (int r = 0; r < NR; r++) if (offr[r] + 2 > need[r]) need[r] = offr[r] + 2;
        free(cur_r); cur_r = nxt_r; nxt_r = 0;
    }
    free(cur_r);
    mm->N = N; mm->a0 = a0; mm->nterms = nterms; mm->S = S; mm->nspan = nspan; mm->per = per; mm->logl = bs_mdev_logl; mm->bal = bs_balance_n;
    memcpy(mm->need, need, sizeof mm->need); mm->ok = 1;
}
/* S2fix (ECALC_PE_PER_GPU=1): the largest batch-tier product of this process's leaf -- rns_mul_batch's local tier needs np x 2^logL limbs in the one device's plane pool
 * (rns_mul.c: `rns_plane_limbs() / (ec_np * L) >= 1`); a product that does not fit goes to the striped tier, which needs all 4 devices and is fatal here.  Walks region_need's levels
 * (batch while the lower bound of the level's Q is within 2^BS_MDEV_LOGL), the level's largest product P1 Q2 = min(2 m, span) x per limbs (+3 %: per is an estimate); the caller
 * sets the term range.  *nc = that product's limbs; returns its logL (0: no batch level). */
static int bs_batch_logl_lv(unsigned long N, size_t *nc, size_t *lv, int nlv)   /* lv[l] (nlv entries, zeroed here) = level l's largest product */
{
    if (lv) memset(lv, 0, nlv * sizeof *lv);
    size_t per; unsigned long nspan; seed_limbs(N, &per, &nspan);
    unsigned long S = bs_seed_terms, a0 = bs_b1 ? bs_a0 : 1, nterms = bs_b1 ? bs_b1 - bs_a0 : N; size_t best = 0;
    for (int l = 0; ; l++) {
        size_t n_in = (nspan + ((size_t)1 << l) - 1) >> l; if (n_in <= 1) break;
        size_t m_full = (size_t)1 << l;
        if (2 * level_q_lower(a0, nterms, S, m_full, nspan) + 1 > ((size_t)1 << bs_mdev_logl)) break;
        size_t span = 2 * m_full < nspan ? 2 * m_full : nspan, c = (size_t)((double)span * (double)per * 1.03) + (size_t)l + 2;
        if (c > best) best = c;
        if (lv && l < nlv) lv[l] = c;
    }
    if (nc) *nc = best;
    int lg = 0; while (((size_t)1 << lg) < best) lg++;
    return best ? (lg < 10 ? 10 : lg) : 0;
}
static int bs_batch_logl(unsigned long N, size_t *nc) { return bs_batch_logl_lv(N, nc, 0, 0); }
int binsplit_ppg_batch_logl(unsigned long N, int g, size_t *nc)   /* rank 0's (PE 0's) range, restored after */
{
    unsigned long a0 = bs_a0, b1 = bs_b1; bs_a0 = 1; bs_b1 = bs_pe_b1(N, g);
    int lg = bs_batch_logl(N, nc); bs_a0 = a0; bs_b1 = b1; return lg;
}
/* RNS_BATCH_SPLIT's planner: every batch level's largest product (limbs, level 0 first; 0 = no such level) of PE 0's leaf; returns the number of levels */
int binsplit_ppg_batch_levels(unsigned long N, int g, size_t *lv, int nlv)
{
    unsigned long a0 = bs_a0, b1 = bs_b1; bs_a0 = 1; bs_b1 = bs_pe_b1(N, g);
    bs_batch_logl_lv(N, 0, lv, nlv); bs_a0 = a0; bs_b1 = b1;
    int n = 0; for (int i = 0; i < nlv; i++) if (lv[i]) n = i + 1;
    return n;
}
/* the dm phase's block-pool need per device from N alone, the way the driver's formulas
 * size it (ecalc.c: n_Q = Q's limbs, Q = N! in the chosen base; dl = the limbs of 10^d; k_mu = P.n + 1 + dl - n_Q + 1;
 * newton_db_recip: r, r2 of k + 4 limbs, t1 of max(n_Q + k, 2k) + 8; rns_dist's piece temporary of at most 2^pool_log + 8
 * limbs; Q and S = P + Q live from the top of bs with their bound's margin).  Every device holds a quarter of each.  The
 * hole is t1's quarter -- the one block that no arena laid out for bs holds contiguously (archive:results_merged_2026-10-24/MEMORY.md §M: 8.9 / 15.6 / 17.8 GB
 * at 4 / 7 / 8e10, exactly the in-phase hipMalloc) -- reserved as the arena's tail; thresh is what the pool treats as "large"
 * (5/8 of the hole: r, r2 at ~ half of it stay out of the tail, t1 takes it).  At size > 1 the shares are 1/size of it
 * (the sharded division, A-div) and the tree's products add their slabs (tree_need_dev). */
struct dm_layout { size_t nq, k, tcap, hole, thresh, need_dev, tree_dev; size_t jl, v2, v3, div; int tight; size_t room; int dkm, lean; size_t wrap, trim; };   /* DL: dkm = the layout follows the DKM division; lean = the lean division set */   /* room = BS_ARENA_ROOM's term (in need_dev) */   /* jl = the last doubling's start, v2 / v3 / div = the terms per device, the switches */
static size_t quarter_bytes(size_t limbs) { size_t q = (size_t)bs_nrn(); return ((limbs + q - 1) / q + 4095) / 4096 * 4096 * 8; }   /* the share of one device (a quarter on the 4-APU node; one PE's with ECALC_PE_PER_GPU) */
/* DM_TIGHT=1 -- the reciprocal reserves r2 only when d is first written (2j + 4 limbs, freed whenever
 * its content is dead) and t1 per doubling at Q_t (take) + r (j + 1) + 8, and the device tier's top levels free each input pair as it is
 * consumed; dm_layout's v2 / v3 follow (E2: -1 n_Q of arena).  */
int dm_tight = -1;
void dm_switches(void) { if (dm_tight < 0) { dm_tight = getenv("DM_TIGHT") ? atoi(getenv("DM_TIGHT")) : db_pool_vmm_on();   /* default: on with the VMM pool (it places DM_TIGHT's blocks) */ } }
static size_t tree_need_dev(size_t nq_leaf, int size, int pool_log, size_t *top_scratch)
{
    /* the tree's levels follow the MN_GROUPS schedule (mn_groups_parse; default the binary
     * tree) and every level's products are piece grids over the group's plane cap (rns_dist.c mn_grid) -- their scratch per
     * device is rns_mul_dist_mn_scratch's, O(share) whatever the group size (the plane pools stay at their init size, the spills
     * are exchanged exactly).  A level of nch children of gp nodes (each holding P, Q of nq_leaf x gp limbs over gp nodes) is
     * combined by Horner from the top child (mn.c tree_level_k): the largest product is P_0 x Q_run with Q_run of (nch - 1) gp
     * nq_leaf limbs; the live shares are the child's pair, the running pair (nch > 2) and the new pair, a quarter each + 1/8 */
    size_t best = 0; if (top_scratch) *top_scratch = 0;
    dm_switches();
    int gs[32]; int L = mn_groups_parse(size, gs, 31);
    for (int l = 1; l <= L; l++) {
        int Gl = gs[l - 1], Gp = l > 1 ? gs[l - 2] : 1, g = Gl < size ? Gl : size, nch = (g + Gp - 1) / Gp; if (nch < 2) continue;
        size_t nqc = nq_leaf * (size_t)Gp + 8, na = nqc, nb = nqc * (size_t)(nch - 1), nc = na + nb;
        size_t share_child = nq_leaf + 8, share_run = (nb + g - 1) / g, share_new = (nc + g - 1) / g;
        int pieces = 0; size_t scratch = rns_mul_dist_mn_scratch(na, nb, 1, g, share_child, share_run, share_new, &pieces);
        size_t c = quarter_bytes(share_child + share_child / 8), r = nch > 2 ? quarter_bytes(share_run + share_run / 8) : 0, n = quarter_bytes(share_new + share_new / 8);
        size_t m1 = 2 * c + 2 * r + n, m2 = c + r + 2 * n, live = m1 > m2 ? m1 : m2;   /* P_i, P_run freed after P_n = P_i Q_run + P_run, before Q_n = Q_i Q_run */
        size_t tot = live + scratch; if (tot > best) best = tot; if (top_scratch) *top_scratch = scratch;   /* the top level's: the sharded division's products carry the same */
        if (getenv("ECALC_VERBOSE") && atoi(getenv("ECALC_VERBOSE")) > 1) printf("bs: tree layout: level %d group %d (%d children of %d): %zu x %zu limbs, %d pieces, scratch %.2f GB + live %.2f GB per device\n", l, g, nch, Gp, na, nb, pieces, scratch * 1e-9, live * 1e-9);
    }
    (void)pool_log;
    return best + best / 16;
}
/* B7ACCT (results/GRID_GROUPS.md §B7ACCT; STD17 4): the general map's v-exchange slots, resident per APU thread (bytes).  Every group whose transform
 * is the general map (g not a power of two, or DIST_GEN) owns a layered communicator per APU thread (rns_dist.c lay_get) whose v-slots
 * grow to its largest product's exchange (rns_mul_dist_mn_vslot) and stay until groups_finalize at the run's end -- outside the arena,
 * the plane pools and the comm pools.  So the slots of every general-map level of the tree add up, and the whole machine's group (the
 * tree's top level, then the reciprocal and the division: the same communicator) holds the larger of its tree products' and the dm
 * phase's (the division's A_h mu, n_Q x n_Q, and the reciprocal's Q_t r, as binsplit_shmem_pool_need lists them; pieces at the cap).
 * A level whose last group is cut by the size (MN_GROUPS: 512 -> 576) counts the larger of the two group shapes.  Returns the resident
 * sum from the dm phase on (the run's largest); *tree_top: the resident sum during the tree's top level; by: the per-level terms.
 * Not in the arena request nor in any allocation: the layout reports it (BS_LAYOUT_ONLY `vslot:`, MN_PLAN_ONLY `plan vslot`,
 * MEM_REPORT_DEVS); the node budget also counts it in the node budget (binsplit_node_bytes, as_room_fits,
 * ecalc.c budget_check), which can change the room / cache-fit / plane-cap decisions. */
static size_t dm_nq_of(unsigned long N)                    /* dm_layout's n_Q (Q's limbs), without the rest of the layout (no room decision) */
{
    double lg = lgamma((double)N + 1.0) / log(10.0), dl10 = 18.0;
    return (size_t)ceil(lg / dl10) + 2;
}
size_t binsplit_vslot_bytes(unsigned long N, int size, size_t *tree_top, char *by, size_t bylen)
{
    if (tree_top) *tree_top = 0;
    if (by && bylen) by[0] = 0;
    if (size < 2) return 0;
    size_t nq = dm_nq_of(N), nq_leaf = (nq + size - 1) / size, lower = 0, maxlow = 0, top_tree = 0, o = 0;
    int gs[32]; int L = mn_groups_parse(size, gs, 31);
    for (int l = 1; l <= L; l++) {
        int Gl = gs[l - 1], Gp = l > 1 ? gs[l - 2] : 1, g = Gl < size ? Gl : size, nch = (g + Gp - 1) / Gp;
        size_t nqc = nq_leaf * (size_t)Gp + 8, v = nch >= 2 ? rns_mul_dist_mn_vslot(nqc, nqc * (size_t)(nch - 1), g) : 0;   /* tree_need_dev's largest product P_0 x Q_run */
        if (g < size && size % g) { int gc = size % g, ncc = (gc + Gp - 1) / Gp; size_t vc = ncc >= 2 ? rns_mul_dist_mn_vslot(nqc, nqc * (size_t)(ncc - 1), gc) : 0; if (vc > v) v = vc; }   /* the cut last group */
        if (g == size) top_tree = v; else { lower += v; if (v > maxlow) maxlow = v; }
        if (by && v && o + 64 < bylen) o += (size_t)snprintf(by + o, bylen - o, "%slevel %d g %d %.3f GB", o ? ", " : "", l, g, v * 1e-9);
    }
    size_t a = rns_mul_dist_mn_vslot(nq, nq, size), b = rns_mul_dist_mn_vslot(nq, nq / 2 + 1, size), dm = a > b ? a : b, top = top_tree > dm ? top_tree : dm;
    if (by && dm && o + 64 < bylen) o += (size_t)snprintf(by + o, bylen - o, "%sdm g %d %.3f GB", o ? ", " : "", size, dm * 1e-9);
    if (by && !o && bylen) snprintf(by, bylen, "none (no general-map group)");
    /* S36: the levels' pairs are one pair on a device (comm_layered.c), at the largest level's size: max, not sum */
    size_t tt = top_tree > maxlow ? top_tree : maxlow, all = top > maxlow ? top : maxlow;
    if (by && o + 64 < bylen) snprintf(by + o, bylen - o, "%sS36 shared pair: max not sum (sum would be %.3f GB)", o ? ", " : "", (lower + top) * 1e-9);
    if (tree_top) *tree_top = tt;
    return all;
}
/* X2 (results/OFI_FABRIC.md item 2): the v-slots come from the comm pool (comm_sym_alloc), so the pool rule counts them and the hipMalloc'd budget does not */
/* S31: each v-slot is allocated once at the model's per-slot maximum of its group size g (env COMM_LAYER_VSLOT_MB_<g>, MiB; comm_layered.c need_vslot),
 * not regrown with every larger product -- a slot regrown in the comm pool (free, then allocate) strands holes between the short-lived staging blocks (S28: 10 nodes,
 * COMM_OFI_POOL_MB 4096 "too small" with 1432 MiB free but no 1023 MiB hole).  A hand-set COMM_LAYER_VSLOT_MB or COMM_LAYER_VSLOT_MB_<g> wins.  Called by the pool rule on every rank. */
static void vslot_prealloc_env(unsigned long N, int size)
{
    if (size < 2) return;
    const int slots = 2;                                    /* the v-exchange two deep (two deep, built in) */
    size_t nq = dm_nq_of(N), nq_leaf = (nq + size - 1) / size; int gs[32]; int L = mn_groups_parse(size, gs, 31);
    size_t mx[40]; int gg[40], n = 0;
#define VS_NOTE(g_, v_) do { size_t v2_ = (v_) / slots; int k_ = 0; while (k_ < n && gg[k_] != (g_)) k_++; if (k_ == n && n < 40) { gg[n] = (g_); mx[n++] = 0; } if (k_ < 40 && v2_ > mx[k_]) mx[k_] = v2_; } while (0)
    for (int l = 1; l <= L; l++) {
        int Gl = gs[l - 1], Gp = l > 1 ? gs[l - 2] : 1, g = Gl < size ? Gl : size, nch = (g + Gp - 1) / Gp;
        size_t nqc = nq_leaf * (size_t)Gp + 8, v = nch >= 2 ? rns_mul_dist_mn_vslot(nqc, nqc * (size_t)(nch - 1), g) : 0;
        if (v) VS_NOTE(g, v);
        if (g < size && size % g) { int gc = size % g, ncc = (gc + Gp - 1) / Gp; size_t vc = ncc >= 2 ? rns_mul_dist_mn_vslot(nqc, nqc * (size_t)(ncc - 1), gc) : 0; if (vc) VS_NOTE(gc, vc); }
    }
    { size_t a = rns_mul_dist_mn_vslot(nq, nq, size), b = rns_mul_dist_mn_vslot(nq, nq / 2 + 1, size); if (a || b) VS_NOTE(size, a > b ? a : b); }
    { size_t m = 0; for (int k = 0; k < n; k++) if (mx[k] > m) m = mx[k]; for (int k = 0; k < n; k++) mx[k] = m; }   /* S36: the pair is shared, allocated once at the overall maximum (every group size the same) */
    for (int k = 0; k < n; k++) { char nm[48], v[32]; snprintf(nm, sizeof nm, "COMM_LAYER_VSLOT_MB_%d", gg[k]); snprintf(v, sizeof v, "%zu", (mx[k] >> 20) + 1); if (!getenv(nm)) bs_setenv(nm, v, 1);
        { const char *er = getenv("COMM_RANK"), *ve = getenv("ECALC_VERBOSE"); if (getenv("MN_PLAN_ONLY") || ((!er || atoi(er) == 0) && ve && atoi(ve) >= 2)) printf("plan vslot-prealloc  %s=%s MiB per slot (S31: each v-slot of a %d-node group allocated once)\n", nm, getenv(nm), gg[k]); } }
#undef VS_NOTE
}
int binsplit_vslot_budget_on(void) { return 1; }   /* always 1; kept for the `vslot:` line's budget field */
/* BS_ARENA_ROOM=<f> (default 0.14, Q99; 0 = off; f > 0 with the VMM pool) -- the division's room.  With
 * BS_SEED_FILL=128 the dm phase's large blocks (the division's t and xq, 2 n_Q quarters) missed a contiguous extent of the arenas at
 * the dm need and the VMM pool remapped them (15 remaps, 9.5 s at 1e11 on B1, measured): (1) every VMM arena is laid out in whole
 * chunks (DB_POOL_VMM_CHUNK_GB) -- the chunks are mapped whole anyway, so this costs no memory, and the reserved tail then ends
 * where the arena's last extent ends (a remainder after it made the pool carve spills from the tail's front: the reciprocal's two
 * remaps); (2) the dm need gains f x the hole (the division's largest block), so the blocks that spill around the tail leave it
 * whole.  f from the pool replays of measured traces (archive:scripts_2026-10-23/as_pool_replay.py). */
static double bs_arena_room(void) { static double f = -1; if (f < 0) { const char *e = getenv("BS_ARENA_ROOM"); f = e ? atof(e) : 0.14; if (f < 0) f = 0; } return f; }   /* default 0.14 (memfix; 0.16 until then) */
/* BS_ARENA_TRIM_GB=<GB per APU> (0 = off, the default) -- trim the dm need by this much (before the arenas are rounded to chunks): the model's
 * need sits above the measured live peak (2 n 1.2882e11: arena 66.57 GB, peak 55.47 GB) and every chunk is mapped (the extra by the background
 * mapper, which slows the GPU while it runs).  dm_layout clamps the trim so the need stays at the largest live set max(v2, v3) (and the
 * tree's, via want); the decision is printed once on rank 0.  Experiment: the remaps (DB_POOL_VERBOSE=1) say whether the cut is safe. */
static double bs_arena_trim_gb(void) { static double f = -1; if (f < 0) { const char *e = getenv("BS_ARENA_TRIM_GB"); f = e ? atof(e) : 0; if (!(f > 0)) f = 0; if (f > 1e4) f = 1e4; }   /* nan, inf, negative, huge: capped (memfix) */ return f; }
static size_t as_arena(size_t bytes)                  /* an arena's bytes as BS_ARENA_ROOM lays it out: whole VMM chunks */
{
    if (!(bs_arena_room() > 0) || !db_pool_vmm_on()) return bytes;
    size_t C = db_pool_vmm_chunk(); return (bytes + C - 1) / C * C;
}
/* The room is time, the budget is not: the room is dropped (the arenas stay in whole chunks, which costs nothing) when the node with it
 * would exceed ECALC_NODE_GB (480).  the node is counted as mem_model.py's node peak
 * counts it (mem_per_node: with the room the bs phase binds), so the room is dropped exactly when that node would go over:
 *   device = the plane pools at this run's cap and prime count (ECALC_NP=auto: pool 0 by the group, as binsplit_node_bytes) + the arena
 *            with the room + the VMM pool's growth in the bs phase (AS_VMM_BS_GROW = mem_model VMM_BS_GROW, 6.0 GB measured);
 *   host   = size 1: the measured host HWM (mem_model host_size1_vmm: 25.85 GB + 0.0208 GB per 10^9 digits above 4e10, the digits taken
 *            as log10 N!); size > 1: the larger of init -- the runtime 7.0 GB + the checkpoint staging 4 GiB + the two VMM seed buffers
 *            (seeds_stream's: as_seedbuf) + the transport 6.0 GB + the xshmem pool (binsplit_shmem_pool_rule's pool) --
 *            and the division's (the runtime, the staging, the writer 0.65 GB, the transport, the pool, MN_OUT_EARLY's 0.9 GB).
 * Before DL the host was 7.0 GB + 8 GiB (+ 6 GB at size > 1) + a 10 GB margin (AS_HOST_MARGIN), 18.8 GB below the node at the target,
 * over three-prime planes under ECALC_NP=auto (MS15 1.3): the room stayed up to ~5.63e13 on 576 with the node at 480.5-497.7 GB.  The
 * decision is made once per (N, size) (layout_arena simulates the levels) and printed once (BS_LAYOUT_ONLY: a `room:` line per point). */
#define AS_VMM_BS_GROW ((size_t)6000000000)
static size_t layout_arena(unsigned long N, int g, size_t *bs2_);
static __thread int g_room_probe;                              /* 1: dm_layout keeps the room without deciding (the probe's own layout_arena) */
static __thread int g_fit_probe;                               /* FIT: 1 while binsplit_fit lays out an arm (the room's drop message is not printed) */
static __thread size_t g_room_planes, g_room_arena, g_room_host, g_room_pool, g_room_seedbuf, g_room_ofi;   /* the terms of the last decision (the layout's `room:` line: mem_model.py --check-c decides alike) */
static __thread int g_room_fit = -1;
static size_t as_seedbuf(unsigned long N)             /* seeds_stream's two pinned seed buffers (bytes): min(BS_SEED_CHUNK_MB, the largest region's spans) in whole spans */
{
    size_t per; unsigned long nspan; seed_limbs(N, &per, &nspan);
    size_t r0[NRMAX + 1]; for (int r = 0; r <= NR; r++) { r0[r] = 0; while (r0[r] < nspan && region_of(r0[r], nspan) < r) r0[r]++; }
    size_t mb = getenv("BS_SEED_CHUNK_MB") ? (size_t)atol(getenv("BS_SEED_CHUNK_MB")) : (db_pool_vmm_on() ? 8192 : 2048), bytes = mb << 20, span_bytes = 2 * per * 8, mx = 0;
    for (int r = 0; r < NR; r++) { size_t b = span_bytes * (r0[r + 1] - r0[r]); if (b > mx) mx = b; }
    if (bytes > mx) bytes = mx;
    if (bytes < span_bytes) bytes = span_bytes;
    return 2 * (bytes / span_bytes * span_bytes);
}
size_t binsplit_shmem_pool_need(unsigned long N, int size, char *by, size_t bylen);
static int xsh_on(void) { const char *t = getenv("COMM_TRANSPORT"); return t && !strcmp(t, "xshmem"); }   /* COMM_TRANSPORT=xshmem (make XSHMEM=ofi): the pool rule's model: one DATA heap per device (XSHMEM_HEAP_MB = the OFI device pool law), rounds from XSHMEM_ROUND_MB */
static __thread size_t g_ofi_dev_mb;                           /* the last need's device pool per device (MiB; 0 = no need computed) */
static size_t ofi_pool_mb(void)                       /* the device pool per device this run will have (MiB): COMM_OFI_POOL_MB, else the need */
{
    const char *e = getenv("COMM_OFI_POOL_MB");
    return g_ofi_dev_mb ? (e ? (size_t)atol(e) : g_ofi_dev_mb) : 0;
}
/* the pool this run will have (bytes; binsplit_shmem_pool_rule's rule), 0 without the transport; *ofi: the four device pools
 * (the device staging lives there; the host pool keeps only control, rings and host ops) */
static size_t as_shmem_pool(unsigned long N, int size, size_t *ofi)
{
    if (ofi) *ofi = 0;
    if (size < 2 || !xsh_on()) return 0;
    size_t need = binsplit_shmem_pool_need(N, size, 0, 0), mb = (need + ((size_t)1 << 20) - 1) >> 20; mb = (mb + 255) / 256 * 256;
    size_t have = getenv("COMM_SHMEM_POOL_MB") ? (size_t)atol(getenv("COMM_SHMEM_POOL_MB")) : g_ofi_dev_mb ? mb : 8192;   /* OFIMEM: unset under OFI = the need (the rule sets it) */
    if (ofi) *ofi = (size_t)bs_nrn() * (ofi_pool_mb() << 20);
    return (mb > have ? mb : have) << 20;   /* the pool rule always raises the pool to the need */
}
/* the host term of the node (bytes) as the room decision counts it (FIT: also the fit report's host; see the comment above) */
static size_t as_host_bytes(unsigned long N, int size, size_t pool, size_t ofi, size_t *sb_)
{
    size_t sb = size > 1 ? as_seedbuf(N) * (size_t)bs_lanes() : 0, host;
    if (size == 1) { double D = lgamma((double)N + 1.0) / log(10.0) / 1e9 - 40.0; host = (size_t)((25.85 + 0.0208 * (D > 0 ? D : 0.0)) * 1e9) + (bs_ppn_proc() - 1) * (size_t)7000000000; }   /* S2c: + a runtime per further process */
    else { size_t base = bs_ppn_proc() * ((size_t)7000000000 + (size_t)6000000000) + bs_staging() + pool + ofi;   /* OFIMEM: + the device pools */
           int early = getenv("MN_OUT_EARLY") ? atoi(getenv("MN_OUT_EARLY")) != 0 : 1;
           size_t hi = base + sb, hd = base + (size_t)bs_lanes() * ((size_t)650000000 + (early ? (size_t)900000000 : 0)); host = hi > hd ? hi : hd; }
    if (sb_) *sb_ = sb;
    return host;
}
static int dm_wrap_req(void);
static __thread unsigned long g_room_cN; static int g_room_cfit = -1;   /* as_room_fits' cache (FIT: file scope, keyed also by the term range and the wrap) */
static int as_room_fits(unsigned long N, int size, size_t room)
{
    static __thread int cs, cpl = -1, cw = -1; static __thread unsigned long ca0, cb1; int pl = rns_pool_log() > 0 ? rns_pool_log() : 31, w = dm_wrap_req();
    if (g_room_cfit >= 0 && g_room_cN == N && cs == size && cpl == pl && cw == w && ca0 == bs_a0 && cb1 == bs_b1) return g_room_cfit;   /* FIT: the fit lays out both wraps on rank 0's range */
    int np = ec_np_init(); if (np == 3 && ec_np_auto) np = ec_np_planes(pl, size);   /* DL: pool 0's planes under ECALC_NP=auto (as binsplit_node_bytes) */
    size_t planes = (size_t)bs_nrn() * rns_plane_pool_bytes(pl, np, 0, 0) + ((size_t)610000000);   /* BS_TABLES_BYTES */
    size_t ofi = 0; g_room_probe = 1; size_t ar = layout_arena(N, size, 0), pool = as_shmem_pool(N, size, &ofi); g_room_probe = 0;
    size_t sb, host = as_host_bytes(N, size, pool, ofi, &sb);
    size_t node = planes + ar + AS_VMM_BS_GROW + host;
    double budget = (getenv("ECALC_NODE_GB") ? atof(getenv("ECALC_NODE_GB")) : 480.0) * 1e9;
    g_room_cN = N; cs = size; cpl = pl; cw = w; ca0 = bs_a0; cb1 = bs_b1; g_room_cfit = (double)node <= budget;
    g_room_planes = planes; g_room_arena = ar; g_room_host = host; g_room_pool = pool; g_room_seedbuf = sb; g_room_ofi = ofi; g_room_fit = g_room_cfit;
    const char *er = getenv("COMM_RANK");
    if (!g_room_cfit && (!er || atoi(er) == 0) && !getenv("BS_LAYOUT_ONLY") && !g_fit_probe)
        printf("bs: BS_ARENA_ROOM: the room (%.2f GB per APU) is dropped -- the node with it, %.1f GB (planes %.1f + arena %.1f + the bs growth %.1f + host %.1f), exceeds ECALC_NODE_GB %.0f; the arenas stay in whole chunks\n",
               room * 1e-9, node * 1e-9, planes * 1e-9, ar * 1e-9, AS_VMM_BS_GROW * 1e-9, host * 1e-9, budget * 1e-9);
    return g_room_cfit;
}
/* D19 (results/LAUNCH_DEFAULTS.md §DEF; mem_model.py wrap_bytes): DM_MN_WRAP's extra bytes per device in the mn division's low-product moment
 * (newton_db.c mn_wrap_step) -- Q B^-h folded (Qpp, basis P + 4, held over both steps) and the folded A' (T1, basis P + 4) beside
 * the assembly's Rc + Tt (basis w each, where Aw + xql were): 2 quarters of P + 4 limbs; 0 when the wrap is off or not used
 * (mn_wrap_plane's rule: P = 2^cap or 2^k in [2^logmin, 2^cap]; w = n_Q + 2 >= P + 2; at 1 not when X_hi Q fits one plane) */
static size_t dm_wrap_bytes(size_t nq, size_t k, int size)
{
    int v = dm_wrap_req();   /* FIT (D26): DM_MN_WRAP if set, else the run's automatic decision (binsplit_fit), else 1 (D22) */
    if (v <= 0 || size < 2) return 0;
    int cap = rns_mul_dist_mn_logcap_g(size), lmin = rns_mul_dist_mn_logmin_g(size), lg = v == 1 ? cap : v;
    if (lg < lmin || lg > cap || lg > 62) return 0;
    size_t P = (size_t)1 << lg, w = nq + 2;
    if (w < P + 2) return 0;
    if (v == 1 && (k - k / 2 + 1) + nq <= ((size_t)1 << cap)) return 0;
    return 2 * quarter_bytes((P + 4 + size - 1) / size);
}
static void dm_layout(unsigned long N, int size, struct dm_layout *L)
{
    double lg = lgamma((double)N + 1.0) / log(10.0);                          /* log10 N! = log10 Q */
    double dl10 = 18.0;                                                     /* digits per limb */
    size_t nq = (size_t)ceil(lg / dl10) + 2, dl = (size_t)ceil((lg - 50.0) / dl10) + 1;   /* Q's limbs; the limbs of 10^d for the run's d (the driver: d <= log10 N! - 50) */
    size_t k = nq + 1 + dl - nq + 2 + 1, tcap = (nq + k > 2 * k ? nq + k : 2 * k) + 8;   /* k_mu with P.n = n_Q + 1 (P > Q) */
    int pl = rns_pool_log() > 0 ? rns_pool_log() : 31;
    dm_switches(); int tight = dm_tight > 0;
    /* the last doubling starts at jl = ceil(k/2) (newton_chain_next's anchored targets k, ceil(k/2), ...).  Its first product Q_t r is take + (jl + 1)
     * limbs (r has j + 1 limbs), reserved + 8 by the grid: the largest block of the tight schedule, i.e. the hole */
    /* the DKM division (built in since Batch 3) -- the reciprocal runs to h = floor(k/2) + 1
     * (its last doubling at ceil(h/2)) and the division forms two quotient halves (newton_db.c divmod_shifted_dkm / mn_divmod_dkm): the
     * high products t = A_h mu_1 of ~k limbs instead of 2k + 8, the low products X_hi Q / X_lo Q over w.  So the hole is DKM's largest block
     * (below) and the division's set is DKM's.  The division takes DKM at every size; the layout counts DKM's set at size > 1 and, at
     * size 1, where the top level runs on the device tier -- n_Q above 2^BS_MDEV_LOGL, by 1/8 for the bound; below it the full-length
     * set (larger, never short: the sizes where memory is plentiful) */
    int logl = getenv("BS_MDEV_LOGL") ? atoi(getenv("BS_MDEV_LOGL")) : bs_mdev_logl;
    int dkm = k >= 5 && (size > 1 || (logl < 62 && nq >= ((size_t)1 << logl) + ((size_t)1 << logl) / 8));
    size_t kr = dkm ? newton_dkm_h(k) : k, k1 = k - k / 2;                    /* (k1: step 1's quotient limbs, s = floor(k/2) <= dl) */
    int lean = dkm && size > 1;                                               /* the lean mn division set (size > 1 only) */
    size_t jl = (kr + 1) / 2 + 1;                 /* the chain guard (RECIP): the last doubling starts at ceil(kr/2) + 1 */
    size_t take = 2 * jl + 2 < nq ? 2 * jl + 2 : nq, t1a = take + (jl + 2) + 8;   /* r has j + 2 limbs (measured at 4e10, job 21131: r 555555559 at j 555555557) */
    size_t nq_s = (nq + size - 1) / size, k_s = (k + size - 1) / size, jl_s = (jl + size - 1) / size;
    /* the hole is the DIVISION's largest block, t = A_h mu of 2k + 8 limbs (xq = low(X Q)
     * is n_Q + k + 8 <= it), and during the reciprocal it holds r (jl + 4) and t1 (Q_t r at jl) together -- r lands in its front because
     * the space outside it is Q, S, r2 and the piece's; so the tail is whole again for t, and S's slot + the tail's rest hold xq once
     * Aw is formed and S freed before the low product (newton_db_divmod_shifted).  Without this the tight reciprocal left t1 and r
     * apart and t (2 n_Q) or Aw (1 n_Q) fell back to hipMalloc at 4e10 and 1e11 */
    size_t hole_t = quarter_bytes((2 * k + 8 + size - 1) / size), hole_r = quarter_bytes((t1a + size - 1) / size) + quarter_bytes(jl_s + 4);
    if (tight && dkm) {                                                        /* DL: DKM's largest block -- size 1: step 1's xq = X_hi Q, reserved whole by
                                                                                * mul_grid (k1 + n_Q + 8, ~1.5 n_Q) before it is cut to w; size > 1: t (~k; the
                                                                                * mn low products are delivered in basis w = n_Q + 2 <= it) */
        tcap = size == 1 ? nq + k1 + 8 : k + 8; hole_t = quarter_bytes((tcap + size - 1) / size); L->hole = hole_t > hole_r ? hole_t : hole_r; }
    else if (tight) { tcap = 2 * k + 8; L->hole = hole_t > hole_r ? hole_t : hole_r; }
    else { size_t tcap_s = (tcap + size - 1) / size; L->hole = quarter_bytes(tcap_s); if (size == 1) L->hole = quarter_bytes(tcap); }
    L->hole += L->hole / 64;
    L->nq = nq; L->k = k; L->tcap = tcap; L->jl = jl; L->tight = tight; L->dkm = dkm; L->lean = lean;
    L->thresh = L->hole - L->hole * 3 / 8;
    size_t piece = ((size_t)1 << pl) + 8; if (piece > nq_s + k_s + 16) piece = nq_s + k_s + 16;
    size_t qp = quarter_bytes(nq_s + nq_s / 10 + 8);                          /* Q, or S = P + Q, with the bound's margin */
    if (!tight) L->v2 = 2 * qp + 2 * quarter_bytes(k_s + 4) + L->hole + quarter_bytes(piece);
    else L->v2 = 2 * qp + quarter_bytes(2 * jl_s + 4) + L->hole + quarter_bytes(piece);   /* E2: Q, S, r2 (d and r' share it: db_pow_sub writes 2j + 1 limbs), the hole (r + t1), a piece */
    /* the division's own set -- S, Q, mu (k + 1) and t = A_h mu (2k + 8), or (tight: S freed after the window Aw is formed)
     * Q, X (k), Aw (n_Q + 2) and the low product xq (n_Q + k + 8), a piece beside them.  Below the reciprocal's set today (5.2 n_Q
     * against 6.2), it binds once the reciprocal is tight or P is spilled */
    L->div = quarter_bytes(piece);
    { size_t hi = 2 * qp + quarter_bytes(k_s + 1) + quarter_bytes(2 * k_s + 8), lo = 2 * qp + quarter_bytes(k_s) + quarter_bytes(nq_s + k_s + 8);
      if (tight) lo = qp + quarter_bytes(k_s) + quarter_bytes(nq_s + 2) + quarter_bytes(nq_s + k_s + 8);
      if (dkm) {                                                               /* DL: DKM's moments (mem_model.dm_layout dkm) -- step 1's high product Q + S + mu (h) + t (k);
                                                                                * step 2's Q + X_hi + R1 (n_Q) + mu + t; step 2's low product Q + X + Aw + xq (n_Q + h) + X_lo */
          size_t h_s = (k / 2 + 1 + size - 1) / size, h1 = 2 * qp + quarter_bytes(h_s + 1) + quarter_bytes(k_s + 8),
                 h2 = qp + quarter_bytes(h_s) + quarter_bytes(nq_s + 2) + quarter_bytes(h_s + 1) + quarter_bytes(k_s + 8);
          hi = h1 > h2 ? h1 : h2;
          lo = qp + quarter_bytes(k_s) + quarter_bytes(nq_s + 2) + quarter_bytes(nq_s + h_s + 8) + quarter_bytes(h_s);
          if (lean) lo = 3 * quarter_bytes(nq_s + 2) + quarter_bytes(k_s + 1) + quarter_bytes(h_s + 1); }   /* lean: step 2's low product with Qw in place of Q and
                                                                                                             * the low product in basis w -- Qw + Aw + xql (w each) + X (k, the X0 form; X_hi under MN_OUT_DKM_HI) + X_lo (h) */
      L->wrap = lean ? dm_wrap_bytes(nq, k, size) : 0; lo += L->wrap;   /* D19: DM_MN_WRAP's Qpp + T1 (mn division only) */
      L->div += hi > lo ? hi : lo; }
    if (L->div > L->v2) L->v2 = L->div;
    L->v2 += L->v2 / 8 < ((size_t)1 << 30) ? L->v2 / 8 : ((size_t)1 << 30);   /* slack for the odd small block (C3's 1 GiB at the large sizes) */
    L->need_dev = L->v2;
    /* v3: the device top levels of bs must fit beside the tail, or their P, Q (alive into dm) spill into it and t1 finds it broken
     * (1e11 with v2: two APUs fell back by 22.2 GB with the tail's 30 GB free in two extents): the last level's inputs (2 P, 2 Q of
     * half n_Q) and outputs (P, Q of n_Q, with the bound's margin) + 1/8 for the fit, plus the hole.  DM_TIGHT frees
     * P1, P2 after P = P1 Q2 + P2, so Q = Q1 Q2 runs beside 2 inputs and 2 outputs: max of the two moments */
    { size_t in = quarter_bytes(nq_s / 2 + nq_s / 20 + 8), out = qp, top;
      if (!tight) top = 4 * in + 2 * out; else { size_t m1 = 4 * in + out, m2 = 2 * in + 2 * out; top = m1 > m2 ? m1 : m2; }
      top += top / 8 + L->hole; L->v3 = top;
      if (top > L->need_dev) L->need_dev = top; }
    size_t top_scratch = 0; L->tree_dev = size > 1 ? tree_need_dev(nq_s, size, pl, &top_scratch) : 0;
    L->need_dev += top_scratch;
    L->room = bs_arena_room() > 0 && db_pool_vmm_on() ? (size_t)(bs_arena_room() * (double)L->hole) : 0;
    if (L->room && !g_room_probe && !as_room_fits(N, size, L->room)) L->room = 0;                              /* (over the node budget: dropped) */
    L->need_dev += L->room;
    if (bs_arena_trim_gb() > 0) {                                              /* BS_ARENA_TRIM_GB: never below the largest live set (mem_model.arena_trim is the twin) */
        size_t want_t = (size_t)(bs_arena_trim_gb() * 1e9), floor_ = L->v2 > L->v3 ? L->v2 : L->v3, room_ = L->need_dev > floor_ ? L->need_dev - floor_ : 0;
        L->trim = want_t < room_ ? want_t : room_; L->need_dev -= L->trim;
        static int said; const char *er = getenv("COMM_RANK");
        if (!g_room_probe && !said && (!er || atoi(er) == 0)) { said = 1;
            printf("bs: BS_ARENA_TRIM_GB: dm need %.2f -> %.2f GB per APU (trim %.2f of the %.2f asked; the largest live set %.2f GB%s)\n", (L->need_dev + L->trim) * 1e-9, L->need_dev * 1e-9,
                   L->trim * 1e-9, bs_arena_trim_gb(), floor_ * 1e-9, L->trim < want_t ? "; clamped" : ""); fflush(stdout); } }
}
/* the xshmem transport's pool this run needs per node-process, in bytes (mem_model.py
 * shmem_pool is the same law): 4 APU threads x the largest staged exchange's send + receive over the run's products (the tree's
 * levels as tree_need_dev forms them; the division's A_h mu -- nq x nq, C of 2 nq limbs, the largest -- and the reciprocal's last
 * Q_t r; mdb_shift at MDB_SHIFT_CHUNK_MB) + the communicators' control blocks (4 per level of size S < size, 4 meshes of size:
 * 8 words per member) + the mailbox + a 256 MiB margin.
 * by: the product that sets the staging. */
/* limbs of the last `nodes` of `size` nodes' term ranges of the series (N terms, equal term counts per node; Q's limbs nq in all): lgamma weights -- later terms are longer, so the last node's
 * share is above the average nq / size (bs_pe_b1's ranges) */
static size_t bs_tail_limbs(unsigned long N, size_t nq, int size, size_t nodes)
{
    if (nodes >= (size_t)size) return nq;
    double t0 = 1.0 + (double)(((unsigned __int128)(size - nodes) * N) / (unsigned)size), tot = lgamma((double)N + 1.0), part = tot - lgamma(t0);
    return (size_t)((double)nq * part / tot) + 1;
}
size_t binsplit_shmem_pool_need(unsigned long N, int size, char *by, size_t bylen)
{
    if (size < 2) return 0;
    struct dm_layout L; memset(&L, 0, sizeof L); dm_layout(N, size, &L);
    size_t nq = L.nq, nq_leaf = (nq + size - 1) / size, best = 0, qmax = 0, q; const char *who = "-"; static __thread char lvl[48];
    int gs[32]; int nl = mn_groups_parse(size, gs, 31), prev = 1;
    for (int l = 0; l < nl; l++) {                         /* the tree's levels: nch children of prev nodes; the largest product P_0 x Q_run */
        int S = gs[l], nch = (S + prev - 1) / prev;
        if (nch >= 2) {
            size_t nqc = nq_leaf * (size_t)prev + 8, na = nqc, nb = nqc * (size_t)(nch - 1), nc = na + nb;
            size_t st = rns_mul_dist_mn_stage(na, nb, S, nq_leaf + 8, (nb + S - 1) / S, (nc + S - 1) / S, &q); if (q > qmax) qmax = q;
            if (st > best) { best = st; snprintf(lvl, sizeof lvl, "tree level of %d", S); who = lvl; }
            if (xsh_on()) {   /* the level's operand exchanges (mn.c tree_level / tree_level_k -> comm_xshmem.c xex_post).  A whole exchange (no rounds: one process per GPU,
                 * XSHMEM_ROUND_MB 0) stages both sides outside the heap at once: sst = the PE's part of the operand, rst = its part of the operand's sequence.
                 * A child's P (prev nodes) is cut over its prev x NL PEs: a sending PE stages s = (the child's limbs) / (prev NL); the product's group has S nodes x NL PEs, so the
                 * operand's sequence is cut over S NL and a receiver stages (the child's limbs) / (S NL).  The child's limbs are the largest child's (the last one: terms grow, node shares are not
                 * uniform: bs_tail_limbs from lgamma).  The running Q_c (c children, group-sharded from c = 2) stages 2 x its limbs / (S NL).  Measured 3 n 1.93e11: sst 6912 + rst 2304 MiB (s = 6912, prev / S = 1/3); at 2 n 1e11 4965 + 1241 (the bound 1.5 s is above it) */
                const char *er = xshmem_round_env(); size_t Rl = er && atof(er) > 0 ? (size_t)(atof(er) * 1048576.0) / 8 : 0;
                size_t NLp = (size_t)bs_nrn(), SNL = (size_t)S * NLp, tl = bs_tail_limbs(N, nq, size, (size_t)prev), tl1 = tl > nq_leaf * (size_t)prev ? tl : nq_leaf * (size_t)prev;   /* (the last child: the largest) */
                size_t sA = (tl1 / (size_t)prev + 8 + NLp - 1) / NLp, rA = (tl1 + 8 + SNL - 1) / SNL, sx = sA + rA;
                for (int c = 2; c < nch; c++) { size_t lc = bs_tail_limbs(N, nq, size, (size_t)c * (size_t)prev), sq = 2 * ((lc + 8 + SNL - 1) / SNL); if (sq > sx) sx = sq; }
                if (!Rl && sx > best) { best = sx; snprintf(lvl, sizeof lvl, "tree level of %d (operand exchange)", S); who = lvl; }
            }
        }
        prev = S;
    }
    size_t sh = (nq + size - 1) / size;
    { size_t st = rns_mul_dist_mn_stage(nq, nq, size, sh, sh, (2 * nq + size - 1) / size, &q); if (q > qmax) qmax = q; if (st > best) { best = st; who = "the division's A_h mu"; } }
    { size_t st = rns_mul_dist_mn_stage(nq, nq / 2 + 1, size, sh, (nq / 2 + size - 1) / size, (3 * nq / 2 + size - 1) / size, &q); if (q > qmax) qmax = q; if (st > best) { best = st; who = "the reciprocal's Q_t r"; } }
    { const char *e = getenv("MDB_SHIFT_CHUNK_MB"); size_t ch = (size_t)((e ? atof(e) : 1024.0) * 1048576.0 / 8), so = (2 * nq + size - 1) / size / 4, si = sh / 4;   /* the division's mdb_shift: t (2 nq) >> (k + 1) -> X (nq) */
      size_t sl = so < si ? so : si, K = ch && sl > ch ? (sl + ch - 1) / ch : 1, st = (so + si + K - 1) / K;   /* newton_db.c: K rounds from the shorter side's part */
      const char *er = (xsh_on() ? xshmem_round_env() : NULL); size_t Rl = er && atof(er) > 0 ? (size_t)(atof(er) * 1048576.0) / 8 : 0;   /* the transport's rounds bound each side */
      if (Rl) { size_t a = (so + K - 1) / K / size * (size - 1), b = (si + K - 1) / K / size * (size - 1); st = (a > Rl ? Rl : a) + (b > Rl ? Rl : b); }   /* (the peers' part: no self slab) */
      if (st > best) { best = st; who = "the division's mdb_shift"; } }
    size_t ring = (size_t)256 << 10, members = (size_t)size;      /* the device staging and the symmetric slabs are in the device pools */
    for (int l = 0; l < nl; l++) if (gs[l] < size) members += (size_t)gs[l];
    size_t control = (size_t)bs_nrn() * members * (ring + 64), mailbox = (size_t)1024 * size * 8;
    size_t vsp = binsplit_vslot_bytes(N, size, 0, 0, 0);   /* X2: the v-slots per APU thread, resident to the run's end */
    g_ofi_dev_mb = 0;
    {   /* a pool per device: one APU's staging (at least the self-tests' 128 MiB) + its slabs + a 256 MiB margin, in whole 256 MiB */
        size_t stg = best * 8 > ((size_t)128 << 20) ? best * 8 : ((size_t)128 << 20);
        size_t dv = (stg > vsp ? stg : vsp) + ((size_t)256 << 20);
        if (vpe_ppg()) {   /* one process per GPU: first-fit fragmentation of the DATA heap (comm_xshmem.c xalloc) scales with the staged blocks (1/16 of them, at least the 256 MiB); the heap's other symmetric blocks are
                            * the allgather scratch per communicator, (2 n + 1) x 4 KiB with n = S x NL PEs (the control words are in the CTRL heap, not here), + 1 MiB of alignment */
            size_t mg = stg / 16 > ((size_t)256 << 20) ? stg / 16 : ((size_t)256 << 20), gb = 0;
            gb += ((size_t)2 * (size_t)size * (size_t)bs_nrn() + 1) * 4096; for (int l = 0; l < nl; l++) if (gs[l] < size) gb += ((size_t)2 * (size_t)gs[l] * (size_t)bs_nrn() + 1) * 4096;
            dv = (stg > vsp ? stg : vsp) + mg + gb + ((size_t)1 << 20); }
        g_ofi_dev_mb = ((dv + ((size_t)1 << 20) - 1) >> 20); g_ofi_dev_mb = (g_ofi_dev_mb + 255) / 256 * 256;
    }
    if (by) snprintf(by, bylen, "SHMEM staging 128 MiB (host ops), control %.1f MiB; the device staging in 4 device pools of %zu MiB = %.1f MiB by %s + margin%s", control / 1048576.0, g_ofi_dev_mb, best * 8 / 1048576.0, who, "");
    size_t stg0 = (size_t)128 << 20;
    size_t need = stg0 + control + mailbox + ((size_t)256 << 20);
    return need;
}
/* the pool rule before the transport starts (ecalc.c, after rns_init, before mn_init; plan = MN_PLAN_ONLY's line).
 * It sets COMM_SHMEM_POOL_MB to the model's need when that exceeds the pool asked for (8192 by default).  Returns the need in MiB
 * (0: not a multi-node run).  mnrun.sh sizes the heap (XSHMEM_HEAP_MB, and for the Cray backend the libsma heap) from MN_PLAN_ONLY's `plan pool` line. */
size_t binsplit_shmem_pool_rule(unsigned long N, int size, int plan)
{
    if (size < 2 && vpe_ppg()) size = vpe_lanes();   /* S2fix: one node under ECALC_PE_PER_GPU=1 still exchanges among its PEs (the lanes): the model of that group, so the plan prints `plan pool` and mnrun sizes the heap (the default path: size < 2 = 0, as before); memfix: under any transport (before, only xshmem: plain shmem got no `plan pool` line and kept the 8192 MiB default) */
    if (size < 2 || (!plan && !xsh_on())) return 0;
    vslot_prealloc_env(N, size);                          /* S31 */
    char by[256]; unsigned long ra0 = bs_a0, rb1 = bs_b1; int rset = vpe_on() && !rb1 && size > 1;
    if (rset) { bs_a0 = 1; bs_b1 = bs_pe_b1(N, size); }   /* S2fix: a vPE / one-process-per-GPU run has no term range here yet (vpe_set_range comes later): the model of PE 0's, as the fit lays it out -- with the whole range the room probe saw a 1726 GB arena and printed a bogus `BS_ARENA_ROOM ... is dropped` */
    size_t need = binsplit_shmem_pool_need(N, size, by, sizeof by), mb = (need + ((size_t)1 << 20) - 1) >> 20;
    if (rset) { bs_a0 = ra0; bs_b1 = rb1; }
    mb = (mb + 255) / 256 * 256;                          /* whole 256 MiB */
    size_t have = getenv("COMM_SHMEM_POOL_MB") ? (size_t)atol(getenv("COMM_SHMEM_POOL_MB")) : g_ofi_dev_mb ? mb : 8192;   /* OFIMEM: unset = the need */
    const char *er = getenv("COMM_RANK"); int rank0 = !er || atoi(er) == 0;
    if (plan) { printf("plan pool   the SHMEM pool per node-process: COMM_SHMEM_POOL_MB=%zu (%s); the SHMEM heap (SHMEM_SYMMETRIC_SIZE / SHMEM_SYMMETRIC_HEAP_SIZE) >= %zu MiB", mb, by, mb + 512);
                if (g_ofi_dev_mb) printf("; the device pools 4 x COMM_OFI_POOL_MB=%zu", ofi_pool_mb());
                if (g_ofi_dev_mb && xsh_on()) printf("; XSHMEM_HEAP_MB=%zu", getenv("XSHMEM_HEAP_MB") ? (size_t)atol(getenv("XSHMEM_HEAP_MB")) : ofi_pool_mb());
                printf("\n"); return mb; }
    if (g_ofi_dev_mb) {   /* the device pools sized by the rule */
        if (!getenv("COMM_SHMEM_POOL_MB")) { char v[32]; snprintf(v, sizeof v, "%zu", mb); bs_setenv("COMM_SHMEM_POOL_MB", v, 1); have = mb; }
        if (!getenv("COMM_OFI_POOL_MB")) { char v[32]; snprintf(v, sizeof v, "%zu", g_ofi_dev_mb); bs_setenv("COMM_OFI_POOL_MB", v, 1); }
        else if ((size_t)atol(getenv("COMM_OFI_POOL_MB")) < g_ofi_dev_mb && rank0) fprintf(stderr, "device pool: WARNING: COMM_OFI_POOL_MB=%s is below the modelled need of this run, %zu MiB per device\n", getenv("COMM_OFI_POOL_MB"), g_ofi_dev_mb);
        if (xsh_on() && !getenv("XSHMEM_HEAP_MB")) { char v[32]; snprintf(v, sizeof v, "%zu", ofi_pool_mb()); bs_setenv("XSHMEM_HEAP_MB", v, 1); }   /* the xshmem DATA heap per loopback PE = the device pool law */
        if (rank0) printf("pool: the SHMEM pool %zu MiB (need %zu MiB) + 4 device pools of COMM_OFI_POOL_MB=%zu (%s)\n", mb > have ? mb : have, mb, ofi_pool_mb(), by);
    }
    if (mb > have) {
        char v[32]; snprintf(v, sizeof v, "%zu", mb); bs_setenv("COMM_SHMEM_POOL_MB", v, 1);
        if (rank0) printf("pool: COMM_SHMEM_POOL_AUTO=1 -- COMM_SHMEM_POOL_MB %zu -> %zu (%s)\n", have, mb, by);
    } else if (rank0 && getenv("ECALC_VERBOSE") && atoi(getenv("ECALC_VERBOSE")) >= 2) printf("pool: COMM_SHMEM_POOL_MB=%zu holds the modelled need %zu MiB (%s)\n", have, mb, by);
    return mb;
}
/* BS_LAYOUT_ONLY="D:g[,D:g...]" -- print the arena request binsplit_pregrow would make for D digits
 * per node over g node-processes (rank 0's term range, this process's POOL_LOG and switches), without allocating it, and
 * exit.  One `layout:` line per point (bytes, per device unless named): mem_model.py --check-c compares its own port of these
 * formulas with the lines, term by term.  Needs no other process: g comes from the list (MS: and is set as COMM_SIZE per point). */
/* the arena binsplit_pregrow requests per node (bytes) for N terms of a run over g node-processes (rank 0's range
 * set by the caller), at the current rns_pool_log(); *bs2 = its bs-region part */
static size_t layout_arena(unsigned long N, int g, size_t *bs2_)
{
    size_t need[NRMAX]; region_need(N, need);
    struct dm_layout L; memset(&L, 0, sizeof L); dm_layout(N, g, &L);
    size_t want = L.need_dev > L.tree_dev ? L.need_dev : L.tree_dev, arena = 0, bs2 = 0;
    for (int r = 0; r < NR; r++) {
        size_t cap = need[r] + need[r] / 8 + 4096;
        cap = (cap * 8 + ((size_t)2 << 20) - 1) / ((size_t)2 << 20) * ((size_t)2 << 20) / 8;
        size_t base = 2 * cap * 8, extra = want > base ? want - base : 0; extra = (extra + ((size_t)2 << 20) - 1) / ((size_t)2 << 20) * ((size_t)2 << 20);
        arena += as_arena(base + extra); bs2 += base;   /* whole chunks under BS_ARENA_ROOM */
    }
    arena *= (size_t)bs_lanes(); bs2 *= (size_t)bs_lanes();   /* PE layout: the node's L PEs are alike */
    if (bs2_) *bs2_ = bs2;
    return arena;
}
/* the plane caps, their settings and the node's device and host bytes at a cap.
 * cap c: 0 = 2^30 (POOL_LOG 30), 1 = 2^31 (POOL_LOG 31).  Device = the plane pools at the prime count (rns_plane_pool_bytes) + the arena + the transform contexts
 * (0.61 GB measured at 2^31, taken for every cap); host = the init peak's constants (runtime 7.0 GB, the checkpoint staging
 * 4 GiB, the seed buffers 4 GiB: mem_model.py HOST_*), + 6 GB per process of the comm at size > 1.  Only before rns_init (the
 * pool_log is switched per cap); after it, the caller's own cap only.
 * B5 fix (06 EVALUATION §2 row B5 / §4.1 item 3): host at g > 1 was BS_HOST_INIT_BYTES + 6 GB flat, which undercounts the
 * VMM seed buffers (as_seedbuf can be up to 2 x 8 GiB, not the flat 8 GiB in BS_HOST_INIT_BYTES) and omits the SHMEM pool
 * entirely.  Now mirrors as_room_fits's size > 1 host term exactly: runtime 7.0 GB + staging 4 GiB + transport 6 GB + the
 * SHMEM pool (as_shmem_pool), plus the larger of the two VMM seed buffers (as_seedbuf) or the writer 0.65 GB + MN_OUT_EARLY's
 * 0.9 GB.  size 1 is unchanged (BS_HOST_INIT_BYTES: as_room_fits's size 1 path is a different, measured formula that this
 * function does not mirror). */
const char *const bs_cap_name[2] = { "2^30", "2^31" };   /* the cap index c = pool_log - 30 */
#define BS_TABLES_BYTES ((size_t)610000000)
#define BS_HOST_INIT_BYTES ((size_t)7000000000 + ((size_t)8 << 30))
/* the device pools of the node (bytes; as_shmem_pool's ofi term): allocated by the transport at init, so ecalc.c budget_check counts them as already taken (plq2) */
size_t binsplit_comm_resident_bytes(unsigned long N, int g) { size_t ofi = 0; (void)as_shmem_pool(N, g, &ofi); return ofi; }
size_t binsplit_node_bytes(unsigned long N, int g, int cap, int np, size_t *planes_, size_t *arena_, size_t *host_)
{
    int pl0 = rns_pool_log(), pl = 30 + cap;
    if (np == 3 && ec_np_auto) np = ec_np_planes(pl, g);                /* pool 0 holds four planes where a product of g nodes may need them */
    rns_preinit_pool_log(pl);
    size_t arena = layout_arena(N, g, 0), planes = (size_t)bs_nrn() * rns_plane_pool_bytes(pl, np, 0, 0) + BS_TABLES_BYTES;
    rns_preinit_pool_log(pl0);
    size_t host;
    if (g > 1) {
        size_t ofi = 0, pool = as_shmem_pool(N, g, &ofi), sb = as_seedbuf(N) * (size_t)bs_lanes();
        size_t base = bs_ppn_proc() * ((size_t)7000000000 + (size_t)6000000000) + bs_staging() + pool + ofi;   /* OFIMEM: + the device pools */
        int early = getenv("MN_OUT_EARLY") ? atoi(getenv("MN_OUT_EARLY")) != 0 : 1;
        size_t hd = base + (size_t)bs_lanes() * ((size_t)650000000 + (early ? (size_t)900000000 : 0)), hi = base + sb;
        host = hi > hd ? hi : hd;
    } else host = BS_HOST_INIT_BYTES_P();
    if (planes_) *planes_ = planes;
    if (arena_) *arena_ = arena;
    if (host_) *host_ = host;
    return planes + arena + host;
}
/* ECALC_PLANE_CAP=run: the run's largest single product, in limbs: the division's t = A_h mu of 2 k + 8 limbs (dm_layout's tcap before DKM / tight
 * trim it; k = dl + 4, dl the limbs of 10^d, from N alone as dm_layout reads them).  A 2^30 cap serves the run when this fits one mn plane of the cap
 * (ecalc.c plane_cap_switch: <= 2^(mn_logn_cap(g)) at pool_log 30): above that the products are gridded into pieces and the plan has not been measured there
 * (1 node: 2 k + 8 > 2^30 from about 9.66e9 digits; Q122 measured the cap up to 8e9). */
size_t binsplit_div_product_limbs(unsigned long N)
{
    double lg = lgamma((double)N + 1.0) / log(10.0);
    size_t dl = (size_t)ceil((lg - 50.0) / 18.0) + 1, k = dl + 4;
    return 2 * k + 8;
}
/* MS: BS_LAYOUT_ONLY / its plan check at a point of g node-processes set COMM_SIZE to g while the point is laid out, and
 * restore the environment's value after (g = 0).  rns_pool0_np() reads COMM_SIZE, and P24's decision (rns_dist.c mn_p24_of)
 * reads rns_pool0_np(): without this the layout and its check did not see P24 (P24's open issue 3).  Nothing else in the
 * layout reads COMM_SIZE (the output is byte-identical without P24). */
void binsplit_layout_comm_size(int g)
{
    static int saved; static char *old;
    if (g > 1) { if (!saved) { const char *e = getenv("COMM_SIZE"); old = e ? strdup(e) : 0; saved = 1; }
                 char b[16]; snprintf(b, sizeof b, "%d", g); bs_setenv("COMM_SIZE", b, 1); return; }
    if (!saved) return;
    if (old) { bs_setenv("COMM_SIZE", old, 1); free(old); old = 0; } else bs_unsetenv("COMM_SIZE");
    saved = 0;
}
/* ---- FIT (user decision D26, results/LAUNCH_DEFAULTS.md §FIT): the pre-launch memory fit, and DM_MN_WRAP automatic --------------------------------
 * The device memory of a node against the machine's device edge (ECALC_DEVICE_GB, the profile's measured figure; code default
 * 373.44 = the target's, 93.36 GB per APU, results/TARGET.md §TGTBENCH2) and the node against the node budget (ECALC_NODE_GB, 480), from the
 * layout's own terms at rank 0's range (the BS_LAYOUT_ONLY convention, so every rank decides alike):
 *   device = max(the dm phase: planes + arena + device pools + 4 v-slots,  the bs phase: planes + arena + device pools + the VMM
 *            pool's bs growth (AS_VMM_BS_GROW, 6.0 GB measured) + 4 v-slots of the tree's top level)   (mem_model.py dev_max's form)
 *   node   = planes + arena + the bs growth + host (as_host_bytes: the room's host, the comm pools in it) + 4 v-slots
 * DM_MN_WRAP unset: the run takes the wrap (1) when the device with it is at most ECALC_DEVICE_GB - ECALC_DEVICE_MARGIN_GB (5.0 GB per
 * node by default), else 0; an explicit DM_MN_WRAP wins (a warning when its device is over the edge).  Size 1: the wrap is not used
 * (the mn division only), the value stays 1 as before.  The device pools (COMM_OFI_POOL_MB) are device memory. */
static __thread int g_wrap_force = -1;                          /* the arm binsplit_fit lays out (-1: none) */
static __thread int g_wrap_run = -1;                            /* the run's automatic decision (-1: not decided: 1, D22's default) */
static int dm_wrap_env(int *v)
{
    const char *e = getenv("DM_MN_WRAP"); if (!e) return 0;
    *v = atoi(e); if (*v < 0) *v = 0; return 1;
}
static int dm_wrap_req(void)
{
    int v; if (g_wrap_force >= 0) return g_wrap_force;
    if (dm_wrap_env(&v)) return v;
    return g_wrap_run >= 0 ? g_wrap_run : 1;
}
int binsplit_dm_wrap(void) { int v; if (dm_wrap_env(&v)) return v; return g_wrap_run >= 0 ? g_wrap_run : 1; }   /* newton_db.c dm_wrap, the config line */
const char *binsplit_dm_wrap_src(void) { int v; return dm_wrap_env(&v) ? "set" : g_wrap_run >= 0 ? "fit" : "default"; }     /* the config line's word */
static void fit_arm(unsigned long N, int g, int wrap, struct bs_fit *F)
{
    g_wrap_force = wrap; g_fit_probe = 1;
    int pl = rns_pool_log() > 0 ? rns_pool_log() : 31, np = ec_np_init(); if (np == 3 && ec_np_auto) np = ec_np_planes(pl, g);
    struct dm_layout L; memset(&L, 0, sizeof L); dm_layout(N, g, &L);
    F->planes = (size_t)bs_nrn() * rns_plane_pool_bytes(pl, np, 0, 0) + BS_TABLES_BYTES;
    F->arena = layout_arena(N, g, &F->bs2);
    size_t ofi = 0; F->pool = as_shmem_pool(N, g, &ofi);
    F->ofi = ofi; F->ofi_all = ofi;
    F->host = as_host_bytes(N, g, F->pool, ofi, &F->seedbuf);
    F->vslot = g > 1 ? binsplit_vslot_bytes(N, g, &F->vslot_top, 0, 0) : 0; if (g < 2) F->vslot_top = 0;
    F->grow = db_pool_vmm_on() ? AS_VMM_BS_GROW : 0;
    F->dev_dm = F->planes + F->arena + F->ofi + (size_t)bs_nrn() * F->vslot;
    F->dev_bs = F->planes + F->arena + F->ofi + F->grow + (size_t)bs_nrn() * F->vslot_top;
    F->device = F->dev_dm > F->dev_bs ? F->dev_dm : F->dev_bs;
    F->node = F->planes + F->arena + F->grow + F->host + (size_t)bs_nrn() * F->vslot;
    F->need_dev = L.need_dev; F->tree_dev = L.tree_dev; F->v2 = L.v2; F->v3 = L.v3; F->div = L.div; F->room = L.room; F->wrapb = L.wrap; F->hole = L.hole;
    g_wrap_force = -1; g_fit_probe = 0;
}
int binsplit_fit(unsigned long N, int g, struct bs_fit *F)
{
    static __thread unsigned long cN; static __thread int cg; static __thread struct bs_fit cF; static __thread int have;
    memset(F, 0, sizeof *F);
    int v, env = dm_wrap_env(&v);
    if (have && cN == N && cg == g && cF.wrap_env == env && (!env || cF.wrap == v)) { *F = cF; if (!env) g_wrap_run = F->wrap; return F->fits; }
    unsigned long a0 = bs_a0, b1 = bs_b1;
    bs_a0 = 1; bs_b1 = bs_pe_b1(N, g);   /* rank 0's range (BS_LAYOUT_ONLY's) */
    double limit = (getenv("ECALC_DEVICE_GB") ? atof(getenv("ECALC_DEVICE_GB")) : 373.44) * 1e9;            /* the device edge per node (the target's, m) */
    double margin = (getenv("ECALC_DEVICE_MARGIN_GB") ? atof(getenv("ECALC_DEVICE_MARGIN_GB")) : 5.0) * 1e9;  /* the wrap's safety margin per node */
    double budget = (getenv("ECALC_NODE_GB") ? atof(getenv("ECALC_NODE_GB")) : 480.0) * 1e9;
    struct bs_fit A, B; memset(&A, 0, sizeof A); memset(&B, 0, sizeof B);
    if (g < 2) { fit_arm(N, g, env ? v : 1, F); F->wrap = env ? v : 1; }
    else {
        fit_arm(N, g, 1, &A); fit_arm(N, g, 0, &B);
        if (env) { if (v == 0) *F = B; else if (v == 1) *F = A; else fit_arm(N, g, v, F); F->wrap = v; }
        else { int on = A.wrapb > 0 && (double)A.device <= limit - margin; *F = on ? A : B; F->wrap = on; }
        F->dev_wrap1 = A.device; F->dev_wrap0 = B.device; F->wrapb1 = A.wrapb;
    }
    F->wrap_env = env; F->limit = limit; F->margin = margin; F->budget = budget;
    F->fits_dev = (double)F->device <= limit; F->fits_node = (double)F->node <= budget; F->fits = F->fits_dev && F->fits_node;
    bs_a0 = a0; bs_b1 = b1;
    g_room_cfit = -1;                                  /* the runtime decides its room again on its own range (as before FIT) */
    if (!env) g_wrap_run = F->wrap;
    cF = *F; cN = N; cg = g; have = 1;
    return F->fits;
}
/* the fit report: `<pre>` starts every line ("plan fit   " for MN_PLAN_ONLY, "fit: " in the run's log); D = the run's digits */
void binsplit_fit_print(const struct bs_fit *F, double D, int g, const char *pre)
{
    const double G = 1e-9;
    printf("%s%.4g digits on %d node-process%s (rank 0's layout): device %.2f GB per node = %.2f GB per APU vs the device limit %.2f GB (ECALC_DEVICE_GB): margin %+.2f GB; "
           "node %.2f GB vs the node budget %.0f GB (ECALC_NODE_GB): margin %+.2f GB; host %.2f GB\n",
           pre, D, g, g > 1 ? "es" : "", F->device * G, F->device * G / bs_nrn(), F->limit * G, (F->limit - F->device) * G, F->node * G, F->budget * G, (F->budget - F->node) * G, F->host * G);
    int bs = F->dev_bs > F->dev_dm;
    printf("%sdevice at the %s phase (the peak; the %s phase %.2f GB) = planes %.2f + arena %.2f + device pools %.2f%s + v-slots %.2f GB\n",
           pre, bs ? "bs" : "dm", bs ? "dm" : "bs", (bs ? F->dev_dm : F->dev_bs) * G, F->planes * G, F->arena * G, F->ofi * G, bs && F->grow ? " + the bs growth 6.00" : "",
           (bs ? (size_t)bs_nrn() * F->vslot_top : (size_t)bs_nrn() * F->vslot) * G);
    if (g > 1) { char rt[24] = "7.00", tr[24] = "6.00";   /* S2c: one process per PE: runtime and transport once per process of the node */
                 if (bs_ppn_proc() > 1) { snprintf(rt, sizeof rt, "7.00 x %zu", bs_ppn_proc()); snprintf(tr, sizeof tr, "6.00 x %zu", bs_ppn_proc()); }
                 printf("%shost %.2f GB = runtime %s + staging 4.29 + transport %s + SHMEM pool %.2f + device pools %.2f + the larger of the seed buffers %.2f and the writer's 0.65 (+ 0.90 MN_OUT_EARLY) GB\n",
                      pre, F->host * G, rt, tr, F->pool * G, F->ofi_all * G, F->seedbuf * G); }
    else printf("%shost %.2f GB (the measured size-1 host high-water mark, mem_model host_size1_vmm%s)\n", pre, F->host * G, bs_ppn_proc() > 1 ? " + a runtime per further process" : "");
    printf("%sthe consumers per APU (largest first): arena %.2f GB (the dm need %.2f: reciprocal set v2 %.2f, division %.2f%s, the bs top levels v3 %.2f, the hole %.2f, the room %.2f; the tree %.2f; "
           "the bs regions %.2f), plane pools %.2f, v-slots %.2f, device pool %.2f\n",
           pre, F->arena * G / bs_nrn(), F->need_dev * G, F->v2 * G, F->div * G, F->wrapb ? "" : "", F->v3 * G, F->hole * G, F->room * G, F->tree_dev * G, F->bs2 * G / bs_nrn(), F->planes * G / bs_nrn(), F->vslot * G, F->ofi * G / bs_nrn());
    if (g > 1)
        printf("%sDM_MN_WRAP=%d (%s): device with the wrap %.2f GB, without %.2f GB (the wrap's term %.2f GB per APU); %s\n", pre, F->wrap, F->wrap_env ? "set" : "automatic",
               F->dev_wrap1 * G, F->dev_wrap0 * G, F->wrapb1 * G,
               F->wrap_env ? ((double)F->device > F->limit ? "WARNING: the set value does not fit the device limit" : "the set value wins")
                           : F->wrap ? "on: the device with it is within the limit less the margin" : (F->dev_wrap1 == F->dev_wrap0 ? "off: the wrap is not used at this size" : "off: the device with it exceeds the limit less the margin"));
    if (g > 1 && !F->wrap_env) printf("%s(the rule: on when the device with the wrap <= ECALC_DEVICE_GB %.2f - ECALC_DEVICE_MARGIN_GB %.2f = %.2f GB)\n", pre, F->limit * G, F->margin * G, (F->limit - F->margin) * G);
    printf("%s%s: %.4g digits on %d: device %.2f / %.2f GB (%+.2f), node %.2f / %.0f GB (%+.2f), DM_MN_WRAP=%d%s\n", pre, F->fits ? "FITS" : "REFUSED (does not fit)", D, g,
           F->device * G, F->limit * G, (F->limit - F->device) * G, F->node * G, F->budget * G, (F->budget - F->node) * G, F->wrap, F->wrap_env ? " (set)" : g > 1 ? " (auto)" : " (not used at size 1)");
}
static void binsplit_layout_only(const char *spec)
{
    unsigned long a0 = bs_a0, b1 = bs_b1;
    int np = ec_np_init();
    for (const char *s = spec; s && *s; ) {
        char *e; double D = strtod(s, &e); int g = 1; if (*e == ':') g = (int)strtol(e + 1, &e, 10); if (g < 1) g = 1;
        s = *e ? e + 1 : e;
        unsigned long d = (unsigned long)(D * g + 0.5); d = (d + 17) / 18 * 18; unsigned long N = e_terms(d);
        bs_a0 = 1; bs_b1 = bs_pe_b1(N, g);
        binsplit_layout_comm_size(g);                                   /* MS: COMM_SIZE = g for this point (restored below) */
        struct bs_fit F; binsplit_fit(N, g, &F); bs_a0 = 1; bs_b1 = bs_pe_b1(N, g);   /* FIT: the point's wrap decided first (its layout follows it) */
        size_t need[NRMAX]; region_need(N, need);
        struct dm_layout L; memset(&L, 0, sizeof L); dm_layout(N, g, &L);
        size_t top = 0; if (g > 1) { size_t nq_s = (L.nq + g - 1) / g; tree_need_dev(nq_s, g, rns_pool_log() > 0 ? rns_pool_log() : 31, &top); }
        size_t want = L.need_dev > L.tree_dev ? L.need_dev : L.tree_dev, arena = 0, bs2 = 0;
        for (int r = 0; r < NR; r++) {
            size_t cap = need[r] + need[r] / 8 + 4096;
            cap = (cap * 8 + ((size_t)2 << 20) - 1) / ((size_t)2 << 20) * ((size_t)2 << 20) / 8;   /* arena_get's rounding */
            size_t base = 2 * cap * 8, extra = want > base ? want - base : 0; extra = (extra + ((size_t)2 << 20) - 1) / ((size_t)2 << 20) * ((size_t)2 << 20);
            arena += as_arena(base + extra); bs2 += base;
        }
        arena *= (size_t)bs_lanes(); bs2 *= (size_t)bs_lanes();
        if (vpe_on()) printf("pe: D %.4g g %d | lanes %d npe %d regions_per_pe 1 b1 %lu | pe_region_need %zu pe_arena %zu\n", D, g, vpe_lanes(), g * vpe_lanes(), bs_b1, need[0], arena / (size_t)vpe_lanes());   /* S1d: additive, only with ECALC_VPE */
        char wrapb[40] = ""; if (L.wrap) snprintf(wrapb, sizeof wrapb, " wrap %zu", L.wrap);
        printf("layout: D %.4g g %d d %lu N %lu nq %zu k %zu tcap %zu | hole %zu dm_need %zu (top scratch %zu) tree_need %zu want %zu | bs regions %zu arena %zu (node, bytes) | tight %d jl %zu v2 %zu v3 %zu div %zu early_free %d room %zu chunk %zu room_planes %zu%s%s\n",
               D, g, d, N, L.nq, L.k, L.tcap, L.hole, L.need_dev, top, L.tree_dev, want, bs2, arena, L.tight, L.jl, L.v2, L.v3, L.div, 1,
               L.room, bs_arena_room() > 0 && db_pool_vmm_on() ? db_pool_vmm_chunk() : (size_t)0, bs_arena_room() > 0 && db_pool_vmm_on() ? g_room_planes : (size_t)0,
               L.dkm ? (L.lean ? " dkm 1 lean 1" : " dkm 1") : "", wrapb);   /* D19: " wrap <bytes>" with DM_MN_WRAP's term (absent: off) */   /* BS_ARENA_ROOM's term (in dm_need) and the chunk the arenas are rounded to (0: not rounded) */   /* lean 1 = the lean division set */   /* early_free 1 (always) */   /* the variant and its terms (per device) */   /* DL: dkm 1 = the layout follows NEWTON_DKM (absent: today's; the line is then unchanged) */
        if (bs_arena_room() > 0 && db_pool_vmm_on() && g_room_fit >= 0)       /* DL: the room's decision, term by term (bytes; mem_model.py --check-c compares them) */
            printf("room: D %.4g g %d | planes %zu arena_with_room %zu bs_grow %zu host %zu (seedbuf %zu shmem_pool %zu ofi_pool %zu) | node %zu budget %.0f fits %d\n",
                   D, g, g_room_planes, g_room_arena, AS_VMM_BS_GROW, g_room_host, g_room_seedbuf, g_room_pool, g_room_ofi, g_room_planes + g_room_arena + AS_VMM_BS_GROW + g_room_host,
                   (getenv("ECALC_NODE_GB") ? atof(getenv("ECALC_NODE_GB")) : 480.0) * 1e9, g_room_fit);
        int vroom = bs_arena_room() > 0 && db_pool_vmm_on() && g_room_fit >= 0;   /* B7ACCT: the room's terms, kept before the caps below redo the decision */
        size_t vdev0 = vroom ? g_room_planes + g_room_arena + g_room_ofi : 0, vnode0 = vroom ? g_room_planes + g_room_arena + AS_VMM_BS_GROW + g_room_host : 0;
        /* the plane pools at this run's prime count and the node totals at each plane cap (GB); '*' = the cap this
         * run's settings give (POOL_LOG, ECALC_PLANE_CAP) */
        { int pl = rns_pool_log(), cur = pl >= 31 ? 1 : 0;
          if (getenv("ECALC_PLANE_CAP_IDX")) cur = atoi(getenv("ECALC_PLANE_CAP_IDX"));
          printf("planes: D %.4g g %d np %d |", D, g, np);
          for (int c = 0; c < 2; c++) {
              if (rns_ndev() && c != cur) continue;                      /* after rns_init: the run's own cap only */
              size_t pb, ab, hb, tot = binsplit_node_bytes(N, g, c, np, &pb, &ab, &hb);
              printf(" cap %s%s: planes %.2f arena %.2f device %.2f node %.2f |", bs_cap_name[c], c == cur ? "*" : "", pb * 1e-9, ab * 1e-9, (pb + ab) * 1e-9, tot * 1e-9);
          }
          printf(" (GB; device = plane pools + arena + 0.61 tables, node = + host init %.1f)\n", (BS_HOST_INIT_BYTES_P() + (g > 1 ? 6e9 * bs_ppn_proc() : 0)) * 1e-9);
          /* B7ACCT: the general map's v-exchange slots (hipMalloc'd beside the layout; binsplit_vslot_bytes) and the device / node with them:
           * device = planes + arena_with_room + ofi_pool (the room's terms; without the room line: this cap's plane pools + arena), node = the
           * room's node (else binsplit_node_bytes at this cap), each + 4 x per_apu (bytes; mem_model.py --check-c compares them) */
          if (g > 1) {
              char by[512]; size_t tt = 0, v = binsplit_vslot_bytes(N, g, &tt, by, sizeof by), dev0, node0;
              if (vroom) { dev0 = vdev0; node0 = vnode0; }
              else { size_t pb, ab, hb, tot = binsplit_node_bytes(N, g, cur, np, &pb, &ab, &hb); dev0 = pb + ab; node0 = tot; }
              printf("vslot: D %.4g g %d | per_apu %zu tree_top %zu node_vslot %zu | device %zu device_with %zu node %zu node_with %zu | budget %d | %s\n",
                     D, g, v, tt, (size_t)bs_nrn() * v, dev0, dev0 + (size_t)bs_nrn() * v, node0, node0 + (size_t)bs_nrn() * v, binsplit_vslot_budget_on(), by);
          } }
        printf("fit: D %.4g g %d | wrap %d set %d | device_wrap1 %zu device_wrap0 %zu | planes %zu arena %zu ofi %zu grow %zu vslot %zu vslot_top %zu host %zu | dev_dm %zu dev_bs %zu device %zu node %zu | limit %.0f margin %.0f budget %.0f | fits %d\n",
               D, g, F.wrap, F.wrap_env, F.dev_wrap1, F.dev_wrap0, F.planes, F.arena, F.ofi, F.grow, F.vslot, F.vslot_top, F.host, F.dev_dm, F.dev_bs, F.device, F.node, F.limit, F.margin, F.budget, F.fits);   /* FIT: mem_model.py --check-c compares it */
        binsplit_layout_comm_size(0);
    }
    fflush(stdout);
    bs_a0 = a0; bs_b1 = b1;
}
void binsplit_pregrow(unsigned long N)
{
    static __thread unsigned long done_N; if (done_N == N) return; done_N = N;   /* once per run -- binsplit_seeds_begin calls it inside rns_init (the seeds stream into the regions), the driver again after */
    if (getenv("BS_LAYOUT_ONLY")) { binsplit_layout_only(getenv("BS_LAYOUT_ONLY")); exit(0); }   /* the arena request without the arena */
    if (getenv("BS_BALANCE_N")) bs_balance_n = atoi(getenv("BS_BALANCE_N"));
    double t_pg = mem_now();
    size_t need[NRMAX] = {0}; region_need(N, need);   /* C4: regions from the simulated layout */
    /* the arena also serves the dm phase as the block pool (the regions are donated to it); its size is the bs need alone
     *.
     * ECALC_ARENA_GB sets the arena per APU outright. */
    { size_t half = getenv("ECALC_ARENA_GB") ? (size_t)(atof(getenv("ECALC_ARENA_GB")) * 1e9 / 2) / 8 : 0;
      if (half) for (int r = 0; r < NR; r++) if (half > need[r] + need[r] / 8) need[r] = half - half / 9 - 4096; }
    if (bs_verbose) printf("bs: regions from the level layouts: %.2f / %.2f / %.2f / %.2f GB (+1/8)%s\n", need[0] * 8e-9, need[1] * 8e-9, need[2] * 8e-9, need[3] * 8e-9, ", one arena per device for both parities");
    if (mem_device_count() > 0 && !g_arena[0].base && !g_pool[0][0]) {
        double ta = mem_now();
        /* the arena also holds the dm phase (and, at size > 1, the tree) -- its need per device beyond
         * the two parities is mapped here, at init, and its last bytes (t1's quarter, the hole) are the block pool's reserved tail
         *. */
        struct dm_layout dml; memset(&dml, 0, sizeof dml); int sz = getenv("COMM_SIZE") ? atoi(getenv("COMM_SIZE")) : 1; if (sz < 1) sz = 1;
        dm_layout(N, sz, &dml);
        size_t extra[NRMAX], hole[NRMAX], cap[NRMAX]; int nd = mem_device_count(), per_dev = (NR + nd - 1) / nd;   /* regions per device (one, on the four-APU node) */
        for (int r = 0; r < NR; r++) {
            cap[r] = need[r] + need[r] / 8 + 4096;
            size_t base = 2 * cap[r] * 8, want = dml.need_dev > dml.tree_dev ? dml.need_dev : dml.tree_dev;   /* per device; a region's arena is its share */
            want = (want + per_dev - 1) / per_dev; hole[r] = dml.hole / per_dev;
            extra[r] = want > base ? want - base : 0;             /* the hole is a policy over the arena's last bytes, not bytes added: at the dm phase the
                                                                  * level pools are dead and the pool's blocks keep out of the tail, so it is free whenever
                                                                  * the arena holds the dm need at all (v2; v1 added the hole to the arena: +3.7 GB at 4e10, +32 at 9e10) */
            if (bs_arena_room() > 0 && db_pool_vmm_on()) {         /* whole chunks under BS_ARENA_ROOM (the tail ends at the last chunk's end) -- from the
                                                                  * base arena_get lays out (cap and extra in 2 MiB steps), or the arena ends a few MiB into one more
                                                                  * chunk and its remainder lies after the tail again (job 21619's first runs) */
                size_t m2 = (size_t)2 << 20, capr = (cap[r] * 8 + m2 - 1) / m2 * m2 / 8, b2 = 2 * capr * 8, ex = want > b2 ? (want - b2 + m2 - 1) / m2 * m2 : 0;
                extra[r] = as_arena(b2 + ex) - b2;
            }
        }
        if (bs_verbose) printf("bs: dm layout: n_Q %zu limbs, k %zu, t1 %zu limbs; per device: need %.2f GB (v2 %.2f, v3 %.2f, division %.2f; tree %.2f), tail %.2f GB (thresh %.2f)%s%s%s\n", dml.nq, dml.k, dml.tcap, dml.need_dev * 1e-9, dml.v2 * 1e-9, dml.v3 * 1e-9, dml.div * 1e-9, dml.tree_dev * 1e-9, dml.hole * 1e-9, dml.thresh * 1e-9,
                                          dml.room ? "; BS_ARENA_ROOM: room in need, arenas in whole chunks" : "", dml.tight ? "; DM_TIGHT" : "", dml.dkm ? "; NEWTON_DKM (the reciprocal to h, the two quotient halves)" : "");
        if (dml.tight) db_pool_pack_large(1);                  /* the large blocks packed at the arena's top (dbig.c ext_take) */
#pragma omp parallel for num_threads(NR) schedule(static)   /* the regions per device in parallel */
        for (int r = 0; r < NR; r++) arena_get(r, cap[r], extra[r], hole[r], dml.thresh);
        if (bs_arena_room() > 0 && db_pool_vmm_on()) for (int r = 0; r < NR; r++)   /* the layout's promise (whole chunks, the tail at the last chunk's end) */
            if (g_arena[r].base && g_arena[r].bytes % db_pool_vmm_chunk()) fprintf(stderr, "bs: BS_ARENA_ROOM: WARNING: region %d's arena %zu bytes is not whole chunks (the tail is not at the arena's end)\n", r, g_arena[r].bytes);
        size_t ar_sum = 0, ex_sum = 0, ho_sum = 0; for (int r = 0; r < NR; r++) { ar_sum += g_arena[r].bytes; ex_sum += extra[r]; ho_sum += hole[r]; }   /* S2fix: the process's own regions only (one under ECALC_PE_PER_GPU: extra[1..3] were never set -- tails 1.4e6 GB in the log) */
        if (bs_verbose || (getenv("ECALC_VERBOSE") && atoi(getenv("ECALC_VERBOSE")) >= 2)) printf("bs: arenas %.1f GB allocated in %.2f s (layout pass %.2f s; dm extra %.1f GB, tails %.1f GB)\n", ar_sum / 1e9, mem_now() - ta, ta - t_pg, ex_sum / 1e9, ho_sum / 1e9);
    }
    g_in_pregrow = 1;
#pragma omp parallel for num_threads(NR) schedule(static)
    for (int r = 0; r < NR; r++) for (int w = 0; w < 2; w++) pool_get(w, r, need[r]);
    g_in_pregrow = 0;
}
/* The top-level set (ECALC_CKPT_TOP, for ECALC_RECHECK): this node's P and Q (size 1: the whole numbers, written by
 * ecalc.c as tree level 0; size > 1: the node's shares after mn_tree's top level) and their mdb descriptors.  Files in
 * bs_ckpt_dir: tree_LLL.hdr (header) and tree_LLL.rR, R = 0..3 (quarter R by limb range of P, then of Q); multi-node names
 * carry the rank (n<rank>_tree_LLL.*).  Every file is written to a temporary name and renamed, the header last, so a
 * header means a complete set.  The header is v3: v1 + the extension (size, rank, the descriptors) + the schedule that
 * wrote it (node count, MN_GROUPS levels; recorded, not compared).  No restart: the set only feeds the recheck. */
#define CKPT_MAGIC2 "ECBSCKP2"
#define CKPT_MAGIC3 "ECBSCKP3"
#define CKPT_MAGIC4 "ECBSCKP4"                         /* S1d: a set of PEs (ECALC_PE_PER_GPU=1): one file per PE (p<PE>_tree_LLL.*), nregions 1, ext.size = PEs, ext.rank = this PE, ext.dev_nodes = PEs per node; never read by a run of the other mode */
#define CKPT_CHUNK ((size_t)1 << 30)
struct ckpt_hdr { char magic[8]; uint64_t N; int32_t decimal, seed_terms, level, which, mdev_host, nregions; uint64_t n, off, offr[NRMAX], node_bytes; };
struct ckpt_ext { int32_t size, rank, dev_nodes, kind; uint64_t a0, b1; uint64_t tree[10]; };   /* kind 1 = a tree set (tree[] = P.n P.N P.g0 P.g P.sh.n, the same for Q) */
#define CKPT_MAXLEV 32
struct ckpt_sched { int32_t size, nlev; int32_t gs[CKPT_MAXLEV]; };
static void ckpt_sched_now(struct ckpt_sched *s)
{
    memset(s, 0, sizeof *s); s->size = mn_size();
    if (s->size > 1) s->nlev = mn_groups_parse(s->size, s->gs, CKPT_MAXLEV - 1);
}
static int g_ck_node = -2;                            /* -1: single node (no rank in the names); else this node-process's rank */
static void ckpt_env(void) { if (g_ck_node == -2) g_ck_node = mn_size() > 1 ? mn_rank() : -1; }
static void ckpt_path(char *buf, size_t sz, const char *kind, int level, const char *suffix, int r, int tmp)
{
    char pre[64]; ckpt_env();
    if (vpe_on()) snprintf(pre, sizeof pre, "p%04d_%s_%03d", vpe_pe(), kind, level);
    else if (g_ck_node < 0) snprintf(pre, sizeof pre, "%s_%03d", kind, level); else snprintf(pre, sizeof pre, "n%03d_%s_%03d", g_ck_node, kind, level);
    if (r < 0) snprintf(buf, sz, "%s/%s.%s%s", bs_ckpt_dir, pre, suffix, tmp ? ".tmp" : "");
    else snprintf(buf, sz, "%s/%s.%s%d%s", bs_ckpt_dir, pre, suffix, r, tmp ? ".tmp" : "");
}
static int fwrite_all(FILE *f, const void *p, size_t bytes) { return fwrite(p, 1, bytes, f) == bytes; }
static int fread_all(FILE *f, void *p, size_t bytes) { return fread(p, 1, bytes, f) == bytes; }
/* flush + fsync + close + rename tmp -> final; on any failure the temporary is removed */
static int ckpt_finish(FILE *f, int ok, const char *tmp, const char *final)
{
    ok = ok && f && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f) fclose(f);
    if (ok && rename(tmp, final) != 0) ok = 0;
    if (!ok) { fprintf(stderr, "bs: checkpoint write failed for %s: %s\n", final, strerror(errno)); unlink(tmp); }
    if (ok && sp_odirect()) sp_drop_cache_path(final);   /* the header's pages leave the page cache too */
    return ok;
}
/* the context of a background tree-set write (bs_ckpt_bg_*) -- its own DMA buffers of `chunk`
 * bytes (the pinned staging is released while it runs; a smaller chunk bounds the time to stop), a cancel flag the
 * writer checks before every device read, the device reads in flight (the owner may overwrite or free P / Q once cancel
 * is set and none is), and the bytes handed to the kernel so far (the measured rate).  0 = the synchronous write. */
struct ckpt_io { size_t chunk; double mbs; volatile int cancel, dma_active, pdone, qdone; volatile size_t bytes_p, bytes_all; };   /* mbs: test (BS_CKPT_BG_MBS), a slower disk emulated */
/* the 1 GiB host buffer for the DMA of thread r: region r's pinned staging (idle between levels), else malloc */
static uint64_t *ckpt_buf(int r, int *own, const struct ckpt_io *io)
{
    *own = 0; void *p = 0;
    if (io) { *own = 1; if (posix_memalign(&p, (size_t)2 << 20, io->chunk)) p = 0; return (uint64_t *)p; }   /* a background writer's own chunk (aligned, for O_DIRECT) */
    if (mem_device_count() >= NR && rns_staging_bytes() >= CKPT_CHUNK) return rns_hstage(reg_dev(r, mem_device_count()));
    *own = 1; if (posix_memalign(&p, (size_t)2 << 20, CKPT_CHUNK)) p = 0; return (uint64_t *)p;
}
/* a checkpoint data file -- the stdio stream as before, or with ECALC_ODIRECT=1 spill.c's sp_file: O_DIRECT
 * through the (aligned) DMA buffer, the 1 GiB buffer split in two halves so the DMA of one runs under the write of the
 * other; nothing is left in the page cache (on the APU the cache is HBM, results/APUMULT.md, apumult_M) */
typedef struct { FILE *f; sp_file sf; int direct; } ckf;
static size_t ck_halves(uint64_t *buf, void *b2[2], size_t bytes) { b2[0] = buf; b2[1] = (char *)buf + bytes / 2; return bytes / 2; }
static ckf *ckpt_open(const char *kind, int level, int r, int write, char *tmp, char *final)
{
    ckpt_path(tmp, 4096, kind, level, "r", r, 1); ckpt_path(final, 4096, kind, level, "r", r, 0);
    ckf *c = (ckf *)calloc(1, sizeof *c); c->direct = sp_odirect();
    if (c->direct) { if (!spf_open(&c->sf, write ? tmp : final, write, 1)) { free(c); return 0; } return c; }
    c->f = fopen(write ? tmp : final, write ? "wb" : "rb");
    if (!c->f) { fprintf(stderr, "bs: checkpoint: cannot open %s: %s\n", write ? tmp : final, strerror(errno)); free(c); return 0; }
    return c;
}
static int ckpt_close(ckf *c, int ok, int write, const char *tmp, const char *final)
{
    if (c->direct) {
        int e = c->sf.err; sp_file sf = c->sf; ok = spf_close(&c->sf, ok) && ok; free(c);
        if (write) {
            if (ok && rename(tmp, final) != 0) { e = errno; ok = 0; }
            if (!ok) { fprintf(stderr, "bs: checkpoint write failed for %s: %s\n", final, strerror(e ? e : EIO)); unlink(tmp); }
        } else if (!ok) {                               /* the failed read named: errno, where, how much (CKPT) */
            struct stat st; long long fsz = stat(final, &st) == 0 ? (long long)st.st_size : -1;
            fprintf(stderr, "bs: checkpoint: short read from %s: %s at offset %llu (that read got %zu of %zu bytes; %llu payload bytes read before; file %lld bytes)\n",
                    final, strerror(e ? e : EIO), (unsigned long long)sf.eoff, sf.egot, sf.eneed, (unsigned long long)sf.bytes, fsz);
        }
        return ok;
    }
    FILE *f = c->f; free(c);
    if (write) return ckpt_finish(f, ok, tmp, final);
    fclose(f);
    if (!ok) fprintf(stderr, "bs: checkpoint: short read from %s\n", final);
    return ok;
}
static void ckpt_abandon(ckf *c)                       /* a cancelled background write: closed without fsync (the caller unlinks it) */
{
    if (c->direct) spf_close(&c->sf, 0); else fclose(c->f);
    free(c);
}
/* M6: the limbs [lo, hi) of a device number <-> the file, in runs inside one device quarter (limb g lives in
 * quarter (g >= qc) + (g >= 2 qc) + (g >= 3 qc), dbig.c), each run DMA'd on its quarter's device through buf */
static int ckpt_dbig_io(ckf *cf, dbig *x, size_t lo, size_t hi, uint64_t *buf, int write, struct ckpt_io *io, int part)
{
    if (x->off) { ec_fatal(EC_RC_FATAL, "bs: checkpoint of a dbig view\n"); }
    int ok = 1; size_t cap = (io ? io->chunk : CKPT_CHUNK) / 8;
    FILE *f = cf->f;
    void *b2[2] = { buf, 0 }; size_t bc = 0; volatile int *guard[2] = { 0, 0 };   /* the O_DIRECT form */
    if (cf->direct) {
        if (io) { bc = io->chunk; cap = (io->chunk - SP_ALIGN) / 8; guard[0] = &io->dma_active; guard[1] = &io->cancel; }   /* one DMA per run, single-buffered: the cancel contract below */
        else bc = ck_halves(buf, b2, CKPT_CHUNK);
    }
    for (size_t i = lo; i < hi && ok;) {
        size_t d = (i >= x->qc) + (i >= 2 * x->qc) + (i >= 3 * x->qc), so = i - d * x->qc, run = x->qc - so;
        if (i + run > hi) run = hi - i;
        if (run > cap) run = cap;
        int dv = vpe_on() ? vpe_device() : (int)d;       /* the device that holds the run: a vPE's numbers sit on its own APU */
        if (write && io && cf->direct) {               /* the same contract, inside spf_write_dev (the guard) */
            double tw = io->mbs > 0 ? mem_now() : 0;
            ok = spf_write_dev(&cf->sf, dv, x->q[d] + so, run * 8, b2, bc, guard);
            if (!ok && __atomic_load_n(&io->cancel, __ATOMIC_SEQ_CST)) return 0;
            if (io->mbs > 0) { double want = run * 8 / (io->mbs * 1e6 / NR); while (mem_now() - tw < want && !__atomic_load_n(&io->cancel, __ATOMIC_RELAXED)) usleep(5000); }
            __atomic_add_fetch(part ? &io->bytes_all : &io->bytes_p, run * 8, __ATOMIC_RELAXED);
            if (!part) __atomic_add_fetch(&io->bytes_all, run * 8, __ATOMIC_RELAXED);
        }
        else if (write && cf->direct) ok = spf_write_dev(&cf->sf, dv, x->q[d] + so, run * 8, b2, bc, 0);
        else if (cf->direct) ok = spf_read_dev(&cf->sf, dv, x->q[d] + so, run * 8, b2, bc);
        else if (write && io) {                        /* announce the device read, then look at cancel (the owner sets cancel, then waits for no read in flight) */
            __atomic_add_fetch(&io->dma_active, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&io->cancel, __ATOMIC_SEQ_CST)) { __atomic_sub_fetch(&io->dma_active, 1, __ATOMIC_SEQ_CST); return 0; }
            mem_dev_copy_on(dv, buf, x->q[d] + so, run * 8);
            __atomic_sub_fetch(&io->dma_active, 1, __ATOMIC_SEQ_CST);
            double tw = io->mbs > 0 ? mem_now() : 0;
            ok = fwrite_all(f, buf, run * 8);
            if (io->mbs > 0) { double want = run * 8 / (io->mbs * 1e6 / NR); while (mem_now() - tw < want && !__atomic_load_n(&io->cancel, __ATOMIC_RELAXED)) usleep(5000); }   /* test: this thread's share of the emulated rate */
            __atomic_add_fetch(part ? &io->bytes_all : &io->bytes_p, run * 8, __ATOMIC_RELAXED);
            if (!part) __atomic_add_fetch(&io->bytes_all, run * 8, __ATOMIC_RELAXED);
        }
        else if (write) { mem_dev_copy_on(dv, buf, x->q[d] + so, run * 8); ok = fwrite_all(f, buf, run * 8); }
        else { ok = fread_all(f, buf, run * 8); if (ok) mem_dev_copy_on(dv, x->q[d] + so, buf, run * 8); }
        i += run;
    }
    return ok;
}
static size_t range_lo(size_t n, int r) { return n * (size_t)r / NR; }
static void ckpt_remove_kind(const char *kind, int level)
{
    char p[4096];
    for (int r = 0; r < NR; r++) { ckpt_path(p, sizeof p, kind, level, "r", r, 0); unlink(p); }
    ckpt_path(p, sizeof p, kind, level, "hdr", -1, 0); unlink(p);
}
static void ckpt_sync_dir(void) { int dfd = open(bs_ckpt_dir, O_RDONLY | O_DIRECTORY); if (dfd >= 0) { fsync(dfd); close(dfd); } }
/* the v3 header -> <kind>_LLL.hdr; the directory fsync'd after */
static int ckpt_write_hdr(const char *kind, struct ckpt_hdr *h, const struct ckpt_ext *x)
{
    char tmp[4096], final[4096];
    memcpy(h->magic, vpe_on() ? CKPT_MAGIC4 : CKPT_MAGIC3, 8);
    struct ckpt_sched sc; ckpt_sched_now(&sc);
    ckpt_path(tmp, sizeof tmp, kind, h->level, "hdr", -1, 1); ckpt_path(final, sizeof final, kind, h->level, "hdr", -1, 0);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite_all(f, h, sizeof *h) && fwrite_all(f, x, sizeof *x) && fwrite_all(f, &sc, sizeof sc);
    if (!ckpt_finish(f, ok, tmp, final)) return 0;
    ckpt_sync_dir();
    return 1;
}
/* the header of <kind>_LLL: 1 if it belongs to this run (another run's set aborts) and, with check_files, its
 * region files have the sizes the header names (a complete set).  v2 sets (no schedule recorded) are read too. */
static int ckpt_read_hdr(const char *kind, int level, struct ckpt_hdr *h, struct ckpt_ext *x, unsigned long N, int check_files)
{
    char p[4096]; ckpt_env();
    ckpt_path(p, sizeof p, kind, level, "hdr", -1, 0);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    memset(x, 0, sizeof *x);
    struct ckpt_sched s3;
    int ok = fread_all(f, h, sizeof *h);
    if (ok && !vpe_on() && !memcmp(h->magic, CKPT_MAGIC4, 8)) ec_fatal(EC_RC_FATAL, "bs: checkpoint %s is a set of PEs (ECALC_PE_PER_GPU=1); this run is not (a PE set is read only by a run with ECALC_PE_PER_GPU=1)\n", p);
    if (ok && vpe_on() && (!memcmp(h->magic, CKPT_MAGIC2, 8) || !memcmp(h->magic, CKPT_MAGIC3, 8))) ec_fatal(EC_RC_FATAL, "bs: checkpoint %s is a set of node-processes; this run is ECALC_PE_PER_GPU=1 (a node set is read only by a run without it)\n", p);
    ok = ok && ((!vpe_on() && !memcmp(h->magic, CKPT_MAGIC2, 8) && fread_all(f, x, sizeof *x))
                || (!vpe_on() && !memcmp(h->magic, CKPT_MAGIC3, 8) && fread_all(f, x, sizeof *x) && fread_all(f, &s3, sizeof s3))
                || (vpe_on() && !memcmp(h->magic, CKPT_MAGIC4, 8) && fread_all(f, x, sizeof *x) && fread_all(f, &s3, sizeof s3)));
    fclose(f);
    if (!ok) return 0;
    int me = vpe_on() ? vpe_pe() : g_ck_node < 0 ? 0 : g_ck_node;
    int want_size = vpe_on() ? vpe_npe() : mn_size();
    if (vpe_on() && x->dev_nodes != vpe_lanes()) ec_fatal(EC_RC_FATAL, "bs: checkpoint %s was written with %d PEs per node; this run has %d\n", p, x->dev_nodes, vpe_lanes());
    long want_st = bs_seed_fill() > 0 ? (long)bs_seed_terms_for(bs_b1 ? bs_b1 : N + 1) : bs_seed_terms;   /* BS_SEED_FILL's span, as seed_limbs sets it (RECHECK reads the set before seed_limbs runs) */
    if (h->N != N || h->decimal != 1 || h->seed_terms != want_st || h->nregions != NR || x->size != want_size || x->rank != me)
        ec_fatal(EC_RC_FATAL, "bs: checkpoint %s is from another run (N %llu, base %s, seeds %d, %d node-processes, rank %d; this run: N %lu, %d node-processes, rank %d)\n",
                 p, (unsigned long long)h->N, "10^18", h->seed_terms, x->size, x->rank, N, want_size, me);
    if (!check_files) return 1;
    struct stat st;
    for (int r = 0; r < NR && ok; r++) { ckpt_path(p, sizeof p, kind, level, "r", r, 0); ok = stat(p, &st) == 0 && (uint64_t)st.st_size == h->offr[r] * 8; }
    return ok;
}
/* the latest complete set of a kind in bs_ckpt_dir for this run (this node-process): its level, or 0 */
static int ckpt_find_kind(const char *kind, unsigned long N)
{
    int best = 0; struct ckpt_hdr h; struct ckpt_ext x;
    for (int l = 1; l < 128; l++) if (ckpt_read_hdr(kind, l, &h, &x, N, 1) && l > best) best = l;
    return best;
}
/* ---- the top-level set: the node's P and Q (shares); file r = quarter r (by limb range) of P, then of Q ---- */
static int tree_io(int level, dbig *P, dbig *Q, int r, int write, struct ckpt_io *io)
{
    char tmp[4096], final[4096];
    ckf *f = ckpt_open("tree", level, r, write, tmp, final); if (!f) return 0;
    int ok, own; uint64_t *buf = ckpt_buf(r, &own, write ? io : 0);
    ok = ckpt_dbig_io(f, P, range_lo(P->n, r), range_lo(P->n, r + 1), buf, write, write ? io : 0, 0);
    if (write && io) __atomic_add_fetch(&io->pdone, 1, __ATOMIC_RELEASE);
    ok = ok && ckpt_dbig_io(f, Q, range_lo(Q->n, r), range_lo(Q->n, r + 1), buf, write, write ? io : 0, 1);
    if (write && io) __atomic_add_fetch(&io->qdone, 1, __ATOMIC_RELEASE);        /* (after a cancel or a failure too: the file no longer reads P or Q) */
    if (own) free(buf);
    if (write && io && __atomic_load_n(&io->cancel, __ATOMIC_SEQ_CST)) { ckpt_abandon(f); unlink(tmp); return 0; }   /* abandoned: quietly, no fsync */
    return ckpt_close(f, ok, write, tmp, final);
}
static size_t tree_write(int level, unsigned long N, const uint64_t desc[10], dbig *P, dbig *Q, struct ckpt_io *io, double *t_data)
{
    struct ckpt_hdr h; struct ckpt_ext x; memset(&h, 0, sizeof h); memset(&x, 0, sizeof x);
    h.N = N; h.decimal = 1; h.seed_terms = bs_seed_terms; h.level = level; h.nregions = NR;
    for (int r = 0; r < NR; r++) h.offr[r] = range_lo(P->n, r + 1) - range_lo(P->n, r) + range_lo(Q->n, r + 1) - range_lo(Q->n, r);
    x.size = vpe_on() ? vpe_npe() : mn_size(); x.rank = vpe_on() ? vpe_pe() : mn_rank(); x.dev_nodes = vpe_on() ? vpe_lanes() : 0; x.kind = 1; x.a0 = bs_a0; x.b1 = bs_b1; memcpy(x.tree, desc, sizeof x.tree);
    mkdir(bs_ckpt_dir, 0777);
    char hp[4096]; ckpt_path(hp, sizeof hp, "tree", level, "hdr", -1, 0); unlink(hp);   /* a set of this level from before (a rerun of the outfile) is void from here: never its header over new files */
    int oks[NRMAX];
#pragma omp parallel for num_threads(NR) schedule(static, 1)
    for (int r = 0; r < NR; r++) oks[r] = tree_io(level, P, Q, r, 1, io);
    if (t_data) *t_data = mem_now();
    for (int r = 0; r < NR; r++) if (!oks[r]) { ckpt_remove_kind("tree", level); return 0; }   /* (a failed or dropped set leaves no files) */
    if (!ckpt_write_hdr("tree", &h, &x)) return 0;
    size_t bytes = sizeof h + sizeof x + sizeof(struct ckpt_sched); for (int r = 0; r < NR; r++) bytes += h.offr[r] * 8;
    return bytes;
}

/* ---- a tree set written by a background thread ----
 * The top set (size 1: tree_000 from ecalc.c; size > 1: mn_tree's top level) is written while the reciprocal and the
 * division run.  Each of the four files holds P's quarter, then Q's, so P is released first (the owner overwrites it
 * with S = P + Q after the reciprocal) and Q later (the owner holds Q past the output stage when the writer still
 * needs it).  bs_ckpt_bg_release(part) returns once the writer no longer reads that part: all files past it (1), or,
 * budgeted (ECALC_CKPT_TOP=2), as soon as the measured rate -- the bytes handed to the kernel over the writer's
 * elapsed time -- projects the rest of the part beyond `slack` seconds: the set is abandoned (the writer stops before
 * its next device read, its temporaries are removed, no header: the directory keeps no partial set) and it returns 0.
 * So the critical path waits at most `slack` per release plus one DMA of `chunk` bytes. */
struct bs_ckpt_bg {
    int level; unsigned long N; uint64_t desc[10]; dbig P, Q;   /* by value: the owner may move its descriptors (newton_mn_divmod's S = *P) */
    int budget; double slack, t0, t_data, t1, t_cancel, t_wait[2], proj[2]; size_t bytes, tot_p, tot;
    int abandoned, released[2]; pthread_t th; volatile int done; struct ckpt_io io;
};
static void *bg_run(void *a)
{
    struct bs_ckpt_bg *b = (struct bs_ckpt_bg *)a; vpe_adopt();
    b->bytes = tree_write(b->level, b->N, b->desc, &b->P, &b->Q, &b->io, &b->t_data);
    b->t1 = mem_now(); __atomic_store_n(&b->done, 1, __ATOMIC_RELEASE); return 0;
}
struct bs_ckpt_bg *bs_ckpt_bg_start(int level, unsigned long N, const uint64_t desc[10], const dbig *P, const dbig *Q, int budget, double slack)
{
    if (sp_odirect()) sp_dev_sync_all();               /* P and Q complete before the writer's DMA on its own streams */
    struct bs_ckpt_bg *b = (struct bs_ckpt_bg *)calloc(1, sizeof *b);
    b->level = level; b->N = N; memcpy(b->desc, desc, sizeof b->desc); b->P = *P; b->Q = *Q; b->budget = budget; b->slack = slack;
    const char *e = getenv("BS_CKPT_BG_CHUNK_MB"); b->io.chunk = (size_t)(e ? atoi(e) : 256) << 20; if (b->io.chunk < ((size_t)1 << 20)) b->io.chunk = (size_t)1 << 20;
    e = getenv("BS_CKPT_BG_MBS"); b->io.mbs = e ? atof(e) : 0;   /* test: emulate a disk of this many MB/s (the four files together) */
    b->tot_p = P->n * 8; b->tot = (P->n + Q->n) * 8;
    b->t0 = mem_now();
    if (pthread_create(&b->th, 0, bg_run, b)) { fprintf(stderr, "bs: checkpoint: cannot start the background writer\n"); free(b); return 0; }
    return b;
}
static int bg_part_done(struct bs_ckpt_bg *b, int part)
{
    return __atomic_load_n(&b->done, __ATOMIC_ACQUIRE) || __atomic_load_n(part ? &b->io.qdone : &b->io.pdone, __ATOMIC_ACQUIRE) >= NR;
}
int bs_ckpt_bg_done(struct bs_ckpt_bg *b, int part) { return !b || b->abandoned || bg_part_done(b, part); }
int bs_ckpt_bg_release(struct bs_ckpt_bg *b, int part)
{
    if (!b) return 1;
    double ts = mem_now(); int ok = 1;
    if (b->abandoned) ok = 0;
    else while (!bg_part_done(b, part)) {
        if (b->budget) {
            double now = mem_now(), waited = now - ts, el = now - b->t0;
            size_t all = __atomic_load_n(&b->io.bytes_all, __ATOMIC_RELAXED), dp = __atomic_load_n(&b->io.bytes_p, __ATOMIC_RELAXED);
            size_t rem = part ? (b->tot > all ? b->tot - all : 0) : (b->tot_p > dp ? b->tot_p - dp : 0);
            double proj = all && el > 0 ? rem / (all / el) : 1e30;
            if (waited >= b->slack || (all && proj > b->slack - waited)) {
                b->proj[part] = proj;
                __atomic_store_n(&b->io.cancel, 1, __ATOMIC_SEQ_CST);
                while (__atomic_load_n(&b->io.dma_active, __ATOMIC_SEQ_CST)) usleep(200);   /* no device read of P or Q from here on */
                b->abandoned = 1; b->t_cancel = mem_now(); ok = 0; break;
            }
        }
        usleep(2000);
    }
    b->t_wait[part] += mem_now() - ts; b->released[part] = 1;
    return ok;
}
size_t bs_ckpt_bg_join(struct bs_ckpt_bg *b, const char *who, const char *dir, double *t_write, double *t_wait)
{
    if (!b) return 0;
    double tj = mem_now();
    pthread_join(b->th, 0);
    double t_join = mem_now() - tj, el = (b->abandoned ? b->t_cancel : b->t_data) - b->t0;   /* the rate over the time the writer had (to the drop, or to its last byte) */
    size_t all = __atomic_load_n(&b->io.bytes_all, __ATOMIC_RELAXED); double rate = el > 0 ? all / el : 0;
    size_t bytes = b->abandoned ? 0 : b->bytes;
    if (b->abandoned)
        printf("%scheckpoint: the top-level set (tree level %d) -> %s ABANDONED (ECALC_CKPT_TOP=2): %.2f of %.2f GB handed over in %.2f s (%.2f GB/s measured); "
               "the %s part projected %.1f s more against a slack of %.1f s -- waited %.2f s for P, %.2f s for Q; no partial set is left\n",
               who, b->level, dir, all * 1e-9, b->tot * 1e-9, el, rate * 1e-9, b->proj[1] > 0 ? "Q" : "P", b->proj[1] > 0 ? b->proj[1] : b->proj[0], b->slack, b->t_wait[0], b->t_wait[1]);
    else
        printf("%scheckpoint: the top-level P, Q -> %s (tree level %d): %.2f GB in %.2f s in the background (data %.2f s at %.2f GB/s, then fsync and header %.2f s); "
               "waited %.2f s for P before S = P + Q, %.2f s for Q after the output stage, %.2f s at the join%s\n",
               who, dir, b->level, bytes * 1e-9, b->t1 - b->t0, el, rate * 1e-9, b->t1 - b->t_data, b->t_wait[0], b->t_wait[1], t_join, bytes ? "" : "  FAILED");
    if (t_write) *t_write = (b->abandoned ? b->t_cancel : b->t1) - b->t0;
    if (t_wait) *t_wait = b->t_wait[0] + b->t_wait[1] + t_join;
    free(b);
    return bytes;
}
int bs_ckpt_tree_find(unsigned long N) { return bs_ckpt_dir ? ckpt_find_kind("tree", N) : 0; }
int bs_ckpt_tree_read(int level, unsigned long N, uint64_t desc[10], dbig *P, dbig *Q)
{
    struct ckpt_hdr h; struct ckpt_ext x;
    if (!ckpt_read_hdr("tree", level, &h, &x, N, 1)) return 0;
    memcpy(desc, x.tree, sizeof x.tree);
    db_init(P); db_init(Q);
    if (desc[4]) { db_reserve(P, desc[4]); P->n = desc[4]; }
    if (desc[9]) { db_reserve(Q, desc[9]); Q->n = desc[9]; }
    int oks[NRMAX];
#pragma omp parallel for num_threads(NR) schedule(static, 1)
    for (int r = 0; r < NR; r++) oks[r] = tree_io(level, P, Q, r, 0, 0);
    for (int r = 0; r < NR; r++) if (!oks[r]) return 0;
    return 1;
}
/* every tree set of this node above `level` (a fresh run: the sets of an earlier run in the directory go) */
void bs_ckpt_tree_clear(int level) { if (bs_ckpt_dir) for (int l = level + 1; l < 128; l++) ckpt_remove_kind("tree", l); }
/* the seeds of level 0 into the per-region staging: region r's spans computed by the node's threads (or by
 * every thread when unpinned) with the schoolbook, P and Q of each span side by side (per limbs each) */
static void seeds_compute(struct level *cur, size_t per, unsigned long S, unsigned long N, const size_t *r0, uint64_t **stage)
{
    int nt = getenv("BS_SEED_THREADS") ? atoi(getenv("BS_SEED_THREADS")) : bs_nthreads();   /* I2: fewer than all leaves cores to init's allocations */
#pragma omp parallel num_threads(nt)
    {
        bigint p, q; bi_init(&p); bi_init(&q);
        int rk, home, cnt = bs_rt(&rk, &home);   /* region-aware: this node's threads do this region's spans */
        for (int r = 0; r < NR; r++) {
            if (home >= 0 && r % NR != home % NR) continue;               /* (home < 0: threads not pinned, every thread does everything by rank) */
            size_t lo = r0[r], hi = r0[r + 1];
            for (size_t i = lo + (size_t)rk; i < hi; i += (size_t)cnt) {
                unsigned long a = bs_a0 + i * S, b = a + S, bend = bs_b1 ? bs_b1 : N + 1; if (b > bend) b = bend;
                span(&p, &q, a, b);
                struct node *nd = &cur->nd[i];
                nd->r = r; nd->po = 2 * per * (i - lo); nd->pn = p.n; nd->qo = nd->po + per; nd->qn = q.n;
                if (p.n > per || q.n > per) { ec_fatal(EC_RC_FATAL, "bs: seed span overflow\n"); }
                memcpy(stage[r] + nd->po, p.l, p.n * 8); memcpy(stage[r] + nd->qo, q.l, q.n * 8);
            }
        }
        bi_free(&p); bi_free(&q);
    }
}
/* ---- the seeds streamed to the device regions through two small pinned buffers.  Before, region r's
 * spans were computed into a region-sized pinned staging on APU r (4 x 10.7 GB at 4e10: the largest pinned item of the
 * run) and copied to the region in one DMA after all of them.  Now the spans go in chunks of BS_SEED_CHUNK_MB (2048):
 * the threads compute chunk c into buffer c & 1 and issue its DMA to the region (hipMemcpyAsync from pinned memory on a
 * non-blocking stream: on the null stream it queued behind init's hipMemset of the plane pools, 4 GB/s), and the buffer
 * is refilled two chunks later once that copy has landed (0.4 s of computing per 2 GiB against 0.1 s of DMA).  The DMA needs
 * the region pools, so binsplit_seeds_begin (I2: called from rns_init once the staging exists, before the plane pools)
 * allocates them first (binsplit_pregrow: the same bytes, mapped before rns_init's plane pools) and then
 * starts the seed thread; a chunk waits for the pools only if they are not there yet (they are: 4e10 measured below).
 * The same function runs synchronously in binsplit_e when the seeds were not started at init (BS_RESTART, or a caller
 * without the init hook such as tests/t_bs).  The
 * pinned staging of rns_init is no longer used by the seeds (1 GiB per APU remains for the checkpoints' chunks). ---- */
struct seed_stream {
    uint64_t *buf[2]; size_t bytes, chunk_spans; int buf_dev[2];         /* buf_dev: the device of the copy in flight from the buffer (-1: none) */
    struct { int r, b; size_t c0, c1; } pend[2]; int npend;              /* chunks computed into the buffers while the regions did not exist yet */
    uint64_t *pool[NRMAX]; pthread_mutex_t mx; pthread_cond_t cv; int pools_ready;
    double t_span, t_wait_pool, t_wait_dma, t_alloc, t_issue, t_free, t_issue_max; int nchunks, nbuf;   /* t_issue: inside hipMemcpyAsync (blocked while the main thread's hipMalloc holds the runtime); nbuf: chunks that went through a buffer */
};
static void seed_stream_pools(struct seed_stream *ss, uint64_t **pool)   /* the region pools exist: the DMAs may start */
{
    pthread_mutex_lock(&ss->mx); for (int r = 0; r < NR; r++) ss->pool[r] = pool[r]; ss->pools_ready = 1; pthread_cond_broadcast(&ss->cv); pthread_mutex_unlock(&ss->mx);
}
static int seed_pools_ready(struct seed_stream *ss) { pthread_mutex_lock(&ss->mx); int r = ss->pools_ready; pthread_mutex_unlock(&ss->mx); return r; }
/* the spans [c0, c1) of region r (first span lo) computed by all threads into dst (dst_c0 = the address of span c0; pad: zero the
 * slack after p and q -- the device regions are zeroed at their allocation, a buffer is not) */
static void seed_spans(struct level *cur, size_t per, unsigned long S, unsigned long N, int r, size_t lo, size_t c0, size_t c1, uint64_t *dst_c0, int pad, int nt)
{
#pragma omp parallel num_threads(nt)
    {
        bigint p, q; bi_init(&p); bi_init(&q);
#pragma omp for schedule(dynamic, 16)
        for (size_t i = c0; i < c1; i++) {
            unsigned long a = bs_a0 + i * S, bb = a + S, bend = bs_b1 ? bs_b1 : N + 1; if (bb > bend) bb = bend;
            span(&p, &q, a, bb);
            struct node *nd_ = &cur->nd[i];
            nd_->r = r; nd_->po = 2 * per * (i - lo); nd_->pn = p.n; nd_->qo = nd_->po + per; nd_->qn = q.n;
            if (p.n > per || q.n > per) { ec_fatal(EC_RC_FATAL, "bs: seed span overflow\n"); }
            uint64_t *dst = dst_c0 + 2 * per * (i - c0);
            memcpy(dst, p.l, p.n * 8); if (pad) memset(dst + p.n, 0, (per - p.n) * 8);
            memcpy(dst + per, q.l, q.n * 8); if (pad) memset(dst + per + q.n, 0, (per - q.n) * 8);
        }
        bi_free(&p); bi_free(&q);
    }
}
static void seeds_stream(struct seed_stream *ss, struct level *cur, size_t per, unsigned long S, unsigned long N, const size_t *r0)
{
    double t0 = mem_now(); int nd = mem_device_count();
    size_t mb = getenv("BS_SEED_CHUNK_MB") ? (size_t)atol(getenv("BS_SEED_CHUNK_MB")) : (db_pool_vmm_on() ? 8192 : 2048), bytes = mb << 20, span_bytes = 2 * per * 8, mx = 0;
    for (int r = 0; r < NR; r++) { size_t b = span_bytes * (r0[r + 1] - r0[r]); if (b > mx) mx = b; }
    if (bytes > mx) bytes = mx;
    if (bytes < span_bytes) bytes = span_bytes;
    ss->chunk_spans = bytes / span_bytes; ss->bytes = ss->chunk_spans * span_bytes;
    for (int b = 0; b < 2; b++) { ss->buf[b] = (uint64_t *)mem_hstage_alloc(b % (nd > 1 ? 2 : 1), ss->bytes, 0, 0); ss->buf_dev[b] = -1; }
    ss->t_alloc = mem_now() - t0;
    db_tl("seed thread: buffers allocated (%.2f s)", ss->t_alloc);   /* ECALC_INIT_TL=1 */
    int nt = getenv("BS_SEED_THREADS") ? atoi(getenv("BS_SEED_THREADS")) : bs_nthreads();   /* I2: fewer than all leaves cores to init's allocations */
    ss->nchunks = ss->nbuf = ss->npend = 0; ss->t_span = ss->t_wait_pool = ss->t_wait_dma = ss->t_issue = ss->t_free = 0;
    int direct = !db_pool_vmm_on();                              /* the host cannot store into a VMM range */
    int pull = !direct;                                          /* K8b (archive:results_merged_2026-10-24/MEMORY.md §K8b): the copies by the persistent pull kernels (no runtime call in the mapping window; fallback BS_SEED_PULL=0 removed 2026-10-23, Q87) */
    void (*cpy)(int, void *, const void *, size_t) = direct ? mem_dev_copy_async : db_pull_copy_async;   /* into a VMM range: by a kernel (hipMemcpyAsync ran at 6 GB/s, synchronously) */
    void (*cwait)(int) = direct ? mem_dev_copy_wait : db_pull_wait;
    if (pull) for (int b = 0; b < 2; b++) db_pull_resolve(ss->buf[b]);   /* K8b: the buffers' device pointers once, here (the copies make no runtime call) */
    for (int r = 0; r < NR; r++) {
        size_t lo = r0[r], hi = r0[r + 1];
        for (size_t c0 = lo; c0 < hi; c0 += ss->chunk_spans) {
            size_t c1 = c0 + ss->chunk_spans < hi ? c0 + ss->chunk_spans : hi; int b = ss->nchunks & 1;
            if (!direct && ss->npend == 2) {                       /* a VMM arena takes no CPU stores -- the two buffered chunks are DMA'd (the regions must exist) and the buffers reused */
                double tw = mem_now(); int wl = !seed_pools_ready(ss); if (wl) db_tl("seed thread: %d chunks computed, waiting for the region pools", ss->nchunks);
                pthread_mutex_lock(&ss->mx); while (!ss->pools_ready) pthread_cond_wait(&ss->cv, &ss->mx); pthread_mutex_unlock(&ss->mx); ss->t_wait_pool += mem_now() - tw;
                if (wl) db_tl("seed thread: the region pools are there (waited %.2f s)", mem_now() - tw);
                tw = mem_now();
                for (int k = 0; k < ss->npend; k++) { int rr = ss->pend[k].r, dev = reg_dev(rr, nd); double ti = mem_now(); cpy(dev, ss->pool[rr] + 2 * per * (ss->pend[k].c0 - r0[rr]), ss->buf[ss->pend[k].b], 2 * per * (ss->pend[k].c1 - ss->pend[k].c0) * 8); ss->buf_dev[ss->pend[k].b] = dev; ti = mem_now() - ti; if (ti > ss->t_issue_max) ss->t_issue_max = ti; }
                ss->t_issue += mem_now() - tw; tw = mem_now();
                for (int bb = 0; bb < 2; bb++) if (ss->buf_dev[bb] >= 0) { cwait(ss->buf_dev[bb]); ss->buf_dev[bb] = -1; }
                ss->t_wait_dma += mem_now() - tw;
                ss->npend = 0;
            }
            if (direct && (seed_pools_ready(ss) || ss->npend == 2)) {   /* into the region itself (CPU stores into device memory); the regions not there yet: through a buffer (two at most, then wait) */
                double tw = mem_now(); pthread_mutex_lock(&ss->mx); while (!ss->pools_ready) pthread_cond_wait(&ss->cv, &ss->mx); pthread_mutex_unlock(&ss->mx); ss->t_wait_pool += mem_now() - tw;
                double ts = mem_now();
                seed_spans(cur, per, S, N, r, lo, c0, c1, ss->pool[r] + 2 * per * (c0 - lo), 0, nt);
                ss->t_span += mem_now() - ts; ss->nchunks++;
                continue;
            }
            double ts = mem_now();                                 /* (the per-chunk DMA path BS_SEED_DIRECT=0 is gone -- a buffered chunk is copied once the regions exist, below) */
            seed_spans(cur, per, S, N, r, lo, c0, c1, ss->buf[b], 1, nt);
            ss->t_span += mem_now() - ts; ss->nbuf++;
            ss->pend[ss->npend].r = r; ss->pend[ss->npend].b = b; ss->pend[ss->npend].c0 = c0; ss->pend[ss->npend].c1 = c1; ss->npend++; ss->nchunks++;
        }
    }
    db_tl("seed thread: every span computed (%d chunks, spans %.2f s)", ss->nchunks, ss->t_span);
    double tw = mem_now(); pthread_mutex_lock(&ss->mx); while (!ss->pools_ready) pthread_cond_wait(&ss->cv, &ss->mx); pthread_mutex_unlock(&ss->mx); ss->t_wait_pool += mem_now() - tw;
    tw = mem_now();
    for (int k = 0; k < ss->npend; k++) { int r = ss->pend[k].r, dev = reg_dev(r, nd); double ti = mem_now(); cpy(dev, ss->pool[r] + 2 * per * (ss->pend[k].c0 - r0[r]), ss->buf[ss->pend[k].b], 2 * per * (ss->pend[k].c1 - ss->pend[k].c0) * 8); ss->buf_dev[ss->pend[k].b] = dev; ti = mem_now() - ti; if (ti > ss->t_issue_max) ss->t_issue_max = ti; }   /* S2: the region's device (a process or vPE: its own APU; was r % nd = APU0 for every one-region PE, whose pull kernel runs only on its own APU: NULL control block, 9u3) */
    ss->t_issue += mem_now() - tw; tw = mem_now();
    for (int b = 0; b < 2; b++) if (ss->buf_dev[b] >= 0) { cwait(ss->buf_dev[b]); ss->buf_dev[b] = -1; }
    ss->t_wait_dma += mem_now() - tw;
    if (pull) db_pull_stop_all();                                /* K8b: the kernels leave before the buffers are unregistered */
    tw = mem_now(); for (int b = 0; b < 2; b++) { mem_hstage_free(ss->buf[b]); ss->buf[b] = 0; } ss->t_free = mem_now() - tw;
}
static size_t seed_region_limbs(size_t per, const size_t *r0, int r) { return 2 * per * (r0[r + 1] - r0[r]) + 2; }
/* the seeds computed in a background thread during init's device allocations (archive:PLAN §16); binsplit_e
 * joins and takes the level-0 table from here.  B2: streamed to the regions as they are computed (above) */
static struct { int active, joined; pthread_t th; unsigned long N, nspan; size_t per, r0[NRMAX + 1]; struct node *nd; double t; struct seed_stream ss; } g_pre;
static void *pre_seeds_run(void *a)
{
    vpe_adopt();                                      /* a helper thread of this PE (a no-op with the switch off) */
    double t0 = mem_now(); struct level cur; memset(&cur, 0, sizeof cur); cur.n = g_pre.nspan; cur.nd = g_pre.nd;
    db_tl("seed thread starts (%s threads)", getenv("BS_SEED_THREADS") ? getenv("BS_SEED_THREADS") : "all");
    seeds_stream(&g_pre.ss, &cur, g_pre.per, bs_seed_terms, g_pre.N, g_pre.r0);
    g_pre.t = mem_now() - t0; db_tl("seed thread ends (%.2f s)", g_pre.t); return 0;
}
void binsplit_seeds_begin(unsigned long N)
{
    seed_limbs(N, &g_pre.per, &g_pre.nspan); g_pre.N = N;
    for (int r = 0; r <= NR; r++) { g_pre.r0[r] = 0; while (g_pre.r0[r] < g_pre.nspan && region_of(g_pre.r0[r], g_pre.nspan) < r) g_pre.r0[r]++; }
    /* the seeds alongside the plane pools' mapping */
    g_pre.nd = (struct node *)calloc(g_pre.nspan, sizeof *g_pre.nd);
    memset(&g_pre.ss, 0, sizeof g_pre.ss); pthread_mutex_init(&g_pre.ss.mx, 0); pthread_cond_init(&g_pre.ss.cv, 0);
    pthread_create(&g_pre.th, 0, pre_seeds_run, 0); g_pre.active = 1;
    /* B2: the region pools now (the main thread, inside rns_init: the arenas before the plane pools), then the DMAs may start */
    double tp = mem_now(); binsplit_pregrow(N);
    uint64_t *pool[NRMAX]; for (int r = 0; r < NR; r++) pool[r] = pool_get(0, r, seed_region_limbs(g_pre.per, g_pre.r0, r));
    seed_stream_pools(&g_pre.ss, pool);
    if (db_pool_vmm_on()) db_pull_start_all();   /* K8b: the pull kernels now, with no mapping in flight (the background mapping is released after the hook) */
    if (bs_verbose || (getenv("ECALC_VERBOSE") && atoi(getenv("ECALC_VERBOSE")) >= 2)) printf("      init: region pools %.2f s (inside rns_init, before the plane pools: the seeds stream into them)\n", mem_now() - tp);
}
void binsplit_e(bigint *P, bigint *Q, unsigned long N)
{
    double t0 = mem_now(), t;
    memset(&bs_st, 0, sizeof bs_st); bs_N = N;
    unsigned long S = bs_seed_terms, nspan; size_t per;
    /* seed spans: Q(a,b) < b^S, P < S b^S: reserve (S log2(N+1) + 64 + 64) / 64 limbs each */
    seed_limbs(N, &per, &nspan); S = bs_seed_terms;           /* seed_limbs sets it (BS_SEED_TERMS, BS_SEED_FILL) */
    struct level cur, nxt;
    int which = 0;
    cur.n = nspan; cur.nd = (struct node *)calloc(nspan, sizeof *cur.nd);
    size_t r0[NRMAX + 1];                               /* first node of each region at level 0 */
    for (int r = 0; r <= NR; r++) { r0[r] = 0; while (r0[r] < nspan && region_of(r0[r], nspan) < r) r0[r]++; }
    /* region pools sized once from level 0 (the levels' totals stay within a few percent of it;
     * pool_get still grows if a level needs more); binsplit_pregrow does this at init, outside the timed phase */
    size_t total0 = 2 * per * nspan;
    mem_pool_guard = 0;                              /* the level-0 layout may size the regions (a second run in one process re-lays them) */
    binsplit_pregrow(N);
    for (int r = 0; r < NR; r++) cur.pool[r] = pool_get(0, r, 2 * per * (r0[r + 1] - r0[r]) + 2);
    if (bs_st.peak_pool_limbs < total0) bs_st.peak_pool_limbs = total0;
    t = mem_now();
    /* B2: device regions: the seeds streamed into them in chunks through two pinned buffers (seeds_stream) -- computed during
     * init (I2) or now; host regions: a host staging area per region (small scattered writes into device memory are slow,
     * RESULTS.md 56), then one copy per region */
    int own_stage = mem_dev_of(cur.pool[0]) < 0;
    if (g_pre.active && !own_stage && g_pre.N == N) {                      /* I2: computed (and streamed) during init; take the table */
        db_tl("bs: joining the seed thread");
        if (!g_pre.joined) pthread_join(g_pre.th, 0);
        g_pre.active = g_pre.joined = 0;
        db_tl("bs: the seeds are joined");
        free(cur.nd); cur.nd = g_pre.nd; g_pre.nd = 0;
        for (int r = 0; r < NR; r++) if (g_pre.ss.pool[r] != cur.pool[r]) { ec_fatal(EC_RC_FATAL, "bs: region %d's pool moved after the seeds were streamed into it\n", r); }
        if (bs_verbose) printf("bs: seeds were computed during init (%.2f s: buffers %.2f + %.2f, spans %.2f, waited %.2f for the regions, %.2f for the DMA, %.2f issuing it (max %.2f); %d chunks of %zu MB, %d through a buffer%s)\n",
                               g_pre.t, g_pre.ss.t_alloc, g_pre.ss.t_free, g_pre.ss.t_span, g_pre.ss.t_wait_pool, g_pre.ss.t_wait_dma, g_pre.ss.t_issue, g_pre.ss.t_issue_max, g_pre.ss.nchunks, g_pre.ss.bytes >> 20, g_pre.ss.nbuf, ", the rest stored into the regions");
    } else if (!own_stage) {
        if (g_pre.active) { if (!g_pre.joined) pthread_join(g_pre.th, 0); g_pre.active = g_pre.joined = 0; free(g_pre.nd); g_pre.nd = 0; }
        struct seed_stream ss; memset(&ss, 0, sizeof ss); pthread_mutex_init(&ss.mx, 0); pthread_cond_init(&ss.cv, 0);
        seed_stream_pools(&ss, cur.pool);
        seeds_stream(&ss, &cur, per, S, N, r0);
        if (bs_verbose) printf("bs: seeds streamed to the regions: buffers %.2f + %.2f s, spans %.2f, waited %.2f for the DMA, %.2f issuing it; %d chunks of %zu MB, %d through a buffer\n", ss.t_alloc, ss.t_free, ss.t_span, ss.t_wait_dma, ss.t_issue, ss.nchunks, ss.bytes >> 20, ss.nbuf);
    } else {
        if (g_pre.active) { if (!g_pre.joined) pthread_join(g_pre.th, 0); g_pre.active = g_pre.joined = 0; free(g_pre.nd); g_pre.nd = 0; }
        uint64_t *stage[NRMAX];
        for (int r = 0; r < NR; r++) stage[r] = (uint64_t *)malloc((2 * per * (r0[r + 1] - r0[r]) + 2) * 8);
        seeds_compute(&cur, per, S, N, r0, stage);
#pragma omp parallel for num_threads(NR) schedule(static, 1)
        for (int r = 0; r < NR; r++) { size_t limbs = 2 * per * (r0[r + 1] - r0[r]); region_copy(cur.pool[r], stage[r], limbs, r); free(stage[r]); }   /* by DMA when the pool is a VMM range (region_copy decides) */
    }
    bs_st.t_seed = mem_now() - t;
    if (bs_verbose) printf("bs: %lu terms, %lu spans of %lu, seeds %.2f s%s\n", N, nspan, S, bs_st.t_seed, g_pre.N == N && !own_stage ? " (waiting for the init thread)" : "");
    if (bs_after_seeds_hook) bs_after_seeds_hook(bs_hook_arg);   /* the CPU is free from here until the top level */

    mem_pool_guard = 1;                              /* from here nothing may grow (the reciprocal and the division keep it set) */
    if (getenv("ECALC_FATAL_TEST") && atoi(getenv("ECALC_FATAL_TEST")) == 1) {   /* the four APU threads fail at once, as the in-phase guards do */
#pragma omp parallel num_threads(NR)
        {
#pragma omp barrier
            ec_fatal(EC_RC_FATAL, "ECALC_FATAL_TEST=1: a forced failure in bs's worker %d of %d (all at once, after the seeds)", omp_get_thread_num(), NR);
        }
    }
    /* N3 (rns_mul.c; the fallback BS_BATCH_PIPE=0 removed 2026-10-23, Q87): the level's host passes over its nodes in parallel -- the max_nl scan, the layout
     * region by region (a large level's regions are contiguous index ranges, so each region's running offset is the
     * serial one), the product list in one buffer kept across the levels, the lengths read back -- the same layout */
    rns_prod *pr_buf = 0; size_t pr_cap = 0;
    while (cur.n > 1) {
        t = mem_now();
        size_t npairs = cur.n / 2, odd = cur.n & 1, max_nl = 0;
        int par = npairs >= 4096 && npairs + odd > (size_t)bs_balance_n;   /* N3: a large level (place_node = region_of) */
        if (par) {
#pragma omp parallel for num_threads(bs_nthreads()) schedule(static) reduction(max : max_nl)
            for (size_t i = 0; i < cur.n; i++) { size_t m = cur.nd[i].pn > cur.nd[i].qn ? cur.nd[i].pn : cur.nd[i].qn; if (m > max_nl) max_nl = m; }
        } else
        for (size_t i = 0; i < cur.n; i++) { if (cur.nd[i].pn > max_nl) max_nl = cur.nd[i].pn; if (cur.nd[i].qn > max_nl) max_nl = cur.nd[i].qn; }
        /* next level layout: P slot n(P1)+n(Q2)+1, Q slot n(Q1)+n(Q2) */
        nxt.n = npairs + odd; nxt.nd = (struct node *)calloc(nxt.n, sizeof *nxt.nd);
        size_t offr[NRMAX] = {0}, off = 0;
        if (par) {                                   /* N3: region r holds pairs [ceil(r n / NR), ceil((r+1) n / NR)) of the n = nxt.n nodes */
            size_t lo[NRMAX + 1]; for (int r = 0; r <= NR; r++) { size_t v = ((size_t)r * nxt.n + NR - 1) / NR; lo[r] = v < npairs ? v : npairs; }
            lo[NR] = npairs; int bad = 0;
#pragma omp parallel for num_threads(NR) schedule(static, 1) reduction(|| : bad)
            for (int r = 0; r < NR; r++) {
                size_t acc = 0;
                for (size_t i = lo[r]; i < lo[r + 1]; i++) {
                    struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                    o->r = place_node(i, nxt.n, offr, a->r, b->r); if (o->r != r) bad = 1;
                    o->po = acc; acc += a->pn + b->qn + 1; o->qo = acc; acc += a->qn + b->qn;
                    o->pn = o->qn = 0;
                }
                offr[r] = acc;
            }
            if (bad) ec_fatal(EC_RC_FATAL, "bs: a level's regions are not contiguous (level %d, %zu nodes)\n", bs_st.levels + 1, nxt.n);
        } else
        for (size_t i = 0; i < npairs; i++) {
            struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
            o->r = place_node(i, nxt.n, offr, a->r, b->r);   /* C2: balanced at the small top levels */
            o->po = offr[o->r]; offr[o->r] += a->pn + b->qn + 1; o->qo = offr[o->r]; offr[o->r] += a->qn + b->qn;
            o->pn = o->qn = 0;
        }
        if (odd) { struct node *a = &cur.nd[cur.n - 1], *o = &nxt.nd[npairs]; o->r = place_node(npairs, nxt.n, offr, a->r, -1); o->po = offr[o->r]; offr[o->r] += a->pn; o->qo = offr[o->r]; offr[o->r] += a->qn; o->pn = a->pn; o->qn = a->qn; }
        which ^= 1;
        int mdev_level = 2 * max_nl + 1 > ((size_t)1 << bs_mdev_logl);
        for (int r = 0; r < NR; r++) off += offr[r];
        if (mdev_level) for (int r = 0; r < NR; r++) nxt.pool[r] = 0;   /* the mdev levels' results are device numbers */
        else for (int r = 0; r < NR; r++) nxt.pool[r] = pool_get(which, r, offr[r] + 2);
        if (off > bs_st.peak_pool_limbs) bs_st.peak_pool_limbs = off;
        if (bs_st.levels < 3) db_tl("bs: level %d starts (its pools are there)", bs_st.levels + 1);   /* ECALC_INIT_TL=1 */
        double tl1 = 0, tl2 = 0;
        const char *tier; int normed = 0, finished = 0;
        if (2 * max_nl + 1 <= ((size_t)1 << bs_mdev_logl)) {
            tier = "batch"; bs_st.batch_levels++;
            rns_prod *pr;
            /* N3: one buffer for all the levels (level 1's size), every field set */
            if (pr_cap < 2 * npairs) { free(pr_buf); pr_cap = 2 * npairs; pr_buf = (rns_prod *)malloc(pr_cap * sizeof *pr_buf); }
            pr = pr_buf;
#pragma omp parallel for num_threads(bs_nthreads()) schedule(static) if (par)
            for (size_t i = 0; i < npairs; i++) {
                struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                rns_prod *p0 = &pr[2 * i], *p1 = &pr[2 * i + 1];
                p0->a = NODE_P(cur, a); p0->na = a->pn; p0->b = NODE_Q(cur, b); p0->nb = b->qn; p0->c = NODE_P(nxt, o);
                p0->x = NODE_P(cur, b); p0->nx = b->pn; p0->ncn = 0;
                p1->a = NODE_Q(cur, a); p1->na = a->qn; p1->b = p0->b; p1->nb = b->qn; p1->c = NODE_Q(nxt, o);
                p1->x = 0; p1->nx = 0; p1->ncn = 0;
            }

            tl1 = mem_now();
            rns_mul_batch(pr, 2 * npairs);
            tl2 = mem_now();
            normed = 1;
            /* N3: all lengths, then the flag (a missing one: node_norm below redoes them all) */
            int ok = 1;
#pragma omp parallel for num_threads(bs_nthreads()) schedule(static) if (par) reduction(&& : ok)
            for (size_t i = 0; i < npairs; i++) { ok = ok && pr[2 * i].ncn && pr[2 * i + 1].ncn; nxt.nd[i].pn = pr[2 * i].ncn; nxt.nd[i].qn = pr[2 * i + 1].ncn; }
            normed = ok;

        } else {
            tier = "mdev"; bs_st.mdev_levels++;
            {
                /* the top levels on the device tier: operands are region-pool views or the children's device
                 * numbers; results are device numbers (blocks from the region pools no longer needed) */
                if (!cur.nd[0].pd) donate_pools(which);                   /* the first such level: the other parity's pools are free */
                dm_switches(); int pair_free = dm_tight > 0;   /* each pair's inputs freed as consumed (the children as device numbers) */
                for (size_t i = 0; i < npairs; i++) {
                    struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                    dbig A1 = node_p(&cur, a), A2 = node_q(&cur, a), B = node_q(&cur, b), P2 = node_p(&cur, b);
                    o->pd = (dbig *)calloc(1, sizeof(dbig)); o->qd = (dbig *)calloc(1, sizeof(dbig));
                    rns_mul_dist_db(o->pd, &A1, &B);
                    db_add(o->pd, o->pd, &P2);                              /* P = P1 Q2 + P2 */
                    if (pair_free) { if (a->pd) db_free(a->pd); if (b->pd) db_free(b->pd); }   /* P1, P2 are dead: Q = Q1 Q2 runs beside 2 inputs + 2 outputs */
                    rns_mul_dist_db(o->qd, &A2, &B);
                    if (pair_free) { if (a->qd) db_free(a->qd); if (b->qd) db_free(b->qd); }
                    o->pn = o->pd->n; o->qn = o->qd->n;
                }
                if (odd) { struct node *a = &cur.nd[cur.n - 1], *o = &nxt.nd[npairs];
                           if (a->pd) { o->pd = a->pd; o->qd = a->qd; a->pd = a->qd = 0; }
                           else { o->pd = (dbig *)calloc(1, sizeof(dbig)); o->qd = (dbig *)calloc(1, sizeof(dbig)); dbig vp = node_p(&cur, a), vq = node_q(&cur, a); db_copy(o->pd, &vp); db_copy(o->qd, &vq); }
                           o->pn = o->pd->n; o->qn = o->qd->n; }
                for (size_t i = 0; i < cur.n; i++) { if (cur.nd[i].pd) { db_free(cur.nd[i].pd); free(cur.nd[i].pd); } if (cur.nd[i].qd) { db_free(cur.nd[i].qd); free(cur.nd[i].qd); } cur.nd[i].pd = cur.nd[i].qd = 0; }
                if (cur.pool[0] && mem_dev_of(cur.pool[0]) >= 0) donate_pools(which ^ 1);   /* the children's pools are consumed */
                if (nxt.n == 1 && dm_tight > 0) db_pool_pack_large(2);   /* from here the dm phase's large blocks take the tail first (t1), then the highest end (r2 below Q) */
                if (nxt.n == 1) {
                    if (bs_keep_dev) { bs_Pd = *nxt.nd[0].pd; bs_Qd = *nxt.nd[0].qd; P->n = Q->n = 0; }   /* the caller copies them out (overlapped with the reciprocal) */
                    else { db_to_bi(P, nxt.nd[0].pd); db_to_bi(Q, nxt.nd[0].qd); db_free(nxt.nd[0].pd); db_free(nxt.nd[0].qd); }
                    free(nxt.nd[0].pd); free(nxt.nd[0].qd); nxt.nd[0].pd = nxt.nd[0].qd = 0; finished = 1; }
                normed = 1;
            }
        }
        if (normed || finished) {                       /* lengths came back from the device-local batch path, or the top is done */
        } else if (npairs >= 64) {
#pragma omp parallel num_threads(bs_nthreads())
            {
                int rk, home, cnt = bs_rt(&rk, &home);
                for (size_t i = (size_t)rk; i < npairs; i += (size_t)cnt) {
                    struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                    if (home >= 0 && o->r % NR != home % NR) continue;
                    o->pn = node_norm(NODE_P(nxt, o), a->pn + b->qn + 1);
                    o->qn = node_norm(NODE_Q(nxt, o), a->qn + b->qn);
                }
            }
        } else for (size_t i = 0; i < npairs; i++) {
            struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
            o->pn = node_norm(NODE_P(nxt, o), a->pn + b->qn + 1);
            o->qn = node_norm(NODE_Q(nxt, o), a->qn + b->qn);
        }
        if (odd && !mdev_level) { struct node *a = &cur.nd[cur.n - 1], *o = &nxt.nd[npairs];
                   region_copy(NODE_P(nxt, o), NODE_P(cur, a), a->pn, o->r); region_copy(NODE_Q(nxt, o), NODE_Q(cur, a), a->qn, o->r); }
        double dt = mem_now() - t, tl3 = mem_now();
        if (bs_verbose && tl2) printf("bs:   layout %.3f  batch %.3f  add+norm %.3f\n", tl1 - t, tl2 - tl1, tl3 - tl2);
        if (!strcmp(tier, "school")) bs_st.t_school += dt; else if (!strcmp(tier, "batch")) bs_st.t_batch += dt; else bs_st.t_mdev += dt;
        bs_st.levels++;
        if (bs_st.levels <= 3) db_tl("bs: level %d done (%s, %.2f s)", bs_st.levels, tier, dt);   /* ECALC_INIT_TL=1 */
        if (bs_verbose) printf("bs: level %2d %-6s %8zu pairs  max_nl %10zu  pool %6.2f GB  %.2f s  (batch %.2f: scatter %.2f ntt %.2f crt %.2f merge %.2f)\n", bs_st.levels, tier, npairs, max_nl, off * 8e-9, dt, rns_st.tb_total, rns_st.tb_scatter, rns_st.tb_ntt, rns_st.tb_crt, rns_st.tb_merge);
        if (!vpe_on()) memset(&rns_st, 0, sizeof rns_st);   /* (PE layout: not reset here) */
        if (mem_live_on()) { char w[64]; snprintf(w, sizeof w, "bs level %d %s", bs_st.levels, tier); mem_live_line(w); }
        free(cur.nd);
        cur = nxt;
        if (finished) { free(cur.nd); cur.nd = 0; break; }
    }
    if (cur.nd && bs_keep_dev && cur.pool[cur.nd[0].r] && mem_dev_of(cur.pool[cur.nd[0].r]) >= 0) {
        /* a top level (size 1) or leaf (size > 1) that ends on the batch tier leaves P, Q in a region pool; the dm phase
         * (size 1) or the tree (mn_tree) takes them as device numbers and its blocks from the block pool, which held nothing
         * yet (11-44 GB of hipMalloc per process, A-mem open issue 3).  So: the idle parity goes to the block pool,
         * P and Q are copied into blocks from it, and the parity that held the level follows -- the same hand-over
         * the device tier makes at its first level, one level later.  (SIMP: size 1 too -- one single-node division flow) */
        donate_pools(which ^ 1);
        dbig vp = node_p(&cur, &cur.nd[0]), vq = node_q(&cur, &cur.nd[0]);
        db_init(&bs_Pd); db_init(&bs_Qd); db_copy(&bs_Pd, &vp); db_copy(&bs_Qd, &vq);
        donate_pools(which);
        P->n = Q->n = 0;
        if (bs_verbose) printf("bs: leaf P %zu, Q %zu limbs copied to device numbers; the regions went to the block pool before the tree\n", bs_Pd.n, bs_Qd.n);
    } else if (cur.nd) {
        bi_reserve(P, cur.nd[0].pn); region_copy(P->l, NODE_P(cur, &cur.nd[0]), cur.nd[0].pn, cur.nd[0].r); P->n = cur.nd[0].pn;
        bi_reserve(Q, cur.nd[0].qn); region_copy(Q->l, NODE_Q(cur, &cur.nd[0]), cur.nd[0].qn, cur.nd[0].r); Q->n = cur.nd[0].qn;
    }
    free(cur.nd);
    free(pr_buf);                                    /* N3 */
    bs_st.t_total = mem_now() - t0;
}
unsigned long bs_a0 = 1, bs_b1;   /* M2: this process's (vPE's) term range [a0, b1) (b1 = 0: all of [1, N+1)) */
void (*bs_after_seeds_hook)(void *); void *bs_hook_arg;   /* called once the seeds are in the regions */
int bs_keep_dev; dbig bs_Pd, bs_Qd;     /* the top level's P, Q left on device */
void binsplit_free_pools(void)
{
    for (int w = 0; w < 2; w++) for (int r = 0; r < NR; r++) {
        if (g_pool[w][r]) {
            int dev = mem_dev_of(g_pool[w][r]);
            if (dev >= 0) donate_one(w, r);           /* WP5: the device regions go to the dbig block allocator (the dm phase's blocks) */
            else mem_hreg_free(g_pool[w][r]);
        }
        g_pool[w][r] = 0; g_cap[w][r] = 0;
    }
    rns_shutdown_hook = binsplit_release_arenas;       /* the arenas stay for the block pool; released at rns_shutdown */
}

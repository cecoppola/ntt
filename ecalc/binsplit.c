/* binsplit.c - see binsplit.h */
#include <stdio.h>
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
/* M6: the node-process rank/size (the checkpoint names and headers) and the tree-level restart, from mn.c (mn.h needs the HIP headers; this is a C file) */
int mn_rank(void); int mn_size(void); int mn_ckpt_tree_level(unsigned long N);
unsigned long bs_N = 0;                              /* M6: the run's N, for the tree sets written by mn.c */

bs_stats bs_st;
int bs_seed_terms = 256;                             /* BS_SEED_TERMS: seed span; 256 measured best in both bases (RESULTS.md 58), 512 was the paper-era value */
int bs_school_nl = 0;                                /* BS_SCHOOL_NL: CPU schoolbook tier below this many limbs; 0 = never (WP4: the device batch tier is faster at any size, and the pools are device memory) */
int bs_verbose = 0;
const char *bs_ckpt_dir = 0;                         /* BS_CKPT_DIR: WP7 per-level checkpoints of the level loop; unset = none */
int bs_ckpt_every = 4;                               /* BS_CKPT_EVERY: a checkpoint every this many levels */
int bs_ckpt_min_level = 16;                          /* BS_CKPT_MIN_LEVEL: only the top levels, where the time is (a snapshot costs
                                                      * a full pass of the pools: 5.7 s per 8 GB at 10^10, RESULTS.md 61; from level
                                                      * 16 a 24-level run writes two) */
size_t bs_ckpt_min_bytes = (size_t)64 << 30;         /* ... unless the level's pool is already this large */
int bs_restart = 0;                                  /* BS_RESTART=1: resume from the latest complete checkpoint in bs_ckpt_dir */

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
struct node { size_t po, pn, qo, qn; int r; dbig *pd, *qd; };   /* pd/qd: the node as device numbers (top levels on the device tier, WP5) */
struct level { uint64_t *pool[NR]; struct node *nd; size_t n; };
#define NODE_P(lv, nd) ((lv).pool[(nd)->r] + (nd)->po)
#define NODE_Q(lv, nd) ((lv).pool[(nd)->r] + (nd)->qo)
int bs_regions_on_device = -1;                       /* BS_DEVICE_POOLS: 1 device pools, 0 host */
int bs_dev_mdev = -1;                                /* BS_DEV_MDEV: 1 (default with device pools) the top (mdev-tier) levels through the device
                                                      * tier on device numbers; 0 the host mdev tier through the host pools */
int bs_mdev_logl = RNS_BATCH_LOGL_MAX;                /* BS_MDEV_LOGL: products above 2^this go to the mdev tier (lower it to test at 10^9) */
static uint64_t *g_pool[2][NR]; static size_t g_cap[2][NR];
/* a node in a region pool as a (non-owning, single-quarter) device number */
static dbig pool_view(const uint64_t *p, size_t n)
{
    dbig v; memset(&v, 0, sizeof v); v.q[0] = (uint64_t *)p; v.n = n; v.qc = (size_t)1 << 40; return v;
}
static dbig node_p(const struct level *lv, const struct node *nd) { return nd->pd ? *nd->pd : pool_view(lv->pool[nd->r] + nd->po, nd->pn); }
static dbig node_q(const struct level *lv, const struct node *nd) { return nd->qd ? *nd->qd : pool_view(lv->pool[nd->r] + nd->qo, nd->qn); }
/* Phase 9 C4: the two parities of region r are the halves of one device allocation (the arena), so that when both
 * are handed to the dbig block pool they coalesce into one extent (dm's largest quarters need contiguous space:
 * two 14 GB halves that cannot merge left the pool falling back to hipMalloc, RESULTS 70).  A pool that outgrows
 * its half is re-allocated on its own (the half stays and is donated too); the arena itself outlives the block
 * pool's use of it (borrowed, never freed by db_release_pools) and goes at rns_shutdown. */
static struct { uint64_t *base; size_t bytes; int dev; int donated; } g_arena[NR];
static int in_arena(int r, const uint64_t *p) { return g_arena[r].base && p >= g_arena[r].base && p < g_arena[r].base + g_arena[r].bytes / 8; }
static void donate_one(int which, int r)             /* pool (which, r) to the block allocator: its arena half joins the other half when that is already there */
{
    uint64_t *p = g_pool[which][r]; size_t cap = g_cap[which][r]; if (!p || mem_dev_of(p) < 0) return;
    int dev = r % mem_device_count();
    if (in_arena(r, p)) { db_donate_adjacent(dev, p, cap * 8); if (++g_arena[r].donated == 2) mem_dev_forget(g_arena[r].base); }
    else {
        db_donate(dev, p, cap * 8); mem_dev_forget(p);
        if (g_arena[r].base) {                       /* this parity outgrew its arena half: the idle half goes too */
            size_t hb = g_arena[r].bytes / 2; db_donate_adjacent(dev, (char *)g_arena[r].base + which * hb, hb);
            if (++g_arena[r].donated == 2) mem_dev_forget(g_arena[r].base);
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
    for (int r = 0; r < NR; r++) if (g_arena[r].base) { if (g_arena[r].donated < 2) mem_dev_free(g_arena[r].base); else mem_dev_free_raw(g_arena[r].dev, g_arena[r].base); g_arena[r].base = 0; }
}
static int region_of(size_t i, size_t n) { size_t r = i * NR / n; return (int)(r < NR ? r : NR - 1); }
/* Phase 9 C2: the region of output node i of a level of n nodes.  Large levels: region NR i / n -- a subtree per
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

/* copy limbs out of (or into) region r's pool with the threads of r's node (a lone memcpy from device memory runs at a few GB/s) */
static void region_copy(uint64_t *dst, const uint64_t *src, size_t limbs, int r)
{
    if (limbs < ((size_t)1 << 20)) { memcpy(dst, src, limbs * 8); return; }
    if (mem_dev_of(src) >= 0 || mem_dev_of(dst) >= 0) { mem_dev_copy(dst, src, limbs * 8); return; }
#pragma omp parallel
    {
        int rk, cnt = mem_region_threads(&rk), home = mem_thread_home();
        if (home < 0 || r % NR == home % NR) {
            size_t chunk = (limbs + cnt - 1) / cnt, lo = chunk * rk, hi = lo + chunk < limbs ? lo + chunk : limbs;
            if (lo < hi) memcpy(dst + lo, src + lo, (hi - lo) * 8);
        }
    }
}
static hpool g_hpool[2];                              /* host pools for the mdev levels (WP3; WP5 removes them) */
static int g_hpool_taken[2];
/* hand one of the (already faulted) host pools to the caller before binsplit_free_pools: a fresh 35 GB
 * allocation for A costs 4-10 s of first-touch faults (RESULTS.md 62) */
uint64_t *binsplit_take_hpool(size_t *cap_limbs)
{
    for (int w = 0; w < 2; w++) if (g_hpool[w].p && !g_hpool_taken[w]) { g_hpool_taken[w] = 1; *cap_limbs = g_hpool[w].cap / 8; return (uint64_t *)g_hpool[w].p; }
    *cap_limbs = 0; return 0;
}
static uint64_t *pool_get(int which, int r, size_t limbs)
{
    if (g_cap[which][r] < limbs) {
        if (g_pool[which][r] && !in_arena(r, g_pool[which][r])) { if (mem_dev_of(g_pool[which][r]) >= 0) mem_dev_free(g_pool[which][r]); else mem_hreg_free(g_pool[which][r]); }
        size_t cap = limbs + limbs / (bs_region_slack ? 2 * bs_region_slack : 8) + 4096;
        int nd = bs_regions_on_device ? mem_device_count() : 0;
        g_pool[which][r] = (uint64_t *)(nd > 0 ? mem_dev_alloc(r % nd, cap * 8) : mem_hreg_alloc(cap * 8));
        g_cap[which][r] = cap;
        bs_st.n_grow++; bs_st.grow_bytes += cap * 8;
        if (bs_verbose) printf("bs: level pool %d region %d -> %.2f GB (%s)%s\n", which, r, cap * 8e-9, nd > 0 ? "device" : "host", g_arena[r].base ? " [outgrew its arena half]" : "");
    }
    return g_pool[which][r];
}
/* C4: the two parities of region r from one allocation of 2 cap limbs on the region's device */
static void arena_get(int r, size_t cap)
{
    int nd = mem_device_count(); if (g_arena[r].base || g_cap[0][r] >= cap || nd <= 0) return;
    cap = (cap * 8 + ((size_t)2 << 20) - 1) / ((size_t)2 << 20) * ((size_t)2 << 20) / 8;
    g_arena[r].dev = r % nd; g_arena[r].bytes = 2 * cap * 8; g_arena[r].donated = 0;
    g_arena[r].base = (uint64_t *)mem_dev_alloc(g_arena[r].dev, g_arena[r].bytes);
    for (int w = 0; w < 2; w++) { g_pool[w][r] = g_arena[r].base + w * cap; g_cap[w][r] = cap; }
    if (bs_verbose) printf("bs: region %d arena %.2f GB on APU %d (two parities of %.2f GB)\n", r, g_arena[r].bytes * 1e-9, g_arena[r].dev, cap * 8e-9);
}

static size_t seed_limbs(unsigned long N, size_t *per_out, unsigned long *nspan_out)
{
    if (getenv("BS_SEED_TERMS")) bs_seed_terms = atoi(getenv("BS_SEED_TERMS"));
    if (getenv("BS_SCHOOL_NL")) bs_school_nl = atoi(getenv("BS_SCHOOL_NL"));
    if (getenv("BS_MDEV_LOGL")) bs_mdev_logl = atoi(getenv("BS_MDEV_LOGL"));
    if (bs_dev_mdev < 0) bs_dev_mdev = getenv("BS_DEV_MDEV") ? atoi(getenv("BS_DEV_MDEV")) : 1;   /* default on since the coalescing pool (RESULTS.md 64) */
    unsigned long S = bs_seed_terms, nterms = bs_b1 ? bs_b1 - bs_a0 : N, nspan = (nterms + S - 1) / S;
    size_t per = (S * (size_t)ceil(log2((double)N + 2.0)) + 128) / (bi_decimal ? 59 : 64) + 2;   /* a decimal limb holds 59.8 bits */
    if (per_out) *per_out = per; if (nspan_out) *nspan_out = nspan;
    return 2 * per * nspan;
}
/* Phase 9 C4: what each region must hold, by laying out every level of the tree in advance with the seed bound
 * `per` for every span (level l's node i covers spans [i 2^l, (i+1) 2^l) of the nspan, P and Q slots as the level
 * loop lays them out, regions by the same place_node), over the levels that live in the region pools: with the
 * device top levels, the batch levels (the tier switch taken at 0.9 x the bound, so a level whose real sizes fall
 * just below the threshold is still counted); with host regions or the host mdev tier, every level.  The bound
 * is ~11 % above the real sizes (RESULTS 72: level 1 = 0.895 of total0) and pool_get adds 1/8: the margin.  This
 * replaces the flat total0 / NR (1 + 1/4), which held 1.4 x the live data and still grew at the five-node level. */
static void region_need(unsigned long N, size_t need[NR])
{
    size_t per; unsigned long nspan; seed_limbs(N, &per, &nspan);
    for (int r = 0; r < NR; r++) need[r] = 0;
    { size_t r0[NR + 1]; for (int r = 0; r <= NR; r++) { r0[r] = 0; while (r0[r] < nspan && region_of(r0[r], nspan) < r) r0[r]++; }
      for (int r = 0; r < NR; r++) need[r] = 2 * per * (r0[r + 1] - r0[r]) + 2; }
    int *cur_r = 0, *nxt_r = 0;
    for (int l = 0; ; l++) {
        size_t n_in = (nspan + ((size_t)1 << l) - 1) >> l; if (n_in <= 1) break;
        size_t m_full = (size_t)1 << l, max_nl = m_full * per + l;                 /* the bound on any node of level l */
        int mdev_level = 2 * (size_t)(0.9 * max_nl) + 1 > ((size_t)1 << bs_mdev_logl);   /* the real sizes are 0.90-0.945 of the bound at the levels below the top (measured 10^6..4x10^10); a miss costs one pool growth, not a failure; 0.85 pulled the 3-node level in at 4e10 and doubled the regions */
        if (mdev_level && bs_regions_on_device) break;                            /* the mdev levels use device numbers or the host pool */
        size_t npairs = n_in / 2, odd = n_in & 1, n = npairs + odd, offr[NR] = {0};
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
}
void binsplit_pregrow(unsigned long N)
{
    if (bs_regions_on_device < 0) bs_regions_on_device = getenv("BS_DEVICE_POOLS") ? atoi(getenv("BS_DEVICE_POOLS")) : 1;
    if (getenv("BS_BALANCE_N")) bs_balance_n = atoi(getenv("BS_BALANCE_N"));
    size_t total0 = seed_limbs(N, 0, 0), per_region = total0 / NR + total0 / (NR * (bs_region_slack ? bs_region_slack : 4)) + (1 << 20);   /* slack: 1/4 (paper-era) or 1/16 (BS_REGION_SLACK=16; the levels stay within a few percent of level 0) */
    if (bs_dev_mdev < 0) bs_dev_mdev = getenv("BS_DEV_MDEV") ? atoi(getenv("BS_DEV_MDEV")) : 1;   /* default on since the coalescing pool (RESULTS.md 64) */
    int par = getenv("ECALC_OVERLAP") ? atoi(getenv("ECALC_OVERLAP")) : 1;   /* Phase 8 (PLAN 18, O1): regions per device and the host pool touch in parallel */
    int nhp = bs_regions_on_device && total0 > ((size_t)1 << 28) ? (bs_dev_mdev ? 1 : 2) : 0;   /* host pools for the mdev levels (one, for A's buffer, when the top levels run on device), first-touched now */
    double t_pg = mem_now();
    size_t need[NR]; int exact = !(getenv("BS_REGION_FLAT") && atoi(getenv("BS_REGION_FLAT")));   /* C4: regions from the simulated layout (BS_REGION_FLAT=1: the flat paper-era sizing) */
    if (exact) region_need(N, need); else for (int r = 0; r < NR; r++) need[r] = per_region;
    /* the arena also serves the dm phase as the block pool (the regions are donated to it).  ECALC_DM_POOL_K=k makes it
     * at least k x n_Q limbs per APU (n_Q = d/18; the peak of live device numbers in dm is ~6.8 n_Q at 4e10, k = 8 leaves
     * the dm phase without any hipMalloc): measured at 4e10 it moves the mapping from dm to init (init +4.4 s, dm -2 s,
     * recip -2 s) for the same wall clock within the run-to-run spread (93.6 vs 92.5 s, results/A-mem.md), so the
     * default is the bs need alone (k = 0: init -2.2 s against main).  Node 0 only while the dm is not distributed;
     * ECALC_ARENA_GB sets the arena per APU outright. */
    { double k = getenv("ECALC_DM_POOL_K") ? atof(getenv("ECALC_DM_POOL_K")) : 0.0, dig = lgamma((double)N + 1.0) / log(10.0) - 50.0;
      int sz = getenv("COMM_SIZE") ? atoi(getenv("COMM_SIZE")) : 1; if (sz < 1) sz = 1;
      if (sz > 1 && getenv("COMM_RANK") && atoi(getenv("COMM_RANK")) != 0) k = 0;   /* until the division is distributed (A-div) only node 0 runs dm: the others keep the bs need */
      size_t half = getenv("ECALC_ARENA_GB") ? (size_t)(atof(getenv("ECALC_ARENA_GB")) * 1e9 / 2) / 8 : (size_t)(k * (dig / 18.0) / NR / 2);   /* node 0 runs the whole dm at any size for now */
      if (bs_regions_on_device && half) for (int r = 0; r < NR; r++) if (half > need[r] + need[r] / 8) need[r] = half - half / 9 - 4096; }
    if (bs_verbose) printf("bs: regions %s: %.2f / %.2f / %.2f / %.2f GB (+1/8; flat rule %.2f GB)%s\n", exact ? "from the level layouts" : "flat", need[0] * 8e-9, need[1] * 8e-9, need[2] * 8e-9, need[3] * 8e-9, per_region * 8e-9, bs_regions_on_device ? ", one arena per device for both parities" : "");
    if (bs_regions_on_device && mem_device_count() > 0 && !g_arena[0].base && !g_pool[0][0]) {
        double ta = mem_now();
#pragma omp parallel for num_threads(NR) schedule(static) if(par)
        for (int r = 0; r < NR; r++) arena_get(r, need[r] + need[r] / (bs_region_slack ? 2 * bs_region_slack : 8) + 4096);
        if (bs_verbose || (getenv("ECALC_VERBOSE") && atoi(getenv("ECALC_VERBOSE")) >= 2)) printf("bs: arenas %.1f GB allocated in %.2f s (layout pass %.2f s)\n", (g_arena[0].bytes + g_arena[1].bytes + g_arena[2].bytes + g_arena[3].bytes) / 1e9, mem_now() - ta, ta - t_pg);
    }
#pragma omp parallel for num_threads(NR + 1) schedule(static) if(par)
    for (int r = 0; r <= NR; r++) {
        if (r < NR) { for (int w = 0; w < 2; w++) pool_get(w, r, need[r]); }
        else for (int w = 0; w < nhp; w++) { uint64_t *hp = (uint64_t *)hpool_get(&g_hpool[w], (total0 + total0 / 8 + 4 * NR) * 8);
            if (par && bs_dev_mdev) continue;                                  /* I2: with the top levels on device this pool only serves A, formed in the background later: its faults are hidden there */
#pragma omp parallel for schedule(static) num_threads(par ? 96 : omp_get_max_threads())
            for (size_t i = 0; i < total0 + total0 / 8; i += 512) hp[i] = 0; }
    }
}
/* WP7: periodic snapshots of the level loop (PLAN.md 15, WP7).  A checkpoint is the state the
 * loop needs at the top of an iteration: the node table of the current level, `which`, the
 * level number, and the used limbs of each region's pool (offr[r] limbs, laid out sequentially
 * by the level's layout pass).  The mdev-tier levels keep their results in the host pool
 * g_hpool[which] (four pointers into one block, RESULTS.md 56); the header records that so a
 * restart rebuilds the same placement.  Files in bs_ckpt_dir: level_LLL.hdr (header + node
 * table) and level_LLL.rR (region R's limbs); every file is written to a temporary name and
 * renamed, the header last, and the previous level's set is removed only after the new header
 * is in place -- at any moment a complete set exists.  Not streaming: the working set never
 * lives on storage; a snapshot every bs_ckpt_every levels costs one pass through the host
 * (mem_dev_copy from the device regions in 1 GiB chunks through the pinned staging).
 *
 * M6 (PLAN.md 19, results/A-ckpt.md): (1) levels whose nodes are device numbers (the dev_mdev top
 * levels, WP5) are snapshotted too: file r holds, node by node, the limbs [pn r/4, pn (r+1)/4) of
 * P and the same quarter of Q -- a limb range, not a device quarter, so the reader's placement
 * (db_reserve) need not match the writer's; offr[r] is file r's limbs, so ckpt_find checks the
 * sizes as before.  (2) Multi-node: every node-process writes its own sets, named
 * n<rank>_level_LLL.* (several node-processes share a directory), the header extended (magic
 * ECBSCKP2) with size, rank and the term range; single-node runs keep the WP7 names and the v1
 * header where nothing new is needed, and read v1 sets.  (3) The tree levels (mn_tree):
 * n<rank>_tree_LLL.* holds the node's shares of P and Q (the mdb descriptors in the header, the
 * share's limbs in the four quarter files), written by mn.c through bs_ckpt_tree_*; the set it
 * supersedes (the leaf's, and the tree levels below L) is removed only after every node has written
 * level L (mn.c's barrier, taken at the next set level or at mn_finalize -- C6), so the lowest
 * "highest complete level" over the nodes exists on all of them.  BS_CKPT_TREE_EVERY=k writes the
 * tree sets at levels k, 2k, ... and the top. */
#define CKPT_MAGIC  "ECBSCKP1"
#define CKPT_MAGIC2 "ECBSCKP2"
#define CKPT_CHUNK ((size_t)1 << 30)
struct ckpt_hdr { char magic[8]; uint64_t N; int32_t decimal, seed_terms, level, which, mdev_host, nregions; uint64_t n, off, offr[NR], node_bytes; };
struct ckpt_ext { int32_t size, rank, dev_nodes, kind; uint64_t a0, b1; uint64_t tree[10]; };   /* v2 (follows the v1 header): kind 0 = a leaf level, 1 = a tree level (tree[] = P.n P.N P.g0 P.g P.sh.n, the same for Q) */
static int g_ck_node = -2;                            /* -1: single node (WP7 names); else this node-process's rank */
static void ckpt_env(void) { if (g_ck_node == -2) g_ck_node = mn_size() > 1 ? mn_rank() : -1; }
static void ckpt_path(char *buf, size_t sz, const char *kind, int level, const char *suffix, int r, int tmp)
{
    char pre[64]; ckpt_env();
    if (g_ck_node < 0) snprintf(pre, sizeof pre, "%s_%03d", kind, level); else snprintf(pre, sizeof pre, "n%03d_%s_%03d", g_ck_node, kind, level);
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
    return ok;
}
/* the 1 GiB host buffer for the DMA of thread r: region r's pinned staging (idle between levels), else malloc */
static uint64_t *ckpt_buf(int r, int *own)
{
    *own = 0;
    if (mem_device_count() >= NR && rns_staging_bytes() >= CKPT_CHUNK) return rns_hstage(r % mem_device_count());
    *own = 1; return (uint64_t *)malloc(CKPT_CHUNK);
}
static FILE *ckpt_open(const char *kind, int level, int r, int write, char *tmp, char *final)
{
    ckpt_path(tmp, 4096, kind, level, "r", r, 1); ckpt_path(final, 4096, kind, level, "r", r, 0);
    FILE *f = fopen(write ? tmp : final, write ? "wb" : "rb");
    if (!f) fprintf(stderr, "bs: checkpoint: cannot open %s: %s\n", write ? tmp : final, strerror(errno));
    return f;
}
static int ckpt_close(FILE *f, int ok, int write, const char *tmp, const char *final)
{
    if (write) return ckpt_finish(f, ok, tmp, final);
    fclose(f);
    if (!ok) fprintf(stderr, "bs: checkpoint: short read from %s\n", final);
    return ok;
}
/* region r's limbs [0, limbs) -> file (write) or file -> pool (read), through a host buffer when the pool is device memory */
static int ckpt_region_io(int level, uint64_t *pool, size_t limbs, int r, int write)
{
    char tmp[4096], final[4096];
    FILE *f = ckpt_open("level", level, r, write, tmp, final); if (!f) return 0;
    int dev = mem_dev_of(pool), ok = 1, own = 0;
    uint64_t *buf = dev >= 0 ? ckpt_buf(r, &own) : 0;
    for (size_t lo = 0; lo < limbs && ok; lo += CKPT_CHUNK / 8) {
        size_t cnt = limbs - lo < CKPT_CHUNK / 8 ? limbs - lo : CKPT_CHUNK / 8;
        if (dev < 0) ok = write ? fwrite_all(f, pool + lo, cnt * 8) : fread_all(f, pool + lo, cnt * 8);
        else if (write) { mem_dev_copy(buf, pool + lo, cnt * 8); ok = fwrite_all(f, buf, cnt * 8); }
        else { ok = fread_all(f, buf, cnt * 8); if (ok) mem_dev_copy(pool + lo, buf, cnt * 8); }
    }
    if (own) free(buf);
    return ckpt_close(f, ok, write, tmp, final);
}
/* M6: the limbs [lo, hi) of a device number <-> the file, in runs inside one device quarter (limb g lives in
 * quarter (g >= qc) + (g >= 2 qc) + (g >= 3 qc), dbig.c), each run DMA'd on its quarter's device through buf */
static int ckpt_dbig_io(FILE *f, dbig *x, size_t lo, size_t hi, uint64_t *buf, int write)
{
    if (x->off) { fprintf(stderr, "bs: checkpoint of a dbig view\n"); abort(); }
    int ok = 1;
    for (size_t i = lo; i < hi && ok;) {
        size_t d = (i >= x->qc) + (i >= 2 * x->qc) + (i >= 3 * x->qc), so = i - d * x->qc, run = x->qc - so;
        if (i + run > hi) run = hi - i;
        if (run > CKPT_CHUNK / 8) run = CKPT_CHUNK / 8;
        if (write) { mem_dev_copy_on((int)d, buf, x->q[d] + so, run * 8); ok = fwrite_all(f, buf, run * 8); }
        else { ok = fread_all(f, buf, run * 8); if (ok) mem_dev_copy_on((int)d, x->q[d] + so, buf, run * 8); }
        i += run;
    }
    return ok;
}
static size_t range_lo(size_t n, int r) { return n * (size_t)r / NR; }
/* file r of a device-number level: quarter r (by limb range) of every node's P and Q */
static int ckpt_devnodes_io(int level, struct level *lv, int r, int write)
{
    char tmp[4096], final[4096];
    FILE *f = ckpt_open("level", level, r, write, tmp, final); if (!f) return 0;
    int ok = 1, own; uint64_t *buf = ckpt_buf(r, &own);
    for (size_t i = 0; i < lv->n && ok; i++) {
        struct node *nd = &lv->nd[i];
        ok = ckpt_dbig_io(f, nd->pd, range_lo(nd->pn, r), range_lo(nd->pn, r + 1), buf, write)
          && ckpt_dbig_io(f, nd->qd, range_lo(nd->qn, r), range_lo(nd->qn, r + 1), buf, write);
    }
    if (own) free(buf);
    return ckpt_close(f, ok, write, tmp, final);
}
static size_t devnodes_limbs(const struct level *lv, int r)
{
    size_t s = 0;
    for (size_t i = 0; i < lv->n; i++) s += range_lo(lv->nd[i].pn, r + 1) - range_lo(lv->nd[i].pn, r) + range_lo(lv->nd[i].qn, r + 1) - range_lo(lv->nd[i].qn, r);
    return s;
}
/* all regions of a level, in parallel when they are four device regions (each through its own staging) */
static int ckpt_regions(int level, struct level *lv, const size_t *offr, int dev_nodes, int write)
{
    int oks[NR];
    if (dev_nodes || (mem_dev_of(lv->pool[0]) >= 0 && mem_device_count() >= NR)) {
#pragma omp parallel for num_threads(NR) schedule(static, 1)
        for (int r = 0; r < NR; r++) oks[r] = dev_nodes ? ckpt_devnodes_io(level, lv, r, write) : ckpt_region_io(level, lv->pool[r], offr[r], r, write);
    } else for (int r = 0; r < NR; r++) oks[r] = ckpt_region_io(level, lv->pool[r], offr[r], r, write);
    int ok = 1; for (int r = 0; r < NR; r++) ok = ok && oks[r];
    return ok;
}
static void ckpt_remove_kind(const char *kind, int level)
{
    char p[4096];
    for (int r = 0; r < NR; r++) { ckpt_path(p, sizeof p, kind, level, "r", r, 0); unlink(p); }
    ckpt_path(p, sizeof p, kind, level, "hdr", -1, 0); unlink(p);
}
static void ckpt_remove(int level) { ckpt_remove_kind("level", level); }
static void ckpt_sync_dir(void) { int dfd = open(bs_ckpt_dir, O_RDONLY | O_DIRECTORY); if (dfd >= 0) { fsync(dfd); close(dfd); } }
/* the header (v1, or v2 with the extension) + the node table -> <kind>_LLL.hdr; the directory fsync'd after */
static int ckpt_write_hdr(const char *kind, struct ckpt_hdr *h, const struct ckpt_ext *x, const void *nodes)
{
    char tmp[4096], final[4096];
    ckpt_env();
    if (g_ck_node >= 0 || x->dev_nodes || x->kind) memcpy(h->magic, CKPT_MAGIC2, 8); else memcpy(h->magic, CKPT_MAGIC, 8);   /* v1 whenever v1 says it all: main's restart reads it */
    ckpt_path(tmp, sizeof tmp, kind, h->level, "hdr", -1, 1); ckpt_path(final, sizeof final, kind, h->level, "hdr", -1, 0);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite_all(f, h, sizeof *h) && (h->magic[7] == '1' || fwrite_all(f, x, sizeof *x)) && (!h->node_bytes || fwrite_all(f, nodes, h->node_bytes));
    if (!ckpt_finish(f, ok, tmp, final)) return 0;
    ckpt_sync_dir();
    return 1;
}
/* the header of <kind>_LLL: 1 if it belongs to this run (another run's set aborts) and, with check_files, its
 * region files have the sizes the header names (a complete set) */
static int ckpt_read_hdr(const char *kind, int level, struct ckpt_hdr *h, struct ckpt_ext *x, unsigned long N, int check_files)
{
    char p[4096]; ckpt_env();
    ckpt_path(p, sizeof p, kind, level, "hdr", -1, 0);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    memset(x, 0, sizeof *x);
    int ok = fread_all(f, h, sizeof *h) && (!memcmp(h->magic, CKPT_MAGIC, 8) || (!memcmp(h->magic, CKPT_MAGIC2, 8) && fread_all(f, x, sizeof *x)));
    fclose(f);
    if (!ok) return 0;
    int v2 = h->magic[7] == '2', me = g_ck_node < 0 ? 0 : g_ck_node;
    if (h->N != N || h->decimal != bi_decimal || h->seed_terms != bs_seed_terms || h->nregions != NR
        || (v2 && (x->size != mn_size() || x->rank != me || (x->kind == 0 && (x->a0 != bs_a0 || x->b1 != bs_b1))))
        || (!v2 && mn_size() > 1)) {
        fprintf(stderr, "bs: checkpoint %s is from another run (N %llu, base %s, seeds %d, %d node-processes): refusing to restart\n",
                p, (unsigned long long)h->N, h->decimal ? "10^18" : "2^64", h->seed_terms, v2 ? x->size : 1);
        abort();
    }
    if (!check_files) return 1;
    struct stat st;
    for (int r = 0; r < NR && ok; r++) { ckpt_path(p, sizeof p, kind, level, "r", r, 0); ok = stat(p, &st) == 0 && (uint64_t)st.st_size == h->offr[r] * 8; }
    return ok;
}
/* the level just finished (cur, which, level) -> bs_ckpt_dir; returns bytes written (0: failed, the run goes on) */
static size_t ckpt_write(struct level *cur, int which, int level, int mdev_host, const size_t *offr, size_t off, unsigned long N, int prev_level)
{
    int dev_nodes = cur->nd[0].pd != 0;
    struct ckpt_hdr h; struct ckpt_ext x; memset(&h, 0, sizeof h); memset(&x, 0, sizeof x);
    h.N = N; h.decimal = bi_decimal; h.seed_terms = bs_seed_terms; h.level = level; h.which = which;
    h.mdev_host = dev_nodes ? 0 : mdev_host; h.nregions = NR; h.n = cur->n; h.off = off; h.node_bytes = cur->n * sizeof(struct node);
    size_t used[NR];
    for (int r = 0; r < NR; r++) h.offr[r] = used[r] = dev_nodes ? devnodes_limbs(cur, r) : offr[r];
    x.size = mn_size(); x.rank = mn_rank(); x.dev_nodes = dev_nodes; x.kind = 0; x.a0 = bs_a0; x.b1 = bs_b1;
    mkdir(bs_ckpt_dir, 0777);
    if (!ckpt_regions(level, cur, used, dev_nodes, 1)) return 0;
    if (!ckpt_write_hdr("level", &h, &x, cur->nd)) return 0;
    if (prev_level > 0 && prev_level != level) ckpt_remove(prev_level);
    size_t bytes = sizeof h + h.node_bytes; for (int r = 0; r < NR; r++) bytes += used[r] * 8;
    return bytes;
}
/* the latest complete set of a kind in bs_ckpt_dir for this run (this node-process): its level, or 0 */
static int ckpt_find_kind(const char *kind, unsigned long N)
{
    int best = 0; struct ckpt_hdr h; struct ckpt_ext x;
    for (int l = 1; l < 128; l++) if (ckpt_read_hdr(kind, l, &h, &x, N, 1) && l > best) best = l;
    return best;
}
static int ckpt_find(unsigned long N) { return ckpt_find_kind("level", N); }
/* load level `level` into cur, its pools placed as the level loop placed them; sets which and off */
static int ckpt_read(struct level *cur, int *which, int level, size_t *off_out, unsigned long N)
{
    char p[4096]; struct ckpt_hdr h; struct ckpt_ext x;
    if (!ckpt_read_hdr("level", level, &h, &x, N, 0)) return 0;
    ckpt_path(p, sizeof p, "level", level, "hdr", -1, 0);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    int ok = fseek(f, (long)(sizeof h + (h.magic[7] == '2' ? sizeof x : 0)), SEEK_SET) == 0;
    cur->n = h.n; cur->nd = (struct node *)malloc(h.node_bytes);
    ok = ok && fread_all(f, cur->nd, h.node_bytes); fclose(f);
    if (!ok) { fprintf(stderr, "bs: checkpoint: bad header %s\n", p); return 0; }
    *which = h.which; *off_out = h.off;
    size_t offr[NR]; for (int r = 0; r < NR; r++) offr[r] = h.offr[r];
    if (x.dev_nodes) {                                   /* a device-number level: the region pools had been donated to the block pool by then; the nodes get fresh blocks */
        donate_pools(0); donate_pools(1);
        for (int r = 0; r < NR; r++) cur->pool[r] = 0;
        for (size_t i = 0; i < cur->n; i++) {
            struct node *nd = &cur->nd[i];
            nd->pd = (dbig *)calloc(1, sizeof(dbig)); nd->qd = (dbig *)calloc(1, sizeof(dbig));
            db_reserve(nd->pd, nd->pn + 8); nd->pd->n = nd->pn; db_reserve(nd->qd, nd->qn + 8); nd->qd->n = nd->qn;   /* (+8 as rns_mul_dist_db's results have) */
        }
        return ckpt_regions(level, cur, offr, 1, 0);
    }
    for (size_t i = 0; i < cur->n; i++) cur->nd[i].pd = cur->nd[i].qd = 0;
    if (h.mdev_host && bs_regions_on_device) {           /* an mdev level: the four regions inside one host pool */
        uint64_t *hp = (uint64_t *)hpool_get(&g_hpool[h.which], (h.off + 4 * NR) * 8), *q = hp;
        for (int r = 0; r < NR; r++) { cur->pool[r] = q; q += offr[r] + 2; }
    } else for (int r = 0; r < NR; r++) cur->pool[r] = pool_get(h.which, r, offr[r] + 2);
    return ckpt_regions(level, cur, offr, 0, 0);
}
/* ---- M6: the tree levels' sets (mn.c) -- the node's shares of P and Q; file r = quarter r (by limb range) of P's share, then of Q's ---- */
static int tree_io(int level, dbig *P, dbig *Q, int r, int write)
{
    char tmp[4096], final[4096];
    FILE *f = ckpt_open("tree", level, r, write, tmp, final); if (!f) return 0;
    int ok, own; uint64_t *buf = ckpt_buf(r, &own);
    ok = ckpt_dbig_io(f, P, range_lo(P->n, r), range_lo(P->n, r + 1), buf, write) && ckpt_dbig_io(f, Q, range_lo(Q->n, r), range_lo(Q->n, r + 1), buf, write);
    if (own) free(buf);
    return ckpt_close(f, ok, write, tmp, final);
}
size_t bs_ckpt_tree_write(int level, unsigned long N, const uint64_t desc[10], dbig *P, dbig *Q)
{
    struct ckpt_hdr h; struct ckpt_ext x; memset(&h, 0, sizeof h); memset(&x, 0, sizeof x);
    h.N = N; h.decimal = bi_decimal; h.seed_terms = bs_seed_terms; h.level = level; h.nregions = NR;
    for (int r = 0; r < NR; r++) h.offr[r] = range_lo(P->n, r + 1) - range_lo(P->n, r) + range_lo(Q->n, r + 1) - range_lo(Q->n, r);
    x.size = mn_size(); x.rank = mn_rank(); x.kind = 1; x.a0 = bs_a0; x.b1 = bs_b1; memcpy(x.tree, desc, sizeof x.tree);
    mkdir(bs_ckpt_dir, 0777);
    int oks[NR];
#pragma omp parallel for num_threads(NR) schedule(static, 1)
    for (int r = 0; r < NR; r++) oks[r] = tree_io(level, P, Q, r, 1);
    for (int r = 0; r < NR; r++) if (!oks[r]) return 0;
    if (!ckpt_write_hdr("tree", &h, &x, 0)) return 0;
    size_t bytes = sizeof h + sizeof x; for (int r = 0; r < NR; r++) bytes += h.offr[r] * 8;
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
    int oks[NR];
#pragma omp parallel for num_threads(NR) schedule(static, 1)
    for (int r = 0; r < NR; r++) oks[r] = tree_io(level, P, Q, r, 0);
    for (int r = 0; r < NR; r++) if (!oks[r]) return 0;
    return 1;
}
/* after every node has tree level `level` (mn.c's barrier): the sets it supersedes go -- every tree level below it
 * (BS_CKPT_TREE_EVERY may leave gaps) and the leaf's */
void bs_ckpt_tree_remove_below(int level)
{
    if (!bs_ckpt_dir) return;
    for (int l = 1; l < level; l++) ckpt_remove_kind("tree", l);
    for (int l = 1; l < 128; l++) ckpt_remove(l);
}
/* every tree set of this node above `level` (stale after a restart below them; all of them for a fresh run) */
void bs_ckpt_tree_clear(int level) { if (bs_ckpt_dir) for (int l = level + 1; l < 128; l++) ckpt_remove_kind("tree", l); }
/* the seeds of level 0 into the per-region staging: region r's spans computed by the node's threads (or by
 * every thread when unpinned) with the schoolbook, P and Q of each span side by side (per limbs each) */
static void seeds_compute(struct level *cur, size_t per, unsigned long S, unsigned long N, const size_t *r0, uint64_t **stage)
{
    int nt = getenv("BS_SEED_THREADS") ? atoi(getenv("BS_SEED_THREADS")) : omp_get_max_threads();   /* I2: fewer than all leaves cores to init's allocations */
#pragma omp parallel num_threads(nt)
    {
        bigint p, q; bi_init(&p); bi_init(&q);
        int rk, cnt = mem_region_threads(&rk), home = mem_thread_home();   /* region-aware: this node's threads do this region's spans */
        for (int r = 0; r < NR; r++) {
            if (home >= 0 && r % NR != home % NR) continue;               /* (home < 0: threads not pinned, every thread does everything by rank) */
            size_t lo = r0[r], hi = r0[r + 1];
            for (size_t i = lo + (size_t)rk; i < hi; i += (size_t)cnt) {
                unsigned long a = bs_a0 + i * S, b = a + S, bend = bs_b1 ? bs_b1 : N + 1; if (b > bend) b = bend;
                span(&p, &q, a, b);
                struct node *nd = &cur->nd[i];
                nd->r = r; nd->po = 2 * per * (i - lo); nd->pn = p.n; nd->qo = nd->po + per; nd->qn = q.n;
                if (p.n > per || q.n > per) { fprintf(stderr, "bs: seed span overflow\n"); abort(); }
                memcpy(stage[r] + nd->po, p.l, p.n * 8); memcpy(stage[r] + nd->qo, q.l, q.n * 8);
            }
        }
        bi_free(&p); bi_free(&q);
    }
}
/* Phase 8 I2: the seeds computed in a background thread during init's device allocations (PLAN.md 16), into the
 * pinned staging, which exists before the pools; binsplit_e joins and takes the level-0 table from here */
static struct { int active; pthread_t th; unsigned long N, nspan; size_t per, r0[NR + 1]; struct node *nd; double t; } g_pre;
static void *pre_seeds_run(void *a)
{
    (void)a; double t0 = mem_now(); struct level cur; memset(&cur, 0, sizeof cur); cur.n = g_pre.nspan; cur.nd = g_pre.nd;
    uint64_t *stage[NR]; for (int r = 0; r < NR; r++) stage[r] = rns_hstage(r % mem_device_count());
    seeds_compute(&cur, g_pre.per, bs_seed_terms, g_pre.N, g_pre.r0, stage);
    g_pre.t = mem_now() - t0; return 0;
}
/* the pinned staging a region's seeds need (bytes, the largest region): the decimal device flow sizes the staging to this */
size_t binsplit_seed_stage_bytes(unsigned long N)
{
    size_t per; unsigned long nspan; seed_limbs(N, &per, &nspan);
    size_t r0[NR + 1], mx = 0;
    for (int r = 0; r <= NR; r++) { r0[r] = 0; while (r0[r] < nspan && region_of(r0[r], nspan) < r) r0[r]++; }
    for (int r = 0; r < NR; r++) { size_t b = (2 * per * (r0[r + 1] - r0[r]) + 2) * 8; if (b > mx) mx = b; }
    return mx;
}
void binsplit_seeds_begin(unsigned long N)
{
    if (bs_regions_on_device < 0) bs_regions_on_device = getenv("BS_DEVICE_POOLS") ? atoi(getenv("BS_DEVICE_POOLS")) : 1;
    if (!bs_regions_on_device || (bs_restart && bs_ckpt_dir)) return;      /* the staging path only; a restart skips the seeds */
    seed_limbs(N, &g_pre.per, &g_pre.nspan); g_pre.N = N;
    for (int r = 0; r <= NR; r++) { g_pre.r0[r] = 0; while (g_pre.r0[r] < g_pre.nspan && region_of(g_pre.r0[r], g_pre.nspan) < r) g_pre.r0[r]++; }
    if (rns_staging_bytes() < (2 * g_pre.per * (g_pre.r0[1] - g_pre.r0[0]) + 2) * 8) return;   /* would not fit the staging: binsplit_e will report it */
    g_pre.nd = (struct node *)calloc(g_pre.nspan, sizeof *g_pre.nd);
    pthread_create(&g_pre.th, 0, pre_seeds_run, 0); g_pre.active = 1;
}
void binsplit_e(bigint *P, bigint *Q, unsigned long N)
{
    double t0 = mem_now(), t;
    memset(&bs_st, 0, sizeof bs_st); bs_N = N;
    unsigned long S = bs_seed_terms, nspan; size_t per;
    /* seed spans: Q(a,b) < b^S, P < S b^S: reserve (S log2(N+1) + 64 + 64) / 64 limbs each */
    seed_limbs(N, &per, &nspan);
    struct level cur, nxt;
    if (bs_regions_on_device < 0) bs_regions_on_device = getenv("BS_DEVICE_POOLS") ? atoi(getenv("BS_DEVICE_POOLS")) : 1;
    int which = 0, ckpt_level = 0, resumed = 0;      /* ckpt_level: the level whose set is on disk */
    if (bs_restart && bs_ckpt_dir && mn_size() > 1 && mn_ckpt_tree_level(N) > 0) {   /* M6: every node has a tree-level set: the leaf tree is skipped, mn_tree resumes above it */
        binsplit_pregrow(N);
        P->n = Q->n = 0; bs_Pd.n = bs_Qd.n = 0; bs_st.t_total = mem_now() - t0;
        printf("bs: restart at tree level %d: the leaf tree is skipped\n", mn_ckpt_tree_level(N));
        return;
    }
    if (bs_restart && bs_ckpt_dir) {                 /* WP7: resume from the latest complete checkpoint, skipping the seeds and the levels below it */
        int l = ckpt_find(N);
        bs_ckpt_tree_clear(0);                       /* (tree sets, if any, are not agreed on by all nodes: stale) */
        if (l) {
            binsplit_pregrow(N);
            t = mem_now(); size_t off = 0;
            if (!ckpt_read(&cur, &which, l, &off, N)) { fprintf(stderr, "bs: restart from %s level %d failed\n", bs_ckpt_dir, l); abort(); }
            bs_st.levels = l; bs_st.restart_level = l; ckpt_level = l; resumed = 1;
            if (off > bs_st.peak_pool_limbs) bs_st.peak_pool_limbs = off;
            bs_st.t_restart = mem_now() - t;
            printf("bs: restart from %s: level %d, %zu nodes, pool %.2f GB, loaded in %.2f s\n", bs_ckpt_dir, l, cur.n, off * 8e-9, bs_st.t_restart);
        } else printf("bs: no checkpoint for this run in %s: full run\n", bs_ckpt_dir);
    }
    if (!resumed) {
    if (bs_ckpt_dir) { mkdir(bs_ckpt_dir, 0777); for (int l = 1; l < 128; l++) ckpt_remove(l); bs_ckpt_tree_clear(0); }   /* a fresh run owns the directory (its node's names): stale sets go */
    cur.n = nspan; cur.nd = (struct node *)calloc(nspan, sizeof *cur.nd);
    size_t r0[NR + 1];                               /* first node of each region at level 0 */
    for (int r = 0; r <= NR; r++) { r0[r] = 0; while (r0[r] < nspan && region_of(r0[r], nspan) < r) r0[r]++; }
    /* region pools sized once from level 0 (the levels' totals stay within a few percent of it;
     * pool_get still grows if a level needs more); binsplit_pregrow does this at init, outside the timed phase */
    size_t total0 = 2 * per * nspan;
    binsplit_pregrow(N);
    for (int r = 0; r < NR; r++) cur.pool[r] = pool_get(0, r, 2 * per * (r0[r + 1] - r0[r]) + 2);
    if (mem_dev_of(cur.pool[0]) >= 0 && rns_staging_bytes() && rns_staging_bytes() < (2 * per * (r0[1] - r0[0]) + 2) * 8) { fprintf(stderr, "bs: seed region larger than the staging buffer\n"); abort(); }
    if (bs_st.peak_pool_limbs < total0) bs_st.peak_pool_limbs = total0;
    t = mem_now();
    /* seeds go to a host staging area per region (small scattered writes into device memory are slow,
     * RESULTS.md 56), then one bulk copy per region */
    uint64_t *stage[NR]; int own_stage = mem_dev_of(cur.pool[0]) < 0;
    for (int r = 0; r < NR; r++) stage[r] = own_stage ? (uint64_t *)malloc((2 * per * (r0[r + 1] - r0[r]) + 2) * 8) : rns_hstage(r % mem_device_count());
    if (g_pre.active && !own_stage && g_pre.N == N) {                      /* I2: computed during init; take the table */
        pthread_join(g_pre.th, 0); g_pre.active = 0;
        free(cur.nd); cur.nd = g_pre.nd; g_pre.nd = 0;
        if (bs_verbose) printf("bs: seeds were computed during init (%.2f s)\n", g_pre.t);
    } else {
        if (g_pre.active) { pthread_join(g_pre.th, 0); g_pre.active = 0; free(g_pre.nd); g_pre.nd = 0; }
        seeds_compute(&cur, per, S, N, r0, stage);
    }
    double t_span = mem_now() - t;
#pragma omp parallel for num_threads(NR) schedule(static, 1)
    for (int r = 0; r < NR; r++) {
        size_t limbs = 2 * per * (r0[r + 1] - r0[r]);
        if (own_stage) memcpy(cur.pool[r], stage[r], limbs * 8); else mem_dev_copy(cur.pool[r], stage[r], limbs * 8);   /* DMA from the pinned staging */
        if (own_stage) free(stage[r]);
    }
    bs_st.t_seed = mem_now() - t;
    if (bs_verbose) printf("bs: %lu terms, %lu spans of %lu, seeds %.2f s (spans %.2f, copy %.2f)\n", N, nspan, S, bs_st.t_seed, t_span, bs_st.t_seed - t_span);
    if (bs_after_seeds_hook) bs_after_seeds_hook(bs_hook_arg);   /* Phase 8 overlap: the CPU is free from here until the top level */
    }                                                /* !resumed */

    while (cur.n > 1) {
        t = mem_now();
        size_t npairs = cur.n / 2, odd = cur.n & 1, max_nl = 0;
        for (size_t i = 0; i < cur.n; i++) { if (cur.nd[i].pn > max_nl) max_nl = cur.nd[i].pn; if (cur.nd[i].qn > max_nl) max_nl = cur.nd[i].qn; }
        /* next level layout: P slot n(P1)+n(Q2)+1, Q slot n(Q1)+n(Q2) */
        nxt.n = npairs + odd; nxt.nd = (struct node *)calloc(nxt.n, sizeof *nxt.nd);
        size_t offr[NR] = {0}, off = 0;
        for (size_t i = 0; i < npairs; i++) {
            struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
            o->r = place_node(i, nxt.n, offr, a->r, b->r);   /* C2: balanced at the small top levels */
            o->po = offr[o->r]; offr[o->r] += a->pn + b->qn + 1; o->qo = offr[o->r]; offr[o->r] += a->qn + b->qn;
            o->pn = o->qn = 0;
        }
        if (odd) { struct node *a = &cur.nd[cur.n - 1], *o = &nxt.nd[npairs]; o->r = place_node(npairs, nxt.n, offr, a->r, -1); o->po = offr[o->r]; offr[o->r] += a->pn; o->qo = offr[o->r]; offr[o->r] += a->qn; o->pn = a->pn; o->qn = a->qn; }
        which ^= 1;
        int mdev_level = max_nl > (size_t)bs_school_nl && 2 * max_nl + 1 > ((size_t)1 << bs_mdev_logl);
        int dev_mdev = mdev_level && bs_regions_on_device && bs_dev_mdev;
        int top_direct = nxt.n == 1 && mdev_level;                     /* mdev top level: straight to P, Q */
        for (int r = 0; r < NR; r++) off += offr[r];
        if (top_direct || dev_mdev) for (int r = 0; r < NR; r++) nxt.pool[r] = 0;
        else if (mdev_level && bs_regions_on_device) {
            /* mdev levels: results in one host pool (the tier stages through the host anyway and a
             * single region may need the whole level); the batch levels stay in the device regions */
            uint64_t *hp = (uint64_t *)hpool_get(&g_hpool[which], (off + 4 * NR) * 8), *p = hp;
            for (int r = 0; r < NR; r++) { nxt.pool[r] = p; p += offr[r] + 2; }
        } else for (int r = 0; r < NR; r++) nxt.pool[r] = pool_get(which, r, offr[r] + 2);
        if (off > bs_st.peak_pool_limbs) bs_st.peak_pool_limbs = off;
        double tl0 = mem_now(), tl1 = 0, tl2 = 0;
        const char *tier; int normed = 0, finished = 0;
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
        } else if (2 * max_nl + 1 <= ((size_t)1 << bs_mdev_logl)) {
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
            if (dev_mdev) {
                /* the top levels on the device tier: operands are region-pool views or the children's device
                 * numbers; results are device numbers (blocks from the region pools no longer needed) */
                if (!cur.nd[0].pd) donate_pools(which);                   /* the first such level: the other parity's pools are free */
                for (size_t i = 0; i < npairs; i++) {
                    struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                    dbig A1 = node_p(&cur, a), A2 = node_q(&cur, a), B = node_q(&cur, b), P2 = node_p(&cur, b);
                    o->pd = (dbig *)calloc(1, sizeof(dbig)); o->qd = (dbig *)calloc(1, sizeof(dbig));
                    rns_mul_dist_db(o->pd, &A1, &B);
                    db_add(o->pd, o->pd, &P2);                              /* P = P1 Q2 + P2 */
                    rns_mul_dist_db(o->qd, &A2, &B);
                    o->pn = o->pd->n; o->qn = o->qd->n;
                }
                if (odd) { struct node *a = &cur.nd[cur.n - 1], *o = &nxt.nd[npairs];
                           if (a->pd) { o->pd = a->pd; o->qd = a->qd; a->pd = a->qd = 0; }
                           else { o->pd = (dbig *)calloc(1, sizeof(dbig)); o->qd = (dbig *)calloc(1, sizeof(dbig)); dbig vp = node_p(&cur, a), vq = node_q(&cur, a); db_copy(o->pd, &vp); db_copy(o->qd, &vq); }
                           o->pn = o->pd->n; o->qn = o->qd->n; }
                for (size_t i = 0; i < cur.n; i++) { if (cur.nd[i].pd) { db_free(cur.nd[i].pd); free(cur.nd[i].pd); } if (cur.nd[i].qd) { db_free(cur.nd[i].qd); free(cur.nd[i].qd); } cur.nd[i].pd = cur.nd[i].qd = 0; }
                if (cur.pool[0] && mem_dev_of(cur.pool[0]) >= 0) donate_pools(which ^ 1);   /* the children's pools are consumed */
                if (nxt.n == 1) {
                    if (bs_keep_dev) { bs_Pd = *nxt.nd[0].pd; bs_Qd = *nxt.nd[0].qd; P->n = Q->n = 0; }   /* the caller copies them out (overlapped with the reciprocal) */
                    else { db_to_bi(P, nxt.nd[0].pd); db_to_bi(Q, nxt.nd[0].qd); db_free(nxt.nd[0].pd); db_free(nxt.nd[0].qd); }
                    free(nxt.nd[0].pd); free(nxt.nd[0].qd); nxt.nd[0].pd = nxt.nd[0].qd = 0; finished = 1; }
                normed = 1;
            } else {
            bigint A1, A2, B, C1, C2, P2; bi_init(&A1); bi_init(&A2); bi_init(&B); bi_init(&C1); bi_init(&C2); bi_init(&P2);
            for (size_t i = 0; i < npairs; i++) {
                struct node *a = &cur.nd[2 * i], *b = &cur.nd[2 * i + 1], *o = &nxt.nd[i];
                /* views into the pools (rns_mul_pair stages them by DMA when they are device pools) */
                A1.l = NODE_P(cur, a); A1.n = a->pn; A2.l = NODE_Q(cur, a); A2.n = a->qn; B.l = NODE_Q(cur, b); B.n = b->qn;
                A1.cap = A2.cap = B.cap = 0;
                rns_mul_pair(&C1, &A1, &C2, &A2, &B);
                /* P = C1 + P2 on the host (a read-modify-write pass over device memory is slow), then one DMA */
                size_t pn = a->pn + b->qn + 1;
                bi_reserve(&C1, pn); if (C1.n < pn) memset(C1.l + C1.n, 0, (pn - C1.n) * 8);
                bi_reserve(&P2, b->pn); region_copy(P2.l, NODE_P(cur, b), b->pn, b->r); P2.n = b->pn;
                C1.l[pn - 1] = limb_add(C1.l, C1.l, pn - 1, P2.l, P2.n); C1.n = pn;
                if (nxt.n == 1) {                            /* the top: straight into the outputs, no pool */
                    bigint sw = *P; *P = C1; C1 = sw; sw = *Q; *Q = C2; C2 = sw; P->n = limb_norm(P->l, P->n); Q->n = limb_norm(Q->l, Q->n);
                    finished = 1;
                } else {
                    region_copy(NODE_P(nxt, o), C1.l, pn, o->r);
                    if (C2.n < a->qn + b->qn) { bi_reserve(&C2, a->qn + b->qn); memset(C2.l + C2.n, 0, (a->qn + b->qn - C2.n) * 8); }
                    region_copy(NODE_Q(nxt, o), C2.l, a->qn + b->qn, o->r);
                }
            }
            A1.l = A2.l = B.l = 0; bi_free(&C1); bi_free(&C2); bi_free(&P2);
            }
        }
        if (normed || finished) {                       /* lengths came back from the device-local batch path, or the top is done */
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
        if (odd && !dev_mdev) { struct node *a = &cur.nd[cur.n - 1], *o = &nxt.nd[npairs];
                   region_copy(NODE_P(nxt, o), NODE_P(cur, a), a->pn, o->r); region_copy(NODE_Q(nxt, o), NODE_Q(cur, a), a->qn, o->r); }
        double dt = mem_now() - t, tl3 = mem_now();
        if (bs_verbose && tl2) printf("bs:   layout %.3f  batch %.3f  add+norm %.3f\n", tl1 - t, tl2 - tl1, tl3 - tl2);
        if (!strcmp(tier, "school")) bs_st.t_school += dt; else if (!strcmp(tier, "batch")) bs_st.t_batch += dt; else bs_st.t_mdev += dt;
        bs_st.levels++;
        if (bs_verbose) printf("bs: level %2d %-6s %8zu pairs  max_nl %10zu  pool %6.2f GB  %.2f s  (batch %.2f: scatter %.2f ntt %.2f crt %.2f merge %.2f)\n", bs_st.levels, tier, npairs, max_nl, off * 8e-9, dt, rns_st.tb_total, rns_st.tb_scatter, rns_st.tb_ntt, rns_st.tb_crt, rns_st.tb_merge);
        memset(&rns_st, 0, sizeof rns_st);
        free(cur.nd);
        cur = nxt;
        if (finished) { free(cur.nd); cur.nd = 0; break; }
        /* WP7: snapshot the level just finished (never the top: the loop ends there); M6: levels held as device numbers too */
        if (bs_ckpt_dir && cur.n > 1 && bs_ckpt_every > 0 && bs_st.levels % bs_ckpt_every == 0 && (bs_st.levels >= bs_ckpt_min_level || off * 8 > bs_ckpt_min_bytes)) {
            double tc = mem_now();
            size_t bytes = ckpt_write(&cur, which, bs_st.levels, mdev_level && bs_regions_on_device, offr, off, N, ckpt_level);
            double dtc = mem_now() - tc;
            if (bytes) { ckpt_level = bs_st.levels; bs_st.n_ckpt++; bs_st.ckpt_bytes += bytes; bs_st.t_ckpt += dtc; }
            if (bs_verbose || !bytes) printf("bs: checkpoint level %d -> %s: %.3f GB in %.2f s (%.2f GB/s)%s\n", bs_st.levels, bs_ckpt_dir, bytes * 1e-9, dtc, bytes * 1e-9 / (dtc > 0 ? dtc : 1), bytes ? "" : "  FAILED, continuing");
            if (bytes && getenv("BS_CKPT_ABORT") && atoi(getenv("BS_CKPT_ABORT")) == bs_st.levels && (!getenv("BS_CKPT_ABORT_NODE") || atoi(getenv("BS_CKPT_ABORT_NODE")) == mn_rank())) {   /* test hook: die here, as a failed run would (BS_CKPT_ABORT_NODE: this node-process only) */
                printf("bs: BS_CKPT_ABORT: exiting after the level %d checkpoint\n", bs_st.levels); fflush(stdout); _exit(3);
            }
        }
    }
    if (cur.nd) {
        bi_reserve(P, cur.nd[0].pn); region_copy(P->l, NODE_P(cur, &cur.nd[0]), cur.nd[0].pn, cur.nd[0].r); P->n = cur.nd[0].pn;
        bi_reserve(Q, cur.nd[0].qn); region_copy(Q->l, NODE_Q(cur, &cur.nd[0]), cur.nd[0].qn, cur.nd[0].r); Q->n = cur.nd[0].qn;
    }
    free(cur.nd);
    bs_st.t_total = mem_now() - t0;
}
unsigned long bs_a0 = 1, bs_b1 = 0;                                 /* M2: this process's term range [a0, b1) (b1 = 0: all of [1, N+1)) */
void (*bs_after_seeds_hook)(void *) = 0; void *bs_hook_arg = 0;   /* Phase 8: called once the seeds are in the regions */
int bs_keep_dev = 0; dbig bs_Pd, bs_Qd;                             /* Phase 8: the top level's P, Q left on device */
int bs_region_slack = 0;                             /* BS_REGION_SLACK: 4 (default) or 16 */
int bs_donate_pools = 0;                              /* WP5: hand the device regions to the dbig block allocator instead of freeing them */
void binsplit_free_pools(void)
{
    for (int w = 0; w < 2; w++) for (int r = 0; r < NR; r++) {
        if (g_pool[w][r]) {
            int dev = mem_dev_of(g_pool[w][r]);
            if (dev >= 0 && bs_donate_pools) donate_one(w, r);
            else if (dev >= 0) { if (!in_arena(r, g_pool[w][r])) mem_dev_free(g_pool[w][r]); } else mem_hreg_free(g_pool[w][r]);
        }
        g_pool[w][r] = 0; g_cap[w][r] = 0;
    }
    if (!bs_donate_pools) binsplit_release_arenas();
    else rns_shutdown_hook = binsplit_release_arenas;   /* the arenas stay for the block pool; released at rns_shutdown */
    for (int w = 0; w < 2; w++) if (!g_hpool_taken[w]) hpool_free(&g_hpool[w]); else { g_hpool[w].p = 0; g_hpool[w].cap = 0; g_hpool_taken[w] = 0; }
}

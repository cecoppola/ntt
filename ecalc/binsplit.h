/* binsplit.h - bottom-up binary splitting of e = sum 1/k! (ALGORITHM.md §3).
 *
 * Two-variable recursion over [a, b): Q = a (a+1) ... (b-1), P = sum_{k=a}^{b-1}
 * (k+1) ... (b-1), so P/Q = sum_{k=a}^{b-1} 1/(a ... k) and e = 1 + P(1,N+1)/Q(1,N+1);
 * combine P = P1 Q2 + P2, Q = Q1 Q2.  Seeds: spans of bs_seed_terms terms by
 * schoolbook (OpenMP over spans).  Each level pairs adjacent nodes; the tier
 * is chosen per level from the largest node length max_nl:
 *   2 max_nl + 1 <= 2^RNS_BATCH_LOGL_MAX (2^18)   one rns_mul_batch of 2 npairs products
 *   otherwise                                      rns_mul_pair per pair (Q2 transformed once)
 * Nodes live in two alternating registered level pools (grow-only), so the
 * batch tier reads them in place.
 */
#ifndef EC_BINSPLIT_H
#define EC_BINSPLIT_H
#include "bigint.h"
struct dbig_s;

#ifdef __cplusplus
extern "C" {
#endif
/* S1d (TARGET1, ECALC_PE_PER_GPU=1): with the switch on each PE (one process per GPU, vpe.h) runs its own binary splitting over its own term range with ONE region
 * (its device); the run's state below is one copy per process (every process drives exactly one PE).
 */
typedef struct { double t_seed, t_school, t_batch, t_mdev, t_total; int levels, school_levels, batch_levels, mdev_levels; size_t peak_pool_limbs;
                 int n_grow; size_t grow_bytes; } bs_stats;   /* region pool growths inside the phase (count, bytes) */
extern bs_stats bs_st;
extern int bs_seed_terms;                       /* 256 (BS_SEED_TERMS) */
unsigned long bs_seed_terms_for(unsigned long bend);
long bs_seed_fill(void);                                  /* the fill in limbs (BS_SEED_FILL; 128 by default; 0 = the fixed span) */   /* the run's seed span for terms ending at bend (BS_SEED_FILL, else BS_SEED_TERMS / 256) */
int bs_team_size(void);                                    /* the whole-node team (ECALC_OMP_TEAM) and the seed pass's team: the start-up line */
int bs_seed_team_size(void);
extern int bs_verbose;
/* the top-level set (ECALC_CKPT_TOP, read by ECALC_RECHECK): this node's P and Q (shares) and their mdb descriptors
 * desc[10] = P.n P.N P.g0 P.g P.sh.n, the same for Q, as tree set `level` (size 1: 0; size > 1: mn_tree's top level);
 * multi-node names carry the node rank (ecalc/README.md: BS_CKPT_DIR, ECALC_CKPT_TOP) */
extern const char *bs_ckpt_dir;  /* directory; 0 = none */
extern unsigned long bs_N;                      /* the run's N (set by binsplit_e) */
int    bs_ckpt_tree_find(unsigned long N);                 /* this node's highest complete tree set, 0 = none */
int    bs_ckpt_tree_read(int level, unsigned long N, uint64_t desc[10], struct dbig_s *P, struct dbig_s *Q);
void   bs_ckpt_tree_clear(int level);                      /* this node's tree sets above `level` */
/* a tree set written by a background thread (the top set at any size).  release(part 0 = P, 1 = Q)
 * returns once the writer no longer reads that part -- 1 written, 0 abandoned (budget != 0: the measured disk rate projects the
 * part past `slack` seconds; the set is then dropped, no partial files); done() does not wait; join() waits for the end (fsync,
 * header), prints the line (prefix `who`) and returns the bytes (0 = abandoned or failed) */
typedef struct bs_ckpt_bg bs_ckpt_bg;
bs_ckpt_bg *bs_ckpt_bg_start(int level, unsigned long N, const uint64_t desc[10], const struct dbig_s *P, const struct dbig_s *Q, int budget, double slack);
int    bs_ckpt_bg_release(bs_ckpt_bg *b, int part);
int    bs_ckpt_bg_done(bs_ckpt_bg *b, int part);
size_t bs_ckpt_bg_join(bs_ckpt_bg *b, const char *who, const char *dir, double *t_write, double *t_wait);   /* t_write: the writer's seconds; t_wait: the owner's (both releases and the join) */

/* DM_TIGHT (the reciprocal's scratch reserved per doubling, the top levels' pairs freed as consumed).  Read once by dm_switches(); -1 before */
extern int dm_tight;
/* mn.c's tree levels free the dead P_i / P_run shares between a level's two products and
 * tree_need_dev counts max(2c + 2r + n, c + r + 2n) */
void dm_switches(void);
unsigned long e_terms(unsigned long digits);            /* N = min{m : lgamma(m+1)/ln10 >= d + 50} */
void binsplit_e(bigint *P, bigint *Q, unsigned long N); /* P(1,N+1), Q(1,N+1) */
void binsplit_pregrow(unsigned long N);
void binsplit_layout_comm_size(int g);   /* MS: BS_LAYOUT_ONLY sets COMM_SIZE to a point's g (0: restore) */
/* the node's bytes at plane cap `cap` (0 2^30, 1 2^31) and np primes
 * for N terms over g node-processes (the caller sets rank 0's range): plane pools + tables, the arena, the host init constants;
 * before rns_init only (it switches rns_pool_log per cap) */
int binsplit_ppn_proc(void);                       /* S2c: processes per node that carry their own runtime/comm host terms (ECALC_PE_PER_GPU: PEs per node; else 1) */
size_t binsplit_node_bytes(unsigned long N, int g, int cap, int np, size_t *planes, size_t *arena, size_t *host);
size_t binsplit_comm_resident_bytes(unsigned long N, int g);   /* the node's device pool bytes, allocated at init (ecalc.c budget_check: already taken) */
extern const char *const bs_cap_name[2];
size_t binsplit_div_product_limbs(unsigned long N);   /* the run's largest single product (the division's t = A_h mu: 2 k + 8 limbs, dm_layout's non-DKM tcap); ECALC_PLANE_CAP=run asks whether it fits one plane of the cap */
void binsplit_seeds_begin(unsigned long N);
void binsplit_free_pools(void);
/* B7ACCT (results/GRID_GROUPS.md §B7ACCT): the general map's v-exchange slots resident per APU thread (bytes; hipMalloc'd by comm_layered.c beside the
 * layout, alive to the run's end): the dm phase's sum (the run's largest), *tree_top = during the tree's top level, by = the terms per level.
 * (the slots are in the comm pool, not in the node budget) */
size_t binsplit_vslot_bytes(unsigned long N, int size, size_t *tree_top, char *by, size_t bylen);
int binsplit_vslot_budget_on(void);
size_t binsplit_shmem_pool_need(unsigned long N, int size, char *by, size_t bylen);   /* the SHMEM pool this run needs (bytes per node-process) */
size_t binsplit_shmem_pool_rule(unsigned long N, int size, int plan);                   /* COMM_SHMEM_POOL_AUTO / the warning / MN_PLAN_ONLY's line */   /* t1's quarter in bytes -- the arena's reserved tail (the block pool's largest block) */
void binsplit_release_arenas(void);                      /* the region arenas (hooked into rns_shutdown when the block pool borrowed them) */
extern int bs_balance_n;                                 /* levels with at most this many nodes are laid out least-loaded-first (16) */
extern void (*bs_after_seeds_hook)(void *); extern void *bs_hook_arg;
/* FIT (D26, results/LAUNCH_DEFAULTS.md §FIT): the pre-launch memory fit of (N, g) at rank 0's layout, and DM_MN_WRAP automatic (binsplit.c) */
struct bs_fit {
    size_t planes, arena, bs2, ofi, ofi_all, pool, seedbuf, host, vslot, vslot_top, grow;   /* per node (vslot, vslot_top: per APU) */
    size_t dev_apu_max;                                                                         /* ECALC_FIT_PER_APU: the busiest APU's device bytes (per APU) */
    size_t dev_dm, dev_bs, device, node, dev_wrap1, dev_wrap0;                                  /* per node; the device with / without the wrap (g > 1) */
    size_t need_dev, tree_dev, v2, v3, div, room, wrapb, wrapb1, hole;                          /* the layout's terms per APU (wrapb1: the wrap's, with it on) */
    double limit, margin, budget;                                                               /* ECALC_DEVICE_GB, ECALC_DEVICE_MARGIN_GB, ECALC_NODE_GB (bytes) */
    int wrap, wrap_env, fits_dev, fits_node, fits;
};
int  binsplit_ppg_batch_levels(unsigned long N, int g, size_t *lv, int nlv);   /* RNS_BATCH_SPLIT's planner: each batch level's largest product (limbs) */
int  binsplit_ppg_batch_logl(unsigned long N, int g, size_t *nc);   /* S2fix: ECALC_PE_PER_GPU=1: the largest batch-tier product's logL of PE 0's leaf (nc limbs) */
int  binsplit_fit(unsigned long N, int g, struct bs_fit *F);     /* fills F, decides the run's DM_MN_WRAP when unset; returns F->fits */
void binsplit_fit_print(const struct bs_fit *F, double D, int g, const char *pre);
int  binsplit_dm_wrap(void);                                      /* the run's DM_MN_WRAP: set > the fit's decision > 1 */
const char *binsplit_dm_wrap_src(void);                           /* "fit" / "set" / "default" (not decided: 1) */
extern unsigned long bs_a0, bs_b1;   /* M2: the term range [a0, b1) this process (vPE: this PE) computes (b1 = 0: [1, N+1)) */
extern int bs_keep_dev; extern struct dbig_s bs_Pd, bs_Qd;   /* P, Q stay on device (bs_keep_dev = 1); P->n = Q->n = 0 then */
/* reference: the same recursion on the CPU with schoolbook products, any N */
void binsplit_ref(bigint *P, bigint *Q, unsigned long a, unsigned long b);
/* S1d: the planner's PE terms (ECALC_PE_PER_GPU=1; with the switch off 4 devices per node, 1 PE per node, and the node's range as before) */
unsigned long binsplit_pe_b1(unsigned long N, int g);   /* end of rank 0's (PE 0's) term range [1, b1) on g node-processes; 0 = all terms */
int binsplit_nrn(void); int binsplit_lanes(void);        /* devices (PEs) per node; PEs per node */
#ifdef __cplusplus
}
#endif
#endif

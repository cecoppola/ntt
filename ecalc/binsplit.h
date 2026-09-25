/* binsplit.h - bottom-up binary splitting of e = sum 1/k! (PLAN.md 8, step 6).
 *
 * Two-variable recursion over [a, b): Q = a (a+1) ... (b-1), P = sum_{k=a}^{b-1}
 * (k+1) ... (b-1), so P/Q = sum_{k=a}^{b-1} 1/(a ... k) and e = 1 + P(1,N+1)/Q(1,N+1);
 * combine P = P1 Q2 + P2, Q = Q1 Q2.  Seeds: spans of bs_seed_terms terms by
 * schoolbook (OpenMP over spans).  Each level pairs adjacent nodes; the tier
 * is chosen per level from the largest node length max_nl:
 *   max_nl <= bs_school_nl (160)                  CPU schoolbook, OpenMP over pairs
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
typedef struct { double t_seed, t_school, t_batch, t_mdev, t_total, t_ckpt, t_restart; int levels, school_levels, batch_levels, mdev_levels, n_ckpt, restart_level; size_t peak_pool_limbs, ckpt_bytes;
                 int n_grow; size_t grow_bytes; } bs_stats;   /* Phase 9: region pool growths inside the phase (count, bytes) */
extern bs_stats bs_st;
extern int bs_seed_terms;     /* 512 */
extern int bs_school_nl;      /* 160 limbs */
extern int bs_verbose;
/* WP7: per-level checkpoints of the level loop (ecalc/README.md: BS_CKPT_DIR, BS_CKPT_EVERY, BS_RESTART) */
extern const char *bs_ckpt_dir;  /* directory; 0 = no checkpoints */
extern int bs_ckpt_every;        /* every this many levels (4) */
extern int bs_ckpt_min_level;    /* from this level on (8), or once the level pool exceeds bs_ckpt_min_bytes (1 GiB) */
extern size_t bs_ckpt_min_bytes;
extern int bs_restart;           /* 1: resume from the latest complete set in bs_ckpt_dir */
/* M6 (results/A-ckpt.md): the tree levels' sets, written by mn_tree -- this node's shares of P and Q and their mdb
 * descriptors desc[10] = P.n P.N P.g0 P.g P.sh.n, the same for Q; multi-node names carry the node rank */
size_t bs_ckpt_tree_write(int level, unsigned long N, const uint64_t desc[10], struct dbig_s *P, struct dbig_s *Q);   /* bytes, 0 = failed */
extern int bs_ckpt_own_buf;          /* Phase 12 W: 1 = checkpoint DMA buffers malloc'd (a writer that runs past rns_release_staging) */
extern volatile int bs_ckpt_tree_pdone;   /* Phase 12 W: files of the tree set in progress whose P part is written (NR = P may be overwritten) */
extern unsigned long bs_N;                                 /* the run's N (set by binsplit_e) */
int    bs_ckpt_tree_find(unsigned long N);                 /* this node's highest complete tree set, 0 = none */
int    bs_ckpt_tree_read(int level, unsigned long N, uint64_t desc[10], struct dbig_s *P, struct dbig_s *Q);
void   bs_ckpt_tree_remove_below(int level);               /* the sets tree level `level` supersedes (call once every node has it) */
void   bs_ckpt_tree_clear(int level);                      /* this node's tree sets above `level` */
/* Phase 13 N (TASKS 1.7): the restart scan -- this node's highest complete tree set (0 = none) without aborting; *err 1 = a set of
 * another run, 2 = a tree set written under another schedule (node count / MN_GROUPS level map, recorded in the v3 header); msg says which */
int    bs_ckpt_restart_scan(unsigned long N, int *err, char *msg, size_t msz);
/* Phase 13 N (TASKS 4.1, 1.4): a tree set written by a background thread (the top set at any size).  release(part 0 = P, 1 = Q)
 * returns once the writer no longer reads that part -- 1 written, 0 abandoned (budget != 0: the measured disk rate projects the
 * part past `slack` seconds; the set is then dropped, no partial files); done() does not wait; join() waits for the end (fsync,
 * header), prints the line (prefix `who`) and returns the bytes (0 = abandoned or failed) */
typedef struct bs_ckpt_bg bs_ckpt_bg;
bs_ckpt_bg *bs_ckpt_bg_start(int level, unsigned long N, const uint64_t desc[10], const struct dbig_s *P, const struct dbig_s *Q, int budget, double slack);
int    bs_ckpt_bg_release(bs_ckpt_bg *b, int part);
int    bs_ckpt_bg_done(bs_ckpt_bg *b, int part);
size_t bs_ckpt_bg_join(bs_ckpt_bg *b, const char *who, const char *dir, double *t_write, double *t_wait);   /* t_write: the writer's seconds; t_wait: the owner's (both releases and the join) */

/* Phase 14 L1 (APUMULT_STUDY E2 / E5): DM_TIGHT (the reciprocal's scratch reserved per doubling, the top levels' pairs freed as consumed),
 * DM_TAIL_DEAD (1: no hole reserved beside the top level, the tail re-laid over its dead inputs; 2: + the layout without P in the
 * reciprocal, which needs the P spill: dm_p_spill_wired = 1 once it is there).  Read once by dm_switches(); -1 before */
extern int dm_tight, dm_tail_dead, dm_p_spill_wired;
/* Phase 14 T1 (APUMULT_STUDY E10a): MN_TREE_EARLY_FREE=1 -- mn.c's tree levels free the dead P_i / P_run shares between a level's two
 * products and tree_need_dev counts max(2c + 2r + n, c + r + 2n) instead of 2c + 2r + 2n.  Read by dm_switches(); -1 before */
extern int mn_tree_early_free;
void dm_switches(void);
unsigned long e_terms(unsigned long digits);            /* N = min{m : lgamma(m+1)/ln10 >= d + 50} */
void binsplit_e(bigint *P, bigint *Q, unsigned long N); /* P(1,N+1), Q(1,N+1) */
void binsplit_pregrow(unsigned long N);
/* Phase 13b P (PLAN 31 step 0.3, the K axis): the node's bytes at plane cap `cap` (0 2^30, 1 3 2^29, 2 2^31, 3 3 2^30) and np primes
 * for N terms over g node-processes (the caller sets rank 0's range): plane pools + tables, the arena, the host init constants;
 * before rns_init only (it switches rns_pool_log per cap) */
size_t binsplit_node_bytes(unsigned long N, int g, int cap, int np, size_t *planes, size_t *arena, size_t *host);
extern const char *const bs_cap_name[4];
void binsplit_seeds_begin(unsigned long N);
size_t binsplit_seed_stage_bytes(unsigned long N);       /* the pinned staging the seeds need per APU */
extern int bs_region_slack;              /* Phase 8 I2: the seeds in a background thread during init (needs the pinned staging) */                  /* WP3: allocate the region pools at init (outside the timed phase) */
uint64_t *binsplit_take_hpool(size_t *cap_limbs);       /* WP5: a faulted host pool for A (call before binsplit_free_pools) */
void binsplit_free_pools(void);
size_t binsplit_dm_hole_bytes(unsigned long N, int size);   /* Phase 11 M (decision 5): t1's quarter in bytes -- the arena's reserved tail (the block pool's largest block) */
void binsplit_release_arenas(void);                      /* Phase 9 C4: the region arenas (hooked into rns_shutdown when the block pool borrowed them) */
extern int bs_balance_n;                                 /* Phase 9 C2: levels with at most this many nodes are laid out least-loaded-first (16) */
extern int bs_donate_pools;
extern int bs_dev_mdev;
extern void (*bs_after_seeds_hook)(void *); extern void *bs_hook_arg;   /* Phase 8 overlap */
extern unsigned long bs_a0, bs_b1;   /* M2: the term range [a0, b1) this process computes (b1 = 0: [1, N+1)) */
extern int bs_keep_dev; extern struct dbig_s bs_Pd, bs_Qd;             /* Phase 8: P, Q stay on device (bs_keep_dev = 1); P->n = Q->n = 0 then */                          /* release the two level pools */
/* reference: the same recursion on the CPU with schoolbook products, any N */
void binsplit_ref(bigint *P, bigint *Q, unsigned long a, unsigned long b);
#ifdef __cplusplus
}
#endif
#endif

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
typedef struct { double t_seed, t_school, t_batch, t_mdev, t_total, t_ckpt, t_restart; int levels, school_levels, batch_levels, mdev_levels, n_ckpt, restart_level; size_t peak_pool_limbs, ckpt_bytes; } bs_stats;
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
int    bs_ckpt_tree_find(unsigned long N);                 /* this node's highest complete tree set, 0 = none */
int    bs_ckpt_tree_read(int level, unsigned long N, uint64_t desc[10], struct dbig_s *P, struct dbig_s *Q);
void   bs_ckpt_tree_remove_below(int level);               /* the sets tree level `level` supersedes (call once every node has it) */
void   bs_ckpt_tree_clear(int level);                      /* this node's tree sets above `level` */

unsigned long e_terms(unsigned long digits);            /* N = min{m : lgamma(m+1)/ln10 >= d + 50} */
void binsplit_e(bigint *P, bigint *Q, unsigned long N); /* P(1,N+1), Q(1,N+1) */
void binsplit_pregrow(unsigned long N);
void binsplit_seeds_begin(unsigned long N);
size_t binsplit_seed_stage_bytes(unsigned long N);       /* the pinned staging the seeds need per APU */
extern int bs_region_slack;              /* Phase 8 I2: the seeds in a background thread during init (needs the pinned staging) */                  /* WP3: allocate the region pools at init (outside the timed phase) */
uint64_t *binsplit_take_hpool(size_t *cap_limbs);       /* WP5: a faulted host pool for A (call before binsplit_free_pools) */
void binsplit_free_pools(void);
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

/* mn_plan.h - Phase 13d L (PLAN.md 32, row L): the plan printer, MN_PLAN_ONLY=<total digits>:<g> (mn_plan.c).
 *
 * The decisions a run makes about its large products -- one plane or a grid, the grid kA x kB, the pieces the cuts skip, the
 * group a reciprocal step runs on, the Newton chain -- asked of the functions the run itself uses (rns_dist.c, newton_db.c),
 * without a device or a communicator.  Only the operand sizes are predicted (log10 of the binary-splitting P, Q over a term
 * range, mn_plan.c); everything that decides is the code's own. */
#ifndef EC_MN_PLAN_H
#define EC_MN_PLAN_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* one product's plan: one plane (ka = kb = 1) or the grid, the pieces formed and skipped by the cuts, the piece's transform */
struct rns_grid_plan {
    int one, ka, kb, formed, skipped;
    int logcap;          /* the plane cap: 2^logcap points (the mn tier: mn_logn_cap(g); the dist tier: dist_logn_max) */
    size_t cap;          /* the cap in points (the dist tier: dist_cap(), 3 2^k with the radix-3 planes) */
    size_t pa, pb;       /* the largest piece's operand limbs (ceil(na / ka), ceil(nb / kb)) */
    size_t pts;          /* the largest piece's transform length (points) */
    int logR, logC;      /* the mn tier's four-step split of that plane (mn_shape) */
    int form_b;          /* the dist tier under auto: the largest piece fits the B form's planes (split_grid's weight 0.70) */
    double plane_bytes;  /* the transform planes one node holds for the largest piece (all primes, four APUs) */
};
/* the dist tier (one node; rns_mul_dist_db / rns_mul_high_db / rns_mul_low_db): mul_grid's decision for na x nb limbs with the
 * cuts (lowcut 0: none; w = (size_t)-1: none) */
void rns_dist_db_plan(size_t na, size_t nb, size_t lowcut, size_t w, struct rns_grid_plan *p);
/* the mn tier over g nodes (rns_mul_dist_mn / rns_mul_dist_mn_cut): mn_grid's decision; has_x: the tree's added operand */
void rns_dist_mn_plan(size_t na, size_t nb, int g, int has_x, size_t lowcut, size_t w, struct rns_grid_plan *p);
/* the plane pools' bytes per APU that b_fits / b_place read (0, 0: the real pools again) */
void rns_dist_plan_pools(size_t pool0_bytes, size_t pool1_bytes);
/* MN_TREE_LOGN_TEST's cap for the plan (rns_dist_cap_test without releasing the transform cache, which needs a device) */
void rns_dist_plan_cap_test(int logn);
/* newton_db.c: the reciprocal's anchored chain (NEWTON_ANCHOR) and the sharded part's start (NEWTON_MN_SPLIT), the X1 group */
size_t newton_chain_next(size_t j, size_t k);
size_t newton_mn_chain_start(size_t k);
size_t newton_recip_cut(size_t v);      /* Phase 14 R1 (E7): the low cut of a reciprocal product read as t1 >> v under NEWTON_RECIP_CUT (0 = none) */
int newton_mn_x1_level(size_t na, size_t nb, int g, int size, size_t reshard_limbs);   /* L: the step runs on [0, 2^L) (0: the full group of g nodes) */
/* mn_plan.c: print the plan of a run of d digits (N terms) on `size` node-processes and return the exit status */
int mn_plan_run(unsigned long d, unsigned long N, int size, int pool_log);
#ifdef __cplusplus
}
#endif
#endif

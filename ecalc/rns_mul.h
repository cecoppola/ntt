/* rns_mul.h - the batch product tiers of binary splitting, the GPU CRT and the plane pools (ALGORITHM.md §4.4); the
 * distributed tier (rns_dist.c) is declared here too.  The host product tier (rns_mul: mdev, Karatsuba split, schoolbook) was
 * removed by TREF (results/CODE_REDUCTION.md §TREF): no production caller; the tests' reference product is tests/tref.h.
 */
#ifndef EC_RNS_MUL_H
#define EC_RNS_MUL_H
#include <stddef.h>
#include <stdint.h>
#include "bigint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double t_total;
    double tb_scatter, tb_ntt, tb_crt, tb_merge, tb_total;   /* batch tier, max over devices per tile, summed */
    size_t n_batch;
    size_t n_batch_local, n_batch_pair;                      /* products through the locality-aware tier, of them in paired-B tiles */
} rns_stats;
extern rns_stats rns_st;

int  rns_init(int pool_log);      /* 31 (default) or 32; allocates staging + pools, returns ndev */
extern void (*rns_after_staging_hook)(void *); extern void *rns_hook_arg;   /* called inside rns_init once the pinned staging exists */
extern size_t rns_staging_bytes_req;   /* set before rns_init: pinned staging per APU (0 = 8 << pool_log); the decimal device flow needs only the seed stage */
size_t rns_staging_bytes(void);
extern size_t rns_pool1_bytes_req;     /* set before rns_init: plane pool 1 per APU (0 = 8 << pool_log); rns_pool1_default_bytes gives the dist tier's 3 q + 16 limbs */
size_t rns_pool1_default_bytes(int pool_log);
/* rns_plane_limbs() is pool 0's capacity in limbs */
size_t rns_plane_limbs(void);
int rns_pool0_np(void);                                              /* the planes plane pool 0 is made for (ec_np; ECALC_NP=auto: 3 or 4 by the run's largest group, ec_np_planes) */
size_t rns_plane_pool_bytes(int pool_log, int np, size_t *p0, size_t *p1);   /* pools 0 and 1 per APU as rns_init makes them (bytes; returns the sum) */
int  rns_np2cap(void);                          /* MN_NONPOW2_CAP=1 and COMM_SIZE not a power of two: the rank plane is 43/32 x 2^(pool_log-2) */
size_t rns_q_slot(int pool_log);                 /* the rank plane (limbs per prime) the pools are made for */
void rns_preinit_pool_log(int pool_log);                                                /* rns_pool_log() before rns_init (BS_LAYOUT_ONLY without a device) */
size_t rns_dpool_cap(int dev, int which);   /* the pool's current bytes (0 before rns_init) */
extern void (*rns_shutdown_hook)(void);   /* called first by rns_shutdown (binsplit's region arenas) */
int  rns_pool_log(void);
int  rns_ndev(void);
void rns_shutdown(void);

/* batch: N independent products a_i * b_i -> c_i (na_i + nb_i limbs each, not
 * normalised), all with na_i + nb_i <= L_sub = 2^logL, logL <= RNS_BATCH_LOGL_MAX.
 * Tiles of M = min(N, rns_batch_tile_bytes / (3 L_sub 8)) products: a
 * scatter_expand kernel reads the operands straight from registered host
 * memory (mem_hreg_alloc / mem_hstage_alloc) into the device planes, batched
 * transforms, then the GPU S-stripe CRT (every device peer-reads all four
 * planes, one block per product, results stored to host) when M >= rns_gpucrt_min,
 * else the CPU CRT.  Operands or results outside registered memory are staged
 * through the device staging buffers first (slower). */
/* c = a b, or c = a b + x when x is given (nx <= na + nb; c then has na + nb + 1 limbs, the top one the
 * final carry) -- the binary-splitting P = P1 Q2 + P2 in one pass (WP3: no CPU add over the pools) */
typedef struct { const uint64_t *a; size_t na; const uint64_t *b; size_t nb; uint64_t *c; const uint64_t *x; size_t nx;
                 size_t ncn; /* out: normalised length of c when the device-local path ran (0: not computed) */ } rns_prod;
#define RNS_BATCH_LOGL_MAX 30
extern size_t rns_batch_tile_bytes;   /* default 15e9 */
extern int rns_gpucrt_min;            /* default 1: GPU CRT with S stripes per product so M S >= rns_gpucrt_blocks (912) */
extern int rns_gpucrt_blocks;
void rns_mul_batch(rns_prod *P, size_t N);
int rns_crt_scan_on(void);           /* K4b: RNS_CRT_SCAN (default 1 since D21; =0 = the serial carry chain) */
int p24_crt_scan_on(void);          /* K4c: P24_CRT_SCAN (p24_crt.c; default 1 since D24; =0 = the serial chain) */
int rns_batch_pipe_on(void);          /* N3: BS_BATCH_PIPE (default 1 since D20; =0 = the old host side): the batch tier's host side in parallel and its tiles back to back */
uint64_t *rns_hstage(int dev);
/* WP5: the distributed product tier (rns_dist.c): C = A B over the four APUs' four-step transform (dbig operands) */
struct dbig_s; void rns_mul_dist_db(struct dbig_s *C, const struct dbig_s *A, const struct dbig_s *B);   /* dbig operands (dbig.h) */
void rns_mul_low_db(struct dbig_s *C, const struct dbig_s *A, const struct dbig_s *B, size_t w);        /* low w limbs of A B */
/* the high product (only C >> cut used: the grid's pieces below the cut skipped) and the transform
 * cache of the grid's pieces (RNS_DIST_CACHE slots; hold(1) keeps the planes across products -- the reciprocal's last
 * doubling for the division's X Q -- and returns whether it holds; hold(0) empties them; release frees the planes) */
void rns_mul_high_db(struct dbig_s *C, const struct dbig_s *A, const struct dbig_s *B, size_t cut);
void rns_mul_band_db(struct dbig_s *C, const struct dbig_s *A, const struct dbig_s *B, size_t lowcut, size_t w);   /* both cuts -- A B mod B^w, the pieces at or above w and those ending at or below lowcut skipped */
void rns_dist_cache_stats(size_t *hits, size_t *misses);
void rns_dist_cache_release(void);                     /* the cache's planes freed (the end of the dm phase) */
void rns_dist_cache_budget(double room_bytes, int ranks_on_node);   /* ECALC_BUDGET_CHECK's room for this rank and its node's rank count (the transform-cache fit rule) */
void *rns_dpool(int dev, int which, size_t bytes);
extern int rns_pool_grow;   /* RNS_POOL_GROW=1 lets a plane pool grow inside a phase (else abort) */
size_t rns_dpool_donate_tail(int dev, int which, size_t used);   /* I3: the pool's unused tail to the dbig block pool */
void rns_release_staging(void);
struct rns_dist_stats { size_t n; double t_total, t_stage, t_load, t_ntt, t_crt, t_merge; size_t n_formed, n_skipped; };   /* n_formed / n_skipped: mul_grid's piece counts, summed (the reciprocal reports its cut) */
extern struct rns_dist_stats rns_dist_st;                    /* device dev's pinned NUMA-local staging (2^pool_log limbs), free between products */

#ifdef __cplusplus
}
#endif
#endif

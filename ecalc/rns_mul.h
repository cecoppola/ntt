/* rns_mul.h - the multiply tiers (PLAN.md 8, step 3).
 *
 * mdev: one product, one prime per device.  Per device d (prime d): the host
 * threads of NUMA node d copy A and B into the device's pinned staging buffer
 * (paper: "repack to hstage_buf"), a kernel canonicalises them into the
 * device pools ch_da / ch_db (2^POOL_LOG points each), forward NTT x 2,
 * pointwise fused into the inverse (PW_FUSE), and a kernel stores the
 * residue plane back into the staging buffer (rule 5: kernel store, not
 * hipMemcpy).  Then the CPU CRT (crt.h) over the four planes gives the limbs.
 *
 * split: any product over 2^POOL_LOG points is a Karatsuba over halves
 * (Q1 decision (i), RESULTS.md 35); unbalanced operands are chunked instead.
 * POOL_LOG = 32 (alternative (ii)) is a runtime switch: rns_init(32).
 *
 * school: products under RNS_SCHOOL_MAX points on the CPU.
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
    double t_repack, t_h2d, t_fwd, t_inv, t_d2h, t_crt, t_total;
    double tb_scatter, tb_ntt, tb_crt, tb_merge, tb_total;   /* batch tier, max over devices per tile, summed */
    size_t n_mdev, n_split, n_school, n_batch, points_mdev;
} rns_stats;
extern rns_stats rns_st;
extern int rns_school_max;        /* points (na+nb) below which the CPU schoolbook is used (default 1024) */
extern int rns_crt_threads;       /* default: all */
extern int rns_crt_layout;        /* RNS_CRT_LAYOUT: 0 one plane per node (default), 1 quartered node-local planes */

int  rns_init(int pool_log);      /* 31 (default, paper) or 32; allocates staging + pools, returns ndev */
int  rns_pool_log(void);
int  rns_ndev(void);
void rns_shutdown(void);

/* C = A * B (any sizes; C may not alias A or B) */
void rns_mul(bigint *C, const bigint *A, const bigint *B);
void rns_free_scratch(void);      /* release the split tiers' grow-only temporaries */
/* C = (A B) mod 2^(64 w): the low product, split so nothing above w limbs is formed */
void rns_mul_low(bigint *C, const uint64_t *a, size_t na, const uint64_t *b, size_t nb, size_t w);
/* C = A * B by one mdev product; na + nb must fit 2^pool_log points, >= 2^10 */
void rns_mul_mdev(bigint *C, const uint64_t *a, size_t na, const uint64_t *b, size_t nb);

/* mdev_pair: C1 = A1 B and C2 = A2 B, B transformed once (falls back to two rns_mul) */
void rns_mul_pair(bigint *C1, const bigint *A1, bigint *C2, const bigint *A2, const bigint *B);

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
extern size_t rns_batch_tile_bytes;   /* default 15e9 (paper) */
extern int rns_gpucrt_min;            /* default 1: GPU CRT with S stripes per product so M S >= rns_gpucrt_blocks (912) */
extern int rns_gpucrt_blocks;
extern int rns_engine;               /* RNS_ENGINE: 1 paper's (4 x 52-bit FP64 Barrett), 2 two 62-bit primes / 45-bit points (Phase 5 item 5) */
extern int rns_mdev_gpucrt;          /* 1: mdev's CRT on the GPUs (striped, peer reads) into device 0's staging; 0: CPU Garner */
void rns_mul_batch(rns_prod *P, size_t N);
uint64_t *rns_hstage(int dev);                    /* device dev's pinned NUMA-local staging (2^pool_log limbs), free between products */

#ifdef __cplusplus
}
#endif
#endif

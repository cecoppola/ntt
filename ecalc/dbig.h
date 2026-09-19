/* dbig.h - a big integer resident in device memory (WP5 step 3): the limbs
 * [0, n) live in four quarters, quarter d on APU d, each qc = cap/4 limbs
 * (qc = 2^lq or 3 2^lq, quarter of limb i = (i >> lq) [/ 3]).  Every
 * operation runs as one kernel per APU over that APU's quarter of the result,
 * reading the operands wherever they are (peer access); carries across chunks
 * and quarters are resolved by a scan on the host (n / 4096 chunk flags).
 * Both bases (bi_decimal).  Normalised: n == 0 or limb n-1 != 0. */
#ifndef EC_DBIG_H
#define EC_DBIG_H
#include <stdint.h>
#include <stddef.h>
#include "bigint.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DB_NQ 4
typedef struct dbig_s { uint64_t *q[DB_NQ]; size_t n, cap, qc, off; } dbig;   /* qc: limbs per quarter (exact, a multiple of 4096); off: a view's first limb (cap 0: not owning) */
void db_init(dbig *x);
void db_free(dbig *x);
struct db_stats { size_t n_shift, n_addsub, n_maxidx, n_reserve; double t_shift, t_addsub, t_maxidx, t_reserve; };
extern struct db_stats db_st;                        /* accumulated; the caller resets */
void db_release_pools(void);                          /* free the cached quarter blocks and donated regions (end of a phase) */
void db_donate(int dev, void *p, size_t bytes);
void db_donate_ext(int dev, void *p, size_t bytes, int own);
void db_pregrow(int dev, size_t bytes);              /* one owned region of `bytes` into the pool now (callable from a background thread) */   /* own = 0: a borrowed range (e.g. a plane pool's tail), never freed by db_release_pools */      /* a device region for the block free lists; released by db_release_pools */
size_t db_pool_bytes(void);
size_t db_pool_free_bytes(int dev);                  /* free bytes in the pool (all extents) */
int db_pool_extents(int dev);                        /* number of free extents (fragmentation) */
void db_reserve(dbig *x, size_t limbs);               /* grow-only; contents kept up to min(old n, new cap) */
void db_from_bi(dbig *x, const bigint *a);            /* DMA in */
void db_to_bi(bigint *r, const dbig *x);              /* DMA out */
void db_copy(dbig *r, const dbig *a);
void db_set_zero(dbig *r);
void db_set_u64(dbig *r, uint64_t v);
void db_set_base_pow(dbig *r, size_t k);              /* B^k */
void db_norm(dbig *r);
int  db_cmp(const dbig *a, const dbig *b);
void db_add(dbig *r, const dbig *a, const dbig *b);
void db_sub(dbig *r, const dbig *a, const dbig *b);   /* a >= b */
void db_add_spills(dbig *r, const dbig *a, const uint64_t *const sp[4], size_t R, size_t rows, size_t C, size_t n);   /* r = a + spills (rns_dist) */
void db_add_shifted(dbig *r, const dbig *a, size_t k, const dbig *b);   /* r = (a << k limbs) + b */
void db_sub_shifted(dbig *r, const dbig *a, size_t k, const dbig *b);   /* r = (a << k limbs) - b, >= 0 */
void db_sub_pow(dbig *r, const dbig *a, size_t e);   /* r = a - B^e */
void db_pow_sub(dbig *r, size_t e, const dbig *a);   /* r = B^e - a */
void db_shl_limbs(dbig *r, const dbig *a, size_t k);
void db_shr_limbs(dbig *r, const dbig *a, size_t k);
uint64_t db_top(const dbig *a);                       /* limb n-1 (0 if n == 0) */
uint64_t db_limb(const dbig *a, size_t i);
uint64_t db_mod_q(const dbig *x, uint64_t q);
void db_mod_qs(const dbig *x, const uint64_t *qs, int nq, uint64_t *res);   /* several primes (<= 16) in one pass */
void db_set_shifted_low(dbig *r, const dbig *a, size_t m, size_t k, size_t n);   /* r = (a mod B^m) B^k as an n-limb number (zeros + a few limbs) */        /* Phase 8 I3: x mod q (q < 2^63) by a device kernel; the number must start at a chunk boundary (no odd views) */
/* a view of limbs [lo, lo+len) of a (no copy; read-only use; not owning) */
dbig db_view(const dbig *a, size_t lo, size_t len);
#ifdef __cplusplus
}
#endif
#endif

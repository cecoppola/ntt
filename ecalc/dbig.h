/* dbig.h - a big integer resident in device memory (WP5 step 3): the limbs
 * [0, n) live in four quarters, quarter d on APU d, each qc = cap/4 limbs
 * (qc a power of two, so limb i is at q[i >> lq][i & (qc-1)]).  Every
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
typedef struct dbig_s { uint64_t *q[DB_NQ]; size_t n, cap, qc, off; int lq; } dbig;   /* off: a view's first limb (cap 0: not owning) */
void db_init(dbig *x);
void db_free(dbig *x);
struct db_stats { size_t n_shift, n_addsub, n_maxidx, n_reserve; double t_shift, t_addsub, t_maxidx, t_reserve; };
extern struct db_stats db_st;                        /* accumulated; the caller resets */
void db_release_pools(void);                          /* free the cached quarter blocks (end of a phase) */
size_t db_pool_bytes(void);
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
void db_shl_limbs(dbig *r, const dbig *a, size_t k);
void db_shr_limbs(dbig *r, const dbig *a, size_t k);
uint64_t db_top(const dbig *a);                       /* limb n-1 (0 if n == 0) */
uint64_t db_limb(const dbig *a, size_t i);
/* a view of limbs [lo, lo+len) of a (no copy; read-only use; not owning) */
dbig db_view(const dbig *a, size_t lo, size_t len);
#ifdef __cplusplus
}
#endif
#endif

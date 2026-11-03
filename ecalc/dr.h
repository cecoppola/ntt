/* dr.h - C3 "detect and retry" (results/CORRECTNESS.md; ALGORITHM.md §10): the shared part of the checks that make a product repeat itself
 * instead of ending the run when a transient fault corrupts one of its pieces.
 *
 *   switch   ECALC_DR = 0 (default: off, the code paths are the unchanged ones), 1 = the CRT flag of a P24 piece in mn_core is a
 *            collective retry (step 2), 2 = + the residue identity of the tree products (step 3), 3 = + the division checks (step 4).
 *            ECALC_DR_TOTAL (default 8) = the most failures a run survives; ECALC_DR_TEST[_K|_PE|_N|_M] = fault injection (below).
 *   context  dr_ctx_set / dr_where: the phase a product runs in, for the location in a failure line (set on the main thread).
 *   counters the exit summary line (dr_summary).
 *   residues the host helpers of the tracked-residue checks: arithmetic modulo the 62-bit prime DR_Q (= t1_q[0] of verify.c), B^e,
 *            the residue of a limb array and of a number cut into shifted parts, the D identity's m in {-1, 0, 1}, and the
 *            residues modulo B+1 and B-1 (B^2-1 divides B^P-1 for even P) that check a cyclic product.
 * Host only (no HIP): tests/t_dr builds it with gcc. */
#ifndef EC_DR_H
#define EC_DR_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DR_Q 4611686018427388039ULL            /* the checking prime: the first prime above 2^62 (verify.c t1_q[0]) */
#define DR_B 1000000000000000000ULL            /* the limb base 10^18 */

/* ---- switches ---- */
int dr_level(void);                            /* ECALC_DR (0 off) */
static inline int dr_on(void) { return dr_level() > 0; }
int dr_total_cap(void);                        /* ECALC_DR_TOTAL (default 8): failures per run before the run stops */
void dr_init_from_env(void);                   /* parse the environment again (tests); the first use of any function does it once */

/* ---- fault injection: ECALC_DR_TEST=<kind>, _K = the k-th occurrence (default 1), _PE = one PE (default 0; -1 = every PE), _N = consecutive attempts (default 1; 2 = persistent), _M = consecutive products from the K-th (default 1) ---- */
enum { DR_FLAG, DR_DELIVER, DR_STORE, DR_XQL, DR_CYC, DR_LEAF, DR_NKINDS };
#define DR_SLOTS 16                            /* latches: one per local worker thread */
const char *dr_kind_name(int kind);
int dr_test_kind(void);                        /* the kind, or -1 */
int dr_test_k(void);
int dr_test_pe(void);
int dr_test_n(void);
int dr_test_m(void);
int dr_test_leafslow(void);                    /* ECALC_DR_TEST=...,leafslow (a modifier, alone or beside a kind): the T1 recurrence of the leaf waits for the division's start (test only: the leaf check's deferred path) */
/* 1 if this site injects now: kind matches, occurrence occ == K, pe == _PE (or _PE = -1), and this is the attempt the latch of `slot` is on (< N attempts in a row,
 * starting at the first matching site).  Counts the injection. */
int dr_inject(int kind, size_t occ, int pe, int slot, int attempt);

/* ---- counters and the exit summary ---- */
enum { DR_C_CHECK, DR_C_INJECTED, DR_C_FAIL, DR_C_REC_PIECE, DR_C_REC_PRODUCT, DR_C_REC_DIV, DR_C_FATAL, DR_C_PROD, DR_C_LEAF, DR_C_LEAF_DEFER, DR_C_LEAF_SKIP, DR_C_DIV, DR_C_DIV_OFF, DR_C_N };   /* CHECK = the piece decisions of step 2, PROD = the residue checks of the products (B), LEAF = the leaf check (C), DIV = the division checks D and D' (step 4) */
void dr_count(int c, size_t v);                /* atomic add */
size_t dr_counter(int c);
void dr_summary(void);                         /* "DR: <checks> checks, ..." once per call; registered at exit by dr_arm; call it before ec_fatal (which is _exit) */
void dr_arm(void);                             /* register dr_summary at exit (once) */

/* ---- context (main thread) ---- */
void dr_ctx_set(const char *phase, int level, int g0, int g, int sched, int combine, int step);   /* a field that does not apply: -1 */
void dr_ctx_clear(void);
int dr_where(char *buf, size_t n);             /* the phase as text ("tree level 3, group [0, 4), sched 2, combine 1"); "" when unset; returns the length */

/* ---- residues modulo DR_Q ---- */
uint64_t dr_addmod(uint64_t a, uint64_t b);
uint64_t dr_submod(uint64_t a, uint64_t b);
uint64_t dr_mulmod(uint64_t a, uint64_t b);
uint64_t dr_powmod(uint64_t b, uint64_t e);
uint64_t dr_powB(uint64_t e);                  /* B^e mod DR_Q */
uint64_t dr_limbs_res(const uint64_t *l, size_t n);                    /* sum l[i] B^i mod DR_Q (limbs < 2^64 allowed) */
uint64_t dr_res_shift(uint64_t r, uint64_t e);                         /* r B^e */
void dr_res_part(uint64_t *total, uint64_t part, uint64_t off);        /* *total += part B^off: the residue of a number held as shifted parts (shares, stripes) */
/* The D identity of a low product (newton_db.c): E = (R(S) B^e - R(X) R(Q)) - (R(Aw) - R(xql)) must be 0 or +-B^w modulo DR_Q.
 * Returns m in {-1, 0, 1} with E = m B^w, or 2 when it is none of the three (a fault). */
int dr_match_m(uint64_t e_res, uint64_t rBw);

/* ---- residues modulo B+1 and B-1 (a cyclic product of P limbs, P even, is checked modulo B^2-1 = (B-1)(B+1)) ---- */
uint64_t dr_mod_bp1(const uint64_t *l, size_t n);                      /* sum (-1)^i l[i] mod (B+1), in [0, B] */
uint64_t dr_mod_bm1(const uint64_t *l, size_t n);                      /* sum l[i] mod (B-1), in [0, B-2] */
uint64_t dr_mulmod_m(uint64_t a, uint64_t b, uint64_t m);
/* U = A B mod (B^P - 1), the limb arrays' residues (plus / minus) given: 1 if U's match the product of A's and B's in both */
int dr_cyc_ok(uint64_t up, uint64_t um, uint64_t ap, uint64_t am, uint64_t bp, uint64_t bm);

#ifdef __cplusplus
}
#endif
#endif

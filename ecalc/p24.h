/* p24.h - Phase 15 Batch 3, agent P24 (results/P2415.md): four primes at 24 digits per transform point in the mn tier (MN_P24).
 * The index maps, host and device (rns_dist.c's P24 paths, p24_mn.h's kernels, dbig.c's spill decode and tests/t_p24.c use them).
 *
 * Point x of an operand view holds the view's digits [24 x, 24 x + 24): the digits [s, 18) of limb L(x) = floor(4 x / 3), s = s(x) =
 * 6 (x mod 3), and the digits [0, 6 + s) of limb L(x) + 1 -- 4 limbs of 18 digits = 3 points of 24.  A view of n limbs has
 * ceil(3 n / 4) points.  A product's coefficient c_x (weight 10^(24 x)) is c_x 10^s(x) at limb L(x): its four base-10^18 digits,
 * shifted by s digits, land on the limbs L(x) .. L(x) + 3 (at most three points meet a limb).
 *
 * Rank rho of the plane (rows [a, a + r) of R, R a power of two >= 2^10, r >= 32) holds in column j the points [x0, x1) =
 * [R j + a, R j + a + r).  Their limbs are its RUN in column j: the in-run [L(x0), L(x1) + 1) (the operand side: the last point's
 * high part is at most limb L(x1)), the out-run [L(x0), L(x1)) (the result side: the out-runs of all ranks and columns tile the
 * limbs).  A rank's SEQUENCE is its runs in column order (as today's is its rows in column order).  The run lengths are periodic in j
 * with period 3 (L(R j + c) = R j + c + floor((R j + c) / 3) and R = 1 or 2 mod 3), so the prefix S(j) and both directions of the
 * map (limb -> first sequence index at or above it; sequence index -> limb) are O(1). */
#ifndef EC_P24_H
#define EC_P24_H
#include <stdint.h>
#include <stddef.h>
#if defined(__HIPCC__) || defined(__HIP__)
#include <hip/hip_runtime.h>
#define P24_HD __host__ __device__
#else
#define P24_HD
#endif
#include "modarith.h"

#define P24_CAP_LOG 40                            /* a P24 plane is at most 2^40 points: the CRT's four-limb output (below) */
#define P24_MIN_MAX 1000000000000ULL              /* min(Pa, Pb) <= 10^12: c < 10^12 (10^24 - 1)^2 < 10^60, so c 10^12 < 10^72 = four limbs */

P24_HD static inline size_t p24_L(size_t x) { return x + x / 3; }                      /* the limb of point x's low part: floor(4 x / 3) */
P24_HD static inline int p24_s(size_t x) { return 6 * (int)(x % 3); }                  /* the digit of that limb where point x starts */
P24_HD static inline size_t p24_pts(size_t n) { return (3 * n + 3) / 4; }              /* the points of a view of n limbs: ceil(3 n / 4) */
P24_HD static inline size_t p24_xfl(size_t l) { return (3 * l + 2) / 4; }              /* the largest point x with L(x) <= l */
P24_HD static inline size_t p24_xceil(size_t v) { return v ? p24_xfl(v - 1) + 1 : 0; }  /* the smallest point x with L(x) >= v */

/* one rank's runs: R, C the plane, a its first row, r its rows, ex 1 (in-runs) or 0 (out-runs); len[e] the run of column j = 3 w + e */
struct p24_run { size_t R, C, a, r, len[3], P3; int ex; };
P24_HD static inline struct p24_run p24_run_make(size_t R, size_t C, size_t a, size_t r, int ex)
{
    struct p24_run m; m.R = R; m.C = C; m.a = a; m.r = r; m.ex = ex;
    for (int e = 0; e < 3; e++) m.len[e] = r + (size_t)ex + (R * e + a + r) / 3 - (R * e + a) / 3;
    m.P3 = m.len[0] + m.len[1] + m.len[2];
    return m;
}
P24_HD static inline size_t p24_run_S(const struct p24_run *m, size_t j)              /* the sequence index of column j's first limb (j <= C) */
{
    size_t u = j / 3, v = j - 3 * u, s = u * m->P3;
    if (v > 0) s += m->len[0];
    if (v > 1) s += m->len[1];
    return s;
}
P24_HD static inline size_t p24_run_A(const struct p24_run *m, size_t j) { return p24_L(m->R * j + m->a); }   /* column j's first limb */
P24_HD static inline size_t p24_run_total(const struct p24_run *m) { return p24_run_S(m, m->C); }
/* the first sequence index whose limb is >= l (the whole sequence's length when none is) */
P24_HD static inline size_t p24_seq_start(const struct p24_run *m, size_t l)
{
    size_t x = p24_xfl(l);
    if (x < m->a) return 0;                                                            /* every run starts above l */
    size_t j = (x - m->a) / m->R;                                                      /* the last column whose run starts at or below l */
    if (j >= m->C) return p24_run_total(m);
    size_t o = l - p24_run_A(m, j), n = m->len[j % 3];
    return p24_run_S(m, j) + (o < n ? o : n);
}
/* the limb of sequence index t (t < the total), and its column */
P24_HD static inline size_t p24_seq_limb(const struct p24_run *m, size_t t, size_t *col)
{
    size_t u = t / m->P3, rem = t - u * m->P3; int v = 0;
    while (v < 2 && rem >= m->len[v]) { rem -= m->len[v]; v++; }
    size_t j = 3 * u + (size_t)v; if (col) *col = j;
    return p24_run_A(m, j) + rem;
}
/* the columns j whose 4-limb spill block [L(R j + a1), + 4) meets [lo, hi): [j0, j1) (a1 = the first row of the rank after the
 * spilling one: R for the last) */
P24_HD static inline void p24_spill_cols(size_t lo, size_t hi, size_t a1, size_t R, size_t C, size_t *j0, size_t *j1)
{
    size_t y0 = p24_xceil(lo > 3 ? lo - 3 : 0), y1 = p24_xceil(hi);                    /* L(R j + a1) >= lo - 3, L(R j + a1) >= hi */
    size_t x0 = y0 > a1 ? (y0 - a1 + R - 1) / R : 0, x1 = y1 > a1 ? (y1 - a1 + R - 1) / R : 0;
    if (x0 > C) x0 = C;
    if (x1 > C) x1 = C;
    if (x1 < x0) x1 = x0;
    *j0 = x0; *j1 = x1;
}
/* the spill block that meets limb m of a product over nr ranks (rank rho holds rows [R rho / nr, R (rho + 1) / nr)): 0 if none, else
 * 1 with the spilling rank *rho, its column *j and the offset *t < 4 in its block (the blocks are >= 42 limbs apart: one at most) */
P24_HD static inline int p24_spill_at(size_t m, size_t R, size_t C, int nr, int *rho, size_t *j, size_t *t)
{
    size_t x = p24_xfl(m), jj = x / R, rem = x - jj * R;
    size_t q = (((rem + 1) * (size_t)nr + R - 1) / R) - 1;                             /* the rank whose rows hold rem (part_owner) */
    size_t bs = R * jj + R * q / (size_t)nr, tt = m - p24_L(bs);
    if (tt >= 4) return 0;
    if (q == 0) { if (jj < 1 || jj - 1 >= C) return 0; *rho = nr - 1; *j = jj - 1; }
    else { if (jj >= C) return 0; *rho = (int)q - 1; *j = jj; }
    *t = tt; return 1;
}
/* the powers 10^k for k = 0, 6, 12, 18 */
P24_HD static inline uint64_t p24_p10(int k) { return k == 0 ? 1ULL : k == 6 ? 1000000ULL : k == 12 ? 1000000000000ULL : 1000000000000000000ULL; }
/* point x's two parts from its limbs lo = L(x), hi = L(x) + 1: value = part_lo + part_hi 10^(18 - s) < 10^24 */
P24_HD static inline void p24_parts(uint64_t lo, uint64_t hi, int s, uint64_t *plo, uint64_t *phi)
{
    switch (s) {                                                                       /* (constant divisors) */
    case 0:  *plo = lo; *phi = hi % 1000000ULL; break;
    case 6:  *plo = lo / 1000000ULL; *phi = hi % 1000000000000ULL; break;
    default: *plo = lo / 1000000000000ULL; *phi = hi; break;
    }
}
/* the CRT side: coefficient c = d[0] + d[1] B + d[2] B^2 + d[3] B^3 (B = 10^18, the digits < B) times 10^s, s in {0, 6, 12}, as the
 * digits e[0..3] (+ e4, which the bound makes 0): e_k = (d_k mod 10^(18-s)) 10^s + floor(d_(k-1) / 10^(18-s)) < B */
P24_HD static inline uint64_t p24_shift_digits(const uint64_t d[4], int s, uint64_t e[4])
{
    if (s == 0) { e[0] = d[0]; e[1] = d[1]; e[2] = d[2]; e[3] = d[3]; return 0; }
    uint64_t lo = s == 6 ? 1000000000000ULL : 1000000ULL, up = s == 6 ? 1000000ULL : 1000000000000ULL;   /* 10^(18-s), 10^s */
    uint64_t c = 0;
    for (int k = 0; k < 4; k++) { uint64_t q = d[k] / lo, r = d[k] - q * lo; e[k] = r * up + c; c = q; }
    return c;
}
/* the regroup's residue: point x's value mod p from its limbs lo = L(x), hi = L(x) + 1 -- part_lo + part_hi (10^(18 - s) mod p);
 * c18s = 10^(18 - s) mod p (canonical).  Canonical out. */
P24_HD static inline uint64_t p24_point_mod(uint64_t lo, uint64_t hi, int s, ec_mod m, uint64_t c18s)
{
    uint64_t pl, ph; p24_parts(lo, hi, s, &pl, &ph);
    uint64_t r = (uint64_t)ec_mm((double)ec_canon64(ph, m.pu, m.mu), (double)c18s, m.p, m.pinv) + ec_canon64(pl, m.pu, m.mu);
    return ec_fold(r, m.pu);
}
/* 10^(18 - s) mod p for s = 0, 6, 12 (c[s / 6]) */
static inline void p24_c18(int prime, uint64_t c[3]) { for (int i = 0; i < 3; i++) c[i] = p24_p10(18 - 6 * i) % ec_P[prime]; }
#if defined(__HIPCC__) || defined(__HIP__)
/* p24_crt.c: the CRT of a rank's plane at four primes into its out-runs (the sequence of p24_run_make(R, C, a, rows, 0)) and one
 * 4-limb spill per column at L(R j + a + rows); planes p0..p3 hold the rank's points column after column (index j rows + k, as
 * k_crt_batch reads them after mn_core's transpose); gc from rns_gconst (np 4) */
struct gconst;
void p24_crt_launch(const uint64_t *p0, const uint64_t *p1, const uint64_t *p2, const uint64_t *p3, const struct gconst *gc,
                    size_t R, size_t C, size_t a, size_t rows, uint64_t *out, uint64_t *spill, hipStream_t s);
unsigned p24_crt_err(hipStream_t s);                  /* the device flag of the calls on this device since the last read (0: none), cleared */
#endif
#endif

/* p24_mn.h - Phase 15 Batch 3, agent P24 (results/P2415.md): the P24 kernels of mn_core's exchanges (MN_P24).  Included by rns_dist.c
 * after k_scatter_mn (it uses struct acc, struct seg, acc_get / acc_ptr and part0 / partn from there).  The index maps are p24.h's:
 * rank rho's in-runs (ex 1: the operands) and out-runs (ex 0: the result) in place of its rows. */
#ifndef EC_P24_MN_H
#define EC_P24_MN_H
#include "p24.h"
__host__ __device__ static inline struct p24_run p24_rank_run(size_t R, size_t C, int nr, int rho, int ex) { return p24_run_make(R, C, part0(R, nr, rho), partn(R, nr, rho), ex); }
/* k_pack_mn's P24 form: this node's part [lo, ..) of an operand view -> the segments sg[r] of the in-run sequences of the ranks
 * rho = g d + r (a limb at a run's end goes to both ranks whose runs hold it: each segment is packed on its own) */
__global__ void k_pack_mn24(uint64_t *sb, struct acc src, size_t lo, size_t R, size_t C, int nr, int g, int d, size_t S, const struct seg *sg)
{
    size_t total = (size_t)g * S, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t r = t / S, k = t - r * S;
        if (k >= sg[r].t1 - sg[r].t0) continue;
        struct p24_run m = p24_rank_run(R, C, nr, g * d + (int)r, 1);
        sb[sg[r].off + k] = acc_get(src, p24_seq_limb(&m, sg[r].t0 + k, 0) - lo);
    }
}
/* k_gather_mn's P24 form (canonical, the transform's row layout x[il C + j]): point x = R j + a + il of the view from its two limbs
 * in my in-run sequence rb [0, tend) (zero beyond), reduced mod p (c18[s / 6] = 10^(18 - s) mod p) */
__global__ void k_gather_mn24(uint64_t *x, const uint64_t *rb, size_t tend, size_t R, size_t C, size_t a, size_t rows, ec_mod m, uint64_t c0, uint64_t c6, uint64_t c12)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    struct p24_run mr = p24_run_make(R, C, a, rows, 1);
    for (; t < total; t += stride) {
        size_t j = t / rows, il = t - j * rows, x0 = R * j + a, xp = x0 + il, u = p24_run_S(&mr, j) + (p24_L(xp) - p24_L(x0));
        uint64_t lo = u < tend ? rb[u] : 0, hi = u + 1 < tend ? rb[u + 1] : 0; int s = p24_s(xp);
        x[il * C + j] = p24_point_mod(lo, hi, s, m, s == 0 ? c0 : s == 6 ? c6 : c12);
    }
}
/* k_gather_mn24 for the np planes at once (A's gathers: the two limbs and the parts read and split once, np residues written) */
struct p24_c18s { uint64_t c[3 * EC_NP]; };                  /* c[3 p + s / 6] = 10^(18 - s) mod p (by value) */
__global__ void k_gather_mn24_np(uint64_t *x0p, uint64_t *x1p, uint64_t *x2p, uint64_t *x3p, int np, const uint64_t *rb, size_t tend, size_t R, size_t C, size_t a, size_t rows,
                                 ec_mod m0, ec_mod m1, ec_mod m2, ec_mod m3, struct p24_c18s cs)
{
    const uint64_t *c18 = cs.c;
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    struct p24_run mr = p24_run_make(R, C, a, rows, 1);
    for (; t < total; t += stride) {
        size_t j = t / rows, il = t - j * rows, x0 = R * j + a, xp = x0 + il, u = p24_run_S(&mr, j) + (p24_L(xp) - p24_L(x0)), o = il * C + j;
        uint64_t lo = u < tend ? rb[u] : 0, hi = u + 1 < tend ? rb[u + 1] : 0, pl, ph; int s = p24_s(xp), si = s / 6;
        p24_parts(lo, hi, s, &pl, &ph);
        #define P24_RES(m, ip) ec_fold((uint64_t)ec_mm((double)ec_canon64(ph, (m).pu, (m).mu), (double)c18[3 * (ip) + si], (m).p, (m).pinv) + ec_canon64(pl, (m).pu, (m).mu), (m).pu)
        x0p[o] = P24_RES(m0, 0); x1p[o] = P24_RES(m1, 1); x2p[o] = P24_RES(m2, 2); if (np > 3) x3p[o] = P24_RES(m3, 3);
        #undef P24_RES
    }
}
/* k_scatter_mn's P24 form: the received segments of the ranks (r, d), r < g (their out-run sequences) -> the window's limbs */
__global__ void k_scatter_mn24(struct acc dst, size_t lo, const uint64_t *rb, const struct seg *sg, int g, size_t S, size_t R, size_t C, int nr, int d)
{
    size_t total = (size_t)g * S, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t r = t / S, k = t - r * S; if (k >= sg[r].t1 - sg[r].t0) continue;
        struct p24_run m = p24_rank_run(R, C, nr, g * d + (int)r, 0);
        *acc_ptr(dst, p24_seq_limb(&m, sg[r].t0 + k, 0) - lo) = rb[sg[r].off + k];
    }
}
#endif

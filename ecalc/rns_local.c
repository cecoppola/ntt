/* rns_local.c - form S: the product of one PE on its own device (S1b of the TARGET1 plan, ECALC_VPE=1 only).
 *
 * C = A B for dbig operands of one quarter each (dbig.h, vPE), n = a transform length (2^k or 3 2^(k-2), at least 2^20) >= na + nb points.  It is
 * form B (rns_dist.c b_core) with the four-APU region and the peer reads removed: the np primes one after the other on this PE's device and
 * stream (bload, forward of A and of B, pointwise product fused into the inverse), np planes of n points kept (plane p = the product mod prime p)
 * and one plane Y for B; then the striped CRT over all stripes on this device and the spills of the stripes added by one chunked-carry pass
 * (db_add_spills, the layout of b_core: stripe g = 4 j + r -> sp[r][j], at limb (g + 1) rows).  The planes come from the dbig block pool.
 * Form B and the one-node four-step (form C over xGMI) do not exist under vPE; the digits are those of every form (the same primes, the same CRT). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hip/hip_runtime.h>
#include "fatal.h"
#include "rns_mul.h"
#include "rns_int.h"
#include "ntt.h"
#include "dbig.h"
#include "mem.h"
#include "vpe.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)

struct lstate {
    int init; ntt_ctx *ctx[EC_NP]; hipStream_t s;
    uint64_t *sp[4]; size_t spcap;                       /* the sparse spill arrays (pinned host: written by the CRT kernel through spall, read by the carry kernel) */
    uint64_t *spall; struct bdesc *desc;                 /* all stripes' spills; the CRT descriptors (device) */
};
VPE_STATE(struct lstate, LS);
/* the transform length for a product of nc limbs: the smaller of 2^k and 3 2^(k-2) (radix-3 when the prime set has it), at least 2^20 (b_len of rns_dist.c) */
size_t rns_local_pts(size_t nc, int *T, int *logk)
{
    int k = 20; while (((size_t)1 << k) < nc) k++;
    if (ec_has_radix3() && k > 20 && ((size_t)3 << (k - 2)) >= nc) { *T = 3; *logk = k - 2; return (size_t)3 << (k - 2); }
    *T = 1; *logk = k; return (size_t)1 << k;
}
/* the operand's n points, canonical, zero beyond its length */
__global__ void k_lload(uint64_t *dst, const uint64_t *src, size_t sn, size_t n, ec_mod m)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) dst[i] = i < sn ? ec_canon64(src[i], m.pu, m.mu) : 0;
}
static unsigned lblk(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > 228 * 16 ? 228 * 16 : b); }
void rns_local_core(dbig *Cd, const dbig *A, const dbig *B, size_t nc)
{
    if (!vpe_on()) { ec_fatal(EC_RC_FATAL, "rns_local_core: form S exists under ECALC_VPE=1 only\n"); }
    struct lstate *v = &VPE(LS); const int dev = vpe_device();   /* S2: pe_geo's ordinal (= the lane in S1) */
    HIP_CHECK(hipSetDevice(dev));
    if (!v->init) {
        for (int p = 0; p < EC_NP; p++) v->ctx[p] = ntt_ctx_create(p);
        HIP_CHECK(hipStreamCreate(&v->s)); HIP_CHECK(hipMalloc((void **)&v->desc, 16 * sizeof(struct bdesc))); v->init = 1;
    }
    const int np = ec_np_prod(nc, "rns_local_core");
    int T, logk; const size_t n = rns_local_pts(nc, &T, &logk);
    const dbig *X = A->n >= B->n ? A : B, *Yo = A->n >= B->n ? B : A;       /* (the product is symmetric; the longer one first, as b_core) */
    uint64_t *pl[EC_NP] = { 0 }, *Y = db_pool_alloc(dev, n * 8);
    for (int p = 0; p < np; p++) pl[p] = db_pool_alloc(dev, n * 8);
    for (int p = 0; p < np; p++) {
        ec_mod m = ec_mod_get(p);
        k_lload<<<lblk(n), 256, 0, v->s>>>(pl[p], X->q[0] + X->off, X->n, n, m);
        k_lload<<<lblk(n), 256, 0, v->s>>>(Y, Yo->q[0] + Yo->off, Yo->n, n, m);
        if (T == 3) { ntt_fwd3(v->ctx[p], pl[p], logk, 1, v->s); ntt_fwd3(v->ctx[p], Y, logk, 1, v->s); ntt_inv3_pw(v->ctx[p], pl[p], Y, logk, 1, v->s); }
        else { ntt_fwd(v->ctx[p], pl[p], logk, 1, v->s); ntt_fwd(v->ctx[p], Y, logk, 1, v->s); ntt_inv_pw(v->ctx[p], pl[p], Y, logk, 1, v->s); }
        HIP_CHECK(hipStreamSynchronize(v->s));
    }
    db_pool_free(dev, Y);
    /* the CRT stripes: rows | qc, G stripes over [0, nc); the spills of the stripes into sp4 */
    size_t rows = 16384; while (Cd->qc % rows) rows /= 2;
    size_t G = (nc + rows - 1) / rows, Ccol = (G + 3) / 4, R = 4 * rows;
    if (v->spcap < Ccol) {
        if (v->spall) { HIP_CHECK(hipHostFree(v->spall)); for (int r = 0; r < 4; r++) HIP_CHECK(hipHostFree(v->sp[r])); }
        v->spcap = Ccol + 1024; HIP_CHECK(hipHostMalloc((void **)&v->spall, v->spcap * 16 * 8, 0));
        for (int r = 0; r < 4; r++) HIP_CHECK(hipHostMalloc((void **)&v->sp[r], v->spcap * 4 * 8, 0));
    }
    memset(v->spall, 0, G * 4 * 8);
    struct gconst gc = rns_gconst(); gc.np = np;
    const uint64_t *P0 = pl[0], *P1 = pl[1], *P2 = pl[2], *P3 = np > 3 ? pl[3] : pl[0];
    struct bdesc hd[16]; size_t c0s[16], S_[16]; int ns = 0;
    for (size_t c0 = 0; c0 < nc;) {
        size_t e = nc;
        if (e - c0 > ((size_t)1 << 31)) e = c0 + ((size_t)1 << 31);
        size_t len = e - c0, full = len / rows * rows;
        if (full) { memset(&hd[ns], 0, sizeof hd[ns]); hd[ns].c = Cd->q[0] + c0; hd[ns].na = (uint32_t)full; c0s[ns] = c0; S_[ns] = full / rows; ns++; }
        if (len > full) { memset(&hd[ns], 0, sizeof hd[ns]); hd[ns].c = Cd->q[0] + c0 + full; hd[ns].na = (uint32_t)(len - full); c0s[ns] = c0 + full; S_[ns] = 1; ns++; }
        c0 = e;
        if (ns > 14) { ec_fatal(EC_RC_FATAL, "rns_local_core: too many CRT segments\n"); }
    }
    HIP_CHECK(hipMemcpyAsync(v->desc, hd, ns * sizeof hd[0], hipMemcpyHostToDevice, v->s));
    HIP_CHECK(hipStreamSynchronize(v->s));                 /* (hd is on the stack) */
    for (int i = 0; i < ns; i++) {
        size_t c0 = c0s[i];
        k_crt_batch<<<(unsigned)S_[i], CRT_THREADS, 0, v->s>>>(P0 + c0, P1 + c0, P2 + c0, P3 + c0, v->desc + i, 0, (int)S_[i], 0, gc, v->spall + (c0 / rows) * 4);
    }
    HIP_CHECK(hipStreamSynchronize(v->s));
    for (int p = 0; p < np; p++) db_pool_free(dev, pl[p]);
    for (size_t g = 0; g < 4 * Ccol; g++) { uint64_t *o = v->sp[g & 3] + (g >> 2) * 4; if (g < G) memcpy(o, v->spall + g * 4, 32); else memset(o, 0, 32); }
    Cd->n = nc;
    const uint64_t *spp[4] = { v->sp[0], v->sp[1], v->sp[2], v->sp[3] };
    db_add_spills(Cd, Cd, spp, R, rows, Ccol, nc);
    if (Cd->n > nc) { ec_fatal(EC_RC_FATAL, "rns_local_core: carry out of the product\n"); }
}

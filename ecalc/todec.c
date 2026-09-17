/* todec.c - see todec.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include <hip/hip_runtime.h>
#include "todec.h"
#include "rns_mul.h"
#include "newton.h"
#include "mem.h"
#include "modarith.h"

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

dec_stats dec_st;
int dec_verbose = 0;
int dec_leaf_u_max = 256;
int dec_seed_prewarm = 1;
int todec_free_input = 0;   /* 1: free X's limbs once copied into the level pool (the caller must not use X after) */

/* ---- LEAF kernel: one thread per piece of nl limbs (< 256), u blocks of 18
 * digits, high block first in the output.  q = floor(x / 10^18) for x < 2^124
 * by Barrett with mu = floor(2^123 / 10^18): q_est = (x mu) >> 123 is off by
 * at most 3 below (the paper's "<= 3 conditional subtracts"). ------------ */
#define TEN18 1000000000000000000ULL
__device__ static inline void div1e18(uint64_t hi, uint64_t lo, uint64_t *q, uint64_t *r)
{
    /* x = hi 2^64 + lo, hi < 10^18 < 2^60, so x < 2^124.  x mu (mu 64 bits): 188 bits. */
    unsigned __int128 x = ((unsigned __int128)hi << 64) | lo;
    unsigned __int128 m_lo = (unsigned __int128)lo * EC_MU_1E18;             /* 128 bits */
    unsigned __int128 m_hi = (unsigned __int128)hi * EC_MU_1E18 + (m_lo >> 64);  /* * 2^64 */
    /* (x mu) >> 123 = (m_hi 2^64 + (m_lo mod 2^64)) >> 123 = m_hi >> 59 */
    uint64_t qe = (uint64_t)(m_hi >> 59);
    unsigned __int128 rem = x - (unsigned __int128)qe * TEN18;
    while (rem >= TEN18) { rem -= TEN18; qe++; }
    *q = qe; *r = (uint64_t)rem;
}
__global__ void k_leaf(const uint64_t *pool, size_t stride, size_t np, int nl, int u, char *out, size_t out_stride, size_t lead)
{
    /* piece i (global index gi = i + piece0) covers padded digit positions [gi Lg, (gi+1) Lg); the
     * first `lead` padded digits are dropped: out points at padded position 0 - lead */
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= np) return;
    uint64_t x[256];
    const uint64_t *src = pool + i * stride;
    int n = nl;
    for (int j = 0; j < nl; j++) x[j] = src[j];
    while (n && x[n - 1] == 0) n--;
    size_t pos0 = i * out_stride;                        /* padded position of this piece's first digit */
    for (int b = u - 1; b >= 0; b--) {                  /* low block first: goes at the end */
        uint64_t r = 0;
        for (int j = n; j-- > 0;) { uint64_t q; div1e18(r, x[j], &q, &r); x[j] = q; }
        while (n && x[n - 1] == 0) n--;
        size_t p = pos0 + (size_t)b * 18;
        for (int c = 17; c >= 0; c--) { size_t g = p + c; if (g >= lead) out[g - lead] = (char)('0' + r % 10); r /= 10; }
    }
}

/* ---- level pools and the divisor cache -------------------------------------- */
static uint64_t *g_pool[2]; static size_t g_cap[2];
static uint64_t *g_rw; static size_t g_rw_cap;
static uint64_t *g_scr; static size_t g_scr_cap, g_scr_need;   /* DEEP scratch: products of a level, registered */      /* per piece: the 2 limbs of the remainder window above the lo slot */
static uint64_t *pool_get(int which, size_t limbs)
{
    if (g_cap[which] < limbs) {
        if (g_pool[which]) mem_hreg_free(g_pool[which]);
        size_t cap = limbs + limbs / 16 + 4096;
        g_pool[which] = (uint64_t *)mem_hreg_alloc(cap * 8);
        g_cap[which] = cap;
        if (dec_verbose) printf("dc: level pool %d -> %.2f GB\n", which, cap * 8e-9);
    }
    if (limbs > dec_st.peak_pool_limbs) dec_st.peak_pool_limbs = limbs;
    return g_pool[which];
}
struct divisor { unsigned long h; bigint T, mu; size_t k; uint64_t *reg; };   /* T = 10^h, mu ~ 2^(64 (nT + k)) / T; both moved into registered memory (reg) */

static size_t limbs_for_digits(unsigned long dig) { return (size_t)ceil(dig * log2(10.0) / 64.0) + 1; }

/* view helpers: bigint over a pool slot */
static void view(bigint *v, uint64_t *p, size_t n) { v->l = p; v->n = limb_norm(p, n); v->cap = 0; }

static void par_copy64(uint64_t *dst, const uint64_t *src, size_t n)
{
#pragma omp parallel for schedule(static) if (n > (1u << 22))
    for (size_t i = 0; i < n; i += 1 << 18) { size_t m = n - i < (1 << 18) ? n - i : (1 << 18); memcpy(dst + i, src + i, m * 8); }
}
static void par_memset0(uint64_t *p, size_t n)
{
#pragma omp parallel for schedule(static) if (n > (1u << 22))
    for (size_t i = 0; i < n; i += 1 << 18) { size_t m = n - i < (1 << 18) ? n - i : (1 << 18); memset(p + i, 0, m * 8); }
}

char *todec_out;      /* the buffer todec allocated when called with out == NULL (registered; free with mem_hreg_free) */
void todec(char *out, const bigint *X, unsigned long ndig)
{
    double t0 = mem_now(), t;
    memset(&dec_st, 0, sizeof dec_st);
    /* padded digit count Lg 2^m, Lg = 18 u, 128 <= u < dec_leaf_u_max */
    int umin = dec_leaf_u_max / 2, m = 0;
    while ((unsigned long)(18 * umin) << (m + 1) <= ndig) m++;
    unsigned long u = (ndig + ((unsigned long)18 << m) - 1) / ((unsigned long)18 << m);
    if (u < (unsigned long)umin) u = umin;
    if (u >= (unsigned long)dec_leaf_u_max) { m++; u = (ndig + ((unsigned long)18 << m) - 1) / ((unsigned long)18 << m); if (u < (unsigned long)umin) u = umin; }
    unsigned long Lg = 18 * u, ndig_p = Lg << m;
    if (dec_verbose) printf("dc: %lu digits -> %lu = %lu x 2^%d (u = %lu), %d split levels\n", ndig, ndig_p, Lg, m, u, m);

    /* divisor cache, bottom up: h = Lg, 2 Lg, ..., Lg 2^(m-1) */
    t = mem_now();
    struct divisor *dv = (struct divisor *)calloc(m > 0 ? m : 1, sizeof *dv);
    for (int i = 0; i < m; i++) {
        struct divisor *d = &dv[i];
        d->h = Lg << i;
        bi_init(&d->T); bi_init(&d->mu);
        if (i == 0) {
            bigint five, t2; bi_init(&five); bi_init(&t2);
            /* 10^Lg = 5^Lg 2^Lg: 5^Lg by repeated squaring (small) */
            bi_set_u64(&five, 1);
            for (int b = 63; b >= 0; b--) {
                if (five.n) { rns_mul(&t2, &five, &five); bi_copy(&five, &t2); }
                if ((Lg >> b) & 1) { bi_mul_u64(&five, &five, 5); }
            }
            bi_shl(&d->T, &five, Lg);
            bi_free(&five); bi_free(&t2);
        } else {
            rns_mul(&d->T, &dv[i - 1].T, &dv[i - 1].T);              /* T(2h) = T(h)^2 */
        }
        /* pieces divided by T(h) have 2h digits: na <= limbs_for_digits(2h) */
        size_t na = limbs_for_digits(2 * d->h), nT = d->T.n;
        d->k = na - nT + 1;
        if (i == 0 || !dec_seed_prewarm) newton_recip(&d->mu, &d->T, d->k);
        else {
            /* seed: mu(h)^2 ~ 2^(128 (nT_prev + k_prev)) / T(2h) = 2^(64 (nT + j)) / T(2h), j = 2 (nT_prev + k_prev) - nT */
            struct divisor *p = &dv[i - 1];
            bigint sq; bi_init(&sq);
            rns_mul(&sq, &p->mu, &p->mu);
            size_t j = 2 * (p->T.n + p->k) - nT;           /* nominal scale of mu(h)^2 */
            size_t jp = p->k < d->k ? p->k : d->k;           /* its real precision: mu(h)'s */
            if (j > jp) bi_shr(&sq, &sq, 64 * (j - jp));
            newton_recip_seeded(&d->mu, &d->T, d->k, &sq, jp);   /* one doubling (repeats if it has to) */
            bi_free(&sq);
        }
        /* move T and mu into registered memory so the batch tier reads them in place */
        d->reg = (uint64_t *)mem_hreg_alloc((d->T.n + d->mu.n) * 8);
        par_copy64(d->reg, d->T.l, d->T.n); par_copy64(d->reg + d->T.n, d->mu.l, d->mu.n);
        free(d->T.l); d->T.l = d->reg; d->T.cap = 0;
        free(d->mu.l); d->mu.l = d->reg + d->T.n; d->mu.cap = 0;
    }
    newton_free_scratch(); rns_free_scratch();          /* the prewarm's temporaries (~70 GB at 4e10) are not needed by the levels */
    dec_st.t_prewarm = mem_now() - t;
    if (dec_verbose) printf("dc: prewarm %.2f s (%d divisors, top T %zu limbs)\n", dec_st.t_prewarm, m, m ? dv[m - 1].T.n : 0);

    /* DEEP scratch sized once: max over levels of np (nl + mu.n) and np (nl_next + nT) */
    {
        size_t need = 0, npl = 1, nll = limbs_for_digits(ndig_p);
        for (int lev = 0; lev < m; lev++) {
            struct divisor *d = &dv[m - 1 - lev];
            size_t nln = limbs_for_digits(d->h);
            size_t a = npl * (nll + d->mu.n), b = npl * (nln + d->T.n);
            if (npl > 2 && a > need) need = a;
            if (npl > 2 && b > need) need = b;
            npl *= 2; nll = nln;
        }
        g_scr_need = need + 4096;                       /* allocated at the first DEEP level, after the TOP levels' peak */
    }
    /* level 0: one piece, the (padded) value X, slot nl_0 limbs */
    int which = 0;
    size_t np = 1, nl = limbs_for_digits(ndig_p);
    uint64_t *cur = pool_get(0, nl);
    par_memset0(cur, nl);
    par_copy64(cur, X->l, X->n);
    if (todec_free_input) { free(X->l); ((bigint *)X)->l = 0; ((bigint *)X)->n = ((bigint *)X)->cap = 0; }
    bigint Xs, Q, R, prod; bi_init(&Q); bi_init(&R); bi_init(&prod);

    for (int lev = 0; lev < m; lev++) {
        t = mem_now();
        struct divisor *d = &dv[m - 1 - lev];                  /* T = 10^(dig/2) */
        size_t nl_next = limbs_for_digits(d->h), np_next = 2 * np;
        which ^= 1;
        uint64_t *nxt = pool_get(which, np_next * nl_next);
        const char *tier;
        if (np <= 2 || nl + d->mu.n > ((size_t)1 << RNS_BATCH_LOGL_MAX) || nl + d->mu.n > ((size_t)1 << (rns_pool_log() - 1))) {
            tier = np <= 2 ? "TOP" : "MID";
            if (np <= 2) dec_st.top_levels++; else dec_st.mid_levels++;
            for (size_t j = 0; j < np; j++) {
                view(&Xs, cur + j * nl, nl);
                newton_divmod(&Q, &R, &Xs, &d->T, &d->mu);
                uint64_t *hi = nxt + (2 * j) * nl_next, *lo = hi + nl_next;
                if (Q.n > nl_next || R.n > nl_next) { fprintf(stderr, "dc: piece overflow at level %d\n", lev); abort(); }
                par_memset0(hi + Q.n, nl_next - Q.n); par_memset0(lo + R.n, nl_next - R.n);
                par_copy64(hi, Q.l, Q.n); par_copy64(lo, R.l, R.n);
            }
        } else {
            tier = "DEEP"; dec_st.deep_levels++;
            double q0 = mem_now(), q1, q2, q3, q4;
            if (g_rw_cap < np * 4) { free(g_rw); g_rw_cap = np * 4; g_rw = (uint64_t *)malloc(g_rw_cap * 8); }
            /* X_j = (A_j mu) >> 64 (nT + k); products into a registered scratch */
            size_t nT = d->T.n, k = d->k, npr = nl + d->mu.n, sh = nT + k;
            if (g_scr_cap < g_scr_need) { if (g_scr) mem_hreg_free(g_scr); g_scr_cap = g_scr_need; g_scr = (uint64_t *)mem_hreg_alloc(g_scr_cap * 8); }
            uint64_t *scr = g_scr;
            if (g_scr_cap < np * npr) { fprintf(stderr, "dc: scratch undersized\n"); abort(); }
            rns_prod *pr = (rns_prod *)calloc(1, np * sizeof *pr);
            for (size_t j = 0; j < np; j++) { pr[j].a = cur + j * nl; pr[j].na = nl; pr[j].b = d->mu.l; pr[j].nb = d->mu.n; pr[j].c = scr + j * npr; }
            q1 = mem_now();
            rns_mul_batch(pr, np);
            q2 = mem_now();
            /* quotient candidates into the hi slots: (piece, chunk) pairs so few big pieces still use every thread */
            size_t n_q = npr > sh ? npr - sh : 0; if (n_q > nl_next) n_q = nl_next;
            size_t chunks = (nl_next + (1 << 18) - 1) >> 18;
#pragma omp parallel for schedule(dynamic, 4)
            for (size_t w = 0; w < np * chunks; w++) {
                size_t j = w / chunks, c = w % chunks, o = c << 18, m = nl_next - o < (1 << 18) ? nl_next - o : (1 << 18);
                uint64_t *hi = nxt + (2 * j) * nl_next + o;
                size_t nq = o < n_q ? (n_q - o < m ? n_q - o : m) : 0;
                if (nq) memcpy(hi, scr + j * npr + sh + o, nq * 8);
                if (nq < m) memset(hi + nq, 0, (m - nq) * 8);
                memset(nxt + (2 * j + 1) * nl_next + o, 0, m * 8);                      /* lo slot */
            }
            /* X_j T into the scratch, then R_j = A_j - X_j T over w = nl_next + 2 limbs (mod 2^(64 w)) with corrections */
            size_t npr2 = nl_next + nT;
            if (g_scr_cap < np * npr2) { fprintf(stderr, "dc: scratch undersized (2)\n"); abort(); }
            for (size_t j = 0; j < np; j++) { pr[j].a = nxt + (2 * j) * nl_next; pr[j].na = nl_next; pr[j].b = d->T.l; pr[j].nb = nT; pr[j].c = scr + j * npr2; }
            q3 = mem_now();
            rns_mul_batch(pr, np);
            q4 = mem_now();
            free(pr);
            size_t wl = nl_next + 2;
            if (wl > nl) wl = nl;
            if (wl > npr2) wl = npr2;
            /* R = A - X T over the window: lo slot (nl_next limbs) + 2 extra limbs (ext); parallel inside for few big pieces */
            size_t wx = wl > nl_next ? wl - nl_next : 0;               /* extra limbs beyond the lo slot (0..2) */
            int par_out = np >= 64;
#pragma omp parallel for schedule(dynamic, 64) if (par_out)
            for (size_t j = 0; j < np; j++) {
                uint64_t *lo = nxt + (2 * j + 1) * nl_next, *xt = scr + j * npr2, *a = cur + j * nl, *hi = nxt + (2 * j) * nl_next;
                uint64_t *ext = g_rw + j * 4;
                uint64_t br = limb_sub(lo, a, nl_next, xt, nl_next);          /* parallel when big and not nested */
                for (size_t k = nl_next; k < wl; k++) { uint64_t ai = a[k], xi = xt[k], v = ai - xi - br; br = (ai < xi) | (ai == xi && br); ext[k - nl_next] = v; }
                size_t nTlo = nT < nl_next ? nT : nl_next;               /* T's limbs inside the slot / beyond it */
                int guard = 0;
                for (;;) {
                    uint64_t top = wx ? ext[wx - 1] : lo[nl_next - 1];
                    if (top >> 63) {                                      /* negative: X--, r += T */
                        uint64_t one = 1; limb_sub(hi, hi, nl_next, &one, 1);
                        uint64_t c = limb_add(lo, lo, nl_next, d->T.l, nTlo);
                        for (size_t k = nl_next; k < wl; k++) { unsigned __int128 sm = (unsigned __int128)ext[k - nl_next] + (k < nT ? d->T.l[k] : 0) + c; ext[k - nl_next] = (uint64_t)sm; c = (uint64_t)(sm >> 64); }
                    } else {
                        int cmp = 0;
                        for (size_t k = wl; cmp == 0 && k-- > nl_next;) { uint64_t rv = ext[k - nl_next], tv = k < nT ? d->T.l[k] : 0; if (rv != tv) cmp = rv < tv ? -1 : 1; }
                        if (cmp == 0) { size_t n1 = limb_norm(lo, nl_next), n2 = limb_norm(d->T.l, nTlo); if (n1 != n2) cmp = n1 < n2 ? -1 : 1;
                                        for (size_t k = n1; cmp == 0 && k-- > 0;) if (lo[k] != d->T.l[k]) cmp = lo[k] < d->T.l[k] ? -1 : 1; }
                        if (cmp < 0) break;
                        uint64_t bb = limb_sub(lo, lo, nl_next, d->T.l, nTlo);
                        for (size_t k = nl_next; k < wl; k++) { uint64_t rv = ext[k - nl_next], tv = k < nT ? d->T.l[k] : 0, v = rv - tv - bb; bb = (rv < tv) | (rv == tv && bb); ext[k - nl_next] = v; }
                        { uint64_t c = 1; for (size_t i = 0; i < nl_next && c; i++) { hi[i] += c; c = hi[i] == 0; } }
                    }
                    if (++guard > 8) { fprintf(stderr, "dc: too many corrections\n"); abort(); }
                }
                for (size_t k = nl_next; k < wl; k++) if (ext[k - nl_next]) { fprintf(stderr, "dc: remainder overflow\n"); abort(); }
            }
            if (dec_verbose) printf("dc:   setup %.3f batch1 %.3f copy %.3f batch2 %.3f corr %.3f\n", q1 - q0, q2 - q1, q3 - q2, q4 - q3, mem_now() - q4);
        }
        double dt = mem_now() - t;
        if (!strcmp(tier, "TOP")) dec_st.t_top += dt; else if (!strcmp(tier, "MID")) dec_st.t_mid += dt; else dec_st.t_deep += dt;
        dec_st.levels++;
        if (dec_verbose) printf("dc: level %2d %-4s %10zu pieces of %10zu limbs -> %10zu  %.2f s  (batch %.2f: scatter %.2f ntt %.2f crt %.2f merge %.2f)\n", lev, tier, np, nl, nl_next, dt, rns_st.tb_total, rns_st.tb_scatter, rns_st.tb_ntt, rns_st.tb_crt, rns_st.tb_merge);
        memset(&rns_st, 0, sizeof rns_st);
        cur = nxt; np = np_next; nl = nl_next;
    }
    /* LEAF: np pieces of Lg = 18 u digits, nl limbs each, into out (padded: skip the leading ndig_p - ndig) */
    t = mem_now();
    {
        int nd; HIP_CHECK(hipGetDeviceCount(&nd)); if (nd > 4) nd = 4;
        if (!out) { out = (char *)mem_hreg_alloc(ndig + 2); todec_out = out; }
        char *obuf = out;
        int registered = mem_is_registered(out, ndig);
        unsigned long lead = ndig_p - ndig;
        if (!registered) obuf = (char *)mem_hreg_alloc(ndig + 1);       /* unregistered caller: one registered copy */
#pragma omp parallel num_threads(nd)
        {
            int dvc = omp_get_thread_num();
            HIP_CHECK(hipSetDevice(dvc));
            size_t p0 = np * dvc / nd, p1 = np * (dvc + 1) / nd;
            /* the kernel's out is indexed by padded position - lead; piece p0's positions start at p0 Lg */
            size_t lead1 = p0 * Lg >= lead ? 0 : lead - p0 * Lg;          /* padding still to drop from this device's first piece */
            if (p1 > p0) k_leaf<<<(unsigned)((p1 - p0 + 255) / 256), 256>>>(cur + p0 * nl, nl, p1 - p0, (int)nl, (int)u,
                                                                              obuf + p0 * Lg - lead + lead1, Lg, lead1);
            HIP_CHECK(hipDeviceSynchronize());
        }
        if (obuf != out) {
#pragma omp parallel for schedule(static)
            for (unsigned long i = 0; i < ndig; i += 1 << 24) { size_t mm = ndig - i < (1 << 24) ? ndig - i : (1 << 24); memcpy(out + i, obuf + i, mm); }
            mem_hreg_free(obuf);
        }
    }
    dec_st.t_leaf = mem_now() - t;
    dec_st.pieces = np;
    for (int i = 0; i < m; i++) mem_hreg_free(dv[i].reg);
    free(dv);
    bi_free(&Q); bi_free(&R); bi_free(&prod);
    dec_st.t_total = mem_now() - t0;
    if (dec_verbose) printf("dc: leaf %zu pieces x %lu digits %.2f s; total %.2f s (prewarm %.2f top %.2f mid %.2f deep %.2f)\n",
                            np, Lg, dec_st.t_leaf, dec_st.t_total, dec_st.t_prewarm, dec_st.t_top, dec_st.t_mid, dec_st.t_deep);
}

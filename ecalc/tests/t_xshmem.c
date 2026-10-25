/* t_xshmem.c -- unit test and bandwidth/latency tables of the target-shaped communication contract (xshmem.h; plan TARGET1, brief B4).
 *
 * Written ONLY against xshmem.h: the same source runs on the libfabric backend (make XSHMEM=ofi) and on the target (XSHMEM=cray).  Compiled as
 * HIP C++ (the Makefile's hipcc -x hip); hip is used only to move pattern bytes in and out of the heap when caps.dev_heap = 1.
 *
 * usage: t_xshmem [section ...]      sections: basic exch deadlock layer limit tail   (default = "correct": all six)
 *                                              lat stream a2a                      (bandwidth / latency tables; "bw" = all three)
 *                                              all = correct + bw
 * env:   XT_PUTSIG=split|nbi   payload+signal form (split = put, quiet, signal_set; nbi = putmem_signal_nbi)   default split
 *        XT_ORDER=seq|rot   post order over members (seq 0..n-1 default; rot (me+q)%n)
 *        XT_MAXMB (64)  largest slab; XT_ROUNDS (30) random exchange rounds; XT_SEED (1); XT_TIMEOUT (120 s per wait: a hang is a FAIL, exit 3);
 *        XT_LBYTES (262144) largest layered slab; XT_REPS (5) a2a repetitions; XT_QUICK=1 fewer iterations.
 * The exchange (xex_*) is the engine pattern of DESIGN 6.1-6.3, written the same way: post all (ROFF words), progress, wait spinning on
 * signal_test + progress (never a blocking library wait); the layered exchange is DESIGN 6.4 (one in flight, single-slot rule).
 * Output lines: "INFO", "MAP", "SEC <name> ...", "<<< FAIL ...", "HDR/ROW <table> | ...", "CTR/CTRSUM/CNT <group> ...", final "XT_RESULT PASS|FAIL".
 * Exit status nonzero on any failure (rank 0 also sums the failures of all PEs). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <regex.h>
#include "../xshmem.h"
#ifdef USE_HIP
#include <hip/hip_runtime.h>
#endif

#define MAXM 256          /* members of one communicator */
#define MAXL 4            /* lanes */
#define NCID 8            /* communicator ids (control blocks) */
#define SEQ_LIMIT (1ull << 22)
#define PACK(seq, v) (((uint64_t)(seq) << 40) | (uint64_t)(v))
#define VMASK ((1ull << 40) - 1)
#define CH (1u << 20)     /* pattern chunk */

/* ---------------- globals ---------------- */
static int me, np, ppn, nl, locr, g_dev, g_nodes, nfail;
static uint64_t g_seed = 1;
static double g_timeout = 120;
static int g_rot;                      /* XT_ORDER=rot: post order (me + q) % n instead of 0..n-1 (avoids incast on member 0) */
#define ORD(q, c) (g_rot ? ((c)->me + (q)) % (c)->n : (q))
static int g_nbi;                      /* putmem_signal_nbi arm */
static xshmem_ctx_t lane[MAXL];
static size_t maxslab;                 /* bytes of one slab slot (DATA) */
static char *dsb, *drb, *dL;           /* DATA regions: send slabs, receive slabs, layered buffers */
static uint64_t *ctl;                  /* CTRL base */
static char *dbase_; static size_t dbytes_;   /* DATA heap base and size (2 MiB multiple) */
#define BLOCKW (MAXM * (1 + MAXL))
#define ROFF(cid) (ctl + (size_t)(cid) * BLOCKW)
#define SIGW(cid, l) (ctl + (size_t)(cid) * BLOCKW + MAXM * (1 + (l)))
#define MISC (ctl + (size_t)NCID * BLOCKW)           /* misc words: BSIG, BACK, GV, GF (MAXM each) */
#define BSIG (MISC)
#define BACK (MISC + MAXM)
#define GV   (MISC + 2 * MAXM)
#define GF   (MISC + 3 * MAXM)
static unsigned char *hb1, *hb2;       /* host chunk buffers */

static double tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int g_neg;                      /* inside a deliberate negative test: failures print as XFAIL-EXPECTED */
static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void fail(const char *fmt, ...)
{
    char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    nfail++; printf(g_neg ? "XFAIL-EXPECTED pe %d: %s\n" : "<<< FAIL pe %d: %s\n", me, b); fflush(stdout);
}
static void hang(const char *what, const char *detail)
{
    printf("<<< FAIL pe %d: HANG (> %.0f s) in %s: %s\n", me, g_timeout, what, detail); fflush(stdout);
    xshmem_global_exit(3);
    exit(3);
}
static inline uint64_t rd64(const uint64_t *p) { return *(volatile const uint64_t *)p; }
static inline int sigge(const uint64_t *p, uint64_t v) { return xshmem_signal_test(p, XSHMEM_CMP_GE, v); }
static inline void spin_wait(const uint64_t *p, uint64_t v, const char *what)
{
    double t0 = tnow(); unsigned n = 0;
    while (!sigge(p, v)) if (!(++n & 1023) && tnow() - t0 > g_timeout) { char d[120]; snprintf(d, sizeof d, "word %llx < %llx", (unsigned long long)rd64(p), (unsigned long long)v); hang(what, d); }
}

/* ---------------- heap byte access (device heap: hipMemcpy; host heap: memcpy) ---------------- */
static void hput(void *dst, const void *src, size_t n) { if (!n) return;
#ifdef USE_HIP
    if (g_dev) { if (hipMemcpy(dst, src, n, hipMemcpyDefault) != hipSuccess) { fail("hipMemcpy put"); } return; }
#endif
    memcpy(dst, src, n); }
static void hget(void *dst, const void *src, size_t n) { hput(dst, src, n); }

/* ---------------- patterns ---------------- */
static inline uint64_t mix64(uint64_t x)
{ x += 0x9E3779B97F4A7C15ull; x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull; x = (x ^ (x >> 27)) * 0x94D049BB133111EBull; return x ^ (x >> 31); }
static uint64_t pbase(uint64_t tag, int src, int dst) { return mix64(g_seed ^ mix64(tag * 1000003ull + (uint64_t)src * 4099ull + (uint64_t)dst)); }
static void pgen(uint64_t base, size_t off, size_t len, unsigned char *buf)   /* off multiple of 8 */
{
    size_t w0 = off / 8, nw = (len + 7) / 8;
    for (size_t i = 0; i < nw; i++) { uint64_t v = mix64(base + w0 + i); size_t k = len - 8 * i; memcpy(buf + 8 * i, &v, k < 8 ? k : 8); }
}
static void pfill(void *dst, size_t n, uint64_t base)
{
    for (size_t o = 0; o < n; o += CH) { size_t l = n - o < CH ? n - o : CH; pgen(base, o, l, hb1); hput((char *)dst + o, hb1, l); }
}
/* returns the number of bad bytes in the first bad chunk (0 = ok); *first = offset of the first bad byte */
static size_t pcheck(const void *src, size_t n, uint64_t base, size_t *first, unsigned *got, unsigned *exp)
{
    for (size_t o = 0; o < n; o += CH) {
        size_t l = n - o < CH ? n - o : CH; hget(hb2, (const char *)src + o, l); pgen(base, o, l, hb1);
        if (memcmp(hb1, hb2, l)) { size_t nb = 0, f = (size_t)-1; for (size_t i = 0; i < l; i++) if (hb1[i] != hb2[i]) { nb++; if (f == (size_t)-1) f = i; }
            *first = o + f; *got = hb2[f]; *exp = hb1[f]; return nb; }
    }
    return 0;
}
static size_t rsize(uint64_t tag, int src, int dst, size_t cap)    /* deterministic random size: 0 .. cap, log-uniform with the special values */
{
    uint64_t h = mix64(pbase(tag ^ 0xABCDEFull, src, dst)); unsigned s = (unsigned)(h % 100); size_t r;
    if (s < 6) r = 0; else if (s < 10) r = 1; else if (s < 14) r = 8; else if (s < 18) r = 192; else if (s < 22) r = 193;
    else if (s < 30) r = 2048 * (1 + (size_t)((h >> 20) % 64)); else if (s < 31) r = cap;
    else { double e = (double)((h >> 24) & 0xFFFFFF) / 16777216.0 * 26.0; r = (size_t)exp2(e) + (size_t)((h >> 3) & 0xFF); }
    return r > cap ? cap : r;
}

/* ---------------- communicators and the exchange (DESIGN 6.1-6.3) ---------------- */
typedef struct { int cid, n, me; int pe[MAXM]; uint64_t seq; } xcomm;
static xcomm *comm_new(int cid, int n, const int *pes)
{
    xcomm *c = (xcomm *)calloc(1, sizeof *c); c->cid = cid; c->n = n; c->me = -1;
    for (int i = 0; i < n; i++) { c->pe[i] = pes[i]; if (pes[i] == me) c->me = i; }
    if (c->me < 0) { fprintf(stderr, "t_xshmem: PE %d not in communicator %d\n", me, cid); exit(2); }
    return c;
}
static int seq_check(uint64_t seq) { return seq > SEQ_LIMIT; }       /* nonzero = over the exchange-count limit (fatal in the engine) */
static void lane_range(size_t n, int l, size_t *lo, size_t *len) { size_t a = n * l / nl, b = n * (l + 1) / nl; *lo = a; *len = b - a; }
static int lane_used(size_t n, int l) { size_t lo, ln; lane_range(n, l, &lo, &ln); return ln > 0 || (n == 0 && l == 0); }

typedef struct {
    xcomm *c; uint64_t seq, tag; int chk;                       /* chk: verify slab data at the moment its signals are all seen */
    char *sb, *rb; size_t sstr, rstr; size_t scnt[MAXM]; size_t rcnt[MAXM];
    int chk_skip[MAXM], posted[MAXM], lanes_left[MAXM], rxdone[MAXM][MAXL], nposted, flushed, touched[MAXL], active, lanes_open;
    int mismatch, bad; double t_post, t_wait; double t0;
} xex;

static void xex_post(xex *x, xcomm *c, char *sb, size_t sstr, const size_t *scnt, char *rb, size_t rstr, const size_t *rcnt, uint64_t tag, int chk)
{
    if (x->active) { fail("xex_post: exchange already pending (comm %d)", c->cid); return; }
    double t0 = tnow(); memset(x, 0, sizeof *x);
    x->c = c; x->t0 = t0; x->tag = tag; x->chk = chk; x->sb = sb; x->rb = rb; x->sstr = sstr; x->rstr = rstr; memcpy(x->scnt, scnt, (size_t)c->n * sizeof *scnt);   /* copied: callers reuse their arrays while an exchange is pending */
    c->seq++; x->seq = c->seq;
    if (seq_check(x->seq)) { fail("exchange-count limit 2^22 exceeded on comm %d", c->cid); }
    for (int i = 0; i < c->n; i++) { x->rcnt[i] = rcnt[i]; x->lanes_left[i] = 0;
        if (i != c->me) for (int l = 0; l < nl; l++) if (lane_used(rcnt[i], l)) x->lanes_left[i]++; }
    xshmem_ctx_quiet(XSHMEM_CTX_DEFAULT);                      /* older word writes (ROFF, acks) */
    for (int q = 0; q < c->n; q++) { int i = ORD(q, c); if (i != c->me)
        xshmem_ctx_signal_set(XSHMEM_CTX_DEFAULT, &ROFF(c->cid)[c->me], PACK(x->seq, xshmem_off(rb + i * rstr)), c->pe[i]); }
    if (scnt[c->me]) hput(rb + c->me * rstr, sb + c->me * sstr, scnt[c->me]);   /* self slab (host copy through a chunk buffer is not needed: same space) */
    x->posted[c->me] = 1; x->nposted = 1; x->active = 1;
    if (c->n == 1) x->flushed = 1;
    x->lanes_open = 0;
    for (int i = 0; i < c->n; i++) if (i != c->me) x->lanes_open += x->lanes_left[i];
    /* the self slab counts as received */
    {   size_t bk, first = 0; unsigned g = 0, e = 0; size_t ns = scnt[c->me];
        if (x->rcnt[c->me] != ns) { x->mismatch++; fail("self count %zu != expected %zu", ns, x->rcnt[c->me]); }
        else if (chk && ns && (bk = pcheck(rb + c->me * rstr, ns, pbase(tag, c->pe[c->me], c->pe[c->me]), &first, &g, &e))) { x->bad++; fail("self slab data bad (%zu bytes)", bk); }
    }
    x->t_post = tnow() - t0;
}
static void xex_progress(xex *x)
{
    xcomm *c = x->c; int n = c->n;
    for (int q = 0; q < n; q++) { int i = ORD(q, c); if (x->posted[i]) continue;
        const uint64_t *w = &ROFF(c->cid)[i];
        if (!sigge(w, PACK(x->seq, 0))) continue;
        uint64_t v = rd64(w);
        if ((v >> 40) != x->seq) { fail("ROFF from member %d seq %llu, expected %llu", i, (unsigned long long)(v >> 40), (unsigned long long)x->seq); x->bad++; x->posted[i] = 1; x->nposted++; continue; }
        char *src = x->sb + i * x->sstr, *dest = (char *)xshmem_at((ptrdiff_t)(v & VMASK)); size_t cn = x->scnt[i];
        for (int l = 0; l < nl; l++) {
            size_t lo, ln; lane_range(cn, l, &lo, &ln);
            if (cn == 0) { if (l == 0) xshmem_ctx_signal_set(lane[0], &SIGW(c->cid, 0)[c->me], PACK(x->seq, 0), c->pe[i]); continue; }
            if (!ln) continue;
            x->touched[l] = 1;
            if (g_nbi) xshmem_ctx_putmem_signal_nbi(lane[l], dest + lo, src + lo, ln, &SIGW(c->cid, l)[c->me], PACK(x->seq, ln), XSHMEM_SIGNAL_SET, c->pe[i]);
            else       xshmem_ctx_putmem_nbi(lane[l], dest + lo, src + lo, ln, c->pe[i]);
        }
        x->posted[i] = 1; x->nposted++;
    }
    if (x->nposted == n && !x->flushed) {
        if (!g_nbi) {
            for (int l = 0; l < nl; l++) if (x->touched[l]) xshmem_ctx_quiet(lane[l]);                  /* one quiet per used lane */
            for (int q = 0; q < n; q++) { int i = ORD(q, c); if (i != c->me) for (int l = 0; l < nl; l++) {
                size_t lo, ln; lane_range(x->scnt[i], l, &lo, &ln);
                if (x->scnt[i] && ln) xshmem_ctx_signal_set(lane[l], &SIGW(c->cid, l)[c->me], PACK(x->seq, ln), c->pe[i]);
            } }
        }
        x->flushed = 1;
    }
    xshmem_progress();
}
static int xex_rx(xex *x)    /* test receive words; returns 1 when all slabs are in */
{
    xcomm *c = x->c; int left = 0;
    for (int i = 0; i < c->n; i++) if (i != c->me && x->lanes_left[i] > 0) {
        for (int l = 0; l < nl; l++) {
            if (x->rxdone[i][l] || !lane_used(x->rcnt[i], l)) continue;
            const uint64_t *w = &SIGW(c->cid, l)[i];
            if (!sigge(w, PACK(x->seq, 0))) continue;
            uint64_t v = rd64(w); size_t lo, ln; lane_range(x->rcnt[i], l, &lo, &ln);
            x->rxdone[i][l] = 1; x->lanes_left[i]--; x->lanes_open--;
            if ((v >> 40) != x->seq || (v & VMASK) != ln) {
                x->mismatch++; x->bad++; x->chk_skip[i] = 1;
                fail("comm %d member %d(pe %d) lane %d: signal seq %llu count %llu, expected seq %llu count %zu (count mismatch)", c->cid, i, c->pe[i], l, (unsigned long long)(v >> 40), (unsigned long long)(v & VMASK), (unsigned long long)x->seq, ln);
            }
        }
        if (x->lanes_left[i] == 0 && x->chk && x->rcnt[i] && !x->chk_skip[i]) {       /* all lanes of this sender seen: data must be complete now */
            size_t first = 0; unsigned g = 0, e = 0; size_t nb = pcheck(x->rb + i * x->rstr, x->rcnt[i], pbase(x->tag, c->pe[i], me), &first, &g, &e);
            if (nb) { x->bad++; fail("DATA comm %d seq %llu from pe %d (%zu B): %zu bad bytes in the first bad chunk, first at byte %zu: got 0x%02x expected 0x%02x", c->cid, (unsigned long long)x->seq, c->pe[i], x->rcnt[i], nb, first, g, e); }
        }
        if (x->lanes_left[i] > 0) left++;
    }
    return left == 0;
}
static void xex_wait(xex *x)
{
    double t0 = tnow(), tl = t0; unsigned n = 0; int rx = 0;
    while (1) {
        xex_progress(x);
        if (!rx) rx = xex_rx(x);
        if (rx && x->flushed) break;
        if (!(++n & 1023) && tnow() - tl > g_timeout) {
            char d[200]; int mp = 0; for (int i = 0; i < x->c->n; i++) if (!x->posted[i]) mp++;
            snprintf(d, sizeof d, "comm %d seq %llu: %d peers not posted (no ROFF), %d lane words missing, flushed %d", x->c->cid, (unsigned long long)x->seq, mp, x->lanes_open, x->flushed);
            hang("xex_wait", d);
        }
    }
    for (int l = 1; l < nl; l++) if (x->touched[l]) xshmem_ctx_quiet(lane[l]);   /* sb reusable */
    xshmem_ctx_quiet(lane[0]);                                                   /* lane 0 also carries the zero-count signals */
    x->active = 0; x->t_wait = tnow() - t0;
}

/* ---------------- gather of PE values to PE 0 (control words), sum of fails ---------------- */
static uint64_t gseq;
static void gather_u64(uint64_t v, uint64_t *all)       /* all[np] valid on PE 0 only */
{
    gseq++;
    if (me == 0) { all[0] = v; }
    else { xshmem_ctx_signal_set(XSHMEM_CTX_DEFAULT, &GV[me], v, 0); xshmem_ctx_quiet(XSHMEM_CTX_DEFAULT); xshmem_ctx_signal_set(XSHMEM_CTX_DEFAULT, &GF[me], gseq, 0); }
    if (me == 0) for (int i = 1; i < np; i++) { spin_wait(&GF[i], gseq, "gather"); all[i] = rd64(&GV[i]); }
    xshmem_ctx_quiet(XSHMEM_CTX_DEFAULT);
    xshmem_barrier_all();
}
static void gather_d(double v, double *out)
{ uint64_t b, ab[MAXM]; memcpy(&b, &v, 8); gather_u64(b, ab); if (me == 0) for (int i = 0; i < np; i++) memcpy(&out[i], &ab[i], 8); }
static void stat_d(double v, double *mn, double *mean, double *mx)    /* PE 0 */
{ double a[MAXM]; gather_d(v, a); if (me == 0) { *mn = *mx = a[0]; double s = 0; for (int i = 0; i < np; i++) { if (a[i] < *mn) *mn = a[i]; if (a[i] > *mx) *mx = a[i]; s += a[i]; } *mean = s / np; } }
static int section_end(const char *name, int f0)
{
    uint64_t a[MAXM]; gather_u64((uint64_t)(nfail - f0), a); int tot = 0;
    if (me == 0) { for (int i = 0; i < np; i++) tot += (int)a[i]; printf("SEC %s: %s (%d failures over %d PEs)\n", name, tot ? "FAIL" : "PASS", tot, np); fflush(stdout); }
    return tot;
}

/* ---------------- cxi telemetry and backend counters around a group ---------------- */
#define MAXCT 6000
typedef struct { char name[72]; int nic; unsigned long long v; } ctr_t;
static ctr_t *ct0, *ct1; static int nct0; static regex_t ctre; static int ctre_ok; static char grp[64];
#ifdef XSHMEM_OFI
static xshmem_ofi_cnt_t oc0;
#endif
static int ct_snap(ctr_t *out)
{
    int n = 0; char dir[160];
    for (int i = 0; i < 16; i++) {
        snprintf(dir, sizeof dir, "/sys/class/cxi/cxi%d/device/telemetry", i); DIR *d = opendir(dir); if (!d) continue;
        struct dirent *e; while ((e = readdir(d)) && n < MAXCT) {
            if (e->d_name[0] == '.' || regexec(&ctre, e->d_name, 0, 0, 0)) continue;
            char p[320]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); FILE *f = fopen(p, "r"); if (!f) continue;
            char b[64] = { 0 }; if (fgets(b, sizeof b, f)) { out[n].v = strtoull(b, 0, 10); snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name); out[n].nic = i; n++; }
            fclose(f); }
        closedir(d);
    }
    return n;
}
static void ctr_begin(const char *g)
{
    xshmem_barrier_all(); snprintf(grp, sizeof grp, "%s", g);
    if (!ctre_ok) { ctre_ok = !regcomp(&ctre, "(^|_)atu(_|$)|page_size|odp|^pi_.*rtrgt|restricted_pkt|dmawr_flush_reqs|atu_flush|pct_req_(un)?ordered|oxe_ioi_pkts_(un)?ordered|miss|ats_", REG_EXTENDED | REG_NOSUB);
        ct0 = (ctr_t *)calloc(MAXCT, sizeof *ct0); ct1 = (ctr_t *)calloc(MAXCT, sizeof *ct1); }
    if (locr == 0 && ctre_ok) nct0 = ct_snap(ct0);
#ifdef XSHMEM_OFI
    xshmem_ofi_counters(&oc0);
#endif
    xshmem_barrier_all();
}
static void ctr_end(void)
{
    xshmem_barrier_all(); static char line[8192]; line[0] = 0; long s[4] = { 0, 0, 0, 0 };
    if (locr == 0 && ctre_ok) {
        int n1 = ct_snap(ct1), k = 0;
        for (int i = 0; i < n1 && i < nct0; i++) {
            if (strcmp(ct0[i].name, ct1[i].name) || ct0[i].nic != ct1[i].nic) continue;
            long long d = (long long)(ct1[i].v - ct0[i].v); if (!d) continue;
            if (strstr(ct1[i].name, "miss")) s[0] += d;
            if (strstr(ct1[i].name, "unrestricted")) s[2] += d; else if (strstr(ct1[i].name, "restricted")) s[1] += d;
            if (strstr(ct1[i].name, "flush")) s[3] += d;
            int l = (int)strlen(line); if (l < (int)sizeof line - 120) k += snprintf(line + l, sizeof line - l, "cxi%d.%s=%lld ", ct1[i].nic, ct1[i].name, d);
        }
        if (!k) snprintf(line, sizeof line, "(no counter changed or no telemetry)");
        printf("CTR %s node %d | %s\nCTRSUM %s node %d | miss=%ld restricted_pkts=%ld unrestricted_pkts=%ld flush_reqs=%ld (this node, all NICs)\n", grp, me / (ppn ? ppn : 1), line, grp, me / (ppn ? ppn : 1), s[0], s[1], s[2], s[3]);
    }
#ifdef XSHMEM_OFI
    if (me == 0) { xshmem_ofi_cnt_t c; xshmem_ofi_counters(&c);
        printf("CNT %s pe 0 | writes=%llu flush_chains=%llu quiets=%llu wait_spins=%llu\n", grp, (unsigned long long)(c.writes - oc0.writes), (unsigned long long)(c.flush_chains - oc0.flush_chains), (unsigned long long)(c.quiets - oc0.quiets), (unsigned long long)(c.wait_spins - oc0.wait_spins)); }
#endif
    fflush(stdout); xshmem_barrier_all();
}

/* ---------------- section basic: put / quiet / signal / wait at sizes 8 B .. 64 MiB, ring, buffer reuse ---------------- */
static uint64_t bseq;
static void basic_one(size_t n, int arm, int iters, uint64_t tag0)
{
    int next = (me + 1) % np, prev = (me + np - 1) % np;
    for (int it = 0; it < iters; it++) {
        uint64_t tag = tag0 + it; bseq++;
        char *src = dsb + (size_t)next * maxslab, *dest = drb + (size_t)me * maxslab;
        pfill(src, n, pbase(tag, me, next));
        if (bseq > 1) spin_wait(&BACK[next], bseq - 1, "basic ack");               /* next has read my previous data */
        if (arm == 1) xshmem_ctx_putmem_signal_nbi(lane[0], dest, src, n, &BSIG[me], PACK(bseq, n), XSHMEM_SIGNAL_SET, next);
        else { xshmem_ctx_putmem_nbi(lane[0], dest, src, n, next); xshmem_ctx_quiet(lane[0]); xshmem_ctx_signal_set(lane[0], &BSIG[me], PACK(bseq, n), next); }
        if (arm == 2) { uint64_t v = xshmem_signal_wait_until(&BSIG[prev], XSHMEM_CMP_GE, PACK(bseq, 0)); (void)v; } else spin_wait(&BSIG[prev], PACK(bseq, 0), "basic signal");
        uint64_t v = rd64(&BSIG[prev]);
        if (v != PACK(bseq, n)) fail("basic n=%zu arm %d it %d: signal word %llx expected %llx", n, arm, it, (unsigned long long)v, (unsigned long long)PACK(bseq, n));
        size_t first = 0; unsigned g = 0, e = 0; size_t nb = pcheck(drb + (size_t)prev * maxslab, n, pbase(tag, prev, me), &first, &g, &e);
        if (nb) fail("basic DATA n=%zu arm %d it %d from pe %d: %zu bad bytes, first at %zu got 0x%02x expected 0x%02x", n, arm, it, prev, nb, first, g, e);
        xshmem_ctx_quiet(lane[0]);
        xshmem_ctx_signal_set(XSHMEM_CTX_DEFAULT, &BACK[me], bseq, prev);
    }
    xshmem_ctx_quiet(XSHMEM_CTX_DEFAULT);
}
static void sec_basic(void)
{
    static const size_t sz[] = { 8, 64, 512, 4096, 32768, 262144, 2u << 20, 16u << 20, 64u << 20 };
    int quick = getenv("XT_QUICK") != 0;
    for (int arm = 0; arm < 3; arm++) {                      /* 0 split, 1 putmem_signal_nbi, 2 split + signal_wait_until */
        for (size_t k = 0; k < sizeof sz / sizeof *sz; k++) {
            if (sz[k] > maxslab) break;
            int it = sz[k] <= 4096 ? (quick ? 20 : 200) : sz[k] <= (2u << 20) ? (quick ? 3 : 20) : 3;
            basic_one(sz[k], arm, it, 1000ull * (arm + 1) + 50 * k);
        }
        if (me == 0) printf("  basic arm %s done\n", arm == 0 ? "split" : arm == 1 ? "putmem_signal_nbi" : "split+signal_wait_until");
    }
    for (size_t n = 1; n <= 200; n += 37) basic_one(n, 0, 2, 7000 + n);                 /* odd sizes */
}

/* ---------------- random alltoallv rounds on a communicator ---------------- */
static void exch_rounds(xcomm *c, int rounds, uint64_t tag0, size_t cap, int skew, int equal)
{
    static xex x; size_t scnt[MAXM], rcnt[MAXM];
    for (int r = 0; r < rounds; r++) {
        uint64_t tag = tag0 + r;
        for (int i = 0; i < c->n; i++) {
            scnt[i] = equal ? cap : rsize(tag, me, c->pe[i], cap);
            rcnt[i] = equal ? cap : rsize(tag, c->pe[i], me, cap);
            pfill(dsb + (size_t)i * maxslab, scnt[i], pbase(tag, me, c->pe[i]));
        }
        if (skew && (mix64(g_seed ^ tag ^ (uint64_t)me * 77) & 3) == 0) usleep((useconds_t)(1000 * (mix64(tag + me) % 20)));
        xex_post(&x, c, dsb, maxslab, scnt, drb, maxslab, rcnt, tag, 1);
        xex_wait(&x);
        if ((&x)->bad) fail("exchange comm %d round %d: %d bad slabs", c->cid, r, (&x)->bad);
        for (int i = 0; i < c->n; i++) if (scnt[i] != rsize(tag, me, c->pe[i], cap) && !equal) fail("scnt changed");
    }
}
static void sec_exch(void)
{
    int rounds = getenv("XT_ROUNDS") ? atoi(getenv("XT_ROUNDS")) : 30; if (getenv("XT_QUICK")) rounds = 8;
    int pes[MAXM];
    for (int i = 0; i < np; i++) pes[i] = i;
    xcomm *w = comm_new(0, np, pes);
    exch_rounds(w, rounds, 100000, maxslab, 1, 0);                                    /* random alltoallv, all peers, random sizes 0..max */
    if (me == 0) printf("  world random alltoallv: %d rounds done\n", rounds);
    static const size_t eq[] = { 8, 192, 193, 2048, 4096, 65536, 1u << 20, 4u << 20 };
    for (size_t k = 0; k < sizeof eq / sizeof *eq; k++) if (eq[k] <= maxslab) exch_rounds(w, 3, 200000 + 100 * k, eq[k], 0, 1);   /* equal alltoall, reused buffers */
    if (me == 0) printf("  world equal alltoall: sizes 8 B .. 4 MiB done\n");
    exch_rounds(w, 40, 300000, 4096, 1, 0);                                           /* many iterations, small */
    if (np % 2 == 0 && np > 2) {                                                      /* pairs: PE and the PE n/2 away */
        int pp[2] = { me < np / 2 ? me : me - np / 2, me < np / 2 ? me + np / 2 : me };
        xcomm *p = comm_new(1, 2, pp); exch_rounds(p, rounds / 2 + 1, 400000, maxslab, 1, 0);
        if (me == 0) printf("  pairs (pe, pe+n/2) random: done\n");
    }
    if (ppn >= 1 && np % ppn == 0) {                                                  /* node-local and strided sets (the layered comms) */
        int g = np / ppn, a[MAXM], b[MAXM];
        for (int d = 0; d < ppn; d++) a[d] = (me / ppn) * ppn + d;
        for (int n = 0; n < g; n++) b[n] = n * ppn + me % ppn;
        xcomm *ci = comm_new(2, ppn, a), *cx = comm_new(3, g, b);
        exch_rounds(ci, rounds / 2 + 1, 500000, maxslab, 1, 0); exch_rounds(cx, rounds / 2 + 1, 600000, maxslab, 1, 0);
        if (me == 0) printf("  node-local (%d PEs) and strided (%d PEs) comms random: done\n", ppn, g);
    }
}

/* ---------------- poll-only deadlock regression: every PE in the same wait loop, skewed posts ---------------- */
static void sec_deadlock(void)
{
    int pes[MAXM]; for (int i = 0; i < np; i++) pes[i] = i;
    static xex x; xcomm *c = comm_new(6, np, pes); size_t cn[MAXM]; size_t sz = (4u << 20) < maxslab ? (4u << 20) : maxslab;
    int rounds = getenv("XT_QUICK") ? 3 : 8;
    for (int r = 0; r < rounds; r++) {
        uint64_t tag = 800000 + r;
        for (int i = 0; i < np; i++) { cn[i] = sz; pfill(dsb + (size_t)i * maxslab, sz, pbase(tag, me, i)); }
        int late = r % 2 ? (np - 1) : (me % 4);            /* odd rounds: one very late PE; even: skew by 15 ms * (pe % 4) */
        if (r % 2 ? me == late : 1) usleep(r % 2 ? 200000 : 15000 * late);
        xex_post(&x, c, dsb, maxslab, cn, drb, maxslab, cn, tag, 1);
        xex_wait(&x);                                      /* every PE: no other call between post and wait, all waiting in the same loop */
        if ((&x)->bad) fail("deadlock regression round %d: %d bad", r, (&x)->bad);
    }
    if (me == 0) printf("  all PEs in one wait loop, skewed posts, %zu B x %d rounds: no hang\n", sz, rounds);
}

/* ---------------- layered exchange, one in flight, single-slot rule (DESIGN 6.4) ---------------- */
static size_t lregion; static int lna, lg; static size_t lbytes, lslot; static xcomm *lci, *lcx; static int lverify_n; static uint64_t ltag;
static char *ltmp, *lrb[2], *lsb;
typedef struct { char *rb, *tmp; int inter_posted; uint64_t tag; } yex;
static yex ye[2]; static int yhead, ynpend, ytwo; static xex xi, xx; static size_t lcnt[MAXM];
static void transpose(char *dst, const char *src, size_t b, int A, int B)       /* src [a][b] -> dst [b][a] slabs of b bytes */
{
    for (int a = 0; a < A; a++) for (int j = 0; j < B; j++) { hget(hb1, src + ((size_t)a * B + j) * b, b); hput(dst + ((size_t)j * A + a) * b, hb1, b); }
}
static int lidx(int pe) { return (pe % lna) * lg + pe / lna; }
static void y_verify(yex *e)
{
    for (int s = 0; s < np; s++) {
        size_t first = 0; unsigned g = 0, ex = 0; size_t nb = pcheck(e->rb + (size_t)lidx(s) * lbytes, lbytes, pbase(e->tag, s, me), &first, &g, &ex);
        if (nb) fail("LAYERED DATA tag %llu from pe %d: %zu bad bytes, first %zu got 0x%02x expected 0x%02x", (unsigned long long)e->tag, s, nb, first, g, ex);
    }
    lverify_n++;
}
static void y_inter_post(yex *e)
{
    for (int i = 0; i < lg; i++) lcnt[i] = lbytes * lna;
    xex_post(&xx, lcx, e->rb, lbytes * lna, lcnt, e->tmp, lbytes * lna, lcnt, 0, 0); e->inter_posted = 1;
}
static void y_complete_oldest(void)
{
    yex *e = &ye[yhead];
    if (!e->inter_posted) y_inter_post(e);
    xex_wait(&xx);
    transpose(e->rb, e->tmp, lbytes, lg, lna);
    y_verify(e); yhead ^= 1; ynpend--;
}
static void y_alltoall(char *rb, uint64_t tag)
{
    if (ynpend == 2) y_complete_oldest();
    if (ynpend && !ytwo) y_complete_oldest();                               /* single-slot rule */
    yex *e = &ye[(yhead + ynpend) & 1]; e->rb = rb; e->tag = tag; e->inter_posted = 0;
    e->tmp = ltmp + (ytwo ? (size_t)((yhead + ynpend) & 1) * lslot : 0); ynpend++;
    for (int i = 0; i < lna; i++) lcnt[i] = lbytes * lg;
    xex_post(&xi, lci, lsb, lbytes * lg, lcnt, e->tmp, lbytes * lg, lcnt, 0, 0); xex_wait(&xi);        /* intra: blocks of g slabs from the caller's sb into the tmp slot */
    transpose(rb, e->tmp, lbytes, lna, lg);
    if (ynpend == 2) y_complete_oldest();                                   /* the inter transport takes one exchange at a time */
    y_inter_post(e);
}
static void sec_layer(void)
{
    if (np % ppn) { printf("  layer: np %d not a multiple of ppn %d: skipped\n", np, ppn); return; }
    lna = ppn; lg = np / ppn;
    size_t cap = getenv("XT_LBYTES") ? (size_t)atol(getenv("XT_LBYTES")) : 262144; if (cap > CH) cap = CH;
    static const size_t bs[] = { 193, 4096, 65536, 1u << 20 };
    int a[MAXM], b[MAXM];
    for (int d = 0; d < lna; d++) a[d] = (me / lna) * lna + d;
    for (int n = 0; n < lg; n++) b[n] = n * lna + me % lna;
    lci = comm_new(4, lna, a); lcx = comm_new(5, lg, b);
    for (int variant = 0; variant < 2; variant++) for (size_t k = 0; k < sizeof bs / sizeof *bs; k++) {
        lbytes = bs[k]; if (lbytes > cap) break;
        lslot = lbytes * np; size_t need = (3 + 2) * lslot; if (need > lregion) { printf("  layer: bytes %zu too big for the heap: skipped\n", lbytes); continue; }
        lsb = dL; lrb[0] = dL + lslot; lrb[1] = dL + 2 * lslot; ltmp = dL + 3 * lslot; ytwo = variant == 0; yhead = ynpend = 0; lverify_n = 0;
        int K = getenv("XT_QUICK") ? 4 : 7;
        for (int q = 0; q < K; q++) {
            uint64_t tag = 900000 + 100 * variant + 10 * k + q;
            for (int dst = 0; dst < np; dst++) pfill(lsb + (size_t)lidx(dst) * lbytes, lbytes, pbase(tag, me, dst));     /* sb is refilled at once: reusable after post returns */
            y_alltoall(lrb[q & 1], tag);
        }
        while (ynpend) y_complete_oldest();
        if (lverify_n != K) fail("layered: %d of %d exchanges verified", lverify_n, K);
        if (me == 0) printf("  layered %zu B/slab, %s: %d exchanges verified\n", lbytes, ytwo ? "two tmp slots (two in flight)" : "single slot (one in flight)", K);
    }
}

/* ---------------- count mismatch (negative), exchange-count limit, high seq ---------------- */
static void sec_limit(void)
{
    int pes[MAXM]; for (int i = 0; i < np; i++) pes[i] = i;
    if (seq_check(SEQ_LIMIT) || !seq_check(SEQ_LIMIT + 1)) fail("seq_check limit wrong");
    xcomm *h = comm_new(7, np, pes); h->seq = SEQ_LIMIT - 5;                               /* seq near 2^22: PACK arithmetic up to the limit, no fatal */
    { int f0 = nfail; exch_rounds(h, 5, 1100000, 65536, 0, 0); if (nfail == f0 && me == 0) printf("  seq up to %llu (the 2^22 limit): ok\n", (unsigned long long)h->seq); }
    if (seq_check(h->seq)) fail("seq %llu over limit at the limit", (unsigned long long)h->seq);
    h->seq = SEQ_LIMIT; { static xex x; size_t cn[MAXM]; for (int i = 0; i < np; i++) cn[i] = 8; int f0 = nfail;
        g_neg = 1; xex_post(&x, h, dsb, maxslab, cn, drb, maxslab, cn, 1, 0); g_neg = 0; xex_wait(&x);
        if (nfail != f0 + 1) fail("exchange number 2^22+1 was not flagged (got %d new failures)", nfail - f0);
        else { nfail = f0; if (me == 0) printf("  exchange 2^22+1 flagged as expected (negative test)\n"); } }
    if (np < 2) return;
    /* negative: PE 0 expects one byte more than peer 1 sends: must report exactly one mismatch, everybody completes */
    static xex x; xcomm *c = comm_new(6, np, pes); size_t sc[MAXM], rc[MAXM]; c->seq = 100;      /* a fresh block (cid 6 words were used by the deadlock test: seq 100 is above) */
    for (int i = 0; i < np; i++) { sc[i] = 64; rc[i] = 64; pfill(dsb + (size_t)i * maxslab, 64, pbase(77, me, i)); }
    if (me == 0) rc[1] = 65;
    int f0 = nfail; g_neg = 1; xex_post(&x, c, dsb, maxslab, sc, drb, maxslab, rc, 77, 1); xex_wait(&x); g_neg = 0;
    int want = me == 0 ? 1 : 0;
    if ((&x)->mismatch != want) fail("count mismatch check: pe %d saw %d mismatches, expected %d", me, (&x)->mismatch, want);
    else if (me == 0 && nfail == f0 + 1) { nfail = f0; printf("  count mismatch detected as expected (negative test)\n"); }
    else if (me != 0 && nfail == f0) {}
    else fail("count mismatch check: unexpected failure count %d", nfail - f0);
}

/* ---------------- section tail: the first and the last bytes of the DATA heap touched by a GPU kernel, and put to a peer at the tail ----------------
 * Catches registration-range bugs (a heap whose last page is not mapped or not registered: the 9s2 tail-page fault).  The DATA heap size is rounded
 * up to 2 MiB here so that the last byte of the request is the last byte of the heap.  A device heap is touched by a kernel (read + write), a host heap by the CPU. */
#ifdef USE_HIP
__global__ void tail_kernel(uint64_t *first, uint64_t *last, uint64_t vf, uint64_t vl, uint64_t *out)
{
    out[0] = *(volatile uint64_t *)first; out[1] = *(volatile uint64_t *)last;      /* reads: what the previous step left */
    *(volatile uint64_t *)first = vf; *(volatile uint64_t *)last = vl;              /* writes */
    ((volatile unsigned char *)last)[7] ^= 0;                                       /* the very last byte read-modify-write */
    out[2] = *(volatile uint64_t *)first; out[3] = *(volatile uint64_t *)last;
}
__global__ void tail_read(const uint64_t *a, const uint64_t *b, uint64_t *out) { out[0] = *(const volatile uint64_t *)a; out[1] = *(const volatile uint64_t *)b; out[2] = (uint64_t)((const volatile unsigned char *)b)[7]; }
#endif
static void tail_touch(uint64_t *f, uint64_t *l, uint64_t vf, uint64_t vl, uint64_t *o)
{
#ifdef USE_HIP
    if (g_dev) {
        uint64_t *d = 0; if (hipMalloc((void **)&d, 4 * sizeof(uint64_t)) != hipSuccess) { fail("tail: hipMalloc"); return; }
        tail_kernel<<<1, 1>>>(f, l, vf, vl, d);
        if (hipDeviceSynchronize() != hipSuccess) fail("tail: kernel on the heap's first/last bytes faulted");
        if (hipMemcpy(o, d, 4 * sizeof(uint64_t), hipMemcpyDeviceToHost) != hipSuccess) fail("tail: hipMemcpy out");
        (void)hipFree(d); return;
    }
#endif
    o[0] = *f; o[1] = *l; *f = vf; *l = vl; o[2] = *f; o[3] = *l;
}
static void tail_peek(const uint64_t *a, const uint64_t *b, uint64_t *o)
{
#ifdef USE_HIP
    if (g_dev) {
        uint64_t *d = 0; if (hipMalloc((void **)&d, 3 * sizeof(uint64_t)) != hipSuccess) { fail("tail: hipMalloc"); return; }
        tail_read<<<1, 1>>>(a, b, d);
        if (hipDeviceSynchronize() != hipSuccess) fail("tail: kernel read of the received tail faulted");
        if (hipMemcpy(o, d, 3 * sizeof(uint64_t), hipMemcpyDeviceToHost) != hipSuccess) fail("tail: hipMemcpy out");
        (void)hipFree(d); return;
    }
#endif
    o[0] = *a; o[1] = *b; o[2] = ((const unsigned char *)b)[7];
}
static void sec_tail(void)
{
    char *hend = dbase_ + dbytes_;                                  /* one past the last byte of the heap */
    uint64_t *F = (uint64_t *)dbase_, *L = (uint64_t *)(hend - 8), *L2 = (uint64_t *)(hend - 16), *F2 = (uint64_t *)(dbase_ + 8);
    int next = (me + 1) % np, prev = (me + np - 1) % np; uint64_t o[4];
    uint64_t vf = mix64(0xf1f1ull ^ (uint64_t)me), vl = mix64(0x1a51ull ^ (uint64_t)me);
    tail_touch(F, L, vf, vl, o);
    if (o[2] != vf || o[3] != vl) fail("tail: pe %d kernel read-back of the first/last word: %llx %llx expected %llx %llx", me, (unsigned long long)o[2], (unsigned long long)o[3], (unsigned long long)vf, (unsigned long long)vl);
    uint64_t hv[2]; hget(hv, F, 8); hget(hv + 1, L, 8);               /* the host-side view of the same bytes */
    if (hv[0] != vf || hv[1] != vl) fail("tail: pe %d copy-out of the first/last word differs from the kernel's write", me);
    { uint64_t w2 = mix64(0x2222ull ^ (uint64_t)me); hput(L2, &w2, 8); }
    /* the last 4 KiB (a page span ending at the heap's last byte): pattern, put to the next PE's span of the last 4 KiB minus the 16 B words above */
    size_t span = 4096 - 32; char *src = hend - 4096 - 4096, *dst = hend - 4096;
    pfill(src, span, pbase(7001, me, next));
    xshmem_barrier_all();
    xshmem_ctx_putmem_nbi(lane[0], L, L2, 8, next);                   /* tail word to the peer's tail word (replaces L: read back below) */
    xshmem_ctx_putmem_nbi(lane[0], F2, F, 8, next);                   /* first word to the peer's second word */
    xshmem_ctx_putmem_nbi(lane[0], dst, src, span, next);             /* a span below the tail (ends 32 B before the end) */
    xshmem_ctx_putmem_nbi(lane[0], hend - 24, L2, 8, next);           /* an 8 B put ending 16 B before the end */
    xshmem_ctx_quiet(lane[0]);
    xshmem_barrier_all();
    uint64_t r[3]; tail_peek(F2, L, r);
    if (r[0] != mix64(0xf1f1ull ^ (uint64_t)prev)) fail("tail: pe %d first word from pe %d: %llx", me, prev, (unsigned long long)r[0]);
    if (r[1] != mix64(0x2222ull ^ (uint64_t)prev)) fail("tail: pe %d tail word from pe %d: %llx expected %llx", me, prev, (unsigned long long)r[1], (unsigned long long)mix64(0x2222ull ^ (uint64_t)prev));
    { size_t first = 0; unsigned g = 0, e = 0; size_t nb = pcheck(dst, span, pbase(7001, prev, me), &first, &g, &e);
      if (nb) fail("tail: pe %d span put from pe %d: %zu bad bytes, first at %zu got 0x%02x expected 0x%02x", me, prev, nb, first, g, e); }
    if (me == 0) printf("  tail: first/last bytes of the DATA heap (%.1f MiB): kernel read/write, put to peer, kernel read of the received tail: ok\n", dbytes_ / 1048576.0);
}

/* ---------------- bandwidth / latency tables ---------------- */
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double median(double *v, int n) { qsort(v, n, sizeof *v, cmp_d); return v[n / 2]; }
static int partner_far(void) { return np % 2 == 0 ? (me + np / 2) % np : (me + 1) % np; }
static void hop(int dst, size_t n, uint64_t seq)         /* payload + signal to BSIG[me]@dst; split form quiets before the signal */
{
    char *src = dsb + (size_t)dst * maxslab, *dest = drb + (size_t)me * maxslab;
    if (g_nbi) xshmem_ctx_putmem_signal_nbi(lane[0], dest, src, n, &BSIG[me], seq, XSHMEM_SIGNAL_SET, dst);
    else { xshmem_ctx_putmem_nbi(lane[0], dest, src, n, dst); xshmem_ctx_quiet(lane[0]); xshmem_ctx_signal_set(lane[0], &BSIG[me], seq, dst); }
}
static uint64_t lseq = 1u << 20;
static void sec_lat(void)
{
    if (np < 2) return;
    static const size_t sz[] = { 8, 64, 512, 4096 }; int reps = getenv("XT_QUICK") ? 50 : 300;
    if (me == 0) printf("HDR lat | form, pattern, bytes, us_per_hop(mean over %d reps), us_hop_pe0min\n", reps);
    for (int ring = 0; ring < 2; ring++) {
        if (ring == 0 && (np % 2 || np < 2)) continue;
        for (size_t k = 0; k < 4; k++) {
            double t0 = 0, t1 = 0; xshmem_barrier_all();
            if (ring) {        /* token ring: 0 -> 1 -> ... -> np-1 -> 0, np*reps hops */
                int next = (me + 1) % np, prev = (me + np - 1) % np;
                t0 = tnow();
                for (int r = 0; r < reps; r++) {
                    uint64_t s = PACK(++lseq, 0);
                    if (me == 0) { hop(next, sz[k], s); spin_wait(&BSIG[prev], s, "ring lat"); }
                    else { spin_wait(&BSIG[prev], s, "ring lat"); hop(next, sz[k], s); }
                }
                t1 = tnow(); xshmem_ctx_quiet(lane[0]);
                double hopus = (t1 - t0) / ((double)reps * np) * 1e6, mn, mean, mx; stat_d(hopus, &mn, &mean, &mx);
                if (me == 0) printf("ROW lat | %s ring(token, np=%d) %zu B | %.2f | %.2f\n", g_nbi ? "nbi" : "split", np, sz[k], mean, mn);
            } else {           /* far-partner ping-pong, all pairs at once */
                int pt = partner_far(), lead = me < pt; t0 = tnow();
                for (int r = 0; r < reps; r++) {
                    uint64_t s = PACK(++lseq, 0);
                    if (lead) { hop(pt, sz[k], s); spin_wait(&BSIG[pt], s, "pp lat"); } else { spin_wait(&BSIG[pt], s, "pp lat"); hop(pt, sz[k], s); }
                }
                t1 = tnow(); xshmem_ctx_quiet(lane[0]);
                double hopus = (t1 - t0) / (2.0 * reps) * 1e6, mn, mean, mx; stat_d(hopus, &mn, &mean, &mx);
                if (me == 0) printf("ROW lat | %s pingpong(pe,pe+n/2) %zu B | %.2f | %.2f\n", g_nbi ? "nbi" : "split", sz[k], mean, mn);
            }
            xshmem_barrier_all();
        }
    }
    fflush(stdout);
}
static void sec_stream(void)
{
    if (np < 2) return;
    int reps = 3; if (me == 0) printf("HDR stream | form, partner, bytes_per_put, window, post_ms, complete_ms, GB/s_per_PE_mean, GB/s_per_PE_min, GB/s_node_mean\n");
    for (size_t b = 64u << 10; b <= maxslab; b *= 4) {
        int W = (int)((256u << 20) / b); if (W < 1) W = 1; if (W > 256) W = 256; if (getenv("XT_QUICK") && W > 8) W = 8;
        int pt = partner_far(), from = np % 2 == 0 ? pt : (me + np - 1) % np;
        pfill(dsb + (size_t)pt * maxslab, b, pbase(555, me, pt));
        double post[8], tot[8];
        for (int r = 0; r < reps; r++) {
            xshmem_barrier_all(); uint64_t s = PACK(++lseq, 0); double t0 = tnow();
            char *src = dsb + (size_t)pt * maxslab, *dest = drb + (size_t)me * maxslab;
            for (int w = 0; w < W; w++) {
                if (g_nbi) xshmem_ctx_putmem_signal_nbi(lane[0], dest, src, b, &BSIG[me], s, XSHMEM_SIGNAL_SET, pt);
                else xshmem_ctx_putmem_nbi(lane[0], dest, src, b, pt);
            }
            double t1 = tnow(); xshmem_ctx_quiet(lane[0]); if (!g_nbi) xshmem_ctx_signal_set(lane[0], &BSIG[me], s, pt);
            xshmem_ctx_quiet(lane[0]); double t2 = tnow(); post[r] = t1 - t0; tot[r] = t2 - t0;
            spin_wait(&BSIG[from], s, "stream signal");
        }
        size_t first = 0; unsigned g = 0, e = 0; size_t nb = pcheck(drb + (size_t)from * maxslab, b, pbase(555, from, me), &first, &g, &e);
        if (nb) fail("stream DATA %zu B from pe %d: %zu bad bytes, first %zu got 0x%02x expected 0x%02x", b, from, nb, first, g, e);
        double mp = median(post, reps), mt = median(tot, reps), gbs = (double)W * b / mt / 1e9, mn, mean, mx; stat_d(gbs, &mn, &mean, &mx);
        if (me == 0) printf("ROW stream | %s %s %zu | %d | %.3f | %.3f | %.2f | %.2f | %.2f\n", g_nbi ? "nbi" : "split", (partner_far() / ppn != me / ppn) ? "off-node" : "on-node", b, W, mp * 1e3, mt * 1e3, mean, mn, mean * ppn);
    }
    fflush(stdout);
}
static void a2a_table(xcomm *c, const char *name, uint64_t tagb)
{
    int reps = getenv("XT_REPS") ? atoi(getenv("XT_REPS")) : 5; if (reps > 16) reps = 16; if (getenv("XT_QUICK") && reps > 2) reps = 2;
    static xex x; size_t cn[MAXM];
    if (me == 0) printf("HDR %s | form, members, bytes_per_slab, post_ms, wait_ms, total_ms(median), min_ms, GB/s_per_PE(received, comparable to t_comm --bw per thread), min_PE, node_GB/s_mean\n", name);
    for (size_t b = 64u << 10; b <= maxslab; b *= 4) {
        for (int i = 0; i < c->n; i++) { cn[i] = b; pfill(dsb + (size_t)i * maxslab, b, pbase(tagb + b, me, c->pe[i])); }
        double tp[16], tw[16], tt[16];
        for (int r = -1; r < reps; r++) {          /* r = -1: warm-up */
            xshmem_barrier_all(); double t0 = tnow();
            xex_post(&x, c, dsb, maxslab, cn, drb, maxslab, cn, tagb + b, r == reps - 1);       /* last rep: verify */
            double t1 = tnow(); xex_wait(&x); double t2 = tnow();
            if (r >= 0) { tp[r] = t1 - t0; tw[r] = t2 - t1; tt[r] = t2 - t0; }
            if ((&x)->bad) fail("%s %zu B rep %d: %d bad", name, b, r, (&x)->bad);
        }
        double mp = median(tp, reps), mw = median(tw, reps), mt = median(tt, reps); double mnt = tt[0];
        for (int r = 0; r < reps; r++) if (tt[r] < mnt) mnt = tt[r];
        double gbs = (double)b * (c->n - 1) / mt / 1e9, mn, mean, mx; stat_d(gbs, &mn, &mean, &mx);
        if (me == 0) printf("ROW %s | %s %d %zu | %.3f | %.3f | %.3f | %.3f | %.2f | %.2f | %.2f\n", name, g_nbi ? "nbi" : "split", c->n, b, mp * 1e3, mw * 1e3, mt * 1e3, mnt * 1e3, mean, mn, mean * ppn);
    }
    fflush(stdout);
}
static void sec_a2a(void)
{
    int pes[MAXM]; for (int i = 0; i < np; i++) pes[i] = i;
    xcomm *w = comm_new(0, np, pes); w->seq = 5000;      /* the words of cid 0 hold the correctness sections' seq values: continue above them */
    a2a_table(w, "a2a", 2000000);
    if (np % 2 == 0 && np > 2) {
        int pp[2] = { me < np / 2 ? me : me - np / 2, me < np / 2 ? me + np / 2 : me };
        xcomm *p = comm_new(1, 2, pp); p->seq = 5000; a2a_table(p, "a2a_pairs", 3000000);
    }
}

/* ---------------- main ---------------- */
int main(int argc, char **argv)
{
    int prov = 0; xshmem_init_thread(XSHMEM_THREAD_SERIALIZED, &prov);
    me = xshmem_my_pe(); np = xshmem_n_pes();
    if (np > MAXM) { fprintf(stderr, "t_xshmem: %d PEs > %d\n", np, MAXM); return 2; }
    xshmem_cfg_t cfg_s, *cfgp = &cfg_s; xshmem_cfg_load(cfgp); xshmem_caps_t caps_s, *capp = &caps_s; xshmem_caps_query(capp); xshmem_topo_t topo_s, *tpo = &topo_s; memset(tpo, 0, sizeof topo_s);
    int have_topo = xshmem_topo(tpo) == 0;
    ppn = have_topo && tpo->pes_per_node > 0 ? tpo->pes_per_node : (getenv("SLURM_NTASKS_PER_NODE") ? atoi(getenv("SLURM_NTASKS_PER_NODE")) : np);
    if (ppn < 1 || np % ppn) ppn = np;
    locr = have_topo ? tpo->local : (getenv("SLURM_LOCALID") ? atoi(getenv("SLURM_LOCALID")) : me % ppn);
    g_nodes = np / ppn;
    nl = have_topo && tpo->nlanes > 0 ? tpo->nlanes : cfgp->lanes; if (nl < 1) nl = 1; if (nl > MAXL) nl = MAXL;
    g_dev = capp->dev_heap;
#ifdef USE_HIP
    if (g_dev && have_topo && tpo->gpu >= 0) (void)!hipSetDevice(tpo->gpu);
#endif
    for (int l = 0; l < nl; l++) if (xshmem_ctx_create(XSHMEM_CTX_SERIALIZED, &lane[l])) { fprintf(stderr, "t_xshmem: pe %d: ctx_create %d failed\n", me, l); xshmem_global_exit(2); }
    const char *e;
    g_nbi = (e = getenv("XT_PUTSIG")) && !strcmp(e, "nbi");
    g_rot = (e = getenv("XT_ORDER")) && !strcmp(e, "rot");
    if ((e = getenv("XT_SEED"))) g_seed = (uint64_t)atoll(e);
    if ((e = getenv("XT_TIMEOUT"))) g_timeout = atof(e);
    long mb = (e = getenv("XT_MAXMB")) ? atol(e) : 64; if (mb < 1) mb = 1;
    maxslab = (size_t)mb << 20;
    size_t lreg = (size_t)6 * np * (getenv("XT_LBYTES") ? (size_t)atol(getenv("XT_LBYTES")) : 262144);
    if (lreg < ((size_t)12 * np << 10)) lreg = (size_t)12 * np << 10;
    size_t lregr = (lreg + (1u << 20) - 1) & ~(size_t)((1u << 20) - 1);
    lregion = lregr;
    size_t dbytes = 2 * (size_t)np * maxslab + lregr + (4u << 20); dbytes = (dbytes + (2u << 20) - 1) & ~(size_t)((2u << 20) - 1);
    char *dbase = (char *)xshmem_heap_alloc(dbytes, XSHMEM_SPACE_DATA);
    ctl = (uint64_t *)xshmem_heap_alloc((size_t)1 << 20, XSHMEM_SPACE_CTRL);
    if (!dbase || !ctl) { fprintf(stderr, "t_xshmem: heap_alloc failed (%zu B data)\n", dbytes); xshmem_global_exit(2); }
    dbase_ = dbase; dbytes_ = dbytes;
    dsb = dbase; drb = dbase + (size_t)np * maxslab; dL = dbase + 2 * (size_t)np * maxslab;
    hb1 = (unsigned char *)malloc(CH + 64); hb2 = (unsigned char *)malloc(CH + 64);
    if ((size_t)(NCID * BLOCKW + 4 * MAXM) * 8 > ((size_t)1 << 20)) { fprintf(stderr, "t_xshmem: control layout too big\n"); return 2; }
    if (me == 0) {
        printf("INFO t_xshmem: %d PEs, %d per node (%d nodes), lanes %d, thread level %d, form %s, order %s, slab max %ld MiB, DATA heap %.1f MiB per PE, seed %llu, timeout %.0f s\n",
               np, ppn, g_nodes, nl, prov, g_nbi ? "putmem_signal_nbi" : "split(put+quiet+signal_set)", g_rot ? "rot" : "seq", mb, dbytes / 1048576.0, (unsigned long long)g_seed, g_timeout);
        printf("INFO caps: dev_heap %d host_pollable %d src_any %d ptr %d nlanes_ok %d | cfg: lanes %d heap_mb %ld ctrl_host %d\n",
               capp->dev_heap, capp->host_pollable, capp->src_any, capp->ptr, capp->nlanes_ok, cfgp->lanes, cfgp->heap_mb, cfgp->ctrl_host);
    }
    if (have_topo) printf("MAP pe %d node %d/%d local %d gpu %d nics %d [%d] numa %d lanes %d\n", me, tpo->node, tpo->nnodes, tpo->local, tpo->gpu, tpo->nics, tpo->nics ? tpo->nic[0] : -1, tpo->numa, tpo->nlanes);
    fflush(stdout);
    xshmem_barrier_all();

    int want[16], nw = 0; enum { S_BASIC, S_EXCH, S_DEAD, S_LAYER, S_LIMIT, S_TAIL, S_LAT, S_STREAM, S_A2A, S_N };
    static const char *names[] = { "basic", "exch", "deadlock", "layer", "limit", "tail", "lat", "stream", "a2a" };
    for (int i = 0; i < S_N; i++) want[i] = 0; (void)nw;
    if (argc < 2) for (int i = S_BASIC; i <= S_TAIL; i++) want[i] = 1;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "correct")) for (int i = S_BASIC; i <= S_TAIL; i++) want[i] = 1;
        else if (!strcmp(argv[a], "bw")) for (int i = S_LAT; i <= S_A2A; i++) want[i] = 1;
        else if (!strcmp(argv[a], "all")) for (int i = 0; i < S_N; i++) want[i] = 1;
        else { int ok = 0; for (int i = 0; i < S_N; i++) if (!strcmp(argv[a], names[i])) { want[i] = 1; ok = 1; } if (!ok) { if (me == 0) fprintf(stderr, "t_xshmem: unknown section %s\n", argv[a]); xshmem_global_exit(2); } }
    }
    int total = 0;
    for (int i = 0; i < S_N; i++) if (want[i]) {
        int f0 = nfail; double t0 = tnow(); ctr_begin(names[i]);
        switch (i) { case S_BASIC: sec_basic(); break; case S_EXCH: sec_exch(); break; case S_DEAD: sec_deadlock(); break; case S_LAYER: sec_layer(); break;
                     case S_LIMIT: sec_limit(); break; case S_TAIL: sec_tail(); break; case S_LAT: sec_lat(); break; case S_STREAM: sec_stream(); break; default: sec_a2a(); }
        ctr_end();
        total += section_end(names[i], f0);
        if (me == 0) printf("  (%s took %.1f s)\n", names[i], tnow() - t0);
    }
    xshmem_barrier_all();
    if (me == 0) { printf("XT_RESULT %s (pe0 failures %d, all PEs %d)\n", total || nfail ? "FAIL" : "PASS", nfail, total); fflush(stdout); }
    xshmem_barrier_all();
    xshmem_heap_free(ctl); xshmem_heap_free(dbase);
    for (int l = 0; l < nl; l++) xshmem_ctx_destroy(lane[l]);
    xshmem_finalize();
    return (total || nfail) ? 1 : 0;
}

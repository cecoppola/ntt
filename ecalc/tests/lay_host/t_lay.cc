/* t_lay - S45: the layered communicator (comm_layered.c, host-only build through hip_host_shim.h) on many ranks over TCP, to exercise the
 * COMM_LAYER_VSLOT_SHARE group / level / v-slot logic without a GPU.  One process = one APU (node r, device d): NA = 4 processes per node form
 * the intra communicator (a TCP mesh), the nodes of each group the inter communicator of APU d (a TCP mesh, wrapped so it has sym_alloc /
 * sym_free over an emulated first-fit pool, comm_shmem.c's algorithm, so COMM_LAYER_VSLOT_POOL=1 runs).  Levels = group sizes (e.g. 6,15:
 * groups of 6, the last group cut to 3, then the whole 15-node machine, like the target's 192 then 576).  Per level: batches of unequal
 * all-to-alls (the v-exchange, depth 2 pipelined, the data verified word for word); then the cases SHARE changes: the owner hand-over with the
 * previous owner's exchange still pending, and going back to the lower level after the top.  Prints one line per process.
 *   t_lay --nodes N --levels g1,g2,...,N [--hosts h0,h1,..] [--first a --count b] [--port P] [--rounds R] [--pool-mb M]
 * the SHARE / pool / INTER2 switches are the product's own environment variables (COMM_LAYER_VSLOT_SHARE, COMM_LAYER_VSLOT_POOL, COMM_LAYER_INTER2).
 * Needs: build.sh (g++ over a perl-rewritten copy of comm_layered.c).  With --first/--count one invocation forks the processes of nodes [first, first+count)
 * (several hosts: one invocation per host, same --hosts list). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>
#include <algorithm>
#include <string>
#include "comm.h"
extern "C" int comm_xgmi_last_push(int, double *, double *) { return 0; }
static const int NA = 4;
static int g_N, g_nl; static int g_lv[16]; static std::vector<std::string> g_host; static int g_port = 30000, g_rounds = 3; static size_t g_pool_mb = 256;
static int g_pool_on, g_share_on, g_i2_on;
/* ---- the emulated symmetric pool: first-fit, coalescing (comm_shmem.c pool_alloc / pool_free), one per process ---- */
struct blk { size_t off, len; int used, kind; blk *next; };
static int g_ofi; static blk *g_blocks; static size_t g_pool_bytes; static const size_t ALIGN_ = 256;
struct pev { int op; size_t off, len; int kind; };
static std::vector<pev> g_log;
static size_t pool_alloc(size_t len, int kind)
{
    len = (len + ALIGN_ - 1) & ~(ALIGN_ - 1); if (!len) len = ALIGN_;
    if (g_ofi && kind == 2) {   /* comm_ofi.c comm_ofi_alloc with COMM_OFI_SYM_TOP (on with COMM_LAYER_VSLOT_POOL): the symmetric buffers last-fit, carved from the hole's top */
        blk *best = 0; for (blk *b = g_blocks; b; b = b->next) if (!b->used && b->len >= len) best = b;
        if (!best) { fprintf(stderr, "t_lay: emulated pool (%zu MiB) cannot hold %zu MiB\n", g_pool_bytes >> 20, len >> 20); exit(3); }
        blk *b = best; if (b->len > len) { blk *nb = new blk{ b->off + b->len - len, len, 0, 0, b->next }; b->next = nb; b->len -= len; b = nb; }
        b->used = 1; b->kind = kind; g_log.push_back({ 1, b->off, len, kind }); return b->off;
    }
    for (blk *b = g_blocks; b; b = b->next) if (!b->used && b->len >= len) {
        if (b->len > len) { blk *nb = new blk{ b->off + len, b->len - len, 0, 0, b->next }; b->next = nb; b->len = len; }
        b->used = 1; b->kind = kind; g_log.push_back({ 1, b->off, len, kind }); return b->off;
    }
    fprintf(stderr, "t_lay: emulated pool (%zu MiB) cannot hold %zu MiB\n", g_pool_bytes >> 20, len >> 20); exit(3);
}
static void pool_free(size_t off)
{
    blk *p = 0;
    for (blk *b = g_blocks; b; p = b, b = b->next) if (b->off == off) {
        b->used = 0; g_log.push_back({ 0, off, b->len, b->kind });
        if (b->next && !b->next->used) { blk *n = b->next; b->len += n->len; b->next = n->next; delete n; }
        if (p && !p->used) { p->len += b->len; p->next = b->next; delete b; }
        return;
    }
    fprintf(stderr, "t_lay: free of an unknown pool offset\n"); exit(3);
}
static char *g_pool;
static void *w_sym_alloc(comm *, size_t bytes) { return g_pool + pool_alloc(bytes, 2); }
static void w_sym_free(comm *, void *p) { pool_free((size_t)((char *)p - g_pool)); }
static comm_ops g_wops;
static comm *wrap(comm *t, size_t ctrl)
{
    if (!g_wops.rank) { g_wops = *t->ops; g_wops.sym_alloc = w_sym_alloc; g_wops.sym_free = w_sym_free; }
    comm *w = (comm *)calloc(1, sizeof *w); *w = *t; w->ops = &g_wops;
    if (!g_ofi) pool_alloc(ctrl, 1);   /* SHMEM transport: the communicator's control block lives in the same pool (emulated size: 4 KiB + 64 B per rank); the OFI pool holds only the symmetric buffers and staging */
    return w;
}
/* ---- the exchange pattern ---- */
static uint64_t mix(uint64_t z) { z += 0x9E3779B97F4A7C15ULL; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
static uint64_t word(int lev, int from, int to, int round, size_t k) { return mix(((uint64_t)lev << 56) ^ ((uint64_t)from << 40) ^ ((uint64_t)to << 24) ^ ((uint64_t)round << 16) ^ k); }
static int g_ubase = 1024;
static size_t unit_of(int lev) { return 8 * (size_t)(g_ubase + (g_ubase * 3 / 2) * lev); }       /* bytes per count unit: grows with the level, so the upper level's slots are larger */
static size_t cnt(int lev, int from, int to, int round) { int u = (from + to + round) % 5 == 0 ? 0 : (from * 7 + to * 13 + round * 5) % 9; return (size_t)u * unit_of(lev); }
struct xch { int lev, round, nr, me; size_t *scnt, *sdsp, *rcnt, *rdsp; char *sb, *rb; size_t ts, tr; };
static void xch_post(xch *x, comm *lay, int lev, int round, int me, int nr)
{
    x->lev = lev; x->round = round; x->nr = nr; x->me = me;
    x->scnt = (size_t *)malloc(nr * 8); x->sdsp = (size_t *)malloc(nr * 8); x->rcnt = (size_t *)malloc(nr * 8); x->rdsp = (size_t *)malloc(nr * 8);
    size_t ts = 0, tr = 0;
    for (int r = 0; r < nr; r++) { x->scnt[r] = cnt(lev, me, r, round); x->sdsp[r] = ts; ts += x->scnt[r]; x->rcnt[r] = cnt(lev, r, me, round); }
    for (int r = nr - 1; r >= 0; r--) { x->rdsp[r] = tr; tr += x->rcnt[r]; }       /* reverse rank order on the receive side */
    x->ts = ts; x->tr = tr; x->sb = (char *)malloc(ts + 8); x->rb = (char *)malloc(tr + 8); memset(x->rb, 0xEE, tr + 8);
    for (int r = 0; r < nr; r++) for (size_t k = 0; k < x->scnt[r]; k += 8) *(uint64_t *)(x->sb + x->sdsp[r] + k) = word(lev, me, r, round, k / 8);
    comm_alltoallv(lay, x->sb, x->scnt, x->sdsp, x->rb, x->rcnt, x->rdsp, 0);
}
static long xch_check(xch *x)
{
    long bad = 0;
    for (int r = 0; r < x->nr; r++) for (size_t k = 0; k < x->rcnt[r]; k += 8) if (*(uint64_t *)(x->rb + x->rdsp[r] + k) != word(x->lev, r, x->me, x->round, k / 8)) bad++;
    free(x->scnt); free(x->sdsp); free(x->rcnt); free(x->rdsp); free(x->sb); free(x->rb); return bad;
}
static std::string hostlist(int g0, int g)
{
    std::string s; for (int i = 0; i < g; i++) { if (i) s += ","; s += g_host[g0 + i]; } return s;
}
static std::string hostlist_intra(int node) { std::string s; for (int i = 0; i < NA; i++) { if (i) s += ","; s += g_host[node]; } return s; }
static uint64_t dig_live(void)   /* the live K_SYM blocks (offset, length) of the pool: what a put into a peer's pool relies on */
{
    uint64_t h = 1469598103934665603ULL;
    for (blk *b = g_blocks; b; b = b->next) if (b->used && b->kind == 2) { h = (h ^ b->off) * 1099511628211ULL; h = (h ^ b->len) * 1099511628211ULL; }
    return h;
}
static uint64_t dig_log(void) { uint64_t h = 1469598103934665603ULL; for (auto &e : g_log) if (e.kind == 2) { h = (h ^ (uint64_t)e.op) * 1099511628211ULL; h = (h ^ e.off) * 1099511628211ULL; h = (h ^ e.len) * 1099511628211ULL; } return h; }
static int child(int node, int d)
{
    g_pool_bytes = g_pool_mb << 20; g_pool = (char *)malloc(g_pool_bytes); g_blocks = new blk{ 0, g_pool_bytes, 0, 0, 0 };
    int base = g_port, commno = 0; const int SPAN = 64;
    auto nextbase = [&]() { int b = base + SPAN * commno++; return b; };
    /* global creation order: intra of every node, then per level every group's inter comms (APU d of the members), then the second meshes */
    comm *intra = 0;
    for (int n = 0; n < g_N; n++) { int b = nextbase(); if (n == node) intra = comm_tcp_create_at(d, NA, hostlist_intra(n).c_str(), b); }
    comm *inter[16] = {}, *inter2[16] = {}, *lay[16] = {}; int gsz[16], gidx[16], g0s[16];
    for (int L = 0; L < g_nl; L++) {
        int g = g_lv[L]; int ng = (g_N + g - 1) / g; gidx[L] = node / g; g0s[L] = gidx[L] * g; gsz[L] = std::min(g, g_N - g0s[L]);
        for (int G = 0; G < ng; G++) for (int dd = 0; dd < NA; dd++) {
            int b = nextbase(); int g0 = G * g, gs = std::min(g, g_N - g0);
            if (G == gidx[L] && dd == d) { comm *t = comm_tcp_create_at(node - g0, gs, hostlist(g0, gs).c_str(), b); inter[L] = wrap(t, 4096 + 64 * (size_t)gs); }
        }
        if (g_i2_on) for (int G = 0; G < ng; G++) for (int dd = 0; dd < NA; dd++) {
            int b = nextbase(); int g0 = G * g, gs = std::min(g, g_N - g0);
            if (G == gidx[L] && dd == d) { comm *t = comm_tcp_create_at(node - g0, gs, hostlist(g0, gs).c_str(), b); inter2[L] = wrap(t, 4096 + 64 * (size_t)gs); }
        }
    }
    /* the pool rule (binsplit.c vslot_prealloc_env): under COMM_LAYER_VSLOT_POOL every v-slot is allocated once at its group size's bound (all sizes the overall maximum under SHARE) */
    if (g_pool_on) {
        size_t mx[16] = {}, all = 0;
        for (int L = 0; L < g_nl; L++) { int g = g_lv[L]; for (int G = 0; G * g < g_N; G++) { int gs = std::min(g, g_N - G * g); size_t bound = 2 * (size_t)NA * gs * 8 * unit_of(L) + 8192; if (bound > mx[L]) mx[L] = bound; } if (mx[L] > all) all = mx[L]; }
        for (int L = 0; L < g_nl; L++) for (int G = 0; G * g_lv[L] < g_N; G++) {
            int gs = std::min(g_lv[L], g_N - G * g_lv[L]); size_t bound = 2 * (size_t)NA * gs * 8 * unit_of(L) + 8192, m = g_share_on ? all : bound; (void)bound; (void)mx;
            char nm[64], v[32]; snprintf(nm, sizeof nm, "COMM_LAYER_VSLOT_MB_%d", gs); snprintf(v, sizeof v, "%zu", (m >> 20) + 1); setenv(nm, v, 1);
        }
    }
    for (int L = 0; L < g_nl; L++) { lay[L] = comm_layered_create(intra, inter[L], d); if (g_i2_on) comm_layered_set_inter2(lay[L], inter2[L]); }
    long bad = 0; size_t live_lvl[16] = {};
    auto me_of = [&](int L) { return gsz[L] * d + (node - g0s[L]); };
    auto nr_of = [&](int L) { return NA * gsz[L]; };
    const int R = g_rounds;
    int rnd = 0;
    for (int L = 0; L < g_nl; L++) {
        xch xs[8];
        for (int k = 0; k < R; k++) { xch_post(&xs[k], lay[L], L, rnd++, me_of(L), nr_of(L)); if (getenv("LAY_WAIT_EACH")) comm_wait(lay[L]); }
        if (!getenv("LAY_WAIT_EACH")) for (int k = 0; k < R; k++) comm_wait(lay[L]);   /* one wait per exchange (comm_layered y_wait counts them) */
        for (int k = 0; k < R; k++) bad += xch_check(&xs[k]);
        live_lvl[L] = shim_live;
    }
    uint64_t live_top = dig_live();
    /* the hand-over: an exchange of the lower level still pending when the top level posts, then back to the lower level, then the top again */
    if (g_nl >= 2) {
        int A = 0, T = g_nl - 1; xch xa, xb, xc, xd;
        xch_post(&xa, lay[A], A, rnd++, me_of(A), nr_of(A));
        xch_post(&xb, lay[T], T, rnd++, me_of(T), nr_of(T));
        comm_wait(lay[T]); bad += xch_check(&xb); comm_wait(lay[A]); bad += xch_check(&xa);
        xch_post(&xc, lay[A], A, rnd++, me_of(A), nr_of(A)); comm_wait(lay[A]); bad += xch_check(&xc);
        xch_post(&xd, lay[T], T, rnd++, me_of(T), nr_of(T)); comm_wait(lay[T]); bad += xch_check(&xd);
        live_top = dig_live();
    }
    size_t peak = shim_peak, live = shim_live;
    /* symmetry: the live pool blocks must agree across the members of the top communicator (and of the lower level's group); the whole log is informational */
    uint64_t mine[3] = { live_top, dig_log(), (uint64_t)bad }, *all = (uint64_t *)malloc(3 * 8 * g_N); long asym_top = 0, asym_low = 0, asym_log = 0;
    if (g_pool_on) {
        int T = g_nl - 1; comm_allgather_host(inter[T], mine, all, 24);
        for (int r = 0; r < gsz[T]; r++) { if (all[3 * r] != mine[0]) asym_top++; if (all[3 * r + 1] != mine[1]) asym_log++; }
        comm_allgather_host(inter[0], mine, all, 24);
        for (int r = 0; r < gsz[0]; r++) if (all[3 * r] != mine[0]) asym_low++;
    }
    comm_barrier(inter[g_nl - 1]);
    size_t live_before = shim_live, pool_live = 0; for (blk *b = g_blocks; b; b = b->next) if (b->used && b->kind == 2) pool_live += b->len;
    for (int L = g_nl - 1; L >= 0; L--) comm_destroy(lay[L]);   /* (the last communicator of the device frees the shared pair) */
    size_t after = shim_live, pool_after = 0; for (blk *b = g_blocks; b; b = b->next) if (b->used && b->kind == 2) pool_after += b->len;
    printf("T node %d d %d bad %ld peak %zu live %zu lv", node, d, bad, peak, live_before);
    for (int L = 0; L < g_nl; L++) printf(" %zu", live_lvl[L]);
    printf(" after %zu pool_sym %zu pool_sym_after %zu asym_top %ld asym_low %ld asym_log %ld\n", after, pool_live, pool_after, asym_top, asym_low, asym_log);
    fflush(stdout);
    return (bad || asym_top || asym_low || after || pool_after) ? 1 : 0;
}
int main(int argc, char **argv)
{
    std::string hosts; int first = 0, count = -1;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i]; const char *v = argv[i + 1];
        if (a == "--nodes") g_N = atoi(v); else if (a == "--levels") { char *s = strdup(v), *sp; for (char *t = strtok_r(s, ",", &sp); t; t = strtok_r(0, ",", &sp)) g_lv[g_nl++] = atoi(t); }
        else if (a == "--hosts") hosts = v; else if (a == "--first") first = atoi(v); else if (a == "--count") count = atoi(v);
        else if (a == "--port") g_port = atoi(v); else if (a == "--rounds") g_rounds = atoi(v); else if (a == "--ubase") g_ubase = atoi(v); else if (a == "--pool") g_ofi = !strcmp(v, "ofi"); else if (a == "--pool-mb") g_pool_mb = (size_t)atoi(v);
        else { fprintf(stderr, "bad arg %s\n", a.c_str()); return 2; }
    }
    if (g_N < 2 || !g_nl) { fprintf(stderr, "usage: t_lay --nodes N --levels g1,..,N [--hosts ..] [--first a --count b] [--port P] [--rounds R] [--pool-mb M]\n"); return 2; }
    if (count < 0) count = g_N - first;
    for (int i = 0; i < g_N; i++) g_host.push_back("");
    if (hosts.empty()) for (int i = 0; i < g_N; i++) g_host[i] = "127.0.0.1";
    else { std::vector<std::string> hl; char *s = strdup(hosts.c_str()), *sp; for (char *t = strtok_r(s, ",", &sp); t; t = strtok_r(0, ",", &sp)) hl.push_back(t); for (int i = 0; i < g_N; i++) g_host[i] = hl[i % hl.size()]; }
    { const char *e; e = getenv("COMM_LAYER_VSLOT_POOL"); g_pool_on = e && atoi(e); e = getenv("COMM_LAYER_VSLOT_SHARE"); g_share_on = e && atoi(e); e = getenv("COMM_LAYER_INTER2"); g_i2_on = e && atoi(e); }
    setvbuf(stdout, 0, _IOLBF, 0);
    std::vector<pid_t> pids;
    for (int n = first; n < first + count; n++) for (int d = 0; d < NA; d++) {
        pid_t p = fork(); if (p < 0) { perror("fork"); return 2; }
        if (!p) _exit(child(n, d));
        pids.push_back(p);
    }
    int fails = 0; for (pid_t p : pids) { int st; waitpid(p, &st, 0); if (!WIFEXITED(st) || WEXITSTATUS(st)) fails++; }
    printf("t_lay nodes %d levels", g_N); for (int i = 0; i < g_nl; i++) printf(" %d", g_lv[i]); printf(" share %d vslotpool %d inter2 %d pool %s: %zu processes, %d failed -> %s\n", g_share_on, g_pool_on, g_i2_on, g_ofi ? "ofi(top-down last-fit)" : "shmem(first-fit+ctrl)", pids.size(), fails, fails ? "FAIL" : "VERIFY OK");
    return fails ? 1 : 0;
}

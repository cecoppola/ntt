/* t_comm - the TCP communicator (WP6) on N forked localhost processes:
 * S12: the same exchanges on buffers of the transport's symmetric pool (comm_sym_alloc: unstaged over SHMEM; malloc'd
 * where the transport has no pool), twice in a row on the same buffers.
 * all-to-all of random slabs checked against the senders, barrier, reductions, (M7) the all-gathers
 * (host blocks below and above the no-thread threshold; the "device" op, which is the host one in this build), and
 * (B7) the unequal all-to-all: per-pair counts of 0..8 units (units of 8 B, 1000 B and 3 MiB), slabs back to back
 * on the send side and in reverse rank order on the receive side, the device and the host op.
 * Point-to-point (S): small values to all before any receive, a 3 MiB message between pairs.
 * COMM_TRANSPORT=shmem (S): one PE per process under oshrun (mnrun.sh <n> ./tests/t_comm): the same checks over the SHMEM
 * transport on the full PE set, then on the strided sets of the even and the odd PEs (n >= 4).
 * Host-only build: cc -O2 -DCOMM_HOST_ONLY -I.. t_comm.c ../comm_tcp.c ../comm_shmem.c ../comm_util.c -lpthread
 * With COMM_RANK set (one process per rank, e.g. under wp6run.sh across nodes)
 * this process is that rank: it prints its own VERIFY line and exits nonzero on failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../comm.h"
static uint64_t mix(uint64_t z) { z += 0x9E3779B97F4A7C15ULL; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
static uint64_t slab_word(int from, int to, int round, size_t k) { return mix(((uint64_t)from << 40) ^ ((uint64_t)to << 28) ^ ((uint64_t)round << 20) ^ k); }
/* B7: the unequal exchange's pattern -- units per pair, zero for some pairs */
static size_t vunits(int from, int to, int round) { return ((from + to + round) % 5 == 0) ? 0 : (size_t)((from * 7 + to * 13 + round * 5) % 9); }
static void *xalloc(comm *c, size_t bytes, int *sym);
static void xfree(comm *c, void *p, int sym);
static int check_alltoallv(comm *c, int me, int n)
{
    size_t units[] = { 8, 1000, 3 << 20 }; int bad = 0;
    for (int round = 0; round < 9; round++) {                /* 0-2 the device op, 3-5 the host op, 6-8 the device op on symmetric buffers (S12) */
        size_t u = units[round % 3]; int sym = round >= 6;
        size_t *scnt = malloc(n * sizeof *scnt), *sdsp = malloc(n * sizeof *sdsp), *rcnt = malloc(n * sizeof *rcnt), *rdsp = malloc(n * sizeof *rdsp);
        for (int r = 0; r < n; r++) { scnt[r] = vunits(me, r, round) * u; rcnt[r] = vunits(r, me, round) * u; }
        size_t ts = comm_prefix(scnt, sdsp, n), tr = 0;
        for (int r = n - 1; r >= 0; r--) { rdsp[r] = tr; tr += rcnt[r]; }          /* reverse rank order on the receive side */
        int ssym = sym, rsym = sym; char *sb = xalloc(c, ts + 8, &ssym), *rb = xalloc(c, tr + 8, &rsym); memset(rb, 0xEE, tr + 8);
        for (int r = 0; r < n; r++) for (size_t k = 0; k < scnt[r]; k++) sb[sdsp[r] + k] = (char)slab_word(me, r, 100 + round, k / 8);
        if (round < 3 || sym) { comm_alltoallv(c, sb, scnt, sdsp, rb, rcnt, rdsp, NULL); comm_wait(c); }
        else comm_alltoallv_host(c, sb, scnt, sdsp, rb, rcnt, rdsp);
        for (int r = 0; r < n; r++) for (size_t k = 0; k < rcnt[r]; k++) if (rb[rdsp[r] + k] != (char)slab_word(r, me, 100 + round, k / 8)) bad++;
        xfree(c, sb, ssym); xfree(c, rb, rsym); free(scnt); free(sdsp); free(rcnt); free(rdsp);
        comm_barrier(c);
    }
    return bad;
}
/* point-to-point (S): small values to every rank before any receive (the carry flags' pattern), then a 3 MiB message
 * between pairs (even ranks send first, odd ranks receive first) */
static int check_p2p(comm *c, int me, int n)
{
    int bad = 0;
    for (int round = 0; round < 2; round++) {
        size_t bytes = round ? 8 : 1; char sb[8], rb[8];
        for (int r = 0; r < n; r++) if (r != me) { uint64_t w = slab_word(me, r, 200 + round, 0); memcpy(sb, &w, 8); comm_send(c, r, sb, bytes); }
        for (int r = 0; r < n; r++) if (r != me) { uint64_t w = slab_word(r, me, 200 + round, 0); comm_recv(c, r, rb, bytes); if (memcmp(rb, &w, bytes)) bad++; }
    }
    if (n >= 2) {
        int peer = me ^ 1; if (peer < n) {
            size_t words = (3 << 20) / 8; uint64_t *sb = malloc(words * 8), *rb = malloc(words * 8);
            for (size_t k = 0; k < words; k++) sb[k] = slab_word(me, peer, 300, k);
            if (me & 1) { comm_recv(c, peer, rb, words * 8); comm_send(c, peer, sb, words * 8); }
            else { comm_send(c, peer, sb, words * 8); comm_recv(c, peer, rb, words * 8); }
            for (size_t k = 0; k < words; k++) if (rb[k] != slab_word(peer, me, 300, k)) bad++;
            free(sb); free(rb);
        }
    }
    comm_barrier(c);
    return bad;
}
static int check_all(comm *c, int me, int n);
static int shmem_mode;
static int run_rank(int me, int n, const char *hosts, int port)
{
    comm *c;
    if (shmem_mode) c = comm_shmem_create_at(0, 1, n, 0);
    else {
        char b[32]; snprintf(b, sizeof b, "%d", me); setenv("COMM_RANK", b, 1);
        snprintf(b, sizeof b, "%d", n); setenv("COMM_SIZE", b, 1); setenv("COMM_HOSTS", hosts, 1);
        snprintf(b, sizeof b, "%d", port); setenv("COMM_PORT", b, 1);
        c = comm_tcp_create();
    }
    int bad = check_all(c, me, n);
    comm_destroy(c);
    if (shmem_mode && n >= 4) {                        /* the strided PE sets (the teams shim): the even and the odd PEs, both alive at once */
        int par = me & 1, ns = (n - par + 1) / 2;
        comm *sc = comm_shmem_create_at(par, 2, ns, 1 + par);
        int b2 = check_all(sc, me / 2, ns); if (b2) printf("t_comm: rank %d: the strided set of the %s PEs: %d bad\n", me, par ? "odd" : "even", b2);
        bad += b2; comm_destroy(sc);
    }
    return bad;
}
/* S12: a buffer of the transport's symmetric pool when it has one (rounds >= the plain ones: the pool-resident, unstaged
 * path of the SHMEM transport), else malloc'd; mode 1: only the send side in the pool, 2: only the receive side */
static void *xalloc(comm *c, size_t bytes, int *sym) { void *p = *sym ? comm_sym_alloc(c, bytes) : 0; *sym = p != 0; return p ? p : malloc(bytes); }   /* *sym: wanted in, got out */
static void xfree(comm *c, void *p, int sym) { if (sym) comm_sym_free(c, p); else free(p); }
static int check_all(comm *c, int me, int n)
{
    int bad = 0;
    size_t sizes[] = { 1, 8, 1000, 1 << 16, 3 << 20, 8, 1 << 16, 3 << 20, 3 << 20, 3 << 20 };   /* bytes per slab, incl. one larger than the socket buffers; the last five with symmetric buffers */
    for (int round = 0; round < 10; round++) {
        size_t bytes = sizes[round], words = bytes / 8 ? bytes / 8 : 1, alloc = words * 8;
        int ssym = round >= 5 && round != 9, rsym = round >= 5 && round != 8;
        uint64_t *sb = xalloc(c, alloc * n, &ssym), *rb = xalloc(c, alloc * n, &rsym);
        for (int s = 0; s < n; s++) for (size_t k = 0; k < words; k++) sb[s * words + k] = slab_word(me, s, round, k);
        comm_alltoall(c, sb, rb, alloc, NULL); comm_wait(c);
        for (int s = 0; s < n; s++) for (size_t k = 0; k < words; k++) if (rb[s * words + k] != slab_word(s, me, round, k)) bad++;
        if (round >= 5) {                                    /* twice in a row on the same buffers: the sequence / roff handshake with a fixed landing place */
            for (int s = 0; s < n; s++) for (size_t k = 0; k < words; k++) sb[s * words + k] = slab_word(me, s, round + 50, k);
            comm_alltoall(c, sb, rb, alloc, NULL); comm_wait(c);
            for (int s = 0; s < n; s++) for (size_t k = 0; k < words; k++) if (rb[s * words + k] != slab_word(s, me, round + 50, k)) bad++;
        }
        xfree(c, sb, ssym); xfree(c, rb, rsym);
        comm_barrier(c);
        if (getenv("T_COMM_TRACE")) fprintf(stderr, "t_comm: rank %d: alltoall round %d (%zu B%s): %d bad so far\n", me, round, bytes, round >= 5 ? ", symmetric" : "", bad);
    }
    size_t ag_sizes[] = { 1, 8, 4096, 4097, 1 << 16, 3 << 20, 8, 3 << 20 };   /* the last two on symmetric buffers (S12) */
    for (int round = 0; round < 8; round++) {
        size_t bytes = ag_sizes[round], words = (bytes + 7) / 8, alloc = words * 8; int sym = round >= 6;
        int ssym = sym, rsym = sym; uint64_t *sb = xalloc(c, alloc, &ssym), *rb = xalloc(c, alloc * n, &rsym);
        for (size_t k = 0; k < words; k++) sb[k] = slab_word(me, 99, round, k);
        if (round & 1) comm_allgather_host(c, sb, rb, alloc); else comm_allgather(c, sb, rb, alloc);
        for (int r = 0; r < n; r++) for (size_t k = 0; k < words; k++) if (rb[r * words + k] != slab_word(r, 99, round, k)) bad++;
        /* in place: my block already at its slot */
        memcpy(rb + me * words, sb, alloc); memset(sb, 0, alloc);
        comm_allgather_host(c, rb + me * words, rb, alloc);
        for (int r = 0; r < n; r++) for (size_t k = 0; k < words; k++) if (rb[r * words + k] != slab_word(r, 99, round, k)) bad++;
        xfree(c, sb, ssym); xfree(c, rb, rsym);
        if (getenv("T_COMM_TRACE")) fprintf(stderr, "t_comm: rank %d: allgather round %d (%zu B): %d bad so far\n", me, round, bytes, bad);
    }
    bad += check_alltoallv(c, me, n);
    if (getenv("T_COMM_TRACE")) fprintf(stderr, "t_comm: rank %d: alltoallv: %d bad so far\n", me, bad);
    size_t mx = comm_allreduce_max(c, (size_t)(me * 7 + 3));
    if (mx != (size_t)((n - 1) * 7 + 3)) bad++;
    uint64_t q = 3923057487904769ULL, s = c->ops->allreduce_modq(c, (uint64_t)me + 1, q, 10), expect = 0, w = 1;
    for (int r = 0; r < n; r++) { expect = (expect + (uint64_t)((unsigned __int128)(r + 1) * w % q)) % q; w = (uint64_t)((unsigned __int128)w * 10 % q); }
    if (s != expect) bad++;
    if (getenv("T_COMM_TRACE")) fprintf(stderr, "t_comm: rank %d: reductions: %d bad so far\n", me, bad);
    bad += check_p2p(c, me, n);
    if (getenv("T_COMM_TRACE")) fprintf(stderr, "t_comm: rank %d: p2p: %d bad so far\n", me, bad);
    return bad;
}
/* ---- Phase 16 C (results/C16.md): the bandwidth / latency sweep over the SHMEM transport ---------------------------------
 * t_comm --bw  [threads (4)] [reps (5)] [max MiB (256)] [sym|host] [pairs]     slab sizes 64 KiB .. max MiB per destination
 * t_comm --lat [threads (1)] [reps (200)] [sym|host] [pairs]                   8 B .. 4 KiB
 * Each thread owns one communicator (its own SHMEM context, as ecalc's four APU threads do) and runs the equal all-to-all
 * (comm_alltoall + comm_wait) on buffers of the symmetric pool (sym: pool-resident, unstaged -- the pure put + signal path)
 * or malloc'd (host: staged through the pool by memcpy; the host-only build has no device).  `pairs` with n > 2 PEs makes
 * every PE exchange with the one PE n/2 away only (4 PEs per node on 2 nodes: four inter-node pairs, no intra-node traffic).
 * Printed per PE and size: the median time of one exchange per thread and the GB/s per thread received ((n - 1) x the slab),
 * and the aggregate over the threads (all started together: the node's rate through the PE's NIC(s)); --lat prints the
 * median us per message (the exchange's time / (n - 1)). */
#include <pthread.h>
#include <sys/time.h>
static double tnow(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec * 1e-6; }
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
struct sw_arg { comm *c; int t, n, reps, host; size_t bytes; double med, min, tot; pthread_barrier_t *bar; };
static void *sw_thread(void *a)
{
    struct sw_arg *w = (struct sw_arg *)a; comm *c = w->c; int n = w->n, reps = w->reps; size_t b = w->bytes, alloc = b * n;
    int ssym = !w->host, rsym = !w->host; char *sb = xalloc(c, alloc, &ssym), *rb = xalloc(c, alloc, &rsym);
    if (!w->host && (!ssym || !rsym)) { fprintf(stderr, "t_comm: the pool could not hold %zu B x 2 (raise COMM_SHMEM_POOL_MB)\n", alloc); exit(2); }
    memset(sb, 0x5A, alloc); memset(rb, 0, alloc);
    double *ts = malloc(reps * sizeof *ts);
    comm_alltoall(c, sb, rb, b, NULL); comm_wait(c);                         /* warm-up */
    comm_barrier(c);
    pthread_barrier_wait(w->bar);
    double t0 = tnow();
    for (int r = 0; r < reps; r++) { double x = tnow(); comm_alltoall(c, sb, rb, b, NULL); comm_wait(c); ts[r] = tnow() - x; }
    w->tot = tnow() - t0;
    qsort(ts, reps, sizeof *ts, cmp_d); w->med = ts[reps / 2]; w->min = ts[0];
    comm_barrier(c);
    free(ts); xfree(c, sb, ssym); xfree(c, rb, rsym);
    return 0;
}
static int run_sweep(int argc, char **argv)
{
    int lat = !strcmp(argv[1], "--lat"), T = lat ? 1 : 4, reps = lat ? 200 : 5, host = 0, pairs = 0; size_t maxmb = 256;
    int pos = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "sym")) host = 0; else if (!strcmp(argv[i], "host")) host = 1; else if (!strcmp(argv[i], "pairs")) pairs = 1;
        else { long v = atol(argv[i]); if (pos == 0) T = (int)v; else if (pos == 1) reps = (int)v; else maxmb = (size_t)v; pos++; }
    }
    if (T < 1 || T > 16 || reps < 1) { fprintf(stderr, "t_comm: bad sweep arguments\n"); return 2; }
    int n = comm_shmem_init(), me = comm_shmem_rank();
    comm *cs[16], *all = 0; int cn = n, cme = me;
    if (pairs && n > 2 && n % 2 == 0) all = comm_shmem_create_at(0, 1, n, 31);   /* the full set: a barrier before every timed loop, so the pairs' walls overlap */
    for (int t = 0; t < T; t++) {
        if (pairs && n > 2 && n % 2 == 0) { cs[t] = comm_shmem_create_at(me % (n / 2), n / 2, 2, t); cn = 2; cme = me / (n / 2); }
        else cs[t] = comm_shmem_create_at(0, 1, n, t);
    }
    size_t sizes[32]; int ns = 0;
    if (lat) { for (size_t b = 8; b <= 4096; b *= 8) sizes[ns++] = b; }
    else { for (size_t b = 64 << 10; b <= (maxmb << 20); b *= 4) sizes[ns++] = b; if (sizes[ns - 1] != (maxmb << 20)) sizes[ns++] = maxmb << 20; }
    const char *tag = lat ? "t_comm lat" : "t_comm bw";
    if (me == 0) printf("%s: %d PEs%s, %d thread(s) with a communicator each, %d reps, %s buffers\n", tag, n, pairs && cn == 2 && n > 2 ? " in pairs (pe, pe + n/2)" : "", T, reps, host ? "malloc'd (staged)" : "symmetric (pool-resident)");
    for (int s = 0; s < ns; s++) {
        pthread_barrier_t bar; pthread_barrier_init(&bar, 0, T);
        struct sw_arg w[16]; pthread_t th[16];
        if (all) comm_barrier(all);
        for (int t = 0; t < T; t++) { w[t].c = cs[t]; w[t].t = t; w[t].n = cn; w[t].reps = reps; w[t].host = host; w[t].bytes = sizes[s]; w[t].bar = &bar; pthread_create(&th[t], 0, sw_thread, &w[t]); }
        double tot = 0, medsum = 0; for (int t = 0; t < T; t++) { pthread_join(th[t], 0); if (w[t].tot > tot) tot = w[t].tot; medsum += w[t].med; }
        pthread_barrier_destroy(&bar);
        double recv = (double)sizes[s] * (cn - 1);                           /* bytes received per thread per exchange */
        if (lat) printf("%s: pe %d (rank %d of %d): %6zu B: median %.2f us per message (%.2f us per exchange, min %.2f), %d threads\n", tag, me, cme, cn, sizes[s], medsum / T / (cn - 1) * 1e6, medsum / T * 1e6, w[0].min * 1e6, T);
        else printf("%s: pe %d (rank %d of %d): %9zu B per slab: median %.3f ms per exchange (min %.3f), %.2f GB/s per thread, aggregate %.2f GB/s over %d threads (%.3f s wall)\n", tag, me, cme, cn, sizes[s],
                    medsum / T * 1e3, w[0].min * 1e3, recv / (medsum / T) / 1e9, recv * T * reps / tot / 1e9, T, tot);
        fflush(stdout);
    }
    for (int t = 0; t < T; t++) comm_destroy(cs[t]);
    if (all) comm_destroy(all);
    printf("%s: pe %d done\n", tag, me);
    comm_shmem_finalize();
    return 0;
}
int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "--bw") || !strcmp(argv[1], "--lat"))) {   /* Phase 16 C: the sweep (SHMEM only) */
        if (!(getenv("COMM_TRANSPORT") && !strcmp(getenv("COMM_TRANSPORT"), "shmem"))) { fprintf(stderr, "t_comm %s needs COMM_TRANSPORT=shmem\n", argv[1]); return 2; }
        return run_sweep(argc, argv);
    }
    if (getenv("COMM_TRANSPORT") && !strcmp(getenv("COMM_TRANSPORT"), "shmem")) {   /* S: one PE per process under oshrun (mnrun.sh) */
        shmem_mode = 1; int n = comm_shmem_init(), me = comm_shmem_rank();
        int bad = run_rank(me, n, 0, 0);
        printf("t_comm: PE %d of %d over SHMEM: %s (%d bad)\n", me, n, bad ? "VERIFY FAILED" : "VERIFY OK", bad);
        comm_shmem_finalize();
        return bad != 0;
    }
    if (getenv("COMM_RANK")) {
        int me = atoi(getenv("COMM_RANK")), n = atoi(getenv("COMM_SIZE")), port = getenv("COMM_PORT") ? atoi(getenv("COMM_PORT")) : 27000;
        int bad = run_rank(me, n, getenv("COMM_HOSTS"), port);
        printf("t_comm: rank %d of %d: %s (%d bad)\n", me, n, bad ? "VERIFY FAILED" : "VERIFY OK", bad);
        return bad != 0;
    }
    int n = argc > 1 ? atoi(argv[1]) : 4, port = argc > 2 ? atoi(argv[2]) : 27100;
    char hosts[4096] = ""; for (int r = 0; r < n; r++) strcat(hosts, r ? ",localhost" : "localhost");
    pid_t pid[64];
    for (int r = 0; r < n; r++) { pid[r] = fork(); if (pid[r] == 0) _exit(run_rank(r, n, hosts, port) ? 1 : 0); }
    int fails = 0;
    for (int r = 0; r < n; r++) { int st; waitpid(pid[r], &st, 0); if (!WIFEXITED(st) || WEXITSTATUS(st)) fails++; }
    printf("t_comm: %s (%d ranks, %d failed)\n", fails ? "VERIFY FAILED" : "VERIFY OK", n, fails);
    return fails != 0;
}

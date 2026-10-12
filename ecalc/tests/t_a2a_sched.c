/* t_a2a_sched.c -- C02 (results/C02.md): a stand-alone OpenSHMEM all-to-all probe that compares PEER SCHEDULES on the real
 * fabric without touching the engine's transport (comm_shmem.c / comm_ofi.c are untouched; this is test code only).
 *
 * Launch: srun -N<nodes> --ntasks-per-node=4 ... ./t_a2a_sched [options]   (Cray OpenSHMEMX on aac7: one NIC per PE)
 * PE p sits on node p / 4 with local index p % 4.  A MESH is the set of PEs with the same local index (ecalc's layered inter
 * communicators: APU d of every node), g = nodes members; -g <k> splits the nodes into groups of k consecutive nodes
 * (-g 2 on 4 nodes: two 2-node meshes per local index).  Every PE exchanges a slab of S bytes with each of its g - 1 peers
 * (the equal all-to-all), posted as chunks of c bytes with shmem_putmem_signal_nbi (SHMEM_SIGNAL_ADD: the receiver's word
 * of this sender counts the bytes delivered; no fence needed), then shmem_quiet; the receiver waits for every sender's word
 * to reach the exchange's cumulative byte count.  Digits are not involved; the data is verified once at the end.
 *
 * Schedules (-o list, comma separated; the order in which the chunks are posted to the NIC's queue):
 *   seq   peers in index order, all chunks of a peer before the next      (comm_shmem.c today: push_peers r = 0 .. n-1)
 *   rot   the same, starting at me + 1 (X1 rot; the transport form was removed after X1 was rejected)
 *   rr    rotated start, one chunk per peer round robin (every receiver sees every sender at once: no moving incast)
 *   winK  rotated start, at most K peers open at a time, round robin among the open ones (K = 1 is rot, K = g - 1 is rr)
 *   pair  the pairwise rounds in lockstep: round i sends to me + 1 + i and waits for the data from me - 1 - i (+ quiet)
 *         before round i + 1 (the upper bound of what a synchronised pairwise schedule costs)
 * Options: -s <slab MiB per peer> (64)  -c <chunk MiB> (4)  -r <reps> (10)  -g <nodes per mesh> (all)  -o <orders> (seq,rot,rr,win2,pair)
 *          -S <slab list MiB, comma separated> (overrides -s)
 * Output, one line per PE, size and order: median / min ms per exchange and GB/s received per PE; the item's awk aggregates
 * (results/C02.md "Microbenchmark").  Heap: SHMEM_SYMMETRIC_SIZE >= 2 g S + 1 MiB. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <shmem.h>

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static int me, npes, loc, node, nodes, g, g0, mrank;    /* mesh: nodes [g0, g0 + g) at local index loc; my rank in it */
static int pe_of(int r) { return (g0 + r) * 4 + loc; }
static size_t S, C;                                     /* slab per peer, chunk */
static char *sb, *rb; static long *sig;                 /* symmetric: send slabs [g][S], receive slabs [g][S], sig[g] (bytes received from rank r, cumulative) */
static long cum;                                        /* bytes expected from each peer so far */

static unsigned char pat(int s, int r, size_t i) { return (unsigned char)(s * 31 + r * 7 + (int)(i & 0xff)); }
static void post_chunk(int r, size_t off)
{
    size_t len = S - off < C ? S - off : C;
    shmem_putmem_signal_nbi(rb + (size_t)mrank * S + off, sb + (size_t)r * S + off, len, (uint64_t *)&sig[mrank], (uint64_t)len, SHMEM_SIGNAL_ADD, pe_of(r));
}
static int rot(int i) { return (mrank + 1 + i) % g; }   /* i = 0 .. g - 2 */
static void wait_all(void) { for (int r = 0; r < g; r++) if (r != mrank) shmem_long_wait_until(&sig[r], SHMEM_CMP_GE, cum); }

/* one exchange under schedule `o` (window w for winK); returns the wall from the first post to all data in and my puts quiet */
static double exchange(const char *o, int w)
{
    size_t nch = (S + C - 1) / C; int n1 = g - 1;
    cum += (long)S;
    double t0 = now_s();
    if (!strcmp(o, "seq")) { for (int r = 0; r < g; r++) if (r != mrank) for (size_t j = 0; j < nch; j++) post_chunk(r, j * C); }
    else if (!strcmp(o, "rot")) { for (int i = 0; i < n1; i++) for (size_t j = 0; j < nch; j++) post_chunk(rot(i), j * C); }
    else if (!strcmp(o, "rr")) { for (size_t j = 0; j < nch; j++) for (int i = 0; i < n1; i++) post_chunk(rot(i), j * C); }
    else if (!strncmp(o, "win", 3)) {                       /* at most w peers open: the next chunk of the open peer with the least progress */
        size_t *done = (size_t *)calloc((size_t)n1, sizeof *done); int next = 0, open[64], nopen = 0;
        if (w < 1) w = 1; if (w > n1) w = n1; if (w > 64) w = 64;
        while (nopen < w && next < n1) open[nopen++] = next++;
        while (nopen) {
            int b = 0; for (int k = 1; k < nopen; k++) if (done[open[k]] < done[open[b]]) b = k;
            int i = open[b]; post_chunk(rot(i), done[i]); done[i] += C;
            if (done[i] >= S) { open[b] = open[--nopen]; if (next < n1) open[nopen++] = next++; }
        }
        free(done);
    }
    else if (!strcmp(o, "pair")) {                          /* lockstep rounds: send to rot(i), receive from the PE whose rot(i) is me, complete, next */
        for (int i = 0; i < n1; i++) {
            int r = rot(i), from = (mrank - 1 - i + 2 * g) % g;
            for (size_t j = 0; j < nch; j++) post_chunk(r, j * C);
            shmem_long_wait_until(&sig[from], SHMEM_CMP_GE, cum);
            shmem_quiet();
        }
        return now_s() - t0;
    }
    else { if (me == 0) fprintf(stderr, "t_a2a_sched: unknown order %s\n", o); exit(2); }
    wait_all();
    shmem_quiet();
    return now_s() - t0;
}

int main(int argc, char **argv)
{
    size_t slab_mb = 64, chunk_mb = 4; int reps = 10, gper = 0; const char *orders = "seq,rot,rr,win2,pair"; const char *slabs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) slab_mb = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) chunk_mb = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gper = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) orders = argv[++i];
        else if (!strcmp(argv[i], "-S") && i + 1 < argc) slabs = argv[++i];
        else { fprintf(stderr, "usage: t_a2a_sched [-s slabMiB] [-S list] [-c chunkMiB] [-r reps] [-g nodes_per_mesh] [-o seq,rot,rr,winK,pair]\n"); return 2; }
    }
    shmem_init();
    me = shmem_my_pe(); npes = shmem_n_pes();
    if (npes % 4) { if (me == 0) fprintf(stderr, "t_a2a_sched: launch 4 PEs per node (%d PEs)\n", npes); shmem_finalize(); return 2; }
    loc = me % 4; node = me / 4; nodes = npes / 4;
    g = gper > 0 ? gper : nodes; if (nodes % g) { if (me == 0) fprintf(stderr, "t_a2a_sched: -g %d does not divide %d nodes\n", g, nodes); shmem_finalize(); return 2; }
    g0 = (node / g) * g; mrank = node - g0;
    size_t sl[16]; int nsl = 0;
    if (slabs) { char *t = strdup(slabs); for (char *p = strtok(t, ","); p && nsl < 16; p = strtok(0, ",")) sl[nsl++] = (size_t)atol(p); free(t); } else sl[nsl++] = slab_mb;
    size_t smax = 0; for (int i = 0; i < nsl; i++) if (sl[i] > smax) smax = sl[i];
    size_t Smax = smax << 20; C = chunk_mb << 20; if (C < 65536) C = 65536;
    sb = (char *)shmem_malloc((size_t)g * Smax); rb = (char *)shmem_malloc((size_t)g * Smax); sig = (long *)shmem_malloc((size_t)g * sizeof(long));
    if (!sb || !rb || !sig) { fprintf(stderr, "t_a2a_sched: pe %d: shmem_malloc failed (raise SHMEM_SYMMETRIC_SIZE: need > %zu MiB)\n", me, (2 * (size_t)g * Smax >> 20) + 1); shmem_global_exit(2); }
    for (int r = 0; r < g; r++) for (size_t i = 0; i < Smax; i++) sb[(size_t)r * Smax + i] = (char)pat(mrank, r, i);
    memset(rb, 0, (size_t)g * Smax); for (int r = 0; r < g; r++) sig[r] = 0; cum = 0;
    char hn[64] = "?"; FILE *hf = fopen("/proc/sys/kernel/hostname", "r"); if (hf) { if (fscanf(hf, "%63s", hn) != 1) strcpy(hn, "?"); fclose(hf); }
    if (me == 0) printf("t_a2a_sched: %d PEs on %d nodes, meshes of g = %d (%d peers), chunk %zu MiB, %d reps, orders %s\n", npes, nodes, g, g - 1, C >> 20, reps, orders);
    shmem_barrier_all();
    char *ol = strdup(orders); double *ts = (double *)malloc((size_t)reps * sizeof *ts);
    for (int si = 0; si < nsl; si++) {
        S = sl[si] << 20;
        for (char *o = strtok(si == 0 ? ol : strcpy(ol, orders), ","); o; o = strtok(0, ",")) {
            int w = !strncmp(o, "win", 3) ? atoi(o + 3) : 0;
            exchange(o, w); shmem_barrier_all();                                         /* warm-up, then everyone starts together */
            double t0 = now_s();
            for (int k = 0; k < reps; k++) { ts[k] = exchange(o, w); }
            double tot = now_s() - t0;
            qsort(ts, (size_t)reps, sizeof *ts, cmp_d);
            double recv = (double)S * (g - 1);
            printf("t_a2a_sched: pe %d node %d (%s) loc %d mesh rank %d of %d: slab %5zu MiB order %-5s: median %8.3f ms (min %8.3f) %6.2f GB/s per PE, loop %6.2f GB/s\n",
                   me, node, hn, loc, mrank, g, S >> 20, o, ts[reps / 2] * 1e3, ts[0] * 1e3, recv / ts[reps / 2] * 1e-9, recv * reps / tot * 1e-9);
            fflush(stdout);
            shmem_barrier_all();
        }
    }
    /* verify the last exchange's data (the slab size of the last pass) */
    long bad = 0; for (int r = 0; r < g; r++) if (r != mrank) for (size_t i = 0; i < S; i++) if ((unsigned char)rb[(size_t)r * S + i] != pat(r, mrank, i)) bad++;
    long badall = bad;
    long *src = (long *)shmem_malloc(sizeof(long)), *dst = (long *)shmem_malloc(sizeof(long)); *src = bad; shmem_barrier_all();
    shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dst, src, 1); badall = *dst;
    if (me == 0) printf("t_a2a_sched: VERIFY %s (%ld bad bytes over all PEs)\n", badall ? "FAILED" : "OK", badall);
    if (bad) fprintf(stderr, "t_a2a_sched: pe %d: %ld bad bytes\n", me, bad);
    shmem_barrier_all();
    shmem_free(sb); shmem_free(rb); shmem_free(sig); shmem_free(src); shmem_free(dst); free(ol); free(ts);
    shmem_finalize();
    return badall ? 1 : 0;
}

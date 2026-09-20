/* t_comm - the TCP communicator (WP6) on N forked localhost processes:
 * all-to-all of random slabs checked against the senders, barrier, reductions, (M7) the all-gathers
 * (host blocks below and above the no-thread threshold; the "device" op, which is the host one in this build), and
 * (B7) the unequal all-to-all: per-pair counts of 0..8 units (units of 8 B, 1000 B and 3 MiB), slabs back to back
 * on the send side and in reverse rank order on the receive side, the device and the host op.
 * Host-only build: cc -O2 -DCOMM_HOST_ONLY -I.. t_comm.c ../comm_tcp.c -lpthread
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
static int check_alltoallv(comm *c, int me, int n)
{
    size_t units[] = { 8, 1000, 3 << 20 }; int bad = 0;
    for (int round = 0; round < 6; round++) {
        size_t u = units[round % 3];
        size_t *scnt = malloc(n * sizeof *scnt), *sdsp = malloc(n * sizeof *sdsp), *rcnt = malloc(n * sizeof *rcnt), *rdsp = malloc(n * sizeof *rdsp);
        for (int r = 0; r < n; r++) { scnt[r] = vunits(me, r, round) * u; rcnt[r] = vunits(r, me, round) * u; }
        size_t ts = comm_prefix(scnt, sdsp, n), tr = 0;
        for (int r = n - 1; r >= 0; r--) { rdsp[r] = tr; tr += rcnt[r]; }          /* reverse rank order on the receive side */
        char *sb = malloc(ts + 8), *rb = malloc(tr + 8); memset(rb, 0xEE, tr + 8);
        for (int r = 0; r < n; r++) for (size_t k = 0; k < scnt[r]; k++) sb[sdsp[r] + k] = (char)slab_word(me, r, 100 + round, k / 8);
        if (round < 3) { comm_alltoallv(c, sb, scnt, sdsp, rb, rcnt, rdsp, NULL); comm_wait(c); }
        else comm_alltoallv_host(c, sb, scnt, sdsp, rb, rcnt, rdsp);
        for (int r = 0; r < n; r++) for (size_t k = 0; k < rcnt[r]; k++) if (rb[rdsp[r] + k] != (char)slab_word(r, me, 100 + round, k / 8)) bad++;
        free(sb); free(rb); free(scnt); free(sdsp); free(rcnt); free(rdsp);
        comm_barrier(c);
    }
    return bad;
}
static int run_rank(int me, int n, const char *hosts, int port)
{
    char b[32]; snprintf(b, sizeof b, "%d", me); setenv("COMM_RANK", b, 1);
    snprintf(b, sizeof b, "%d", n); setenv("COMM_SIZE", b, 1); setenv("COMM_HOSTS", hosts, 1);
    snprintf(b, sizeof b, "%d", port); setenv("COMM_PORT", b, 1);
    comm *c = comm_tcp_create();
    int bad = 0;
    size_t sizes[] = { 1, 8, 1000, 1 << 16, 3 << 20 };          /* bytes per slab, incl. one larger than the socket buffers */
    for (int round = 0; round < 5; round++) {
        size_t bytes = sizes[round], words = bytes / 8 ? bytes / 8 : 1, alloc = words * 8;
        uint64_t *sb = malloc(alloc * n), *rb = malloc(alloc * n);
        for (int s = 0; s < n; s++) for (size_t k = 0; k < words; k++) sb[s * words + k] = slab_word(me, s, round, k);
        comm_alltoall(c, sb, rb, alloc, NULL); comm_wait(c);
        for (int s = 0; s < n; s++) for (size_t k = 0; k < words; k++) if (rb[s * words + k] != slab_word(s, me, round, k)) bad++;
        free(sb); free(rb);
        comm_barrier(c);
    }
    size_t ag_sizes[] = { 1, 8, 4096, 4097, 1 << 16, 3 << 20 };
    for (int round = 0; round < 6; round++) {
        size_t bytes = ag_sizes[round], words = (bytes + 7) / 8, alloc = words * 8;
        uint64_t *sb = malloc(alloc), *rb = malloc(alloc * n);
        for (size_t k = 0; k < words; k++) sb[k] = slab_word(me, 99, round, k);
        if (round & 1) comm_allgather_host(c, sb, rb, alloc); else comm_allgather(c, sb, rb, alloc);
        for (int r = 0; r < n; r++) for (size_t k = 0; k < words; k++) if (rb[r * words + k] != slab_word(r, 99, round, k)) bad++;
        /* in place: my block already at its slot */
        memcpy(rb + me * words, sb, alloc); memset(sb, 0, alloc);
        comm_allgather_host(c, rb + me * words, rb, alloc);
        for (int r = 0; r < n; r++) for (size_t k = 0; k < words; k++) if (rb[r * words + k] != slab_word(r, 99, round, k)) bad++;
        free(sb); free(rb);
    }
    bad += check_alltoallv(c, me, n);
    size_t mx = comm_allreduce_max(c, (size_t)(me * 7 + 3));
    if (mx != (size_t)((n - 1) * 7 + 3)) bad++;
    uint64_t q = 3923057487904769ULL, s = c->ops->allreduce_modq(c, (uint64_t)me + 1, q, 10), expect = 0, w = 1;
    for (int r = 0; r < n; r++) { expect = (expect + (uint64_t)((unsigned __int128)(r + 1) * w % q)) % q; w = (uint64_t)((unsigned __int128)w * 10 % q); }
    if (s != expect) bad++;
    comm_destroy(c);
    return bad;
}
int main(int argc, char **argv)
{
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

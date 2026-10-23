/* t_out - the per-node output pieces (mn_out.c) on the host: random X in base 10^18, d_out up to 3000, the
 * string split over 1..5 "nodes" (comm_shard) and formatted in chunks of 1..9 limbs: every part equals its range of the
 * whole-string file, cat of the parts equals it, the digit residues joined over the chunks and nodes equal the whole
 * string's, 12 random 50-digit windows (ECALC_WINDOWS) are each checked exactly once across chunk and node boundaries;
 * the T1 term recurrence over ranges joined in node order equals the whole range's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include "../vpe.h"
#include "../mn_out.h"
#include "harness.h"
static uint64_t rnd(void) { static uint64_t s = 0x9E3779B97F4A7C15ull; s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
/* --ppg: the PEs write their parts at once, one PROCESS per PE (the thread arm, one thread per PE, was removed 2026-10-23 with the thread mode): the
 * ranks first run in turn for their ranges and heads, then again concurrently; every part equals its range of the reference file, the
 * digit residues join to the whole string's */
struct vjob { mn_out o; const uint64_t *src; size_t lo, n; };
static void *vjob_run(void *a) { struct vjob *j = (struct vjob *)a; mn_out_src src = { j->src + j->lo, 0, j->lo, j->n }; mn_out_run(&j->o, &src); mn_out_finish(&j->o); return 0; }
/* (fork, vpe_set_ppg(local): vpe_idx() is the process's, every thread of it is that PE): each process writes its part
 * and its T2 window table is its own (read once per process); the results come back through a pipe.  Host only. */
struct ppg_res { size_t ndig, k0, k1; int nwin, bad2; uint64_t dres[T1_NQ]; };
static void t_vpe_parts(int *fails, int *checks)
{
    for (int it = 0; it < 12; it++) {
        unsigned long d_out = 200 + rnd() % 3000, d = ((d_out + 17) / 18) * 18;
        size_t nl = (d + 1 + 17) / 18; uint64_t *X = (uint64_t *)malloc(nl * 8);
        for (size_t i = 0; i < nl; i++) X[i] = rnd() % 1000000000000000000ULL;
        X[nl - 1] = 2;
        char *ref = (char *)malloc(d + 2); digits_format(ref, X, nl, d);
        char fn[64]; snprintf(fn, sizeof fn, "/tmp/t_out_%d.vref", getpid());
        FILE *f = fopen(fn, "w"); fputc(ref[0], f); fputc('.', f); fwrite(ref + 1, 1, d_out, f); fputc('\n', f); fclose(f);
        char wf[64]; snprintf(wf, sizeof wf, "/tmp/t_out_%d.vwin", getpid()); FILE *w = fopen(wf, "w");
        for (int k = 0; k < 12; k++) { unsigned long o = 1 + rnd() % (d_out + 1 - 50); fprintf(w, "%lu %.50s\n", o, ref + o); }
        fclose(w); setenv("ECALC_WINDOWS", wf, 1);
        uint64_t Dref[T1_NQ]; vf_digits_mods(ref, d + 1, t1_q, T1_NQ, Dref);
        int size = 2 + (int)(rnd() % 4); size_t L = 1 + rnd() % 9;
        char out[64]; snprintf(out, sizeof out, "/tmp/t_out_%d.vout", getpid());
        struct vjob job[8]; size_t k0s[8]; char cmd[256];
        /* pass 0: in turn from the top: the ranges (k0) */
        tier2_windows_reset(); { char tail[64]; size_t ntail = 0;
            for (int r = size - 1; r >= 0; r--) {
                struct vjob *j = &job[r]; size_t lo, hi; comm_shard(nl, r, size, &lo, &hi); memset(j, 0, sizeof *j);
                j->o.d = d; j->o.d_out = d_out; j->o.outfile = out; j->o.rank = r; j->o.size = size; j->o.chunk_limbs = L; j->src = X; j->lo = lo; j->n = hi - lo;
                memcpy(j->o.head, tail, ntail); j->o.nhead = ntail; vjob_run(j); k0s[r] = j->o.k0;
                if (j->o.k1 > j->o.k0) { size_t n = j->o.k1 - j->o.k0, t = n < 49 ? n : 49; size_t keep = 49 - t < ntail ? 49 - t : ntail; memmove(tail, tail + ntail - keep, keep); memcpy(tail + keep, ref + j->o.k1 - t, t); ntail = keep + t; }
            } }
        snprintf(cmd, sizeof cmd, "rm -f %s.part*", out); system(cmd);
        /* pass 1: all ranks at once, heads from the reference */
        tier2_windows_reset();
        int pfd[8][2]; pid_t pid[8];
        fflush(stdout);
        for (int r = 0; r < size; r++) {
            struct vjob *j = &job[r]; size_t lo, hi; comm_shard(nl, r, size, &lo, &hi); memset(j, 0, sizeof *j);
            j->o.d = d; j->o.d_out = d_out; j->o.outfile = out; j->o.rank = r; j->o.size = size; j->o.chunk_limbs = L; j->src = X; j->lo = lo; j->n = hi - lo;
            size_t t = k0s[r] < 49 ? k0s[r] : 49; memcpy(j->o.head, ref + k0s[r] - t, t); j->o.nhead = t;
            if (pipe(pfd[r])) { perror("pipe"); exit(2); }
            if ((pid[r] = fork()) == 0) {                 /* the PE's process */
                close(pfd[r][0]); vpe_set_ppg(r % 4); tier2_windows_reset(); vjob_run(j);
                struct ppg_res rs; memset(&rs, 0, sizeof rs); rs.ndig = j->o.ndig; rs.k0 = j->o.k0; rs.k1 = j->o.k1; rs.nwin = j->o.nwin; rs.bad2 = j->o.bad2; memcpy(rs.dres, j->o.dres, sizeof rs.dres);
                if (write(pfd[r][1], &rs, sizeof rs) != (ssize_t)sizeof rs) _exit(3);
                _exit(0);
            }
            close(pfd[r][1]);
        }
        for (int r = 0; r < size; r++) {
            struct ppg_res rs; int st = 0; ssize_t g = read(pfd[r][0], &rs, sizeof rs); close(pfd[r][0]); waitpid(pid[r], &st, 0);
            (*checks)++; if (g != (ssize_t)sizeof rs || !WIFEXITED(st) || WEXITSTATUS(st)) { (*fails)++; printf("ppg it %d size %d: process of PE %d failed (status %d)\n", it, size, r, st); memset(&rs, 0, sizeof rs); }
            struct vjob *j = &job[r]; j->o.ndig = rs.ndig; j->o.k0 = rs.k0; j->o.k1 = rs.k1; j->o.nwin = rs.nwin; j->o.bad2 = rs.bad2; memcpy(j->o.dres, rs.dres, sizeof rs.dres);
        }
        uint64_t D[T1_NQ] = {0}; int nwin = 0, bad2 = 0; size_t nd = 0;
        for (int r = size - 1; r >= 0; r--) {
            mn_out *o = &job[r].o; nwin += o->nwin; bad2 += o->bad2; nd += o->ndig;
            for (int i = 0; i < T1_NQ; i++) D[i] = vf_digits_join(D[i], o->ndig, o->dres[i], t1_q[i]);
        }
        (*checks)++; if (nd != d + 1 || memcmp(D, Dref, sizeof D)) { (*fails)++; printf("vpe it %d size %d: digit residue / count mismatch\n", it, size); }
        (*checks)++; if (nwin != 13 || bad2 != 1) { (*fails)++; printf("vpe it %d size %d: %d windows (13 = 12 + the built-in), %d bad (1)\n", it, size, nwin, bad2); }
        snprintf(cmd, sizeof cmd, "cat %s.part* | cmp -s - %s", out, fn);
        (*checks)++; if (system(cmd)) { (*fails)++; printf("vpe it %d size %d: cat of the parts differs (d_out %lu)\n", it, size, d_out); }
        snprintf(cmd, sizeof cmd, "rm -f %s.part*", out); system(cmd);
        free(X); free(ref); unlink(wf); unlink(fn);
    }
}
int main(int argc, char **argv)
{
    printf("== t_out ==\n"); harness_meta("t_out");
    setenv("ECALC_OUT_PACKED", "0", 0);   /* this test checks the ASCII part files byte by byte; the packed form (the default) is checked by mnaccept's e9 / mn / full steps (digcmp.sh) and tools/test_unpack.py */
    int fails = 0, checks = 0;
    if (argc > 1 && !strcmp(argv[1], "--ppg")) { t_vpe_parts(&fails, &checks); printf("%d checks, %d failures\n", checks, fails); VERIFY(fails == 0, "t_out %s: %d of %d checks failed", argv[1], fails, checks); return verify_done("t_out"); }
    for (int it = 0; it < 60; it++) {
        unsigned long d_out = 1 + rnd() % 3000, d = ((d_out + 17) / 18) * 18;
        size_t nl = (d + 1 + 17) / 18; uint64_t *X = (uint64_t *)malloc(nl * 8);
        for (size_t i = 0; i < nl; i++) X[i] = rnd() % 1000000000000000000ULL;
        X[nl - 1] = 2; size_t xn = nl;                                   /* the top limb is the "2" (pad = 17) */
        if (it % 5 == 4) { X[nl - 1] = 0; xn = nl - 1; }              /* X shorter than nl (zero top limb) */
        char *ref = (char *)malloc(d + 2); digits_format(ref, X, xn, d);
        /* the reference file */
        char fn[64]; snprintf(fn, sizeof fn, "/tmp/t_out_%d.ref", getpid());
        FILE *f = fopen(fn, "w"); fputc(ref[0], f); fputc('.', f); fwrite(ref + 1, 1, d_out, f); fputc('\n', f); fclose(f);
        /* windows: a few at random offsets, written to a file for ECALC_WINDOWS */
        char wf[64]; snprintf(wf, sizeof wf, "/tmp/t_out_%d.win", getpid()); FILE *w = fopen(wf, "w"); int nw = 0;
        for (int k = 0; k < 12 && d_out + 1 > 60; k++) { unsigned long o = 1 + rnd() % (d_out + 1 - 50); fprintf(w, "%lu %.50s\n", o, ref + o); nw++; }
        fclose(w); setenv("ECALC_WINDOWS", wf, 1);
        /* windows cover offsets up to d_out + 1 - 50; count the expected checks */
        uint64_t Dref[T1_NQ]; vf_digits_mods(ref, d + 1, t1_q, T1_NQ, Dref);
        for (int size = 1; size <= 5; size++) { tier2_windows_reset();
            size_t L = 1 + rnd() % 9;                                    /* tiny chunks: many boundaries */
            uint64_t D[T1_NQ] = {0}; int nwin = 0, bad2 = 0; size_t ndig_tot = 0;
            char out[64]; snprintf(out, sizeof out, "/tmp/t_out_%d.out", getpid());
            /* nodes from the top down so the head (previous node's tail) is at hand */
            char tail[64]; size_t ntail = 0;
            for (int r = size - 1; r >= 0; r--) {
                size_t lo, hi; comm_shard(xn, r, size, &lo, &hi);
                mn_out o; memset(&o, 0, sizeof o); o.d = d; o.d_out = d_out; o.outfile = out; o.rank = r; o.size = size; o.chunk_limbs = L;
                memcpy(o.head, tail, ntail); o.nhead = ntail;
                mn_out_src src = { X + lo, 0, lo, hi - lo };
                mn_out_run(&o, &src); mn_out_finish(&o);
                nwin += o.nwin; bad2 += o.bad2; ndig_tot += o.ndig;
                /* the residue join, top node first: D = D 10^ndig + dres */
                for (int i = 0; i < T1_NQ; i++) D[i] = vf_digits_join(D[i], o.ndig, o.dres[i], t1_q[i]);
                /* the head for the node below = the last 49 of (tail + this node's digits) */
                if (o.k1 > o.k0) { size_t n = o.k1 - o.k0, t = n < 49 ? n : 49; size_t keep = 49 - t < ntail ? 49 - t : ntail; memmove(tail, tail + ntail - keep, keep); memcpy(tail + keep, ref + o.k1 - t, t); ntail = keep + t; }
                /* the node's part vs the reference file range */
                char pn[96]; if (size > 1) snprintf(pn, sizeof pn, "%s.part%04d", out, size - 1 - r); else snprintf(pn, sizeof pn, "%s", out);
                FILE *pf = fopen(pn, "r"); fseek(pf, 0, SEEK_END); long pl = ftell(pf); fseek(pf, 0, SEEK_SET);
                char *pb = (char *)malloc(pl + 1); fread(pb, 1, pl, pf); fclose(pf);
                size_t fb = o.k0 == 0 ? 0 : o.k0 + 1, fe = (o.k1 < d_out + 1 ? o.k1 : d_out + 1) + 1 + (o.k0 <= d_out && d_out < o.k1 ? 1 : 0);
                if (o.k0 >= d_out + 1) fe = fb;
                FILE *rf = fopen(fn, "r"); fseek(rf, 0, SEEK_END); long rl = ftell(rf); fseek(rf, 0, SEEK_SET); char *rb = (char *)malloc(rl + 1); fread(rb, 1, rl, rf); fclose(rf);
                checks++;
                if ((size_t)pl != fe - fb || memcmp(pb, rb + fb, pl)) { fails++; if (size == 1) { int q = 0; while (q < pl && pb[q] == rb[fb + q]) q++; printf("first diff at %d: part %.40s ref %.40s\n", q, pb + (q > 10 ? q - 10 : 0), rb + fb + (q > 10 ? q - 10 : 0)); } printf("it %d size %d node %d: part %ld bytes vs [%zu, %zu) of the file (d_out %lu, k [%zu, %zu), L %zu)\n", it, size, r, pl, fb, fe, d_out, o.k0, o.k1, L); }
                free(pb); free(rb);
                if (o.k1 > o.k0 && r == size - 1) { checks++; if (strncmp(o.first, ref, strlen(o.first))) { fails++; printf("it %d: first mismatch\n", it); } }
                if (o.k0 <= d_out && d_out < o.k1) { checks++; size_t t = strlen(o.last); if (memcmp(o.last, ref + d_out + 1 - t, t)) { fails++; printf("it %d: last mismatch\n", it); } }
            }
            checks++; if (ndig_tot != d + 1) { fails++; printf("it %d size %d: ndig %zu vs %lu\n", it, size, ndig_tot, d + 1); }
            checks++; if (memcmp(D, Dref, sizeof D)) { fails++; printf("it %d size %d: digit residue mismatch\n", it, size); }
            /* every window checked exactly once, none bad; expected count: the file's windows with o + 50 <= d_out + 1, plus the built-in @50 if it fits */
            int exp = nw + (50 + 50 <= d_out + 1 ? 1 : 0);
            int expbad = 50 + 50 <= d_out + 1 ? 1 : 0;   /* the built-in window at 50 is not e here */
            checks++; if (nwin != exp || bad2 != expbad) { fails++; printf("it %d size %d: %d windows checked (expected %d), %d bad (d_out %lu, L %zu)\n", it, size, nwin, exp, bad2, d_out, L); }
            /* cat of the parts == the file */
            if (size > 1) {
                char cmd[512]; snprintf(cmd, sizeof cmd, "cat %s.part* | cmp -s - %s", out, fn);
                checks++; if (system(cmd)) { fails++; printf("it %d size %d: cat of the parts differs (d_out %lu)\n", it, size, d_out); }
                snprintf(cmd, sizeof cmd, "rm -f %s.part*", out); system(cmd);
            } else { char cmd[256]; snprintf(cmd, sizeof cmd, "cmp -s %s %s", out, fn); checks++; if (system(cmd)) { fails++; printf("it %d size 1: file differs\n", it); } }
        }
        unlink(fn); unlink(wf); free(X); free(ref); tier2_windows_reset();
    }
    /* the term recurrence over ranges joined in order == over [1, N+1) */
    for (int it = 0; it < 20; it++) {
        unsigned long N = 1 + rnd() % 200000; int size = 1 + rnd() % 6;
        for (int i = 0; i < T1_NQ; i++) {
            uint64_t P, Q, Pj = 0, Qj = 1; vf_pq_mod(N, t1_q[i], &P, &Q);
            for (int r = 0; r < size; r++) { unsigned long a = 1 + (unsigned long)((unsigned __int128)N * r / size), b = 1 + (unsigned long)((unsigned __int128)N * (r + 1) / size); uint64_t p, q; vf_pq_range_mod(a, b, t1_q[i], &p, &q); if (r == 0) { Pj = p; Qj = q; } else vf_pq_join(&Pj, &Qj, p, q, t1_q[i]); }
            checks++; if (P != Pj || Q != Qj) { fails++; printf("pq range join: N %lu size %d prime %d differs\n", N, size, i); }
        }
    }
    printf("%d checks, %d failures\n", checks, fails);
    VERIFY(fails == 0, "t_out: %d of %d checks failed", fails, checks);
    return verify_done("t_out");
}

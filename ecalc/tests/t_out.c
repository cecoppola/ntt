/* t_out - the per-node output pieces (Phase 9 A-out, mn_out.c) on the host: random X in base 10^18, d_out up to 3000, the
 * string split over 1..5 "nodes" (comm_shard) and formatted in chunks of 1..9 limbs: every part equals its range of the
 * whole-string file, cat of the parts equals it, the digit residues joined over the chunks and nodes equal the whole
 * string's, 12 random 50-digit windows (ECALC_WINDOWS) are each checked exactly once across chunk and node boundaries;
 * the T1 term recurrence over ranges joined in node order equals the whole range's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include "../mn_out.h"
#include "harness.h"
static void digits_format(char *digits, const uint64_t *l, size_t n, unsigned long d)
{
    size_t nl = (d + 1 + 17) / 18, pad = nl * 18 - (d + 1);
    for (size_t i = 0; i < nl; i++) {
        char buf[19]; uint64_t v = i < n ? l[i] : 0;
        snprintf(buf, sizeof buf, "%018llu", (unsigned long long)v);
        size_t pos = (nl - 1 - i) * 18;
        for (int c = 0; c < 18; c++) if (pos + c >= pad) digits[pos + c - pad] = buf[c];
    }
    digits[d + 1] = 0;
}
static uint64_t rnd(void) { static uint64_t s = 0x9E3779B97F4A7C15ull; s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
int main(void)
{
    printf("== t_out ==\n"); harness_meta("t_out");
    bi_set_decimal(1);
    int fails = 0, checks = 0;
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
                if (r == 0 && o.k1 > o.k0) { checks++; size_t t = strlen(o.last); if (memcmp(o.last, ref + d + 1 - t, t)) { fails++; printf("it %d: last mismatch\n", it); } }
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

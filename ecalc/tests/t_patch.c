/* t_patch - Phase 15 K (ECALC_CORR_PATCH): the division's correction applied to digits already written, as a patch of the
 * tail (mn_out_tail_core / mn_out_tail_fix), against the reference path (the file written from the corrected X).
 * Random X in base 10^18 (d_out up to 6000), a correction dx in [-9, 9] \ {0} with a forced carry / borrow chain of 0 ..
 * nl - 2 whole limbs (X ending in 18k nines / zeros, and runs of 9s / 0s inside the limb where the chain ends), the string
 * split over 1..5 "nodes" and chunks of 1..9 limbs, T2 zones 0 / 30 / 100 / 4096 digits.  The parts are written from the
 * uncorrected X, then patched node by node (the low limbs gathered in growing windows, as the collective does); checks:
 * every part equals its range of the corrected file, the joined digit residues + the adjustment equal the corrected
 * string's, every ECALC_WINDOWS window (random ones, ones ending in the last 60 digits, ones over the chain) checked
 * exactly once and none bad, first / last / tail follow.  Size 1 also through mn_out_tail_fix.  Directory: T_PATCH_DIR
 * (default /tmp); ECALC_ODIRECT=1 (the default) patches whole 4 KiB blocks with O_DIRECT where the file system takes it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../mn_out.h"
#include "../packed_fmt.h"
#include "harness.h"
/* Phase 15 KP: the packed form (ECALC_OUT_PACKED=1, the default).  Both forms are run: without ECALC_OUT_PACKED in the
 * environment t_patch runs itself twice, =0 (ASCII: K's checks) and =1 (packed: every part's limbs == the corrected X's limbs
 * [lo, hi) in file order, its header complete with residues == those limbs mod q, tools/unpack_digits --cmp against the corrected
 * ASCII file with its residue check; T_PATCH_UNPACK = the binary, default ../tools/unpack_digits).  Both forms: a second patch of
 * a patched part must fail (the bytes are no longer the uncorrected ones) exactly when the first one changed bytes. */
static int packed_part_check(const char *pn, const uint64_t *X1, size_t nl, const mn_out *o, int it, int size)
{
    FILE *f = fopen(pn, "rb"); if (!f) { printf("it %d size %d node %d: cannot open %s\n", it, size, o->rank, pn); return 1; }
    ecp_hdr h; int bad = 0;
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, ECP_MAGIC, 8)) { printf("it %d size %d node %d: no packed header\n", it, size, o->rank); fclose(f); return 1; }
    if (!h.complete || h.k0 != o->k0 || h.k1 != o->k1 || h.hi > nl || h.lo > h.hi) { printf("it %d size %d node %d: header fields\n", it, size, o->rank); fclose(f); return 1; }
    size_t n = h.hi - h.lo; uint64_t *l = (uint64_t *)malloc(n * 8 + 8);
    if (fseek(f, ECP_HDR_BYTES, SEEK_SET) || fread(l, 8, n, f) != n || fgetc(f) != EOF) { printf("it %d size %d node %d: the part does not hold %zu limbs\n", it, size, o->rank, n); bad = 1; }
    for (size_t j = 0; !bad && j < n; j++) if (l[j] != X1[h.hi - 1 - j]) { printf("it %d size %d node %d: limb %zu is not the corrected X's\n", it, size, o->rank, (size_t)(h.hi - 1 - j)); bad = 1; }
    for (int i = 0; !bad && i < T1_NQ; i++) if (h.dres[i] != vf_limbs_mod(X1 + h.lo, n, t1_q[i])) { printf("it %d size %d node %d: header residue %d is not the corrected part's\n", it, size, o->rank, i); bad = 1; }
    free(l); fclose(f);
    return bad;
}
static const uint64_t B = 1000000000000000000ULL;
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
static uint64_t rnd(void) { static uint64_t s = 0x2545F4914F6CDD1Dull; s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static char *slurp(const char *fn, long *len) { FILE *f = fopen(fn, "r"); if (!f) { *len = -1; return 0; } fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET); char *b = (char *)malloc(*len + 1); if (fread(b, 1, *len, f) != (size_t)*len) *len = -1; fclose(f); return b; }
int main(int argc, char **argv)
{
    (void)argc;
    if (!getenv("ECALC_OUT_PACKED")) {                       /* Phase 15 KP: both forms, each in its own process (the form is read once) */
        int rc = 0; char cmd[4200];
        for (int pk = 0; pk <= 1; pk++) { snprintf(cmd, sizeof cmd, "ECALC_OUT_PACKED=%d %s", pk, argv[0]); fflush(stdout); int r = system(cmd); printf("t_patch: ECALC_OUT_PACKED=%d: %s\n", pk, r == 0 ? "ok" : "FAILED"); fflush(stdout); rc |= r != 0; }
        printf("t_patch: %s\n", rc ? "VERIFY FAILED (both forms)" : "VERIFY OK (both forms)");
        return rc;
    }
    printf("== t_patch (ECALC_OUT_PACKED=%s) ==\n", getenv("ECALC_OUT_PACKED")); harness_meta("t_patch");
    const char *unpack = getenv("T_PATCH_UNPACK") ? getenv("T_PATCH_UNPACK") : "../tools/unpack_digits";
    int npk = 0, nneg = 0;
    bi_set_decimal(1);
    const char *dir = getenv("T_PATCH_DIR") ? getenv("T_PATCH_DIR") : "/tmp";
    int fails = 0, checks = 0; size_t maxchain = 0; int nlong = 0;
    for (int it = 0; it < 80; it++) {
        unsigned long d_out = 60 + rnd() % 6000, d = ((d_out + 17) / 18) * 18;
        size_t nl = (d + 1 + 17) / 18; uint64_t *X0 = (uint64_t *)malloc(nl * 8), *X1 = (uint64_t *)malloc(nl * 8);
        for (size_t i = 0; i < nl; i++) X0[i] = rnd() % B;
        X0[nl - 1] = 2;
        long dx = 1 + (long)(rnd() % 9); if (rnd() & 1) dx = -dx;
        /* the chain: L whole limbs of 9s (dx > 0) / 0s (dx < 0) above limb 0, limb 0 overflowing / borrowing, a run inside limb L + 1 */
        size_t L = it % 4 == 0 ? nl - 2 - 1 : it % 4 == 1 ? 0 : rnd() % (nl - 2); if (L > nl - 3) L = nl - 3;
        if (it % 7 == 6) { L = 0; X0[0] = dx > 0 ? rnd() % (B - 10) : 10 + rnd() % (B - 10); }   /* no carry out of limb 0 */
        else {
            X0[0] = dx > 0 ? B - 1 - rnd() % (uint64_t)dx : rnd() % (uint64_t)(-dx);
            for (size_t i = 1; i <= L; i++) X0[i] = dx > 0 ? B - 1 : 0;
            uint64_t p10 = 1; int k = (int)(rnd() % 18); for (int q = 0; q < k; q++) p10 *= 10;   /* the next limb ends in k 9s / 0s */
            uint64_t v = rnd() % B; v = v - v % p10; if (dx > 0) v += p10 - 1; if (dx > 0 && v == B - 1) v -= p10;   /* (not all 9s: the chain ends here) */
            if (dx < 0 && v == 0) v = p10 < B ? p10 : 1;
            X0[L + 1] = v;
        }
        memcpy(X1, X0, nl * 8);
        { uint64_t c = (uint64_t)(dx < 0 ? -dx : dx); for (size_t i = 0; c && i < nl; i++) { if (dx > 0) { uint64_t v = X1[i] + c; if (v >= B) { X1[i] = v - B; c = 1; } else { X1[i] = v; c = 0; } } else { if (X1[i] >= c) { X1[i] -= c; c = 0; } else { X1[i] = X1[i] + B - c; c = 1; } } } }
        char *r0 = (char *)malloc(d + 2), *ref = (char *)malloc(d + 2); digits_format(r0, X0, nl, d); digits_format(ref, X1, nl, d);
        size_t kp = 0; while (kp <= d && r0[kp] == ref[kp]) kp++;
        if (d + 1 - kp > maxchain) maxchain = d + 1 - kp; if (d + 1 - kp > 18) nlong++;
        char fn[512]; snprintf(fn, sizeof fn, "%s/t_patch_%d.ref", dir, getpid());
        FILE *f = fopen(fn, "w"); fputc(ref[0], f); fputc('.', f); fwrite(ref + 1, 1, d_out, f); fputc('\n', f); fclose(f);
        /* windows (of the corrected string): random, ending in the last 60 digits, over the chain's first digit */
        char wf[512]; snprintf(wf, sizeof wf, "%s/t_patch_%d.win", dir, getpid()); FILE *w = fopen(wf, "w"); int nw = 0;
        for (int k = 0; k < 8; k++) { unsigned long o = 1 + rnd() % (d_out + 1 - 50); fprintf(w, "%lu %.50s\n", o, ref + o); nw++; }
        for (int k = 0; k < 3; k++) { unsigned long o = d_out + 1 - 50 - rnd() % 10; fprintf(w, "%lu %.50s\n", o, ref + o); nw++; }
        if (kp > 30 && kp + 20 <= d_out) { unsigned long o = kp - 30; fprintf(w, "%lu %.50s\n", o, ref + o); nw++; }
        fclose(w); setenv("ECALC_WINDOWS", wf, 1);
        uint64_t Dref[T1_NQ]; vf_digits_mods(ref, d + 1, t1_q, T1_NQ, Dref);
        for (int size = 1; size <= 5; size++) { tier2_windows_reset();
            size_t Lc = 1 + rnd() % 9; static const size_t zones[4] = { 0, 30, 100, 4096 }; size_t zone = zones[rnd() % 4];
            uint64_t D[T1_NQ] = {0}, adj[T1_NQ]; int nwin = 0, bad2 = 0, badp = 0, gotadj = 0;
            char out[512]; snprintf(out, sizeof out, "%s/t_patch_%d.out", dir, getpid());
            char tail[64]; size_t ntail = 0;
            mn_out os[5];
            for (int r = size - 1; r >= 0; r--) {             /* the writers, top node first (the head = the node above's tail), on X0 */
                size_t lo, hi; comm_shard(nl, r, size, &lo, &hi);
                mn_out *o = &os[r]; memset(o, 0, sizeof *o); o->d = d; o->d_out = d_out; o->outfile = out; o->rank = r; o->size = size; o->chunk_limbs = Lc; o->t2_defer = zone;
                memcpy(o->head, tail, ntail); o->nhead = ntail;
                mn_out_src src = { X0 + lo, 0, lo, hi - lo };
                mn_out_run(o, &src); mn_out_finish(o);
                for (int i = 0; i < T1_NQ; i++) D[i] = vf_digits_join(D[i], o->ndig, o->dres[i], t1_q[i]);
                if (o->k1 > o->k0) { size_t n = o->k1 - o->k0, t = n < 49 ? n : 49; size_t keep = 49 - t < ntail ? 49 - t : ntail; memmove(tail, tail + ntail - keep, keep); memcpy(tail + keep, r0 + o->k1 - t, t); ntail = keep + t; }
            }
            for (int r = size - 1; r >= 0; r--) {             /* the patch, node by node: size 1 through the wrapper, else the core over growing windows */
                mn_out *o = &os[r]; mn_out_fix fx; memset(&fx, 0, sizeof fx); int rc;
                if (size == 1) { mn_out_src src = { X0, 0, 0, nl }; rc = mn_out_tail_fix(o, &src, 0, dx, &fx); }
                else { size_t wl = 4; for (;;) { rc = mn_out_tail_core(o, X0, wl, dx, &fx); if (rc >= 0 || wl >= nl) break; wl = wl * 2 < nl ? wl * 2 : nl; } }
                badp += rc != 0;
                { mn_out o2 = *o; mn_out_fix f2; memset(&f2, 0, sizeof f2); int r2 = mn_out_tail_core(&o2, X0, nl, dx, &f2);   /* Phase 15 KP: patching again must fail where bytes changed */
                  checks++; nneg += fx.parts > 0; if ((r2 != 0) != (fx.parts > 0)) { fails++; printf("it %d size %d node %d: a second patch returned %d (the first changed %d parts)\n", it, size, r, r2, fx.parts); } }
                if (!gotadj) { memcpy(adj, fx.dres_adj, sizeof adj); gotadj = 1; } else if (memcmp(adj, fx.dres_adj, sizeof adj)) { fails++; printf("it %d size %d: the nodes' adjustments differ\n", it, size); }
                checks++; if (fx.kp != kp) { fails++; printf("it %d size %d node %d: kp %zu, expected %zu\n", it, size, r, fx.kp, kp); }
                nwin += o->nwin; bad2 += o->bad2;
                char pn[600]; if (size > 1) snprintf(pn, sizeof pn, "%s.part%04d", out, size - 1 - r); else snprintf(pn, sizeof pn, "%s", out);
                if (o->packed) { checks++; npk++; if (o->k1 > o->k0 && packed_part_check(pn, X1, nl, o, it, size)) fails++; }
                else {
                long pl, rl; char *pb = slurp(pn, &pl), *rb = slurp(fn, &rl);
                size_t fb = o->k0 == 0 ? 0 : o->k0 + 1, fe = (o->k1 < d_out + 1 ? o->k1 : d_out + 1) + 1 + (o->k0 <= d_out && d_out < o->k1 ? 1 : 0);
                if (o->k0 >= d_out + 1) fe = fb;
                checks++;
                if (pl < 0 || (size_t)pl != fe - fb || memcmp(pb, rb + fb, pl)) { fails++; int q = 0; while (q < pl && pb[q] == rb[fb + q]) q++; printf("it %d size %d node %d: part (%ld bytes) differs from [%zu, %zu) of the corrected file at %d (dx %+ld, kp %zu, d_out %lu)\n", it, size, r, pl, fb, fe, q, dx, kp, d_out); }
                free(pb); free(rb);
                }
                if (o->k1 > o->k0 && r == size - 1) { checks++; if (strncmp(o->first, ref, strlen(o->first))) { fails++; printf("it %d: first mismatch\n", it); } }
                if (o->k0 <= d_out && d_out < o->k1) { checks++; size_t t = strlen(o->last); if (memcmp(o->last, ref + d_out + 1 - t, t)) { fails++; printf("it %d size %d: last mismatch\n", it, size); } }
                if (o->ntail) { checks++; if (memcmp(o->tail, ref + d_out + 1, o->ntail)) { fails++; printf("it %d: tail mismatch\n", it); } }
            }
            checks++; if (badp) { fails++; printf("it %d size %d: %d nodes report a patch failure\n", it, size, badp); }
            for (int i = 0; i < T1_NQ; i++) D[i] = vf_add_mod(D[i], adj[i], t1_q[i]);
            checks++; if (memcmp(D, Dref, sizeof D)) { fails++; printf("it %d size %d: digit residue + adjustment != the corrected string's\n", it, size); }
            int exp = nw + (100 <= d_out + 1 ? 1 : 0), expbad = 100 <= d_out + 1 ? 1 : 0;   /* + the built-in window at 50 (not e here) */
            checks++; if (nwin != exp || bad2 != expbad) { fails++; printf("it %d size %d: %d windows checked (expected %d), %d bad (expected %d; zone %zu, kp %zu, d_out %lu)\n", it, size, nwin, exp, bad2, expbad, zone, kp, d_out); }
            if (os[0].packed) {                               /* Phase 15 KP: the converter on the patched parts: its residue check and the ASCII */
                char cmd[2400]; if (size > 1) snprintf(cmd, sizeof cmd, "%s -q --cmp %s %s.part* > /dev/null", unpack, fn, out); else snprintf(cmd, sizeof cmd, "%s -q --cmp %s %s > /dev/null", unpack, fn, out);
                checks++; if (system(cmd)) { fails++; printf("it %d size %d: %s on the patched packed parts: residue check or ASCII differs\n", it, size, unpack); }
                if (size > 1) { snprintf(cmd, sizeof cmd, "rm -f %s.part*", out); if (system(cmd)) {} } else unlink(out);
            } else if (size > 1) {
                char cmd[1400]; snprintf(cmd, sizeof cmd, "cat %s.part* | cmp -s - %s", out, fn);
                checks++; if (system(cmd)) { fails++; printf("it %d size %d: cat of the parts differs\n", it, size); }
                snprintf(cmd, sizeof cmd, "rm -f %s.part*", out); if (system(cmd)) {}
            } else { char cmd[1400]; snprintf(cmd, sizeof cmd, "cmp -s %s %s", out, fn); checks++; if (system(cmd)) { fails++; printf("it %d size 1: file differs\n", it); } unlink(out); }
        }
        unlink(fn); unlink(wf); free(X0); free(X1); free(r0); free(ref); tier2_windows_reset();
    }
    printf("%d checks, %d failures; longest chain %zu digits, %d chains over one limb; %d packed parts checked; %d second patches refused\n", checks, fails, maxchain, nlong, npk, nneg);
    VERIFY(fails == 0, "t_patch: %d of %d checks failed", fails, checks);
    return verify_done("t_patch");
}

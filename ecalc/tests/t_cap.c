/* t_cap - PLAN.md 29.2 E2 (Phase 13a, agent S): the plane cap against the grid's cost.  Pure arithmetic, no node time.
 *
 * 1. Each strategy's plane bytes per APU for a plane of n points and P primes -- the forms t_strategy (E0) measured
 *    (plane_GiB/APU there, exact at P = 4):
 *      A product-per-APU  (P + 1) n 8   on one APU       (P result planes + one B plane)
 *      B prime-per-APU    2 n 8         on each of P APUs (X_d, Y_d)
 *      C four-step        (P + 3) n/4 8 on each of 4 APUs (P planes of q = n/4, xb, two slabs: rns_dist.c dist_core)
 *    and, for a per-APU budget, the largest plane (2^k or 3 2^k) each strategy fits: the cap at which it stops fitting.
 * 2. The product shapes the pipeline forms at D digits (decimal limbs, nq = D / 18 as in mn_model.py): the top two
 *    tree levels (2 products of nq/2 x nq/2, 4 of nq/4 x nq/4), the reciprocal's last three doublings (Q_t r:
 *    min(2j+2, nq) x (j+1) and r d: (j+1) x (j+2) at j = k/2, k/4, k/8, k = nq + 1: newton_db.c / mn_model.recip_cost),
 *    the division's A_h mu ((2 nq + 1) x (nq + 2), the pieces all below the low cut nq + 2 skipped) and X Q
 *    ((nq + 1) x nq, the pieces starting at or above w = nq + 2 skipped).  Under each plane cap -- 2^29, 2^30, 3 2^29,
 *    2^31, 3 2^30 (the 3 2^k caps with radix-3 planes, DIST_R3) -- the grid split_grid_cap chooses (mirrored here from
 *    rns_dist.c:476 -- keep in step), the pieces formed, the plane points, the total transform points (3 transforms
 *    per prime per piece, the single-node tier's cache off, its default), the all-to-alls under C (3 P per piece)
 *    and which strategies fit every piece in the budget.
 * 3. With E0's measured STRAT lines (t_strategy output, argument 1), the modelled time of each shape under each
 *    strategy: pieces x the measured median of one product of that plane size (3 2^k planes: 1.5 x 1.05 x the 2^k
 *    time -- modelled; P = 3 from P = 4's measured parts: A and C transform/load parts x 3/4, B unchanged; the grid's
 *    shifted adds are not counted).  Labelled measured / modelled in the output.
 *
 * usage: t_cap [e0_log] [budget_GiB=28] [P=4] [D1 D2 ...]      (default D = 4e10 1e11) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define GiB 1073741824.0

static int NP = 4;
/* ---- E0's measured medians: t[strategy][logn], parts load/ntt/crt/merge ---- */
static double T[3][40], TL[3][40], TN[3][40], TC[3][40], TM[3][40]; static int have[3][40];
static void read_e0(const char *fn)
{
    FILE *f = fopen(fn, "r"); char line[1024];
    if (!f) { fprintf(stderr, "t_cap: cannot read %s\n", fn); exit(1); }
    while (fgets(line, sizeof line, f)) {
        int logn, P; char s; double w, l, n, c, m;
        if (sscanf(line, "STRAT logn=%d P=%d strat=%c wall_med=%lf", &logn, &P, &s, &w) != 4 || P != 4 || logn < 0 || logn >= 40) continue;
        const char *q = strstr(line, "| load "); if (!q || sscanf(q, "| load %lf ntt %lf crt %lf merge %lf", &l, &n, &c, &m) != 4) continue;
        int k = s - 'A'; if (k < 0 || k > 2) continue;
        T[k][logn] = w; TL[k][logn] = l; TN[k][logn] = n; TC[k][logn] = c; TM[k][logn] = m; have[k][logn] = 1;   /* the last line of a size wins */
    }
    fclose(f);
}
/* one product on a plane of pts points under strategy k at NP primes: measured (2^k, P = 4) or modelled; <0: unknown */
static double t_one(int k, double pts, int *modelled)
{
    int logn = 0; while ((double)(1ULL << logn) < pts) logn++;
    int r3 = (double)(1ULL << logn) != pts;                         /* 3 2^j = 1.5 2^(j+1): logn = j + 2, the 2^k below is 2^(logn-1) */
    int lb = r3 ? logn - 1 : logn;
    if (lb < 0 || lb >= 40 || !have[k][lb]) return -1;
    double t = T[k][lb], f = r3 ? 1.5 * 1.05 : 1.0;
    *modelled = r3;
    if (NP == 3) {                                                  /* from the measured parts */
        if (k == 0 || k == 2) t = TL[k][lb] * 0.75 + TN[k][lb] * 0.75 + TC[k][lb] + TM[k][lb] + (T[k][lb] - TL[k][lb] - TN[k][lb] - TC[k][lb] - TM[k][lb]);
        *modelled = 1;
    }
    return t * f;
}
/* ---- plane bytes per APU ---- */
static double bytes_apu(int k, double n)
{
    switch (k) { case 0: return (NP + 1) * n * 8; case 1: return 2 * n * 8; default: return (NP + 3) * n / 4 * 8; }
}
static const char *SN[3] = { "A product-per-APU", "B prime-per-APU", "C four-step" };
/* ---- rns_dist.c's plane_pts / split_grid_cap (mirrored: logmax, r3 = the cap's form) ---- */
static int g_logmax;
static size_t plane_pts(size_t nc, int r3)
{
    size_t n = (size_t)1 << 20;
    while (n < nc) { if (r3 && n >= ((size_t)1 << (g_logmax - 2)) && (n / 2 * 3) >= nc) return n / 2 * 3; n <<= 1; }
    return n;
}
static void split_grid_cap(size_t na, size_t nb, size_t cap, int r3, int *ka, int *kb)
{
    size_t best = 0; *ka = *kb = 0;
    for (int i = 1; i <= 32; i++) for (int j = 1; j <= 32; j++) {
        size_t pa = (na + i - 1) / i, pb = (nb + j - 1) / j;
        if (pa + pb > cap) continue;
        size_t pts = plane_pts(pa + pb, r3);
        size_t cost = (size_t)i * j * pts * ((pts & (pts - 1)) ? 21 : 20);
        if (!*ka || cost < best || (cost == best && i * j < *ka * *kb)) { best = cost; *ka = i; *kb = j; }
    }
}
struct shape { const char *name; size_t na, nb, lowcut, w; int count; };
struct capf { const char *name; int logmax, r3; size_t cap; };

int main(int argc, char **argv)
{
    const char *e0 = argc > 1 && strcmp(argv[1], "-") ? argv[1] : 0;
    double budget = (argc > 2 ? atof(argv[2]) : 28.0) * GiB;
    NP = argc > 3 ? atoi(argv[3]) : 4; if (NP != 3 && NP != 4) NP = 4;
    double Dl[8] = { 4e10, 1e11 }; int nD = 2;
    if (argc > 4) { nD = 0; for (int i = 4; i < argc && nD < 8; i++) Dl[nD++] = atof(argv[i]); }
    if (e0) read_e0(e0);
    printf("t_cap: P = %d primes, per-APU plane budget %.1f GiB%s\n\n", NP, budget / GiB, e0 ? "" : " (no E0 log: no times)");

    /* 1: the fit boundary */
    printf("1. plane bytes per APU (GiB) by plane size -- the forms t_strategy measured at P = 4 (exact); at P = 3 by the same formulas\n");
    printf("   %-8s %10s %10s %10s\n", "plane", "A (1 APU)", "B (P APUs)", "C (4 APUs)");
    for (int lg = 26; lg <= 33; lg++) for (int r3 = 0; r3 < 2; r3++) {
        if (r3 && lg < 28) continue;
        double n = r3 ? 3.0 * (1ULL << (lg - 2)) : (double)(1ULL << lg); char nm[16]; if (r3) snprintf(nm, sizeof nm, "3*2^%d", lg - 2); else snprintf(nm, sizeof nm, "2^%d", lg);
        printf("   %-8s", nm); for (int k = 0; k < 3; k++) printf(" %9.1f%s", bytes_apu(k, n) / GiB, bytes_apu(k, n) <= budget ? " " : "*"); printf("\n");
    }
    printf("   (* = over the %.1f GiB budget)\n   the largest plane each strategy fits in the budget:", budget / GiB);
    for (int k = 0; k < 3; k++) {
        double best = 0; char nm[16] = "none";
        for (int lg = 20; lg <= 34; lg++) for (int r3 = 0; r3 < 2; r3++) { double n = r3 ? 3.0 * (1ULL << (lg - 2)) : (double)(1ULL << lg); if (bytes_apu(k, n) <= budget && n > best) { best = n; if (r3) snprintf(nm, sizeof nm, "3*2^%d", lg - 2); else snprintf(nm, sizeof nm, "2^%d", lg); } }
        printf("  %c %s", 'A' + k, nm);
    }
    printf("\n   budget each strategy needs for a plane of 2^31 / 3*2^30 / 2^32 points: A %.0f/%.0f/%.0f, B %.0f/%.0f/%.0f, C %.0f/%.0f/%.0f GiB\n\n",
           bytes_apu(0, 2147483648.0) / GiB, bytes_apu(0, 3221225472.0) / GiB, bytes_apu(0, 4294967296.0) / GiB, bytes_apu(1, 2147483648.0) / GiB, bytes_apu(1, 3221225472.0) / GiB, bytes_apu(1, 4294967296.0) / GiB,
           bytes_apu(2, 2147483648.0) / GiB, bytes_apu(2, 3221225472.0) / GiB, bytes_apu(2, 4294967296.0) / GiB);

    /* 2 + 3: the pipeline's shapes under each cap */
    struct capf caps[] = { { "2^29", 29, 0, 0 }, { "2^30", 30, 0, 0 }, { "3*2^29", 30, 1, 0 }, { "2^31", 31, 0, 0 }, { "3*2^30", 31, 1, 0 } };
    int ncap = sizeof caps / sizeof caps[0];
    for (int c = 0; c < ncap; c++) caps[c].cap = caps[c].r3 ? (size_t)3 << (caps[c].logmax - 1) : (size_t)1 << caps[c].logmax;
    for (int di = 0; di < nD; di++) {
        double D = Dl[di]; size_t nq = (size_t)(D / 18), k = nq + 1;
        struct shape sh[12]; int ns = 0;
        sh[ns++] = (struct shape){ "tree top (x2)", nq / 2, nq / 2, 0, (size_t)-1, 2 };
        sh[ns++] = (struct shape){ "tree top-1 (x4)", nq / 4, nq / 4, 0, (size_t)-1, 4 };
        size_t js[3] = { (k + 1) / 2, (k + 3) / 4, (k + 7) / 8 };
        const char *jn[3] = { "recip j=k/2", "recip j=k/4", "recip j=k/8" };
        for (int t = 0; t < 3; t++) {
            size_t j = js[t], take = 2 * j + 2 < nq ? 2 * j + 2 : nq;
            static char b1[3][40], b2[3][40]; snprintf(b1[t], 40, "%s Q_t r", jn[t]); snprintf(b2[t], 40, "%s r d", jn[t]);
            sh[ns++] = (struct shape){ b1[t], take, j + 1, 0, (size_t)-1, 1 };
            sh[ns++] = (struct shape){ b2[t], j + 1, j + 2, 0, (size_t)-1, 1 };
        }
        sh[ns++] = (struct shape){ "div A_h mu (low cut)", 2 * nq + 1, nq + 2, nq + 2, (size_t)-1, 1 };
        sh[ns++] = (struct shape){ "div X Q (mod B^w)", nq + 1, nq, 0, nq + 2, 1 };
        printf("2/3. D = %.3g digits (nq = %zu limbs = D/18): per shape and cap -- grid ka x kb, pieces formed x plane, transform points (3 P per piece), all-to-alls under C, strategies fitting every piece in %.1f GiB, modelled time per strategy (s)\n", D, nq, budget / GiB);
        double tot_pts[8] = {0}, tot_a2a[8] = {0}, tot_t[8][3] = {{0}}; int tot_unk[8][3] = {{0}}, tot_fit[8][3];
        for (int c = 0; c < ncap; c++) for (int s = 0; s < 3; s++) tot_fit[c][s] = 1;
        for (int i = 0; i < ns; i++) {
            printf("   %-22s %11zu x %11zu (x%d)\n", sh[i].name, sh[i].na, sh[i].nb, sh[i].count);
            for (int c = 0; c < ncap; c++) {
                g_logmax = caps[c].logmax; int ka, kb; size_t na = sh[i].na, nb = sh[i].nb, nc = na + nb;
                int one = nc <= caps[c].cap;
                if (one && caps[c].r3 && nc > ((size_t)1 << caps[c].logmax)) { split_grid_cap(na, nb, caps[c].cap, 1, &ka, &kb); one = ka * kb == 1; }   /* mul_grid's rule */
                if (one) { ka = kb = 1; } else split_grid_cap(na, nb, caps[c].cap, caps[c].r3, &ka, &kb);
                size_t pa = (na + ka - 1) / ka, pb = (nb + kb - 1) / kb; int formed = 0; double pts_sum = 0, pmax = 0;
                for (int jb = 0; jb < kb; jb++) for (int ia = 0; ia < ka; ia++) {
                    size_t oa = (size_t)ia * pa, ob = (size_t)jb * pb, la = na - oa < pa ? na - oa : pa, lb = nb - ob < pb ? nb - ob : pb;
                    if (oa >= na || ob >= nb) continue;
                    if (oa + ob >= sh[i].w || oa + ob + la + lb <= sh[i].lowcut) continue;
                    double p = one ? (double)plane_pts(nc, caps[c].r3) : (double)plane_pts(la + lb, caps[c].r3);
                    formed++; pts_sum += p; if (p > pmax) pmax = p;
                }
                double tp = pts_sum * 3 * NP * sh[i].count; int a2a = formed * 3 * NP * sh[i].count;
                tot_pts[c] += tp; tot_a2a[c] += a2a;
                char fit[4] = "---"; double ts[3]; int mod[3] = {0, 0, 0}, unk[3] = {0, 0, 0};
                for (int s = 0; s < 3; s++) {
                    if (bytes_apu(s, pmax) <= budget) fit[s] = 'A' + s; else tot_fit[c][s] = 0;
                    ts[s] = 0;
                    for (int jb = 0; jb < kb; jb++) for (int ia = 0; ia < ka; ia++) {
                        size_t oa = (size_t)ia * pa, ob = (size_t)jb * pb, la = na - oa < pa ? na - oa : pa, lb = nb - ob < pb ? nb - ob : pb;
                        if (oa >= na || ob >= nb) continue;
                        if (oa + ob >= sh[i].w || oa + ob + la + lb <= sh[i].lowcut) continue;
                        double p = one ? (double)plane_pts(nc, caps[c].r3) : (double)plane_pts(la + lb, caps[c].r3); int m = 0;
                        double t = e0 ? t_one(s, p, &m) : -1; if (t < 0) unk[s] = 1; else ts[s] += t; mod[s] |= m;
                    }
                    ts[s] *= sh[i].count; if (unk[s]) tot_unk[c][s] = 1; else tot_t[c][s] += ts[s];
                }
                printf("      cap %-7s %2d x %-2d %2d x %-7s %9.3g pts %4d a2a  fits %s", caps[c].name, ka, kb, formed, "", tp, a2a, fit);
                if (e0) for (int s = 0; s < 3; s++) { if (unk[s]) printf("  %c   n/a  ", 'A' + s); else printf("  %c %6.2f%s", 'A' + s, ts[s], mod[s] || NP == 3 ? "m" : " "); }
                printf("   (plane %.3g)\n", pmax);
            }
        }
        printf("   totals over these shapes (D = %.3g):\n", D);
        for (int c = 0; c < ncap; c++) {
            printf("      cap %-7s %10.4g transform points, %5.0f all-to-alls under C, every piece fits:%s%s%s", caps[c].name, tot_pts[c], tot_a2a[c], tot_fit[c][0] ? " A" : "", tot_fit[c][1] ? " B" : "", tot_fit[c][2] ? " C" : "");
            if (e0) for (int s = 0; s < 3; s++) { if (tot_unk[c][s]) printf("  %c n/a", 'A' + s); else printf("  %c %.2f s", 'A' + s, tot_t[c][s]); }
            printf("\n");
        }
        printf("\n");
    }
    printf("labels: bytes and grids exact (the code's formulas); times: E0 medians at P = 4 on 2^k planes = measured, 'm' = modelled (3 2^k planes, or P = 3);\n"
           "        the grid's shifted adds, the cache (off by default in the single-node tier) and the tree's pairing are not counted\n");
    for (int k = 0; k < 3; k++) (void)SN[k];
    return 0;
}

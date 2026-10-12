#!/usr/bin/env python3
"""tools/k8b_stats.py (K8b) - paired ABBA statistics without scipy: mean difference B - A, its 95 % CI and the two-sided
paired t-test p-value, per metric.  Input: a TSV with a header and the columns round, pair, A_<m>, B_<m> for each metric m.
   k8b_stats.py pairs.tsv [metric ...]    -> one line per metric: n, mean diff, sd, 95 % CI, t, p
Exit 0 always; the driver reads the printed values (STOP line: 1 when the first metric meets p < 0.05 or CI half-width <= HALF)."""
import math, sys, os

T975 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145, 15: 2.131, 16: 2.120, 20: 2.086, 30: 2.042}
def t975(df): return T975.get(df, 1.96 if df > 30 else T975[max(k for k in T975 if k <= df)])
def betacf(a, b, x):
    MAXIT, EPS, FPMIN = 200, 3e-14, 1e-300
    qab, qap, qam = a + b, a + 1, a - 1
    c, d = 1.0, 1 - qab * x / qap
    if abs(d) < FPMIN: d = FPMIN
    d = 1 / d; h = d
    for m in range(1, MAXIT + 1):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1 + aa * d; d = FPMIN if abs(d) < FPMIN else d; c = 1 + aa / c; c = FPMIN if abs(c) < FPMIN else c; d = 1 / d; h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1 + aa * d; d = FPMIN if abs(d) < FPMIN else d; c = 1 + aa / c; c = FPMIN if abs(c) < FPMIN else c; d = 1 / d; de = d * c; h *= de
        if abs(de - 1) < EPS: break
    return h
def betai(a, b, x):
    if x <= 0: return 0.0
    if x >= 1: return 1.0
    bt = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log(1 - x))
    return bt * betacf(a, b, x) / a if x < (a + 1) / (a + b + 2) else 1 - bt * betacf(b, a, 1 - x) / b
def t_p_two_sided(t, df): return betai(df / 2, 0.5, df / (df + t * t))

def main():
    f = sys.argv[1]; half = float(os.environ.get("HALF", "0.5"))
    rows = [l.rstrip("\n").split("\t") for l in open(f) if l.strip()]
    hdr, rows = rows[0], rows[1:]
    metrics = sys.argv[2:] or [h[2:] for h in hdr if h.startswith("A_")]
    stop = 0
    for i, m in enumerate(metrics):
        ia, ib = hdr.index("A_" + m), hdr.index("B_" + m)
        d = []
        for r in rows:
            try: d.append(float(r[ib]) - float(r[ia]))
            except (ValueError, IndexError): pass
        n = len(d)
        if n < 2: print(f"{m}: n {n} (too few pairs)"); continue
        mean = sum(d) / n; sd = math.sqrt(sum((x - mean) ** 2 for x in d) / (n - 1)); se = sd / math.sqrt(n)
        t = mean / se if se > 0 else float("inf"); p = t_p_two_sided(t, n - 1) if se > 0 else 0.0; hw = t975(n - 1) * se
        print(f"{m}: n {n} pairs, B - A mean {mean:+.3f} s, sd {sd:.3f}, 95% CI [{mean - hw:+.3f}, {mean + hw:+.3f}] (half-width {hw:.3f}), t {t:.2f}, p {p:.4f}")
        if i == 0 and (p < 0.05 or hw <= half): stop = 1
    print(f"STOP {stop}")
main()

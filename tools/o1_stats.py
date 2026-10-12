#!/usr/bin/env python3
"""o1_stats.py - paired-ABBA statistics for task O1 (results/O1.md).  Pure Python (no numpy/scipy).

usage: o1_stats.py TOTALS_TSV [--min-rounds 3] [--hw 5] [--alpha 0.05] [--quiet]
TOTALS_TSV columns (tab separated, written by tools/o1_item.sh):
  seq  arm(A|B)  round  total_s  init_s  rate_GBs  nicgroup(A|B|NA)  fgfc_M  ok(1|0)
arm A = default MN_GROUPS (2,6), arm B = single level (6).  Effect = B - A (negative: single level faster).
Estimators (all in seconds, 95 % CI from the t distribution):
  raw      mean of the per-round differences mean(B) - mean(A) (round = one ABBA/BAAB quartet; rounds with a
           missing/failed run are dropped); df = rounds - 1.
  nic      OLS on all valid runs: total = c + d*[arm B] + g*[NIC group A]   (df = n - 3; needs both groups present)
  rate     OLS: total = c + d*[arm B] + b/r   (r = per-run all-to-all rate, GB/s per APU thread; the L4 estimator)
  *_clean  the same without cold starts (init > 36 s)
Prints one line per estimator and a last line "STOP yes|no <reason>" for the stop rule on `raw`:
  rounds >= min-rounds and (two-sided p < alpha or CI half-width <= hw).
"""
import math, sys

def betacf(a, b, x):
    qab, qap, qam = a + b, a + 1, a - 1
    c, d = 1.0, 1 - qab * x / qap
    d = 1e-300 if abs(d) < 1e-300 else d
    d = 1 / d; h = d
    for m in range(1, 300):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1 + aa * d; d = 1e-300 if abs(d) < 1e-300 else d
        c = 1 + aa / c; c = 1e-300 if abs(c) < 1e-300 else c
        d = 1 / d; h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1 + aa * d; d = 1e-300 if abs(d) < 1e-300 else d
        c = 1 + aa / c; c = 1e-300 if abs(c) < 1e-300 else c
        d = 1 / d; de = d * c; h *= de
        if abs(de - 1) < 3e-14: break
    return h

def betainc(a, b, x):
    if x <= 0: return 0.0
    if x >= 1: return 1.0
    bt = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log(1 - x))
    if x < (a + 1) / (a + b + 2): return bt * betacf(a, b, x) / a
    return 1 - bt * betacf(b, a, 1 - x) / b

def t_two_sided_p(t, df):
    return betainc(df / 2.0, 0.5, df / (df + t * t))

def t_crit(df, alpha=0.05):
    lo, hi = 0.0, 1000.0
    for _ in range(200):
        mid = (lo + hi) / 2
        if t_two_sided_p(mid, df) > alpha: lo = mid
        else: hi = mid
    return (lo + hi) / 2

def solve(A, b):  # Gauss-Jordan, returns inverse of A and solution
    n = len(A); M = [row[:] + [1.0 if i == j else 0.0 for j in range(n)] for i, row in enumerate(A)]
    for i in range(n):
        p = max(range(i, n), key=lambda r: abs(M[r][i]))
        if abs(M[p][i]) < 1e-12: return None, None
        M[i], M[p] = M[p], M[i]
        piv = M[i][i]; M[i] = [v / piv for v in M[i]]
        for r in range(n):
            if r != i:
                f = M[r][i]; M[r] = [v - f * w for v, w in zip(M[r], M[i])]
    inv = [row[n:] for row in M]
    x = [sum(inv[i][j] * b[j] for j in range(n)) for i in range(n)]
    return inv, x

def ols(X, y):
    """returns (coefs, se, df) or None"""
    n, k = len(X), len(X[0])
    if n - k < 1: return None
    XtX = [[sum(X[r][i] * X[r][j] for r in range(n)) for j in range(k)] for i in range(k)]
    Xty = [sum(X[r][i] * y[r] for r in range(n)) for i in range(k)]
    inv, beta = solve(XtX, Xty)
    if inv is None: return None
    rss = sum((y[r] - sum(beta[i] * X[r][i] for i in range(k))) ** 2 for r in range(n))
    s2 = rss / (n - k)
    return beta, [math.sqrt(max(s2 * inv[i][i], 0)) for i in range(k)], n - k

def fmt(name, d, se, df, extra=""):
    if df < 1 or se is None:
        return f"{name:11s} n/a {extra}"
    tc = t_crit(df); p = t_two_sided_p(d / se, df) if se > 0 else 0.0
    return (f"{name:11s} B-A = {d:+7.1f} s  SE {se:5.1f}  95% CI [{d - tc * se:+.1f}, {d + tc * se:+.1f}]  "
            f"half-width {tc * se:.1f}  p = {p:.3f}  df {df} {extra}")

def load(path):
    runs = []
    for ln in open(path):
        f = ln.rstrip("\n").split("\t")
        if len(f) < 9: continue
        try:
            tot = float(f[3]); ok = f[8] == "1"
        except ValueError:
            tot, ok = None, False
        def num(s):
            try: return float(s)
            except ValueError: return None
        runs.append(dict(seq=int(f[0]), arm=f[1], rnd=int(f[2]), total=tot, init=num(f[4]), rate=num(f[5]),
                         grp=f[6], fgfc=num(f[7]), ok=ok and tot is not None))
    return runs

def raw_rounds(runs):
    by = {}
    for r in runs: by.setdefault(r["rnd"], []).append(r)
    diffs = []
    for k in sorted(by):
        rr = by[k]
        a = [r["total"] for r in rr if r["arm"] == "A" and r["ok"]]
        b = [r["total"] for r in rr if r["arm"] == "B" and r["ok"]]
        if len(rr) == 4 and len(a) == 2 and len(b) == 2: diffs.append(sum(b) / 2 - sum(a) / 2)
    return diffs

def estimates(runs, clean):
    rs = [r for r in runs if r["ok"] and (not clean or r["init"] is None or r["init"] <= 36)]
    out = {}
    if clean:  # raw on clean: only rounds in which all four runs are clean
        keep = {k for k in {r["rnd"] for r in runs}
                if all(r["ok"] and (r["init"] is None or r["init"] <= 36) for r in runs if r["rnd"] == k)}
        d = raw_rounds([r for r in runs if r["rnd"] in keep])
    else:
        d = raw_rounds(runs)
    if len(d) >= 2:
        m = sum(d) / len(d); sd = math.sqrt(sum((x - m) ** 2 for x in d) / (len(d) - 1))
        out["raw"] = (m, sd / math.sqrt(len(d)), len(d) - 1, f"rounds {len(d)}")
    # nic-group adjusted
    g = [r for r in rs if r["grp"] in ("A", "B")]
    if len(g) >= 5 and len({r["grp"] for r in g}) == 2:
        X = [[1.0, 1.0 if r["arm"] == "B" else 0.0, 1.0 if r["grp"] == "A" else 0.0] for r in g]
        res = ols(X, [r["total"] for r in g])
        if res: out["nic"] = (res[0][1], res[1][1], res[2], f"runs {len(g)} (grpA {sum(r['grp']=='A' for r in g)}) grp effect {res[0][2]:+.1f} s")
    elif len(g) >= 4:
        X = [[1.0, 1.0 if r["arm"] == "B" else 0.0] for r in g]
        res = ols(X, [r["total"] for r in g])
        if res: out["nic"] = (res[0][1], res[1][1], res[2], f"runs {len(g)} (one NIC group only: = unadjusted OLS)")
    # rate adjusted
    q = [r for r in rs if r["rate"]]
    if len(q) >= 5:
        X = [[1.0, 1.0 if r["arm"] == "B" else 0.0, 1.0 / r["rate"]] for r in q]
        res = ols(X, [r["total"] for r in q])
        if res: out["rate"] = (res[0][1], res[1][1], res[2], f"runs {len(q)} b = {res[0][2]:.0f} GB*s/thread")
    return out

def main():
    a = sys.argv[1:]
    path = a[0]; opt = lambda k, d: float(a[a.index(k) + 1]) if k in a else d
    minr, hw, alpha = int(opt("--min-rounds", 3)), opt("--hw", 5.0), opt("--alpha", 0.05)
    runs = load(path)
    nok = sum(r["ok"] for r in runs)
    print(f"runs {len(runs)} valid {nok} (A {sum(r['ok'] and r['arm']=='A' for r in runs)}, B {sum(r['ok'] and r['arm']=='B' for r in runs)}); "
          f"NIC group A in {sum(r['ok'] and r['grp']=='A' for r in runs)} runs, B in {sum(r['ok'] and r['grp']=='B' for r in runs)}")
    for clean in (False, True):
        est = estimates(runs, clean)
        for k in ("raw", "nic", "rate"):
            nm = k + ("_clean" if clean else "")
            print(fmt(nm, *est[k][:3], extra=est[k][3]) if k in est else f"{nm:11s} n/a")
    est = estimates(runs, False)
    stop, why = "no", "fewer than %d complete rounds" % minr
    if "raw" in est:
        d, se, df, _ = est["raw"]
        nr = df + 1
        if nr >= minr:
            tc = t_crit(df); p = t_two_sided_p(d / se, df) if se > 0 else 0.0
            if p < alpha: stop, why = "yes", f"p {p:.3f} < {alpha}"
            elif tc * se <= hw: stop, why = "yes", f"CI half-width {tc * se:.1f} <= {hw}"
            else: stop, why = "no", f"p {p:.3f}, half-width {tc * se:.1f} after {nr} rounds"
    print(f"STOP {stop} {why}")

if __name__ == "__main__":
    main()

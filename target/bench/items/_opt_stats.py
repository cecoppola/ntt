#!/usr/bin/env python3
"""_opt_stats.py RUNS.tsv BASE [METRIC] [KEYPREFIX] - per-arm statistics of raw/runs.tsv and paired differences against the base arm.
runs.tsv columns: tag arm round nodes digits rc verify total init bs dm recip devpeak hostpeak (tab separated, header first).
A run counts when rc == 0 and verify == OK.  Pairing: the k-th counted run of an arm with the k-th counted run of the base arm
(ABBA order gives symmetric pairs).  Output lines `key=value` (summary keys, units included) and one `LINE ...` text line per arm."""
import sys, math, csv
T95 = {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31, 9: 2.26, 10: 2.23}
def mean(x): return sum(x) / len(x)
def sd(x):
    if len(x) < 2: return 0.0
    m = mean(x); return math.sqrt(sum((v - m) ** 2 for v in x) / (len(x) - 1))
def main():
    f, base = sys.argv[1], sys.argv[2]
    met = sys.argv[3] if len(sys.argv) > 3 else "total"
    pre = sys.argv[4] if len(sys.argv) > 4 else "m.arm"
    runs = {}; order = []; bad = {}
    for r in csv.DictReader(open(f), delimiter="\t"):
        a = r["arm"]
        if a not in order: order.append(a)
        try: v = float(r[met])
        except (ValueError, KeyError, TypeError): v = None
        if r["rc"] == "0" and r["verify"] == "OK" and v is not None: runs.setdefault(a, []).append(v)
        else: bad[a] = bad.get(a, 0) + 1
    for a in order:
        x = runs.get(a, [])
        if not x:
            print("%s.%s.%s_n=0 (failed %d)" % (pre, a, met, bad.get(a, 0))); continue
        print("%s.%s.%s_mean=%.3f s" % (pre, a, met, mean(x)))
        print("%s.%s.%s_n=%d" % (pre, a, met, len(x)))
        if len(x) > 1: print("%s.%s.%s_sd=%.3f s" % (pre, a, met, sd(x)))
        print("LINE %s mean %.2f s (n=%d%s)" % (a, mean(x), len(x), ", sd %.2f" % sd(x) if len(x) > 1 else ""))
    b = runs.get(base, [])
    for a in order:
        if a == base or not runs.get(a) or not b: continue
        x = runs[a]; k = min(len(x), len(b))
        d = [x[i] - b[i] for i in range(k)]
        md = mean(d); pct = 100 * md / mean(b[:k]) if mean(b[:k]) else 0
        ci = ""
        if k > 1:
            h = T95.get(k - 1, 1.96) * sd(d) / math.sqrt(k); ci = " +-%.2f" % h
            print("%s.%s_minus_%s.%s_ci95=%.3f s" % (pre, a, base, met, h))
        print("%s.%s_minus_%s.%s_diff=%.3f s" % (pre, a, base, met, md))
        print("%s.%s_minus_%s.%s_pct=%.2f %%" % (pre, a, base, met, pct))
        print("LINE %s - %s = %+.2f s%s (%+.1f %%, %d pairs)" % (a, base, md, ci, pct, k))
main()

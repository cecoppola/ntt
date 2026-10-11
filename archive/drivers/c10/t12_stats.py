#!/usr/bin/env python3
# t12_stats.py <runs.tsv>: counts by class, crash-free run count, 95% upper bound on the crash (segv) rate, slow/fast mode counts.
# columns: label n rc class wall total init bs dm a2a mode start_et phase segv maxstill.  Thresholds: slow < 11.95, fast > 12.3 GB/s per APU thread (T05).
import sys, math
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1])][1:]
rows = [r for r in rows if len(r) >= 15]
n = len(rows); c = {}
for r in rows: c[r[3]] = c.get(r[3], 0) + 1
k = c.get('segv', 0); h = c.get('hang', 0) + c.get('timeout', 0)
def upper(k, n):   # exact one-sided 95% Clopper-Pearson upper bound on a binomial rate
    if n == 0: return float('nan')
    if k >= n: return 1.0
    lo, hi = 0.0, 1.0
    for _ in range(60):
        p = (lo + hi) / 2
        cdf = sum(math.comb(n, i) * p ** i * (1 - p) ** (n - i) for i in range(k + 1))
        lo, hi = (p, hi) if cdf > 0.05 else (lo, p)
    return hi
print("runs %d: ok %d, segv %d, hang %d, timeout %d, fail %d" % (n, c.get('ok', 0), k, c.get('hang', 0), c.get('timeout', 0), c.get('fail', 0)))
print("crash-free runs (no segv): %d of %d" % (n - k, n))
if k == 0 and n: print("crash (segv) rate: 0/%d; 95%% upper bound = rule of three 3/n = %.1f%% (exact %.1f%%); base measured about 1/26 = 3.8%%" % (n, 300.0 / n, 100 * upper(0, n)))
elif n: print("crash (segv) rate: %d/%d = %.1f%%; 95%% upper bound (exact) %.1f%%" % (k, n, 100.0 * k / n, 100 * upper(k, n)))
print("hang/timeout rate: %d/%d; 95%% upper bound (exact) %.1f%%" % (h, n, 100 * upper(h, n) if n else float('nan')))
ok = [r for r in rows if r[3] == 'ok']
m = {}
for r in ok: m[r[10]] = m.get(r[10], 0) + 1
print("mode of ok runs (a2a mean GB/s per APU thread; slow < 11.95, fast > 12.3): slow %d, fast %d, mid %d, unknown %d" % (m.get('slow', 0), m.get('fast', 0), m.get('mid', 0), m.get('NA', 0)))
def mean(xs): return sum(xs) / len(xs) if xs else float('nan')
def f(r, i):
    try: return float(r[i])
    except ValueError: return None
for name in ('slow', 'fast'):
    t = [f(r, 5) for r in ok if r[10] == name and f(r, 5) is not None]
    if t: print("  %s: mean total %.1f s (n %d, min %.1f, max %.1f)" % (name, mean(t), len(t), min(t), max(t)))
ms = [int(r[14]) for r in rows if r[14].isdigit()]
if ms: print("max log-silence seen in any run: %d s (watchdog 300 s)" % max(ms))

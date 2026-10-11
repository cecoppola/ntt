#!/usr/bin/env python3
# s36_stats.py <ab.tsv> [--rule]  - paired B-A statistics of the S36 4-node ABBA rounds (tag 'ab') and the 2-node pair (tag 'p2').
# ab.tsv columns: tag round arm rc wall_s verify ecalc_total_s devmax_gb devmean_gb pool_gb vslot_gb segv nodes endtime
# A round counts when both its arms exited rc 0 with VERIFY OK.  95 % CI = mean +- t(n-1) sd / sqrt(n).
# --rule: exit 0 = STOP (n >= 4 and (the wall CI excludes 0, or its half-width <= 15 s) or n >= 8), exit 1 = CONTINUE.
import sys, math
T = {1:12.706,2:4.303,3:3.182,4:2.776,5:2.571,6:2.447,7:2.365,8:2.306,9:2.262,10:2.228,11:2.201,12:2.179,13:2.160,14:2.145,15:2.131}
def load(path):
    rows = []
    for l in open(path):
        f = l.rstrip('\n').split('\t')
        if len(f) < 12: continue
        rows.append(f)
    return rows
def num(x):
    try: return float(x)
    except: return None
def pairs(rows, tag):
    by = {}
    for f in rows:
        if f[0] != tag: continue
        if f[3] != '0' or f[5] != 'VERIFY OK': continue
        by.setdefault(f[1], {})[f[2]] = f
    return [(r, d['A'], d['B']) for r, d in sorted(by.items(), key=lambda kv: int(kv[0])) if 'A' in d and 'B' in d]
def ci(diffs):
    n = len(diffs)
    if n == 0: return None
    m = sum(diffs) / n
    if n == 1: return (n, m, float('nan'), float('nan'), float('nan'), float('nan'))
    sd = math.sqrt(sum((x - m) ** 2 for x in diffs) / (n - 1)); t = T.get(n - 1, 1.96); h = t * sd / math.sqrt(n)
    return (n, m, sd, h, m - h, m + h)
def fmt(name, diffs, unit):
    c = ci(diffs)
    if c is None: return "%s: no complete pairs" % name
    n, m, sd, h, lo, hi = c
    if n == 1: return "%s: 1 pair, B-A %+.2f %s (no CI)" % (name, m, unit)
    sig = "CI excludes 0 (p<0.05)" if (lo > 0 or hi < 0) else "CI includes 0"
    return "%s: %d pairs, mean B-A %+.2f %s, sd %.2f, 95%% CI [%+.2f, %+.2f] (half-width %.2f), %s" % (name, n, m, unit, sd, lo, hi, h, sig)
def series(ps, idx):
    out = []
    for r, a, b in ps:
        x, y = num(a[idx]), num(b[idx])
        if x is not None and y is not None: out.append(y - x)
    return out
def report(rows, tag, title):
    ps = pairs(rows, tag)
    print("%s: %d complete pair(s) (rounds %s)" % (title, len(ps), ",".join(r for r, _, _ in ps)))
    if not ps: return ps
    for name, idx, unit in (("wall (driver)", 4, "s"), ("ecalc total", 6, "s"), ("peak device memory per node, max over nodes", 7, "GB"), ("peak device memory per node, mean over nodes", 8, "GB"), ("comm pool (largest APU line)", 9, "GB")):
        print("  " + fmt(name, series(ps, idx), unit))
    for k, (r, a, b) in enumerate(ps):
        print("    round %s: wall A %s B %s | devmax A %s B %s | ecalc total A %s B %s" % (r, a[4], b[4], a[7], b[7], a[6], b[6]))
    for arm in "AB":
        w = [num(f[4]) for f in rows if f[0] == tag and f[2] == arm and f[3] == '0' and f[5] == 'VERIFY OK' and num(f[4]) is not None]
        if w: print("  arm %s: %d good runs, mean wall %.1f s" % (arm, len(w), sum(w) / len(w)))
    return ps
if __name__ == '__main__':
    rows = load(sys.argv[1])
    if '--rule' in sys.argv:
        ps = pairs(rows, 'ab'); d = series(ps, 4); c = ci(d)
        if c is None or c[0] < 4: print("CONTINUE: %d complete pairs (< 4)" % (c[0] if c else 0)); sys.exit(1)
        n, m, sd, h, lo, hi = c
        if lo > 0 or hi < 0: print("STOP: wall CI [%+.1f, %+.1f] excludes 0 after %d pairs" % (lo, hi, n)); sys.exit(0)
        if h <= 15: print("STOP: wall CI half-width %.1f s <= 15 s after %d pairs" % (h, n)); sys.exit(0)
        if n >= 8: print("STOP: 8 complete pairs reached (half-width %.1f s)" % h); sys.exit(0)
        print("CONTINUE: %d pairs, wall CI half-width %.1f s, CI [%+.1f, %+.1f]" % (n, h, lo, hi)); sys.exit(1)
    report(rows, 'ab', "4-node ABBA (A = base + X2 + DIST_GEN=1, B = A + COMM_LAYER_VSLOT_SHARE=1)")
    print()
    report(rows, 'p2', "2-node pair (A = base + X2, B = A + COMM_LAYER_VSLOT_SHARE=1)")
    print()
    nf = [f for f in rows if f[3] != '0' or f[5] != 'VERIFY OK']
    print("failed runs: %d" % len(nf))
    for f in nf: print("  %s r%s %s rc %s %s segv %s" % (f[0], f[1], f[2], f[3], f[5], f[11]))

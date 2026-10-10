#!/usr/bin/env python3
# s43_stats.py <ab.tsv> [--rule]: paired B-A statistics per tag (p1, p2 = the two concurrent 2-node pairs, ab = 4 nodes).
# ab.tsv columns: tag round arm rc wall_s verify ecalc_total_s nodes endtime.  A round counts when both arms are rc 0 + VERIFY OK.
# --rule (tag ab): exit 0 = STOP (n >= 3 and the wall 95% CI excludes 0, or n >= 5), exit 1 = CONTINUE.
import sys, math
T = {1:12.706,2:4.303,3:3.182,4:2.776,5:2.571,6:2.447,7:2.365,8:2.306}
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1]) if l.count('\t') >= 7]
def num(x):
    try: return float(x)
    except: return None
def pairs(tag):
    by = {}
    for f in rows:
        if f[0] == tag and f[3] == '0' and f[5] == 'VERIFY OK': by.setdefault(f[1], {})[f[2]] = f
    return [(r, d['A'], d['B']) for r, d in sorted(by.items(), key=lambda kv: int(kv[0])) if 'A' in d and 'B' in d]
def ci(d):
    n = len(d)
    if n == 0: return None
    m = sum(d) / n
    if n == 1: return (n, m, None, None, None)
    sd = math.sqrt(sum((x - m) ** 2 for x in d) / (n - 1)); h = T.get(n - 1, 2.0) * sd / math.sqrt(n)
    return (n, m, h, m - h, m + h)
def fmt(name, d):
    c = ci(d)
    if c is None: return "%s: no complete pairs" % name
    if c[0] == 1: return "%s: 1 pair, B-A %+.2f s (no CI)" % (name, c[1])
    n, m, h, lo, hi = c
    return "%s: %d pairs, mean B-A %+.2f s, 95%% CI [%+.2f, %+.2f], %s" % (name, n, m, lo, hi, "excludes 0 (p<0.05)" if lo > 0 or hi < 0 else "includes 0")
if '--rule' in sys.argv:
    c = ci([float(b[4]) - float(a[4]) for _, a, b in pairs('ab')])
    if c is None or c[0] < 3: print("CONTINUE: < 3 complete pairs"); sys.exit(1)
    if c[0] >= 2 and c[1] is not None and c[2] is not None and (c[3] > 0 or c[4] < 0): print("STOP: 4-node wall CI [%+.1f, %+.1f] excludes 0 after %d pairs" % (c[3], c[4], c[0])); sys.exit(0)
    if c[0] >= 5: print("STOP: 5 pairs reached"); sys.exit(0)
    print("CONTINUE: %d pairs, CI [%+.1f, %+.1f]" % (c[0], c[3], c[4])); sys.exit(1)
for tag, title in (("p1", "2-node pair 1 (nodes 1-2)"), ("p2", "2-node pair 2 (nodes 3-4)"), ("ab", "4-node ABBA")):
    ps = pairs(tag)
    print("%s: %d complete pair(s)" % (title, len(ps)))
    print("  " + fmt("wall (driver)", [float(b[4]) - float(a[4]) for _, a, b in ps]))
    e = [(num(b[6]) - num(a[6])) for _, a, b in ps if num(a[6]) is not None and num(b[6]) is not None]
    print("  " + fmt("ecalc total", e))
    for r, a, b in ps: print("    round %s: wall A %s B %s | ecalc total A %s B %s" % (r, a[4], b[4], a[6], b[6]))
bad = [f for f in rows if f[3] != '0' or f[5] != 'VERIFY OK']
print("failed runs: %d" % len(bad))
for f in bad: print("  %s r%s %s rc %s %s" % (f[0], f[1], f[2], f[3], f[5]))

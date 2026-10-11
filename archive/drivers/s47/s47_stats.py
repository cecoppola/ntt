#!/usr/bin/env python3
# s47_stats.py <ab.tsv> [--rule TAG]: paired B-A statistics per comparison (tag i = MN_T_CHUNK_MB A 1024 / B 2048; tag ii = X2 A off / B on).
# ab.tsv columns: tag round arm rc wall_s verify ecalc_total_s end.  A round counts when both arms are rc 0 + VERIFY OK.
# --rule TAG: exit 0 = STOP (n >= MINR and the wall 95% CI excludes 0 or its half-width <= HALF s, or n >= CAP), exit 1 = CONTINUE.
import sys, math, os
MINR = int(os.environ.get('S47_MINR', 6)); CAP = int(os.environ.get('S47_CAP', 10)); HALF = float(os.environ.get('S47_HALF', 3.0))
T = {1:12.706,2:4.303,3:3.182,4:2.776,5:2.571,6:2.447,7:2.365,8:2.306,9:2.262,10:2.228,11:2.201,12:2.179,13:2.160,14:2.145,15:2.131}
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1]) if l.count('\t') >= 6]
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
    neg = sum(1 for x in d if x < 0)
    return "%s: %d pairs, mean B-A %+.2f s, 95%% CI [%+.2f, %+.2f] (half-width %.2f), %s; B faster in %d of %d" % (name, n, m, lo, hi, h, "excludes 0 (p<0.05)" if lo > 0 or hi < 0 else "includes 0", neg, n)
TITLE = {"i": "(i) MN_T_CHUNK_MB: A = 1024, B = 2048", "ii": "(ii) X2: A = off (COMM_LAYER_INTER2=0 COMM_LAYER_VSLOT_POOL=0), B = on"}
if '--rule' in sys.argv:
    tag = sys.argv[sys.argv.index('--rule') + 1]
    c = ci([float(b[4]) - float(a[4]) for _, a, b in pairs(tag)])
    n = 0 if c is None else c[0]
    if n >= CAP: print("STOP: %d pairs (cap)" % n); sys.exit(0)
    if n >= MINR and c[2] is not None:
        if c[3] > 0 or c[4] < 0: print("STOP: wall CI [%+.1f, %+.1f] excludes 0 after %d pairs" % (c[3], c[4], n)); sys.exit(0)
        if c[2] <= HALF: print("STOP: CI half-width %.1f <= %.1f s after %d pairs" % (c[2], HALF, n)); sys.exit(0)
    print("CONTINUE: %d pairs%s" % (n, "" if c is None or c[3] is None else ", CI [%+.1f, %+.1f]" % (c[3], c[4]))); sys.exit(1)
for tag in ("i", "ii"):
    ps = pairs(tag)
    print("%s: %d complete pair(s)" % (TITLE[tag], len(ps)))
    print("  " + fmt("wall (driver, launch to exit)", [float(b[4]) - float(a[4]) for _, a, b in ps]))
    e = [(num(b[6]) - num(a[6])) for _, a, b in ps if num(a[6]) is not None and num(b[6]) is not None]
    print("  " + fmt("ecalc total", e))
    for r, a, b in ps: print("    round %s: wall A %s B %s | ecalc total A %s B %s" % (r, a[4], b[4], a[6], b[6]))
bad = [f for f in rows if f[3] != '0' or f[5] != 'VERIFY OK']
print("failed runs: %d" % len(bad))
for f in bad: print("  %s r%s %s rc %s %s" % (f[0], f[1], f[2], f[3], f[5]))

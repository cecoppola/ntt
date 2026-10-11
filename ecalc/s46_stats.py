#!/usr/bin/env python3
"""s46_stats.py (S46): per-arm counts of a runs.tsv (s46_drive.sh / s46_soak6.sh) and exact Fisher tests between arms.
usage: s46_stats.py runs.tsv   (columns: label cycle arm rc wall result ... phase; result in ok|segv|hang|timeout|fail)"""
import sys, csv
from math import comb
def fisher(a, n1, b, n2):
    """2x2 table (a of n1 events vs b of n2): one-sided p for 'arm 1 has more events' and two-sided p (sum of tables no more probable)."""
    K, N = a + b, n1 + n2
    if N == 0 or n1 == 0 or n2 == 0: return float('nan'), float('nan')
    den = comb(N, K)
    pr = {x: comb(n1, x) * comb(n2, K - x) / den for x in range(max(0, K - n2), min(K, n1) + 1)}
    one = sum(p for x, p in pr.items() if x >= a)
    two = sum(p for p in pr.values() if p <= pr[a] * (1 + 1e-9))
    return min(one, 1.0), min(two, 1.0)
rows = [r for r in csv.DictReader(open(sys.argv[1]), delimiter='\t')]
arms = []
for r in rows:
    if r['arm'] not in arms: arms.append(r['arm'])
st = {}
for a in arms:
    rr = [r for r in rows if r['arm'] == a]
    c = {k: sum(1 for r in rr if r['result'] == k) for k in ('ok', 'segv', 'hang', 'timeout', 'fail')}
    w = [float(r['wall']) for r in rr if r['result'] == 'ok']
    t = [float(r['total']) for r in rr if r['result'] == 'ok' and r.get('total', 'NA') not in ('NA', '')]
    hs = sum(1 for r in rr if r['result'] in ('hang', 'timeout') and r.get('phase') == 'startup')
    st[a] = dict(n=len(rr), c=c, bad=len(rr) - c['ok'], hs=hs)
    print("arm %s: runs %d, ok %d, segv %d, hang %d (startup %d), timeout %d, other fail %d, rate segv %.1f %%, any non-OK %.1f %%, mean wall %.1f s, mean ecalc total %.1f s"
          % (a, len(rr), c['ok'], c['segv'], c['hang'], hs, c['timeout'], c['fail'], 100.0 * c['segv'] / max(1, len(rr)),
             100.0 * (len(rr) - c['ok']) / max(1, len(rr)), sum(w) / max(1, len(w)), sum(t) / max(1, len(t))))
for i in range(len(arms)):
    for j in range(i + 1, len(arms)):
        a, b = arms[i], arms[j]
        for what, ka, kb in (('segv', st[a]['c']['segv'], st[b]['c']['segv']), ('any non-OK', st[a]['bad'], st[b]['bad'])):
            o, t = fisher(ka, st[a]['n'], kb, st[b]['n'])
            print("fisher %s vs %s, %s: %d/%d vs %d/%d, one-sided (%s > %s) p = %.4f, two-sided p = %.4f" % (a, b, what, ka, st[a]['n'], kb, st[b]['n'], a, b, o, t))

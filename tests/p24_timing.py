#!/usr/bin/env python3
"""P24 (Phase 15 Batch 3): the per-piece lines of RNS_VERBOSE=1 runs (dist_mn node 0: 2^L = ...: redistribute a ntt b crt c out d
spills+carry e total f s), summed per run: pieces, plane points, seconds per phase, and seconds per 2^30 points (MEASURED on the node;
the log files are the evidence).  python3 tests/p24_timing.py results/P2415/t10_p0_1.log ...
"""
import re, sys
PAT = re.compile(r'^dist_mn node 0: 2\^(\d+) = .*?(, 4 primes, P24|, 4 primes|, 3 primes|) \(rows.*?\): redistribute ([\d.]+) ntt ([\d.]+) crt ([\d.]+) out ([\d.]+) spills\+carry ([\d.]+) total ([\d.]+) s')
KEYS = ('redistribute', 'ntt', 'crt', 'out', 'carry', 'total')
for fn in sys.argv[1:]:
    rows = {}
    for line in open(fn, errors='replace'):
        m = PAT.match(line)
        if not m: continue
        L = int(m.group(1)); tag = 'P24' if 'P24' in m.group(2) else '18'
        for key in (tag, '%s 2^%d' % (tag, L)):                     # the run's sum, and per plane size (the per-point price at the same plane)
            r = rows.setdefault(key, dict(n=0, pts=0, **{k: 0.0 for k in KEYS}))
            r['n'] += 1; r['pts'] += 1 << L
            for k, v in zip(KEYS, m.groups()[2:]): r[k] += float(v)
    tot = re.findall(r'^total\s+([\d.]+) s\s+\(bs ([\d.]+) .*? dm ([\d.]+)', open(fn, errors='replace').read(), re.M)
    print('%s: %s' % (fn, 'total %s s (bs %s, dm %s)' % tot[-1] if tot else 'no total line'))
    for tag, r in sorted(rows.items()):
        g = r['pts'] / 2 ** 30
        print('  %-9s pieces %3d, 2^30-point planes %6.2f | s: %s | s per 2^30 points: %s' % (tag, r['n'], g,
              ' '.join('%s %.2f' % (k, r[k]) for k in KEYS), ' '.join('%s %.3f' % (k, r[k] / g) for k in KEYS)))

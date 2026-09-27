#!/usr/bin/env python3
"""np15_ratio.py <log at three primes> <log at four primes> - Phase 15 NP: the cost of four primes against three per product.
The two runs are the same size with RNS_STRATEGY=C and RNS_VERBOSE=1 under ECALC_NP=auto: the first with the real bound (every
distributed product at three primes), the second with ECALC_NP_AUTO_TERMS=1 (every one at four).  The product lines of node 0
(`dist ...` at size 1, `dist_mn node 0: ...` at size > 1) are paired in order (the grids do not depend on the prime count) and
their times compared: total, and the transform + CRT part (ntt + crt).  Prints the ratio per size class and overall (MEASURED)."""
import re, sys
from collections import defaultdict


def lines(path):
    out = []
    for l in open(path, errors='replace'):
        if not (l.startswith('dist_mn node 0:') or l.startswith('dist ')): continue
        p = re.search(r', ([34]) primes', l); t = re.search(r'(?:redistribute|load) ([0-9.]+) ntt ([0-9.]+) crt ([0-9.]+).*?total ([0-9.]+) s', l)
        if not p or not t: continue
        if l.startswith('dist_mn'): n = re.search(r'; (\d+) \+ (\d+)(?: \+ x)? limbs', l); n = int(n.group(1)) + int(n.group(2)) if n else 0
        else: n = re.search(r'\((\d+) limbs', l); n = int(n.group(1)) if n else 0
        out.append(dict(kind='dist_mn' if l.startswith('dist_mn') else 'dist', n=n, np=int(p.group(1)), ld=float(t.group(1)), ntt=float(t.group(2)), crt=float(t.group(3)), tot=float(t.group(4))))
    return out

def main():
    a, b = lines(sys.argv[1]), lines(sys.argv[2])
    print('%s: %d product lines (%d at 3 primes); %s: %d (%d at 4)' % (sys.argv[1], len(a), sum(x['np'] == 3 for x in a), sys.argv[2], len(b), sum(x['np'] == 4 for x in b)))
    cls = defaultdict(lambda: [0, 0.0, 0.0, 0.0, 0.0])
    npair = 0
    for x, y in zip(a, b):
        if x['kind'] != y['kind'] or x['n'] != y['n'] or x['np'] != 3 or y['np'] != 4: continue
        k = (x['kind'], x['n'].bit_length())
        c = cls[k]; c[0] += 1; c[1] += x['tot']; c[2] += y['tot']; c[3] += x['ntt'] + x['crt']; c[4] += y['ntt'] + y['crt']; npair += 1
    print('%d pairs (same tier, same limbs, 3 vs 4 primes)' % npair)
    print('%-16s %6s %5s | %9s %9s %6s | %9s %9s %6s' % ('tier', 'limbs', 'n', 'total 3', 'total 4', 'ratio', 'ntt+crt 3', 'ntt+crt 4', 'ratio'))
    T = [0, 0.0, 0.0, 0.0, 0.0]
    for k in sorted(cls):
        c = cls[k]
        for i in range(5): T[i] += c[i]
        print('%-16s ~2^%-3d %5d | %9.3f %9.3f %6.3f | %9.3f %9.3f %6.3f' % (k[0], k[1], c[0], c[1], c[2], c[2] / c[1] if c[1] else 0, c[3], c[4], c[4] / c[3] if c[3] else 0))
    if T[0]: print('%-22s %5d | %9.3f %9.3f %6.3f | %9.3f %9.3f %6.3f' % ('all', T[0], T[1], T[2], T[2] / T[1], T[3], T[4], T[4] / T[3]))

if __name__ == '__main__':
    main()

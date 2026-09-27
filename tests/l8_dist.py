#!/usr/bin/env python3
"""L8 (Phase 15, throwaway): the distribution of transform lengths and each length's share of the transform time.
10^11 one node (S = 256): the batch tier from the MEASURED ntt lines (results/V314/e11_def.log), everything above it from the
plan (results/L815/L815_plan1.txt) with l8_pad.py's MODELLED transform time.
576 target: the plan (results/L815/L815_plan576.txt); a product over g nodes costs every node (pieces x L / g) points, the leaf
and the single-node chain L per node; the time weight is those per-node points x the 2^k cost per point (MODELLED; the mn
tier's pieces are dominated by exchanges, so this is the transform share only), the leaf's batch tier from the top node's
modelled levels (l8_mn.py)."""
import os, re, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l8_pad as P

def fam(L):
    r = L
    while r % 2 == 0: r //= 2
    return r

def show(tag, rows):
    tot = sum(t for _, _, t in rows); by = {}
    for L, n, t in rows:
        a = by.setdefault(L, [0, 0.0]); a[0] += n; a[1] += t
    print('\n%s: %.1f s of transforms (modelled unless marked)' % (tag, tot))
    print('  length       products/pieces   seconds   share')
    for L in sorted(by): print('  %-12s %8d        %7.2f   %5.1f %%' % (P.tstr(L), by[L][0], by[L][1], 100 * by[L][1] / tot))
    f = {}
    for L, (n, t) in by.items(): f[fam(L)] = f.get(fam(L), 0) + t
    print('  by family: ' + ', '.join('%s 2^k %.1f %%' % (r if r > 1 else '', 100 * t / tot) for r, t in sorted(f.items())))

rows = []
for x in P.batch_levels('e11_def.log'):
    if x['tier'] == 'batch': rows.append((P.code_pick(2 * x['nl']), 2 * x['pairs'], x['ntt']))
for p in P.plan_products('L815_plan1.txt'):
    if p['nc'] >= 1 << 20: rows.append((p['L'], p['pieces'], p['pieces'] * 3 * P.wall(p['L'])))
show('10^11, one node, S = 256 (batch tier MEASURED ntt, above it MODELLED)', rows)

rows = []
txt = open(os.path.join(P.ROOT, 'results/L815/L815_plan576.txt')).read().splitlines()
for ln in txt:
    if not ln.startswith('plan '): continue
    m = re.search(r'dist_mn node 0: .* over (\d+) x 4 ranks .*?: (\d+) x (\d+) pieces, (\d+) formed.*\| piece (\d+) \+ (\d+) limbs, 2\^(\d+) points', ln)
    if m:
        g, pcs, L = int(m[1]), int(m[4]), 1 << int(m[7])
        rows.append((L, pcs, pcs * 3 * 3 * (L / (4 * g)) * P.c2(min(31, int(m[7]) - int(math.log2(4 * g)))) * 1e-12)); continue
    m = re.search(r'dist_db (\d+) limbs \| one plane of (3\*)?2\^(\d+) points \((B|C) form', ln)
    if m and int(m[1]) >= 1 << 20:
        L = (3 if m[2] else 1) << int(m[3]); rows.append((L, 1, 3 * P.wall(L))); continue
    m = re.search(r'dist_db (\d+) x (\d+) limbs: .* (\d+) formed.*\| (3\*)?2\^(\d+) points per piece \((B|C) form', ln)
    if m:
        L = (3 if m[4] else 1) << int(m[5]); rows.append((L, int(m[3]), int(m[3]) * 3 * P.wall(L)))
# the top node's leaf batch tier (S = 256): seeds of 179 limbs, 6.09e9 terms
N, S = 3509230438617 // 576, 256; spans = -(-N // S); nl1 = math.ceil(S * math.log10(3.509e12) / 18); pairs = spans // 2; l = 1
while pairs >= 1:
    nc = 2 * nl1 * 2 ** (l - 1)
    if nc + 1 > 1 << 30: break
    L = P.code_pick(nc); rows.append((L, 2 * pairs, P.batch_model(pairs, L))); l += 1; pairs //= 2
show('4.25e13 on 576 nodes, per node (the top node\'s leaf batch tier + node 0\'s plan; all MODELLED)', rows)

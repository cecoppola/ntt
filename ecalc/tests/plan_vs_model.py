#!/usr/bin/env python3
"""tests/plan_vs_model.py - Phase 13d D2: agent L's C-printed piece table (tests/plan_sweep.sh, MN_PLAN_ONLY) against the model's
(mn_model.py --plan, the same columns), size by size: every column that differs, and the steps of each.

    ./tests/plan_vs_model.py results/L13d_plan576.txt results/D2_13d_plan576.txt
"""
import re, sys

COLS = ('tree', 'tree_max', 'recip', 'div')

def load(fn):
    out = {}
    for line in open(fn):
        if line.startswith('#'): continue
        m = re.match(r'([\d.e+]+)\s+tree\s+(\d+)\s+tree_max\s+(\d+)\s+recip\s+(\d+)\s+div\s+(\d+).*levels\s+(\S+)', line)
        if not m: continue
        out[round(float(m.group(1)) / 1e11)] = dict(zip(COLS, map(int, m.groups()[1:5])), levels=m.group(6))
    return out

def steps(t, col):
    ks = sorted(t); return [(a, b, t[a][col], t[b][col]) for a, b in zip(ks, ks[1:]) if t[a][col] != t[b][col]]

def main():
    L, D = load(sys.argv[1]), load(sys.argv[2])
    ks = sorted(set(L) & set(D))
    print('# %d sizes in both (2.0-6.0e13 at 0.01e13); C = the C code (L, MN_PLAN_ONLY), M = the model (D2, mn_model.py --plan)' % len(ks))
    for col in COLS + ('levels',):
        diff = [k for k in ks if L[k][col] != D[k][col]]
        print('%-8s agree at %d / %d sizes' % (col, len(ks) - len(diff), len(ks)))
        for k in diff[:40]: print('    %.2fe13  C %-40s M %s' % (k / 100, L[k][col], D[k][col]))
        if len(diff) > 40: print('    ... %d more' % (len(diff) - 40))
    for col in ('tree_max',):
        sl = steps({k: L[k] for k in ks}, col); sd = steps({k: D[k] for k in ks}, col)
        print('# %s steps: C %s' % (col, ' '.join('%.2f:%d->%d' % (b / 100, x, y) for a, b, x, y in sl)))
        print('# %s steps: M %s' % (col, ' '.join('%.2f:%d->%d' % (b / 100, x, y) for a, b, x, y in sd)))

if __name__ == '__main__':
    main()

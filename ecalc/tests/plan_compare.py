#!/usr/bin/env python3
"""tests/plan_compare.py <run.log> <plan.log> - Phase 13d L: a real run's product lines (RNS_VERBOSE=1) against MN_PLAN_ONLY's.

The run's lines: `dist_mn node 0: A x B limbs over g x 4 ranks (cap 2^c): ka x kb pieces, f formed, s skipped` (every mn product
of node 0, in order) and, for a single-process run, `dist_db A x B limbs: ka x kb pieces of ...` / `dist_db N limbs:` (the dist
tier; at size > 1 the node-processes' dist_db lines interleave in one log and are not compared).  The plan's: the same text
after `plan <phase> <what>: `.  The two sequences are aligned in order; a pair agrees when the grid, the formed and skipped
counts, g and the cap are equal; the operand sizes are compared as a relative difference (the plan predicts them)."""
import re, sys

MN = re.compile(r'dist_mn node 0: (\d+) x (\d+) limbs over (\d+) x 4 ranks \(cap 2\^(\d+)\): (\d+) x (\d+) pieces, (\d+) formed, (\d+) skipped')
DBG = re.compile(r'dist_db (\d+) x (\d+) limbs: (\d+) x (\d+) pieces of \d+ \+ \d+, (\d+) formed, (\d+) skipped')
DB1 = re.compile(r'dist_db (\d+) limbs[: ]')

def products(path, plan):
    out = []
    for line in open(path, errors='replace'):
        if plan and not line.startswith('plan '): continue
        if not plan and line.startswith('plan '): continue
        what = line.split(':')[0][5:].strip() if plan else ''
        m = MN.search(line)
        if m:
            a, b, g, c, ka, kb, f, s = map(int, m.groups()); out.append(dict(t='mn', a=a, b=b, g=g, cap=c, ka=ka, kb=kb, f=f, s=s, what=what)); continue
        m = DBG.search(line)
        if m:
            a, b, ka, kb, f, s = map(int, m.groups()); out.append(dict(t='db', a=a, b=b, g=1, cap=0, ka=ka, kb=kb, f=f, s=s, what=what)); continue
        m = DB1.search(line)
        if m:
            n = int(m.group(1)); out.append(dict(t='db', a=n, b=0, g=1, cap=0, ka=1, kb=1, f=1, s=0, what=what))
    return out

def main():
    run, plan = products(sys.argv[1], False), products(sys.argv[2], True)
    multi = any(p['t'] == 'mn' for p in run)
    if multi: run = [p for p in run if p['t'] == 'mn']; plan = [p for p in plan if p['t'] == 'mn']
    else: plan = [p for p in plan if p['t'] == 'db']
    n = max(len(run), len(plan)); bad = 0; worst = 0.0
    print("%3s %-34s | %-44s | %-44s | %s" % ("#", "plan product", "run: A x B, grid, formed/skipped", "plan: A x B, grid, formed/skipped", "verdict"))
    for i in range(n):
        r = run[i] if i < len(run) else None; p = plan[i] if i < len(plan) else None
        fmt = lambda x: "-" if x is None else ("%d x %d g%d c%d %dx%d %d/%d" % (x['a'], x['b'], x['g'], x['cap'], x['ka'], x['kb'], x['f'], x['s']) if x['t'] == 'mn' or x['b'] else "%d limbs (one plane)" % x['a'])
        if r is None or p is None: v = "MISSING"; bad += 1
        else:
            same = all(r[k] == p[k] for k in ('g', 'cap', 'ka', 'kb', 'f', 's'))
            da = abs(r['a'] - p['a']) / max(r['a'], 1); db = abs(r['b'] - p['b']) / max(r['b'], 1) if r['b'] or p['b'] else 0.0
            worst = max(worst, da, db)
            dl = "sizes %+d %+d" % (p['a'] - r['a'], p['b'] - r['b'])
            v = ("agrees" if same else "DIFFERS") + "; " + dl
            if not same: bad += 1
        print("%3d %-34s | %-44s | %-44s | %s" % (i, (p or {}).get('what', '')[:34], fmt(r), fmt(p), v))
    print("compare: %d run products, %d plan products, %d disagree in grid/formed/skipped/g/cap or count; largest relative size difference %.2e" % (len(run), len(plan), bad, worst))
    return 1 if bad else 0

if __name__ == '__main__': sys.exit(main())

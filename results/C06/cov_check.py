#!/usr/bin/env python3
"""C06: compare the group sizes that run the layered communicator under MN_GROUPS=2,4,..,64,192,<g> (cut groups included) with the sizes
the pool rule (binsplit.c vslot_prealloc_env) gave a COMM_LAYER_VSLOT_MB_<size> to; input: the MN_PLAN_ONLY transcript of c06_cov.sh"""
import re, sys
pow2 = lambda x: x > 1 and not x & (x - 1)
cur = None; got = {}; pool = {}
for l in open(sys.argv[1]):
    m = re.match(r"== g (\d+)", l)
    if m: cur = int(m.group(1)); got[cur] = set(); continue
    m = re.match(r"plan vslot-prealloc\s+COMM_LAYER_VSLOT_MB_(\d+)=(\d+)", l)
    if m: got[cur].add((int(m.group(1)), int(m.group(2))))
    m = re.search(r"comm_ofi pools of (\d+) MiB", l)
    if m: pool[cur] = int(m.group(1))
bad = 0
for g, s in sorted(got.items(), reverse=True):
    sched = [x for x in [2, 4, 8, 16, 32, 64, 192] if x < g] + [g]
    need = set()
    for x in sched:
        if x < g or True:
            if not pow2(x) and x > 1: need.add(x)
        if x < g and g % x and not pow2(g % x) and g % x > 1: need.add(g % x)
    have = {a for a, b in s}; mbs = {b for a, b in s}
    miss = need - have
    print(f"g {g}: layered sizes {sorted(need)}; env set for {sorted(have)}; missing {sorted(miss) or 'none'}; slot MiB {sorted(mbs)} (one value = SHARE); comm_ofi pool {pool.get(g)} MiB")
    bad += bool(miss)
print("coverage:", "OK, every layered group size has its slot size" if not bad else f"{bad} schedule(s) with a missing size")

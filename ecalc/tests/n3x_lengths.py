#!/usr/bin/env python3
"""Phase 15 N3x: the transform time of a run by tier and length, from its RNS_VERBOSE=1 log (the batch-local, striped
batch, mdev and dist lines carry the length and the ntt seconds of each call).  Usage: n3x_lengths.py <log> [<log> ...]"""
import re, sys
from collections import defaultdict

pat = [
    ("batch-local", re.compile(r"^batch-local N=(\d+) .*?L=(3\*)?2\^(\d+).*?ntt ([\d.]+)")),
    ("batch", re.compile(r"^batch N=(\d+) L=(3\*)?2\^(\d+).*?ntt ([\d.]+)")),
    ("mdev", re.compile(r"^mdev\S* (3\*)?2\^(\d+) .*?fwd ([\d.]+) inv ([\d.]+)")),
    ("dist", re.compile(r"^dist (\S+ )?(3\*)?2\^(\d+) .*?ntt ([\d.]+)")),
]
for fn in sys.argv[1:]:
    t = defaultdict(float); n = defaultdict(int)
    for line in open(fn, errors="replace"):
        for tier, p in pat:
            m = p.search(line)
            if not m: continue
            g = m.groups()
            if tier in ("batch-local", "batch"): key, s = (tier, (g[1] or "") + "2^" + g[2]), float(g[3])
            elif tier == "mdev": key, s = (tier, (g[0] or "") + "2^" + g[1]), float(g[2]) + float(g[3])
            else: key, s = ("dist " + (g[0] or "").strip(), (g[1] or "") + "2^" + g[2]), float(g[3])
            t[key] += s; n[key] += 1
            break
    tot = sum(t.values()); r3 = sum(v for k, v in t.items() if k[1].startswith("3*"))
    print(f"== {fn}: transform (ntt) seconds by tier and length; total {tot:.2f} s, 3*2^k {r3:.2f} s ({100 * r3 / max(tot, 1e-9):.1f} %)")
    for k in sorted(t, key=lambda k: -t[k]):
        print(f"  {k[0]:<14} {k[1]:<8} {n[k]:6d} calls {t[k]:8.2f} s")

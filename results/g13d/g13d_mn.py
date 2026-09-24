#!/usr/bin/env python3
"""G13d (c): per tree level, each node's mn-tier pieces (the 'dist_mn node N: A x B limbs over S x 4 ranks (cap 2^c): ka x kb
pieces' lines up to 'mn: node 0 level L'), then the mn-tier products after the tree (reciprocal + division), from ~/g13d/<tag>.log."""
import os, re, sys
D = os.path.expanduser("~/g13d")
for t in sys.argv[1:]:
    s = open(os.path.join(D, t + ".log"), errors="replace").read()
    lv = [m.end() for m in re.finditer(r"^mn: node 0 level \d+ .*$", s, re.M)]
    prev, out = 0, []
    for i, e in enumerate(lv + [len(s)]):
        seg = s[prev:e]; prev = e
        d = {}
        for n, a, b, S, c, ka, kb, fo in re.findall(r"dist_mn node (\d+): (\d+) x (\d+) limbs over (\d+) x \d+ ranks \(cap 2\^(\d+)\): (\d+) x (\d+) pieces, (\d+) formed", seg):
            d.setdefault((int(S), int(n)), []).append(f"{ka}x{kb}")
        name = f"L{i+1}" if i < len(lv) else "after tree"
        if i >= len(lv):
            tot = sum(int(x) for x in re.findall(r"dist_mn node 0: \d+ x \d+ limbs over \d+ x \d+ ranks \(cap 2\^\d+\): \d+ x \d+ pieces, (\d+) formed", seg))
            cnt = len(re.findall(r"dist_mn node 0: \d+ x \d+ limbs over", seg))
            out.append(f"{name}: node 0 {cnt} mn products, {tot} pieces formed")
        else:
            out.append(name + ": " + ", ".join(f"S{S} n{n} {' '.join(v)}" for (S, n), v in sorted(d.items())))
    print(f"{t}: " + " | ".join(out))

#!/usr/bin/env python3
"""pc_hashes.py (Phase 15 Batch 3 PC, results/PC15.md): the cx_grid share hashes of several logs (configurations) compared per (node, shape)
against the first log's: every configuration must give every product's share identically.   tests/pc_hashes.py <ref log> <log> [...]"""
import sys, re
RG = re.compile(r'cx_grid node (\d+): rep \d+(?: \(warm-up\))? shape (\S+) .* slots \d+: .*C sha256 ([0-9a-f]+)')
def hashes(p):
    h = {}
    for l in open(p, errors='replace'):
        m = RG.search(l)
        if m: h.setdefault((int(m.group(1)), m.group(2)), set()).add(m.group(3))
    return h
ref = hashes(sys.argv[1]); bad = 0
for p in sys.argv[2:]:
    h = hashes(p); d = [k for k in h if k not in ref or h[k] != ref[k] or len(h[k]) != 1]
    miss = [k for k in ref if k not in h]
    bad += len(d)
    print('%-40s %3d (node, shape) products: %s%s' % (p, len(h), 'identical to the reference' if not d else '%d DIFFER: %s' % (len(d), d[:4]), (' (%d of the reference not run)' % len(miss)) if miss else ''))
sys.exit(1 if bad else 0)

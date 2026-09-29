#!/usr/bin/env python3
"""EW15 (results/EW15.md 5): MN_OUT_DKM_HI at the target through mn_model's early_exposed() -- off / on, the write at 2.0 / 1.0 / 0.6 GB/s
per node, cache 0 slots (what fits), the base's defaults (NEWTON_DKM=1, MN_P24=2, ECALC_NP=auto).  Every number MODELLED (the fabric and the
write rate ASSUMED; 1.0 GB/s = the user's Lustre test on the target, assumed to hold with 576 nodes writing).  Run from ecalc/:
    python3 ../tests/ew15_model.py [digits]      (default 5.167e13, the user's new target)"""
import sys, io, contextlib
T = float(sys.argv[1]) if len(sys.argv) > 1 else 5.167e13
sys.path.insert(0, '.')
import mn_model as M, estimate as E
G = 576; D = T / G
seen = {}
_orig = M.early_exposed
def spy(W, dc, g):
    r = _orig(W, dc, g)
    if g > 1 and M.DKM_LAST.get('total'):
        sc = dc.t / M.DKM_LAST['total']
        seen.update(W=W, A=(M.DKM_LAST['hide_hi'] - M.DKM_LAST['hide']) * sc, B=M.DKM_LAST['hide'] * sc, fh=M.DKM_LAST['k1'] / float(M.DKM_LAST['k1'] + M.DKM_LAST['s']), div=dc.t, exposed=r)
    return r
M.early_exposed = spy
print('# MN_OUT_DKM_HI at %.4g digits on %d nodes (%.4g per node; modelled; cache 0; the base defaults)' % (T, G, D))
print('%-5s %-4s | %9s %9s %8s | %7s %7s %7s %6s %7s' % ('write', 'hi', 'no-write', 'write', 'exposed', 'W', 'A', 'B', 'f_hi', 'div'))
for bw in (2.0, 1.0, 0.6):
    res = {}
    for hi in (0, 1):
        M.DKM_HI = bool(hi); seen.clear()
        sys.argv = ['estimate.py', '--g', str(G), '--D', repr(D), '--cache-slots', '0', '--write-bw', str(bw)]
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf): E.main()
        line = [l for l in buf.getvalue().splitlines() if l.startswith('%d ' % G)][0].split()
        nw, wr = float(line[4]), float(line[5]); res[hi] = wr
        print('%-5.1f %-4s | %9.1f %9.1f %8.1f | %7.1f %7.1f %7.1f %6.3f %7.1f' % (bw, 'on' if hi else 'off', nw, wr, wr - nw, seen.get('W', 0), seen.get('A', 0), seen.get('B', 0), seen.get('fh', 0), seen.get('div', 0)))
    print('      gain with the file: %+.1f s' % (res[1] - res[0]))
M.DKM_HI = False

#!/usr/bin/env python3
"""L8 (Phase 15, throwaway): the length families at the 576-node target (MODELLED).

1. The mn tier (tree levels, the sharded reciprocal, the division): V3's patch of mn_model's plane_pts (tests/v3_b4.py) with
   the families {2^k} (as built: mn_shape is powers of two only), +3, +3,5, +3,15, +3,5,15, +3,5,7,15.  A non-power-of-two
   plane costs F x its points (the extra radix pass over a piece whose time is exchange-dominated): F = 1.05 for every radix
   (MODELLED: one memory-bound pass, l8_pad.py; V3 used 1.05 for 3), and the sensitivity F = 1.10 for 5, 7, 15.
2. The leaf's batch tier on the top node (the critical path: terms near 3.5e12, seeds of S log10(3.5e12) / 18 limbs) and on
   node 0, with l8_pad.py's cost model, at S = 256 and the best S."""
import os, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import v3_b4 as V
import l8_pad as P

MODES = [('mn 2^k (as built)', None), ('mn +3', (1, 3)), ('mn +3,5', (1, 3, 5)), ('mn +3,15', (1, 3, 15)),
         ('mn +3,5,15', (1, 3, 5, 15)), ('mn +3,5,7,15', (1, 3, 5, 7, 15))]
CASES = [(576, 4.25e13 / 576), (576, 4.30e13 / 576), (576, 4.40e13 / 576)]

def mn_part(F5):
    V.F = {1: 1.0, 3: 1.05, 5: F5, 7: F5, 15: F5}
    print('mn tier, F(3) = 1.05, F(5, 7, 15) = %.2f:' % F5)
    base = None
    for name, R in MODES:
        V.set_mode(R, None)
        out = []
        for g, D in CASES:
            r = V.run(g, D)
            out.append((D * g, r['wall'], r['recip'], r['levels'], r['div'], r['pieces']))
        if base is None: base = out
        print('  %-18s ' % name + ' | '.join('%.3g: %.1f s (rc %.1f lv %.1f dv %.1f, pcs %d; %+.1f s)' % (d, w, rc, lv, dv, pc, w - b[1]) for (d, w, rc, lv, dv, pc), b in zip(out, base)))
    V.set_mode(None, None)

def leaf(tag, amax, Nnode=3509230438617 // 576):
    print('leaf batch tier, %s (terms up to %.3g, %d terms per node; modelled transform s):' % (tag, amax, Nnode))
    for fused in (False, True):
        row = []
        for name, fam, code in [('today', (1, 3), True)] + [(n, f, False) for n, f in P.FAMS]:
            if code and fused: continue
            def tot(S):
                spans = -(-Nnode // S); nl1 = math.ceil(S * math.log10(amax) / 18); pairs = spans // 2; l = 1; t = pw = tw = 0.0
                while pairs >= 1:
                    nc = 2 * nl1 * 2 ** (l - 1)
                    if nc + 1 > 1 << 30: break
                    L = P.code_pick(nc) if code else P.pick(nc, fam, P.B_CAP, fused)
                    x = P.batch_model(pairs, L, fused); t += x
                    if nc >= 2048: pw += x * L / nc; tw += x
                    l += 1; pairs //= 2
                return t, pw / tw
            t256 = tot(256); best = min(((tot(S), S) for S in range(160, 321)), key=lambda z: z[0][0])
            row.append('%s%s S=256 %.2f s (pad %.3f), best S=%d %.2f s (pad %.3f)' % (name, ' fused' if fused else '', t256[0], t256[1], best[1], best[0][0], best[0][1]))
        print('  ' + '\n  '.join(row))

if __name__ == '__main__':
    mn_part(1.05)
    mn_part(1.10)
    leaf('the top node', 3.509e12)
    leaf('node 0', 6.09e9)

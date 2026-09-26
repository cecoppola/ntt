#!/usr/bin/env python3
"""V3 (Phase 14) scratch: the per-phase breakdown of mn_model.run at the target and at one node (throwaway)."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ecalc'))
import mn_model as M

def design(chunk='both'):
    return M.Design(np=3, strategy='auto', cap=1 << 31, chunk=chunk, depth=2, modmul=1)

def brk(g, D, d=None, verbose=True):
    d = d or design()
    r = M.run(M.TARGET, D, g, verbose=False, design=d)
    if verbose:
        print("g=%d D/node=%.4e wall %.1f: init %.1f batch %.1f top %.1f levels %.1f recip %.1f div %.1f out %.1f other %.1f exposed %.1f pieces %d"
              % (g, D, r['wall'], r['init'], r['batch'], r['top'], r['levels'], r['recip'], r['div'], r['out'], r['other'], r['exposed'], r['pieces']))
        if g > 1:
            for a, b, c in r['levels_rows']:
                print("   level %s %s t %.2f exposed %.2f pieces %d" % (a, b, c.t, c.t_exposed, c.pieces))
            for nm in ('rc', 'dc'):
                c = r[nm]
                print("   %s" % nm, {k: (round(v, 3) if isinstance(v, float) else v) for k, v in vars(c).items()})
    return r

if __name__ == '__main__':
    for g, D in [(576, 4.25e13 / 576), (1, 1e11), (1, 7.64e10), (1, 4e10)]:
        brk(g, D)

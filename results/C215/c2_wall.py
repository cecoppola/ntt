#!/usr/bin/env python3
"""C215: mn_model.run() walls with C2 off / on (c2_grid's patched split_grid), the 576 target and size-1 sizes, and the slowest
leaf over every node of the target.  Run from ecalc/ of the model's tree (B0: 7367e25).  Modelled."""
import sys, os, io, contextlib
sys.path.insert(0, os.getcwd()); sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.argv = sys.argv[:1]
import c2_grid as C, mn_model as M
d = M.DEFAULT15() if hasattr(M, 'DEFAULT15') else M.Design(np=3, strategy='auto', cap=1 << 31, chunk='shift', depth=2)
print('design', d.name())
for pc in (0, 1):
    C.PC[0] = pc; M.product_pieces.cache_clear(); M.big_products.cache_clear(); M._BIG.clear()
    for T, g in ((4.25e13, 576), (4e10, 1), (1e11, 1), (1.16e11, 1), (1.3e11, 1)):
        r = M.run(M.TARGET, T / g, g, verbose=False, design=d)
        print('C2 %s  %.4g on %3d: wall %6.1f  init %.1f batch %.1f top %.1f levels %.1f recip %.1f div %.1f out %.1f'
              % ('on ' if pc else 'off', T, g, r['wall'], r['init'], r['batch'], r['top'], r['levels'], r['recip'], r['div'], r['out']))
T, g = 4.25e13, 576; mx = [0, 0]; arg = [0, 0]; nch = 0; lo = (1e9, 0); hi = (-1e9, 0)
for r in range(g):
    with contextlib.redirect_stdout(io.StringIO()):
        tot, ch = C.report('', C.shapes_node(T, g, r))
    for k in (0, 1):
        if tot[k] > mx[k]: mx[k] = tot[k]; arg[k] = r
    nch += ch > 0; dd = tot[1] - tot[0]
    if dd < lo[0]: lo = (dd, r)
    if dd > hi[0]: hi = (dd, r)
print('576 leaves: %d nodes change their grids; slowest leaf off %.2f s (node %d), on %.2f s (node %d); per node %+.2f (node %d) .. %+.2f (node %d)'
      % (nch, mx[0], arg[0], mx[1], arg[1], lo[0], lo[1], hi[0], hi[1]))

#!/usr/bin/env python3
"""SC15: walls and pieces over total digits, cache slots and variant given (modelled; run from ecalc/).
usage: sc15_sweep2.py SLOTS VARIANT LO HI STEP   (VARIANT: base | pack)"""
import sys
slots, var, lo, hi, step = int(sys.argv[1]), sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
sys.argv = ['x']; sys.path.insert(0, '../tests')
import sc15_model as S, mn_model as M
M.CACHE_MN_SLOTS = slots
kw = dict(pack=0.75) if var == 'pack' else {}
print('# %s, CACHE_MN_SLOTS %d, auto: digits, pieces (tree_max + recip + div), walls without / with the write @0.6, node GB (modelled)' % (var, slots))
n = int(round((hi - lo) / step)); prev = None
for i in range(n + 1):
    T = lo + i * step
    S.set_var(**kw); p = S.pieces(T); e = S.est(T)
    tot = p['tree_max'] + p['recip'] + p['div']
    print('%.3e  %3d + %2d + %2d = %3d | %6.1f | %6.1f | %5.1f GB%s' % (T, p['tree_max'], p['recip'], p['div'], tot, e['nowrite_s'], e['wall_s'], e['node_gb'],
          '' if prev in (None, tot) else '   <- %d -> %d' % (prev, tot)), flush=True)
    prev = tot

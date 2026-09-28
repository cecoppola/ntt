#!/usr/bin/env python3
"""SC15: the walls over total digits for three MN_GROUPS schedules (and P24 with them), the 480 GB ceiling of each (modelled; run from
ecalc/).  usage: sc15_groups.py SPEC [pack]"""
import sys
spec = sys.argv[1]; pack = len(sys.argv) > 2 and sys.argv[2] == 'pack'
sys.argv = ['x']; sys.path.insert(0, '../tests')
import sc15_model as S, mn_model as M

kw = dict(pack=0.75) if pack else {}
print('# MN_GROUPS %s%s: total digits, pieces (tree_max + recip + div), walls without / with the write at 0.6 GB/s, node GB (modelled)' % (spec, ', P24' if pack else ''))
prev = None
for i in range(0, 31):
    T = 5.0e13 + i * 0.02e13
    S.set_var(**kw); p = S.pieces(T, groups=spec); e = S.est(T, groups=spec)
    tot = p['tree_max'] + p['recip'] + p['div']
    mark = '' if prev is None or prev == tot else '   <- step %d -> %d' % (prev, tot)
    print('%.4e  %3d + %2d + %2d = %3d | %6.1f | %6.1f | %5.1f GB | %.2f s per 10^11 digits%s' % (T, p['tree_max'], p['recip'], p['div'], tot, e['nowrite_s'], e['wall_s'], e['node_gb'],
          e['nowrite_s'] / (T / 1e11), mark))
    prev = tot
S.set_var(**kw)
D = M.max_digits(576, M.NODE_GB_MARGIN, 'grid', spec, staging='code', design=S.design('auto'))
e = S.est(D * 576, groups=spec)
print('# the 480 GB ceiling: D %.4e per node -> %.4e digits: %.1f / %.1f s, node %.1f GB' % (D, D * 576, e['nowrite_s'], e['wall_s'], e['node_gb']))

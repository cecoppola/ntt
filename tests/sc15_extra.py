#!/usr/bin/env python3
"""SC15: extra what-if variants at 5.1e13 (modelled; run from ecalc/): balanced shares, P24 with a surcharge, P24 or the min test with
the best MN_GROUPS"""
import sys
sys.argv = ['x']; sys.path.insert(0, '../tests')
import sc15_model as S, mn_model as M

print('# extra variants at 5.1e13, auto unless named (modelled; the fabric assumed)')
S.set_var(); b = S.est(); print(S.line('auto (B1 + NP)', b))
M.SHARES = 'even'; S.set_var(); e = S.est(); print(S.line('equal digits per node (SHARES=even)', e)); M.SHARES = 'terms'
for f in (1.05, 1.10, 1.20):
    S.set_var(pack=0.75)
    save = dict(M.T_PIECE_31_NP); M.T_PIECE_31_NP[4] = save[4] * f; S.clear()
    e = S.est(); print(S.line('P24, local passes x %.2f' % f, e)); M.T_PIECE_31_NP.update(save)
S.set_var(pack=0.75); e = S.est(); print(S.line('P24', e))
for spec in ('2,4,8,16,32,64,576', '4,16,64,576'):
    S.set_var(); e = S.est(groups=spec); print(S.line('MN_GROUPS ' + spec, e))
    S.set_var(pack=0.75); e = S.est(groups=spec); p = S.pieces(S.T0, groups=spec)
    print(S.line('P24 + MN_GROUPS ' + spec, e, ' | pieces %d+%d+%d' % (p['tree_max'], p['recip'], p['div'])))
    S.set_var(minnp=True); e = S.est(groups=spec); print(S.line('min(pa,pb) + MN_GROUPS ' + spec, e))
S.set_var(minnp=True); e = S.est(); print(S.line('min(pa,pb)', e))
S.set_var()

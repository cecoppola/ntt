#!/usr/bin/env python3
"""SC15: the variants with the mn transform cache off (CACHE_MN_SLOTS = 0: agent TC -- no memory inside 480 GB holds a slot at 5.1e13).
Modelled; run from ecalc/."""
import sys
sys.argv = ['x']; sys.path.insert(0, '../tests')
import sc15_model as S, mn_model as M
import sc15_km as K
M.division_cost = K._orig_div
print('# at 5.1e13 on 576 (modelled; the fabric assumed; write 0.6 GB/s); CACHE_MN_SLOTS as named')
for slots in (2, 0):
    M.CACHE_MN_SLOTS = slots
    for tag, kw, km, grp in (('ECALC_NP=4', dict(), False, None),):
        S.set_var(**kw); e = S.est(np_mn=4); print(S.line('cache %d: %s' % (slots, tag), e))
    rows = [('auto', {}, False, None), ('auto, min(pa,pb)', dict(minnp=True), False, None),
            ('auto, one 3-prime slot in pool 1', dict(slot1=True), False, None),
            ('MN_GROUPS 4,16,64,576', {}, False, '4,16,64,576'), ('MN_GROUPS 2,4,8,16,32,64,576', {}, False, '2,4,8,16,32,64,576'),
            ('DKM', {}, True, None), ('P24', dict(pack=0.75), False, None), ('P24 + DKM', dict(pack=0.75), True, None),
            ('P24 + DKM + MN_GROUPS 4,16,64,576', dict(pack=0.75), True, '4,16,64,576')]
    for tag, kw, km, grp in rows:
        M.division_cost = K.division_cost_km if km else K._orig_div
        S.set_var(**kw); e = S.est(groups=grp)
        extra = ''
        if km:
            L = K.LAST; hide = L['c4'].t + (L['c'].t - L['c1'].t - L['c2'].t - L['c3'].t - L['c4'].t) * 0.5
            ow = S.T0 / S.G * M.PACKED_BPD / 1e9 / 0.6
            extra = ' | with the write (DKM overlap) %.1f s' % (e['nowrite_s'] - M.DC_FMT_MN * S.T0 / S.G / 1e9 + max(0.0, ow - hide))
        print(S.line('cache %d: %s' % (slots, tag), e, extra))
        M.division_cost = K._orig_div

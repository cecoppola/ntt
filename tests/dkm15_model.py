#!/usr/bin/env python3
"""DKM15: NEWTON_DKM=1 at the target (5.1e13 on 576), through mn_model's own term (MN_MODEL_DKM / mn_model.DKM: division_cost_dkm and
the early writer's overlap ovl_div), against today's; cache 2 and 0 slots.  Every number MODELLED (the fabric ASSUMED).  Run from ecalc/:
    python3 ../tests/dkm15_model.py [T]"""
import sys
T = float(sys.argv[1]) if len(sys.argv) > 1 else 5.1e13
sys.argv = ['x']; sys.path.insert(0, '../tests'); sys.path.insert(0, '.')
import sc15_model as S, mn_model as M
print('# NEWTON_DKM at %.4g digits on 576 (modelled; the fabric assumed; write 0.6 GB/s; ECALC_NP=auto)' % T)
for slots in (2, 0):
    for dkm, hi in ((False, False), (True, False), (True, True)):
        M.DKM = dkm; M.DKM_HI = hi; M.CACHE_MN_SLOTS = slots
        e = S.est(T)
        L = dict(M.DKM_LAST) if dkm else {}
        p = S.pieces(T)
        ex = ' | pieces (model plan) recip %d div %d' % (p['recip'], p['div'])
        if dkm: ex += ' | X_hi %.1f, X_hi Q %.1f, X_lo %.1f, X_lo Q %.1f s (pieces %d/%d/%d/%d); writer under %.1f s; h %d, s %d' % (
            L['c1'], L['c2'], L['c3'], L['c4'], L['p1'], L['p2'], L['p3'], L['p4'], L['hide_hi' if hi else 'hide'], L['h'], L['s'])
        print(S.line('cache %d %s' % (slots, ('NEWTON_DKM=1 + X_hi writer (not built)' if hi else 'NEWTON_DKM=1') if dkm else 'today'), e, ex))
M.DKM = False; M.DKM_HI = False

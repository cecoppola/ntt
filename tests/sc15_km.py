#!/usr/bin/env python3
"""SC15: the division in two quotient halves with a half-length reciprocal (Karp-Markstein / GMP mu_div style, 'DKM'), modelled at the
target (run from ecalc/).  Every number printed is MODELLED (mn_model's piece costs; the fabric ASSUMED).

Today (newton_mn_divmod): mu = 1/Q to k ~ n limbs (the Newton chain's last step j = n/2 -> n), X = (A_h mu) >> (k + 1) [a high product
n x n], R = A - X Q [a low product n x n mod B^w], corrections.
DKM: r = 1/Q to h = n/2 + guard limbs (the chain stops one step earlier); X_hi = high(A_top(h) r(h)); R1 = A - X_hi Q B^h [X_hi Q mod B^n:
h x n]; X_lo = high(R1_top(h) r(h)); R = R1 - X_lo Q [X_lo Q mod B^(n+g): h x n]; X = X_hi B^h + X_lo; the same correction loop.
The early writer (MN_OUT_EARLY) can start only when X_lo exists (before the last low product): the hidden part is that product's share
(as OVL1 x the division today); a variant writes X_hi's parts after X_hi (patched for a carry, ECALC_CORR_PATCH) -- not modelled here.

usage: sc15_km.py [pack] [groups]"""
import sys
args = sys.argv[1:]
pack = 'pack' in args
groups = next((a for a in args if ',' in a), None)
sys.argv = ['x']; sys.path.insert(0, '../tests')
import sc15_model as S, mn_model as M

GUARD = 2
_orig_div = M.division_cost
LAST = {}

def division_cost_km(fab, nq, dl, npn, g, rule, form, sn=None):
    if sn is None: sn = npn
    na = sn + dl; k = na - nq + 1; w = nq + 2; kmu = npn + 1 + dl - nq + 1
    h = (kmu + 1) // 2 + GUARD
    rc, groups_ = M.recip_cost(fab, nq, h, g, rule, form)            # the chain to h: one doubling fewer
    c = M.Cost()
    c.add(M.shift_cost(fab, nq, g)); c.add(M.small_cost(fab, g, 3))    # Q into P's basis, S = P + Q, residues
    c.add(M.shift_cost(fab, h, g))                                     # A_top
    c1 = M.product_cost(fab, h, h, g, lowcut=h, form=form)             # X_hi = high(A_top r)
    c.add(c1); c.add(M.shift_cost(fab, 2 * h, g))
    c2 = M.product_cost(fab, h, nq, g, highcut=nq + GUARD, form=form)  # X_hi Q mod B^n
    c.add(c2); c.add(M.shift_cost(fab, na, g)); c.add(M.small_cost(fab, g, 3))   # R1 = A - X_hi Q B^h (sign, a guard limb)
    c.add(M.shift_cost(fab, h, g))                                     # R1_top
    c3 = M.product_cost(fab, h, h, g, lowcut=h, form=form)             # X_lo = high(R1_top r)
    c.add(c3); c.add(M.shift_cost(fab, 2 * h, g))
    c4 = M.product_cost(fab, h, nq, g, highcut=nq + GUARD, form=form)  # X_lo Q mod B^(n + g)
    c.add(c4)
    c.add(M.shift_cost(fab, nq + GUARD, g)); c.add(M.shift_cost(fab, nq, g))   # the window, Q in basis w
    c.add(M.small_cost(fab, g, 8))                                     # X = X_hi B^h + X_lo, cmp, sub, corrections, residues
    LAST.update(h=h, c1=c1, c2=c2, c3=c3, c4=c4, rc=rc, c=c)
    return rc, c, groups_

def run(tag, kw, km):
    M.division_cost = division_cost_km if km else _orig_div
    S.set_var(**kw)
    e = S.est(groups=groups)
    p = S.pieces(S.T0, groups=groups)
    extra = ' | pieces tree_max %d recip %d div %d' % (p['tree_max'], p['recip'], p['div'])
    if km:
        L = LAST
        # the early writer: X exists before the last low product (c4); it hides under c4 + the window + corrections (as OVL1 x the
        # division today: the low product and what follows).  Recompute the with-write wall with that overlap.
        e2 = S.est(groups=groups)
        hide = L['c4'].t + (L['c'].t - L['c1'].t - L['c2'].t - L['c3'].t - L['c4'].t) * 0.5
        out_write = S.T0 / S.G * M.PACKED_BPD / 1e9 / 0.6
        wall_w = e2['nowrite_s'] - M.DC_FMT_MN * S.T0 / S.G / 1e9 + max(0.0, out_write - hide)
        extra += ' | products X_hi %.1f, X_hi Q %.1f, X_lo %.1f, X_lo Q %.1f s (pieces %d/%d/%d/%d); writer hidden %.1f s -> with the write %.1f s' % (
            L['c1'].t, L['c2'].t, L['c3'].t, L['c4'].t, L['c1'].pieces, L['c2'].pieces, L['c3'].pieces, L['c4'].pieces, hide, wall_w)
    print(S.line(tag, e, extra))
    M.division_cost = _orig_div

if __name__ == "__main__":
    print('# DKM at 5.1e13 on 576 (modelled; the fabric assumed; write 0.6 GB/s)%s' % ((' MN_GROUPS ' + groups) if groups else ''))
    run('auto (B1 + NP)', {}, False)
    run('auto + DKM', {}, True)
    run('min(pa,pb) + DKM', dict(minnp=True), True)
    if pack:
        run('P24', dict(pack=0.75), False)
        run('P24 + DKM', dict(pack=0.75), True)

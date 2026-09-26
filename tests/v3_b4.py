#!/usr/bin/env python3
"""V3 (Phase 14) scratch model for B4 (TASKS 6.2): more transform lengths.

Patches mn_model's length rules and re-prices the run (modelled; the non-power-of-two per-point factors are ASSUMED):
  mn tier  (plane_pts; rns_dist.c mn_shape: today powers of two only):       pow2 | +3 2^k | +3,5,7 2^k
  one node (_plane_pts_cap, _b_len; today 2^k and 3 2^(k-2) near the cap):   as built | +5,7 2^k
A non-power-of-two length costs F[r] x its points (3: 1.05, the code's own grid weight; 5, 7: 1.10 ASSUMED), applied to the whole
piece (transform and exchange: a slight over-charge of the exchange)."""
import sys, os, math
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ecalc'))
import mn_model as M
import mem_model

F = {1: 1.0, 3: 1.05, 5: 1.10, 7: 1.10}
orig = dict(plane_pts=M.plane_pts, _plane_pts_cap=M._plane_pts_cap, _b_len=M._b_len)

def lengths(nc, radices, lo=1 << 20, hi=None):
    """the cheapest admissible length >= nc (effective points = length x F[r]); returns (effective, length)"""
    best = None
    for r in radices:
        k = 0
        while r << k < max(nc, lo): k += 1
        L = r << k
        if hi is not None and L > hi: continue
        e = L * F[r]
        if best is None or e < best[0]: best = (e, L)
    return best

def clear():
    for f in (M.product_pieces, M._split_auto, M._b_fits, M.big_products, M._split_auto_13b):
        try: f.cache_clear()
        except AttributeError: pass
    M._PC.clear(); M._BIG.clear()

def set_mode(mn_radices, one_radices):
    if mn_radices is None: M.plane_pts = orig['plane_pts']
    else:
        def pp(nc, g, R=tuple(mn_radices)):
            e, L = lengths(nc, R)
            return max(int(e), 1 << mem_model.mn_shape(nc, g)[0]) if L & (L - 1) == 0 else max(int(e), 1 << 20)
        M.plane_pts = pp
    if one_radices is None:
        M._plane_pts_cap = orig['_plane_pts_cap']; M._b_len = orig['_b_len']
    else:
        def ppc(nc, r3, logmax, R=tuple(one_radices)):
            cap = (3 << (logmax - 1)) if r3 else (1 << logmax)
            b = lengths(nc, R, hi=max(cap, 1 << 20))
            return int(b[0]) if b else orig['_plane_pts_cap'](nc, r3, logmax)
        def bl(nc, R=tuple(one_radices)):
            e, L = lengths(nc, R)
            return int(e), L & (L - 1) != 0 and False   # the effective points carry the factor; no extra 1.05 in the chooser
        M._plane_pts_cap = ppc; M._b_len = bl
    clear()

def run(g, D):
    d = M.Design(np=3, strategy='auto', cap=1 << 31, chunk='both', depth=2, modmul=1)
    return M.run(M.TARGET, D, g, verbose=False, design=d)

if __name__ == '__main__':
    modes = [('as built', None, None), ('mn +3 2^k', (1, 3), None), ('mn +3,5,7 2^k', (1, 3, 5, 7), None),
             ('one node +5,7 2^k', None, (1, 3, 5, 7)), ('both +3,5,7 2^k', (1, 3, 5, 7), (1, 3, 5, 7))]
    cases = [(576, 4.25e13 / 576), (576, 4.30e13 / 576), (576, 4.40e13 / 576), (1, 7.64e10), (1, 1e11)]
    for name, mr, orr in modes:
        set_mode(mr, orr)
        out = []
        for g, D in cases:
            r = run(g, D)
            out.append("%s %.3g: %.1f s (bs %.1f lv %.1f rc %.1f dv %.1f, pcs %d)" % ('576' if g > 1 else '1', D * g, r['wall'], r['batch'] + r['top'], r['levels'], r['recip'], r['div'], r['pieces']))
        print("%-18s | %s" % (name, " | ".join(out)))

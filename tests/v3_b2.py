#!/usr/bin/env python3
"""V3 (Phase 14) scratch model for B2 (TASKS 6.3): the reciprocal's first product Q_t r as a middle product.

The step reads u = t1 >> v (v = take - j) and only through d = |B^{2j} - u|, and |u - B^{2j}| < B^{j+1}, so u is determined by its
low j + 2 limbs: only t1's limbs [v, v + j + 2) are needed (plus guard limbs below, R114's argument).  Two forms:
  grid  : a product that is a grid forms only the pieces that meet [v - g, v + j + 2 + g) -- mul_grid's lowcut and highcut (w)
  wrap  : a product that fits one plane is a cyclic convolution of length L >= 2j + 1 + g limbs instead of 3j + 3 (the wrapped top
          lands below v - g); in grid form the same wrap is applied when the wrapped length fits one plane
The second product r d (corr = t1 >> j, a high product with no known part) keeps NEWTON_RECIP_CUT's low cut.
Prices: mn_model's pipeline law (law_product, one node) and product_cost / piece_cost (576 nodes).  Everything modelled."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ecalc'))
import mn_model as M
import mem_model

G = 2   # guard limbs

def one_node(D, verbose=False):
    chain, k, nq = M.recip_chain(D)
    cap = 1 << 31
    tot = dict(p1_cut=0.0, p1_band=0.0, p2_cut=0.0, p1_full=0.0, p2_full=0.0)
    rows = []
    for j, jn, take in chain:
        v = take - j
        a_full = M.law_product(take, j + 1)[0]
        a_cut, fc, gc = M.law_product(take, j + 1, v - 1 if v > 1 else 0)
        # band: grid pieces meeting [v - G, v + j + 2 + G); or one plane at the wrapped length
        L = 2 * j + 1 + G
        if L <= cap and (gc[0] * gc[1] <= 1 or True):
            # the wrapped product as one plane of L limbs (law_product prices a one-plane product by na + nb only)
            a_wrap = M.law_product(L - (j + 1), j + 1)[0] if L - (j + 1) > 0 else 0.0
        else: a_wrap = None
        a_bandg, fb, gb = M.law_product(take, j + 1, max(0, v - G), v + j + 2 + G)
        a_band = min(x for x in (a_wrap, a_bandg, a_cut) if x is not None)
        b_full = M.law_product(j + 1, j + 2)[0]
        b_cut = M.law_product(j + 1, j + 2, j - 1)[0]
        tot['p1_full'] += a_full; tot['p1_cut'] += a_cut; tot['p1_band'] += a_band; tot['p2_full'] += b_full; tot['p2_cut'] += b_cut
        rows.append((j, take, gc, fc, fb, a_cut, a_wrap, a_bandg, a_band, b_cut))
    rs, ds = M.dm_phase(D)
    if verbose:
        print("B2 at %.3g digits (one node): k_mu %d" % (D, k))
        for j, take, gc, fc, fb, a_cut, a_wrap, a_bandg, a_band, b_cut in rows:
            if a_cut > 0.05:
                print("   j %11d  Q_t r %d x %d grid %dx%d  cut %d pcs %.2f s | wrap %s | band-grid %d pcs %.2f s | best %.2f s ; r d cut %.2f s"
                      % (j, take, j + 1, gc[0], gc[1], fc, a_cut, "%.2f s" % a_wrap if a_wrap is not None else "-", fb, a_bandg, a_band, b_cut))
        print("   products: Q_t r full %.1f cut %.1f band %.1f s; r d full %.1f cut %.1f s; model recip %.1f s (no cut), div %.1f s"
              % (tot['p1_full'], tot['p1_cut'], tot['p1_band'], tot['p2_full'], tot['p2_cut'], rs, ds))
        cutr = rs - (tot['p1_full'] - tot['p1_cut']) - (tot['p2_full'] - tot['p2_cut'])
        print("   recip with the cut (the default) %.1f s -> with the middle product %.1f s: -%.1f s (%.0f %% of the reciprocal)"
              % (cutr, cutr - (tot['p1_cut'] - tot['p1_band']), tot['p1_cut'] - tot['p1_band'], 100 * (tot['p1_cut'] - tot['p1_band']) / cutr))
    return tot, rs, ds

def mn(T, g=576, verbose=False):
    d = M.Design(np=3, strategy='auto', cap=1 << 31, chunk='both', depth=2, modmul=1)
    saved = M.DZ; M.DZ = d
    try:
        fab = M.TARGET
        ex = M.exact_sizes(T); nq = ex['nq']; kmu = ex['pn'] + 1 + ex['dl'] - nq + 1
        split = 1 << 16; kp = kmu
        while kp > split and (kp + 1) // 2 > 2: kp = (kp + 1) // 2
        j = kp; t = dict(p1_full=0.0, p1_cut=0.0, p1_band=0.0, p2_full=0.0, p2_cut=0.0)
        cap = 1 << mem_model.mn_cap_log(g, d.pool_log())
        rows = []
        while j < kmu:
            jn = kmu
            while (jn + 1) // 2 > j: jn = (jn + 1) // 2
            take = min(2 * j + 2, nq); v = take - j
            gp = M.choose_group(fab, take + j + 1, g, 'model', 'grid')
            capg = 1 << mem_model.mn_cap_log(gp, d.pool_log())
            f = M.product_cost(fab, take, j + 1, gp).t
            c = M.product_cost(fab, take, j + 1, gp, lowcut=v - 1).t
            L = 2 * j + 1 + G
            if L <= capg:
                w = M.piece_cost(fab, M.plane_pts(L, gp), gp, take, j + 1, L).t
            else: w = None
            bg = M.product_cost(fab, take, j + 1, gp, lowcut=max(0, v - G), highcut=v + j + 2 + G).t
            b = min(x for x in (w, bg, c) if x is not None)
            f2 = M.product_cost(fab, j + 1, j + 2, gp).t; c2 = M.product_cost(fab, j + 1, j + 2, gp, lowcut=j - 1).t
            t['p1_full'] += f; t['p1_cut'] += c; t['p1_band'] += b; t['p2_full'] += f2; t['p2_cut'] += c2
            rows.append((j, gp, f, c, w, bg, b, f2, c2))
            j = jn
        r = M.run(fab, T / g, g, verbose=False, design=d)
        if verbose:
            print("B2 at %.3g digits on %d nodes: model recip %.1f s (no cut), div %.1f s" % (T, g, r['recip'], r['div']))
            for j, gp, f, c, w, bg, b, f2, c2 in rows:
                if f > 0.3: print("   j %14d g %3d  Q_t r full %.2f cut %.2f wrap %s band-grid %.2f best %.2f | r d full %.2f cut %.2f" % (j, gp, f, c, "%.2f" % w if w is not None else "-", bg, b, f2, c2))
            print("   Q_t r: full %.1f cut %.1f middle %.1f s; r d: full %.1f cut %.1f s" % (t['p1_full'], t['p1_cut'], t['p1_band'], t['p2_full'], t['p2_cut']))
        return t, r
    finally:
        M.DZ = saved

if __name__ == '__main__':
    for D in (7.64e10, 1e11, 1.3e11): one_node(D, True)
    mn(4.25e13, 576, True)

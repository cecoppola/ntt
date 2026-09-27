#!/usr/bin/env python3
"""M6 (Phase 15 Batch 2, item 6 = E11 + the Q/S margin trim): feasibility model (throwaway; every number MODELLED).

mem_model.dm_layout (= binsplit.c dm_layout at f184d51) re-evaluated with changed terms, then mem_model.mem_per_node on the B1
defaults (DEFAULTS15B) for one node and on the target's launch line (TARGET_LAUNCH, ECALC_NP=4, MN_GROUPS 2..64,192,576).

Variants (per device, limbs; n = n_Q / g, G = guard limbs):
  T  margin trim: Q, S at n + 2 + 8 (S = P + Q <= n_Q + 1 limbs; the blocks are reserved at na + nb + 8 by mul_grid, never the bound);
     v3's top level at the exact sums (inputs P1 + P2 + Q1 + Q2 = 2 x out) -- at g > 1 the local top level at the LAST node's local Q
     (lgamma of its term range: ~1.045 n at the target), the dm set at n.
  B  band-stored products: the reciprocal's t1 reserved at the band a round reads (safe: 2 jl + 2 + G, which also holds a whole round's
     u and r|d| >> (j - G); 'B0' = the middle-product rounds only, jl + 4 + G: a whole round (first after the seed / an overshoot) would
     grow the pool) and the division's t = A_h mu stored from limb k + 1 - G (k + G limbs instead of 2k + 8).  hole = max(t, t1 + r).
  W  the chunked window: R = Aw - low_w(X Q) accumulated piece by piece into Aw (xq never stored: the piece temporary is the chunk).
"""
import sys, os, math
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ecalc'))
import mem_model as MM

orig = MM.dm_layout
GB = 1e9
G = 2                                                      # guard limbs of the band stores

def local_top_frac(N, g):
    """the largest local Q over the nodes / (n_Q / g): the last node's terms [N - N/g, N) (1.0 at g = 1)"""
    if g <= 1: return 1.0
    lg = math.lgamma(N + 1.0); m = N - N // g
    return (lg - math.lgamma(m + 1.0)) / (lg / g)

def make_layout(trim=False, band=None, chunk=False):
    def dm_layout(N, g, pool_log=31, decimal=True, tight=False, tail_dead=0, anchor=True):
        L = orig(N, g, pool_log, decimal, tight, tail_dead, anchor)
        if not tight or not (trim or band or chunk): return L
        qb = MM.quarter_bytes; cd = lambda x: -(-x // g)
        nq, k, jl = L['nq'], L['k'], L['jl']
        nq_s, k_s, jl_s = cd(nq), cd(k), cd(jl)
        take = min(2 * jl + 2, nq); t1a = take + (jl + 2) + 8
        tlen = 2 * k + 8
        if band == 'safe': t1a = 2 * jl + 2 + G + 8; tlen = k + G + 8
        elif band == 'mid': t1a = jl + 4 + G + 8; tlen = k + G + 8
        elif band == 'id': t1a = max(take + 4, 2 * jl + 3) + 8        # value-identical: the middle product stored mod B^(take+4) only, t unchanged
        hole = max(qb(cd(tlen)), qb(cd(t1a)) + qb(jl_s + 4)); hole += hole // 64
        piece = min((1 << pool_log) + 8, nq_s + k_s + 16)
        qp = qb(nq_s + 2 + 8) if trim else qb(nq_s + nq_s // 10 + 8)
        v2 = 2 * qp + qb(2 * jl_s + 4) + hole + qb(piece)
        if tail_dead >= 2: v2 -= qp
        hi = 2 * qp + qb(k_s + 1) + qb(cd(tlen))
        lo = qp + qb(k_s) + qb(nq_s + 2) + (0 if chunk is True else qb(nq_s + 2 + 8) if chunk == 'w' else qb(nq_s + k_s + 8))   # 'w': xq stored at w + 8 (value-identical)
        div = qb(piece) + max(hi, lo)
        v2 = max(v2, div); v2 += min(v2 // 8, 1 << 30)
        if trim:                                           # the local top level: exact sums (at g > 1 the last node's local Q)
            f = local_top_frac(N, g); out = qb(int(nq_s * f) + 2 + 8); in2 = qb(int(nq_s * f) + 16)   # in2 = P1 + P2 (or Q1 + Q2)
            top = max(2 * in2 + out, in2 + 2 * out)
        else:
            inn = qb(nq_s // 2 + nq_s // 20 + 8); out = qp
            top = max(4 * inn + out, 2 * inn + 2 * out)
        top += top // 8 + (0 if tail_dead else hole)
        L.update(need_dev=max(v2, top), need_v2=v2, v2=v2, v3=top, div=div, hole=hole)
        return L
    return dm_layout

ONE = dict()                                               # DEFAULTS15B (np 3, cap 2^31, DM_TIGHT, seed fill 128)
GROUPS = '2,4,8,16,32,64,192,576'
TGT = dict(MM.TARGET_LAUNCH, transport='shmem', staging='code', depth=2, groups=GROUPS, np=MM.TARGET_NP)
DT = 4.25e13 / 576

VARIANTS = [('as built (B1)', orig), ('T  margin trim', make_layout(trim=True)), ('B  band (safe)', make_layout(band='safe')),
            ('W  chunked window', make_layout(chunk=True)), ('B+W', make_layout(band='safe', chunk=True)),
            ('T+B+W (E11 + trim)', make_layout(trim=True, band='safe', chunk=True)),
            ('T+B0+W (t1 mid-only)', make_layout(trim=True, band='mid', chunk=True)),
            ('T+Bid+Ww (identical values)', make_layout(trim=True, band='id', chunk='w'))]

def row(name, layout):
    MM.dm_layout = layout
    try:
        out = []
        for D in (1e11, 1.4e11):
            m = MM.mem_per_node(D, 1, ONE); L = MM.dm_layout(MM.e_terms(MM.digits_of_run(int(D))), 1, 31, True, True)
            out.append('%.2g: node %5.1f arena %5.1f (bs %5.1f, dm %5.1f; v2 %.1f v3 %.1f hole %.1f /dev)' % (D, m['node_peak'] / GB, m['arena'] / GB, m['regions_bs'] / GB, m['dm_need'] / GB, L['v2'] / GB, L['v3'] / GB, L['hole'] / GB))
        c480 = MM.max_digits_per_node(480e9, 1, ONE); c502 = MM.max_digits_per_node(502e9, 1, ONE)
        t = MM.mem_per_node(int(DT), 576, TGT); Lt = MM.dm_layout(MM.e_terms(MM.digits_of_run(int(DT * 576))), 576, 31, True, True)
        ct = MM.max_digits_per_node(480e9, 576, TGT, hi=2e11)
        print('%-24s one node ceiling 480 / 502 GB: %.3g / %.3g' % (name, c480, c502))
        for s in out: print('    ' + s)
        print('    target: node %5.1f arena %5.1f (bs %.1f, dm %.1f, tree %.1f; v2 %.1f v3 %.1f hole %.1f /dev); 576 ceiling at 480 GB %.3g per node = %.3g digits' % (
            t['node_peak'] / GB, t['arena'] / GB, t['regions_bs'] / GB, t['dm_need'] / GB, t['tree_need'] / GB, Lt['v2'] / GB, Lt['v3'] / GB, Lt['hole'] / GB, ct, ct * 576))
    finally:
        MM.dm_layout = orig

if __name__ == '__main__' and len(sys.argv) == 1:
    N = MM.e_terms(MM.digits_of_run(int(DT * 576)))
    print('target: the last node local Q / (n_Q / g) = %.4f; one node top split (Q2 / n_Q at N/2) = %.4f' % (local_top_frac(N, 576),
          (math.lgamma(1.0433891470e10 + 1) - math.lgamma(1.0433891470e10 / 2 + 1)) / math.lgamma(1.0433891470e10 + 1)))
    for name, lay in VARIANTS: row(name, lay)

def ceilings_no_growth():
    """the one-node ceilings if AS removes the division's measured pool growth (VMM_DM_GROW_FILL = 0)"""
    g0 = MM.VMM_DM_GROW_FILL; MM.VMM_DM_GROW_FILL = 0
    try:
        for name, lay in (VARIANTS[0], VARIANTS[5]):
            MM.dm_layout = lay
            print('  no dm growth, %-22s one node ceiling 480 / 502 GB: %.3g / %.3g' % (name, MM.max_digits_per_node(480e9, 1, ONE), MM.max_digits_per_node(502e9, 1, ONE)))
            MM.dm_layout = orig
    finally:
        MM.VMM_DM_GROW_FILL = g0; MM.dm_layout = orig

if __name__ == '__main__' and len(sys.argv) > 1 and sys.argv[1] == '--nogrow':
    ceilings_no_growth()

def mn_moments(D=DT, g=576):
    """the mn dm path's live moments per device at the target, counted from newton_db.c recip_mn / newton_mn_divmod at f184d51
    (u = one n_Q / g share's quarter; + the product scratch = the layout's top scratch; MODELLED -- no size > 1 ECALC_LIVE data exists)"""
    N = MM.e_terms(MM.digits_of_run(int(D * g))); L = orig(N, g, 31, True, True)
    sc = []; MM.tree_need_dev(-(-L['nq'] // g), g, sc, 0, 'grid', 31, GROUPS, 1024, True)
    u = MM.quarter_bytes(-(-L['nq'] // g)) / GB; s = sc[0] / GB
    tree = MM.tree_need_dev(-(-L['nq'] // g), g, None, 0, 'grid', 31, GROUPS, 1024, True) / GB
    rows = [('recip r|d| product: P Q r t_old d + C', 5.0, True, 4.0, 'free t once d is formed'),
            ('recip r\' formed: P Q r t corr rs', 5.5, False, 4.5, 'free t once corr is formed'),
            ('division A_h mu: S Q mu Ah + t (2k basis)', 6.0, True, 5.0, 'A_h as a view of S (4.0 with t from k+1-G)'),
            ('division X Q: S Q X xq(w)', 4.0, True, 4.0, '-'),
            ('division window: S Q X xq Aw', 5.0, False, 5.0, '-')]
    print('target, per device: u = %.2f GB, product scratch %.2f GB, reservation (need_dev + scratch) %.2f GB, tree floor %.2f GB' % (u, s, L['need_dev'] / GB + s, tree))
    for name, a, prod, b, how in rows:
        print('  %-44s now %.1f u = %5.1f GB   after %.1f u = %5.1f GB  (%s)' % (name, a, a * u + (s if prod else 0), b, b * u + (s if prod else 0), how))

if __name__ == '__main__' and len(sys.argv) > 1 and sys.argv[1] == '--mn':
    mn_moments()

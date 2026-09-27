#!/usr/bin/env python3
"""V3 (Phase 14) scratch model for E11 (APUMULT_STUDY V6): what binds the one-node ceiling under the current defaults (DM_TIGHT, three
primes, cap 2^31), and the ceiling with (a) the reciprocal's t1 written only for its band, (b) the division's low-product window in
chunks, (c) both.  mem_model.dm_layout is re-evaluated with the changed terms (modelled; the chunked window's chunk = one plane)."""
import sys, os, math
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ecalc'))
import mem_model as MM

orig = MM.dm_layout
GB = 1e9

def make_layout(band=False, chunk=False):
    def dm_layout(N, g, pool_log=31, decimal=True, tight=False, tail_dead=0, anchor=True):
        L = orig(N, g, pool_log, decimal, tight, tail_dead, anchor)
        if not (band or chunk) or not tight: return L
        qb = MM.quarter_bytes
        lg = math.lgamma(N + 1.0) / MM.LN10; dl10 = 18.0
        nq = math.ceil(lg / dl10) + 2; dl = math.ceil((lg - 50.0) / dl10) + 1
        k = nq + 1 + dl - nq + 2 + 1; jl = (k + 1) // 2
        take = min(2 * jl + 2, nq); t1a = take + (jl + 2) + 8
        if band: t1a = jl + 2 + 8 + 2 * 2                  # u's low j + 2 limbs + guards (the middle product's band); r d's corr keeps j + 2
        t1a = max(t1a, 2 * jl + 4 + 8) if band else t1a     # the second product r d (2 j + 3 limbs, a high product: its output is written whole)
        nq_s, k_s, jl_s = (nq + g - 1) // g, (k + g - 1) // g, (jl + g - 1) // g
        tcap = 2 * k + 8
        hole = max(qb(-(-(2 * k + 8) // g)) if not band else qb(-(-(k + 8) // g)), qb(-(-t1a // g)) + qb(jl_s + 4))
        hole += hole // 64
        piece = min((1 << pool_log) + 8, nq_s + k_s + 16)
        qp = qb(nq_s + nq_s // 10 + 8)
        v2 = 2 * qp + qb(2 * jl_s + 4) + hole + qb(piece)
        if tail_dead >= 2: v2 -= qp
        hi = 2 * qp + qb(k_s + 1) + qb(2 * k_s + 8)
        win = nq_s + k_s + 8
        if chunk: win = min(win, (1 << pool_log) + 8)      # the window A - X Q formed a plane at a time
        lo = qp + qb(k_s) + qb(nq_s + 2) + qb(win)
        if chunk: hi = 2 * qp + qb(k_s + 1) + qb(min(2 * k_s + 8, k_s + (1 << pool_log)))   # t = A_h mu's band only (the low cut's kept part), a plane of slack
        div = qb(piece) + max(hi, lo)
        v2 = max(v2, div); v2 += min(v2 // 8, 1 << 30)
        inn = qb(nq_s // 2 + nq_s // 20 + 8); out = qp
        top = max(4 * inn + out, 2 * inn + 2 * out); top += top // 8 + (0 if tail_dead else hole)
        L.update(need_dev=max(v2, top), need_v2=v2, v2=v2, v3=top, div=div, hole=hole)
        return L
    return dm_layout

OPTS = dict(tight=True, np=3, cap=1 << 31, strategy='auto')

def report(name, layout):
    MM.dm_layout = layout
    try:
        rows = []
        for D in (1e11, 1.4e11, 1.7e11, 2.0e11):
            m = MM.mem_per_node(D, 1, OPTS); L = MM.dm_layout(MM.e_terms(MM.digits_of_run(int(D))), 1, 31, True, True)
            rows.append("%.2g: node %.0f GB (dev %.0f; per APU v2 %.1f v3 %.1f div %.1f)" % (D, m['node_peak'] / GB, m['dev_dm'] / GB, L['v2'] / GB, L['v3'] / GB, L['div'] / GB))
        c480 = MM.max_digits_per_node(480e9, 1, OPTS); c502 = MM.max_digits_per_node(502e9, 1, OPTS)
        print("%-26s ceiling 480 / 502 GB: %.3g / %.3g digits\n    %s" % (name, c480, c502, "\n    ".join(rows)))
    finally:
        MM.dm_layout = orig

if __name__ == '__main__':
    report('as built (DM_TIGHT)', orig)
    report('E11a band-sized t1', make_layout(band=True))
    report('E11b chunked window', make_layout(chunk=True))
    report('E11 both', make_layout(band=True, chunk=True))

#!/usr/bin/env python3
"""M6 (Phase 15): dm_layout's terms per device at 1e11 / 1.4e11 one node and at the target share (throwaway; modelled)."""
import sys, os, math
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ecalc'))
import mem_model as MM
GB = 1e9; qb = MM.quarter_bytes
def terms(Dtot, g):
    N = MM.e_terms(MM.digits_of_run(int(Dtot)))
    L = MM.dm_layout(N, g, 31, True, True)
    nq, k, jl = L['nq'], L['k'], L['jl']
    nq_s, k_s, jl_s = -(-nq // g), -(-k // g), -(-jl // g)
    take = min(2 * jl + 2, nq); t1a = take + jl + 2 + 8
    qp = qb(nq_s + nq_s // 10 + 8); qp0 = qb(nq_s + 8)
    piece = min((1 << 31) + 8, nq_s + k_s + 16)
    hole_t = qb(-(-(2 * k + 8) // g)); hole_r = qb(-(-t1a // g)) + qb(jl_s + 4)
    print('D %.3g g %d: N %d nq %d k %d jl %d (k-nq %d) take %d t1a %d' % (Dtot, g, N, nq, k, jl, k - nq, take, t1a))
    print('  per device GB: qp %.2f (no margin %.2f)  hole %.2f (hole_t %.2f hole_r %.2f)  r2 %.2f  piece %.2f' % (qp/GB, qp0/GB, L['hole']/GB, hole_t/GB, hole_r/GB, qb(2*jl_s+4)/GB, qb(piece)/GB))
    hi = 2 * qp + qb(k_s + 1) + qb(2 * k_s + 8); lo = qp + qb(k_s) + qb(nq_s + 2) + qb(nq_s + k_s + 8)
    v2r = 2 * qp + qb(2 * jl_s + 4) + L['hole'] + qb(piece)
    print('  v2(recip) %.2f  div hi %.2f lo %.2f (+piece)  v2 %.2f  v3 %.2f  need_dev %.2f  x4 = %.1f' % (v2r/GB, hi/GB, lo/GB, L['v2']/GB, L['v3']/GB, L['need_dev']/GB, 4*L['need_dev']/GB))
    return L
if __name__ == '__main__':
    for D, g in ((1e11, 1), (1.4e11, 1), (1.52e11, 1), (4.25e13, 576)): terms(D, g)

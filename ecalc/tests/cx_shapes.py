#!/usr/bin/env python3
"""cx_shapes.py (Phase 15 CX, results/CX15.md): the mn tier's grid products of a modelled run (mn_model's own product list, which
equals the code's plan: MN_PLAN_ONLY), each with the transform cache's hits as the code takes them (mn_model.cache_pieces) at 0, 1 and
2 slots, against the old model's count (every piece but the first with one operand cached).

    tests/cx_shapes.py [--T 5.1e13] [--g 576] [--np-mn 4|auto]      the table
    tests/cx_shapes.py --emit <cap_log> <g>                          the target's distinct grid shapes scaled to a cap of 2^cap_log over g
                                                                     node-processes, as tests/cx_grid's shape lines (na nb lowcut highcut tag)
"""
import sys, os, argparse
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mn_model as M, mem_model

def record(T, g, np_mn=4):
    rec = []; probe = [0]
    orig, orig_cg = M.product_cost, M.choose_group
    def wrap(fab, na, nb, gg, lowcut=0, highcut=None, with_x=False, cache=True, form="grid"):
        if not probe[0]: rec.append((na, nb, gg, lowcut, highcut, with_x))
        return orig(fab, na, nb, gg, lowcut, highcut, with_x, cache, form)
    def cg(*a, **k):                                                  # the reciprocal's group choice prices candidates: not products of the run
        probe[0] += 1
        try: return orig_cg(*a, **k)
        finally: probe[0] -= 1
    M.product_cost = wrap; M.choose_group = cg; M._PC.clear()
    try:
        d = M.DEFAULT15B(np_mn=np_mn)
        M.run(M.TARGET, T / g, g, verbose=False, design=d)
    finally:
        M.product_cost = orig; M.choose_group = orig_cg; M._PC.clear()
    return rec

def grids(rec, pool_log=31):
    out = []
    for na, nb, g, lo, hi, x in rec:
        cap = 1 << mem_model.mn_cap_log(g, pool_log)
        if na + nb <= cap: continue
        out.append((na, nb, g, cap, lo, hi, x))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--T', type=float, default=M.TARGET_DIGITS); ap.add_argument('--g', type=int, default=576)
    ap.add_argument('--np-mn', default='4'); ap.add_argument('--emit', type=int, nargs=2)
    a = ap.parse_args()
    npm = a.np_mn if a.np_mn == 'auto' else int(a.np_mn)
    rec = grids(record(a.T, a.g, npm))
    if a.emit:
        logc, gg = a.emit; seen = set()
        for na, nb, g, cap, lo, hi, x in rec:
            ka, kb, pcs = M.cache_pieces(na, nb, g, cap, lo, hi, 2)
            key = (ka, kb, len(pcs), lo > 0, hi is not None)
            if key in seen: continue
            seen.add(key); f = (1 << logc) / cap
            print('%d %d %d %d %dx%d_%s' % (int(na * f), int(nb * f), int(lo * f), int(hi * f) if hi is not None else -1, ka, kb,
                                           ('low' if lo else '') + ('high' if hi is not None else '') + ('cut' if (lo or hi is not None) else 'full')))
        return
    M.DZ = M.DEFAULT15B(np_mn=npm).at_g(a.g); sv = {1: 0.0, 2: 0.0}
    tot = {1: 0, 2: 0}; told = 0; npieces = 0
    print("the grid products of %.3g digits on %d nodes (modelled: mn_model's product list = the code's plan); the cache hits as the code takes them" % (a.T, a.g))
    print('  %-15s %-5s %-6s %-8s %-5s | pieces | hits, 1 slot | hits, 2 slots | old model (2 slots) | piece s (modelled): no hit / hit = saved (local + fabric + redistribution)' % ('na x nb (1e9)', 'g', 'cap', 'cuts', 'grid'))
    for na, nb, g, cap, lo, hi, x in rec:
        h = {}
        for s in (1, 2):
            ka, kb, pcs = M.cache_pieces(na, nb, g, cap, lo, hi, s)
            h[s] = sum((p[5] == 'hit') + (p[6] == 'hit') for p in pcs)
            tot[s] += h[s]
        o = max(0, len(pcs) - 1); told += o; npieces += len(pcs)
        i0, j0, la, lb, _, _, _ = pcs[0]; pts = M.split_grid(na, nb, cap, g)[2]
        c2 = M.piece_cost(M.TARGET, pts, g, la, lb, la + lb, 2, False, 'grid', grid=True); c1 = M.piece_cost(M.TARGET, pts, g, la, lb, la + lb, 1, False, 'grid', grid=True)
        t_r = M.TARGET.a2a(8 * lb / (4 * g), g, 1)[0]; d = c2.t - c1.t; dx = (c2.t_exposed - c1.t_exposed) - t_r
        for k in (1, 2): sv[k] += h[k] * d
        print('  %6.0f x %-6.0f %-5d 2^%-4d %-8s %dx%d   | %5d  | %5d        | %5d         | %5d               | %.3f / %.3f = %.3f (%.3f + %.3f + %.3f)' % (na / 1e9, nb / 1e9, g, cap.bit_length() - 1,
              ('low' if lo else '') + ('high' if hi is not None else '') or '-', ka, kb, len(pcs), h[1], h[2], o, c2.t, c1.t, d, d - dx - t_r, dx, t_r))
    print('  total: %d pieces in %d grid products; operand hits: 1 slot %d, 2 slots %d; the old model credits %d; the hits x the saving per hit (summed over the critical path\'s grids): 1 slot %.1f s, 2 slots %.1f s' % (npieces, len(rec), tot[1], tot[2], told, sv[1], sv[2]))

if __name__ == '__main__': main()

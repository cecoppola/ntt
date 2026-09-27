#!/usr/bin/env python3
"""C215 (Phase 15, agent C2): RNS_AUTO_PIECE_COST (C2) in the model -- a port of rns_dist.c split_grid with the per-piece cost,
patched into mn_model's _split_auto at run time (mn_model itself is not changed).  Prints the grids and the pipeline-law seconds
(modelled) of every single-node product off / on:
  size 1 at D digits (leaf mdev levels, reciprocal's top doublings, division):   c2_grid.py 1e11
  one node of a g-node run (its leaf's mdev levels):                            c2_grid.py 4.25e13 576 [node ...]
  (node 0 = the plan printer's `plan leaf` lines; g-1 = the top node, the slowest leaf)
Run from ecalc/ (imports mn_model, mem_model)."""
import sys, os, functools
sys.path.insert(0, os.getcwd())
import mn_model as M

PC = [0]

@functools.lru_cache(maxsize=None)
def _split_auto_pc(na, nb, cap, r3, logmax, np, pc):
    best = None
    for i in range(1, 33):
        for j in range(1, 33):
            pa, pb = -(-na // i), -(-nb // j)
            if pa + pb > cap: continue
            bf = M._b_fits(pa + pb, logmax, r3, np)
            if bf:
                n, t3 = M._b_len(pa + pb); cost = i * j * n * (1.05 if t3 else 1.0) * 0.70
            else:
                pts = M._plane_pts_cap(pa + pb, r3, logmax); cost = i * j * pts * (1.05 if pts & (pts - 1) else 1.0)
            if pc and i * j > 1:                                     # rns_dist.c split_grid, Phase 14 V2 (the per-piece cost)
                n = M._b_len(pa + pb)[0] if bf else 0
                cost += i * j * (0.087 * n + 0.092 * (na + nb))
            if best is None or cost < best[0] * 0.999 or (cost <= best[0] * 1.001 and i * j < best[1] * best[2]): best = (cost, i, j)
    return best[1], best[2]

M._split_auto = lambda na, nb, cap, r3, logmax, np: _split_auto_pc(na, nb, cap, r3, logmax, np, PC[0])

def pname(p):
    return '3*2^%d' % ((p // 3).bit_length() - 1) if p & (p - 1) else '2^%d' % (p.bit_length() - 1)

def law(na, nb, lowcut, w, cap=1 << 31, np=3):
    """(ka, kb, formed pieces, pipeline-law seconds, forms) of one product (mn_model.big_products' law under CAL13)"""
    (ka, kb), pcs = M.product_pieces.__wrapped__(na, nb, lowcut, w, 'auto', cap, np)
    t = 0.0
    for st, p in pcs:
        tp = M.t_prod(st, p, np)[0] * M.PIPE_ONE.get(st, 1.0)
        if ka * kb > 1: tp += (M.GRID_ADD.get(st, 0.0) * p + M.GRID_NC * (na + nb)) / (1 << 31)
        t += tp
    forms = ','.join(sorted(set('%s %s' % (st, pname(p)) for st, p in pcs)))
    return ka, kb, len(pcs), t, forms

def shapes_node(T, g, r):
    N = M._terms(T)
    t0 = lambda r: 1 + N * r // g                                    # mn_plan.c term0: node r computes [1 + N r / g, 1 + N (r+1) / g)
    return M.leaf_shapes.__wrapped__(T, t0(r), t0(r + 1) if r + 1 < g else N + 1)

def report(title, shapes):
    tot = [0.0, 0.0]; npcs = [0, 0]; changed = 0
    print('== %s' % title)
    for ph, name, na, nb, lowcut, w, count in shapes:
        r = []
        for pc in (0, 1):
            PC[0] = pc; r.append(law(na, nb, lowcut, w))
        for k in (0, 1): tot[k] += count * r[k][3]; npcs[k] += count * r[k][2]
        ch = (r[0][0], r[0][1]) != (r[1][0], r[1][1]); changed += ch
        print('  %-24s %11d x %11d | off %2d x %2d (%2d formed, %-15s) %6.2f s | on %2d x %2d (%2d formed, %-15s) %6.2f s%s'
              % (name, na, nb, r[0][0], r[0][1], r[0][2], r[0][4], r[0][3], r[1][0], r[1][1], r[1][2], r[1][4], r[1][3], '  CHANGED' if ch else ''))
    print('  sum: pieces %d -> %d, pipeline law %.2f -> %.2f s (%+.2f s, modelled); %d product(s) changed' % (npcs[0], npcs[1], tot[0], tot[1], tot[1] - tot[0], changed))
    return tot, changed

if __name__ == '__main__':
    T = float(sys.argv[1]); g = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    if g == 1:
        report('size 1, %.4g digits (leaf mdev levels, the reciprocal\'s top three doublings, the division)' % T, M.big_shapes(T))
    else:
        for r in ([int(x) for x in sys.argv[3:]] or [0, g // 2, g - 1]):
            report('%.4g digits on %d nodes: node %d\'s leaf (mdev levels)' % (T, g, r), shapes_node(T, g, r))

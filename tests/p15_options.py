#!/usr/bin/env python3
"""P15 (Phase 15 agent P, PLAN 36): the three options for the 576-node target when three primes cannot hold the mn pieces.
   (a) ECALC_NP=4 everywhere; (b) four primes only for the products whose piece exceeds the three-prime bound (per-product
   prime count); (c) three primes with the mn tier's piece cap lowered to 2^35 (every piece below the bound).
All numbers from ecalc/mn_model.py (DEFAULT15 on the target fabric: the Phase 15 model of agent MD) and mem_model.py:
MODELLED.  Run from the repository root: python3 tests/p15_options.py [digits ...]"""
import sys, os, math, functools
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ecalc'))
import mn_model as M
import mem_model

BOUND3 = 58424467928               # crt.c ec_np3_max_terms (floor((p0 p1 p2 - 1) / (10^18 - 1)^2)); the run checks nc = pa + pb against it
G = 576
WBW = M.TARGET_WRITE_BW             # 0.6 GB/s per node (the target's Lustre, measured there)

# ---- option (b): the piece's prime count from its size (nc = pa + pb > the bound: four primes) --------------------------------
_orig_piece = M.piece_cost
_stats = dict(n3=0, n4=0)
def piece_cost_b(fab, pts, g, na, nb, nc, fwd=2, with_x=False, form="grid", grid=False):
    dz = M.DZ
    if dz is not None and not dz.legacy and dz.np == 3 and nc > BOUND3:
        M.DZ = dz4(dz)
        try: c = _orig_piece(fab, pts, g, na, nb, nc, fwd, with_x, form, grid)
        finally: M.DZ = dz
        _stats['n4'] += 1
        return c
    _stats['n3'] += 1
    return _orig_piece(fab, pts, g, na, nb, nc, fwd, with_x, form, grid)
_dz4 = {}
def dz4(dz):
    k = dz.key()
    if k not in _dz4:
        d = M.Design(np=4, strategy=dz.strategy, cap=dz.cap, chunk=dz.chunk, depth=dz.depth, modmul=dz.modmul, chunk_mb=dz.chunk_mb, legacy=dz.legacy,
                     p15=dz.p15, tight=dz.tight, early_free=dz.early_free, round_mb=dz.round_mb, pool=dz.pool, vmm=dz.vmm, recip_cut=dz.recip_cut,
                     out_overlap=dz.out_overlap)
        _dz4[k] = d
    return _dz4[k]

# ---- option (c): the mn tier's cap lowered to 2^CAPC (the grid search widened beyond the model's 64 x 64) ------------------------
_orig_cap = mem_model.mn_cap_log
_orig_split = M.split_grid
CAPC = [None]
def mn_cap_log_c(g, pool_log=31):
    c = _orig_cap(g, pool_log)
    return min(c, CAPC[0]) if CAPC[0] else c
@functools.lru_cache(maxsize=None)
def split_grid_c(na, nb, cap, g):
    """split_grid with up to 512 x 512 pieces: the fewest points, then the fewest pieces (the code's split_grid_cap stops at 32 x 32)"""
    best = None
    ia = max(1, -(-na // cap)); ib = max(1, -(-nb // cap))
    for i in range(ia, 513):
        pa = -(-na // i)
        if pa >= cap: continue
        # the fewest j for this i, then a few more (the points round to powers of two)
        j0 = max(1, -(-nb // (cap - pa)))
        for j in range(j0, min(513, j0 + 8)):
            pb = -(-nb // j)
            if pa + pb > cap: continue
            pts = M.plane_pts(pa + pb, g); cost = i * j * pts
            if best is None or cost < best[0] or (cost == best[0] and i * j < best[1] * best[2]): best = (cost, i, j, pts)
        if best is not None and i > 4 * best[1] + 8: break
    if best is None: raise ValueError("no grid for %d x %d at cap %d" % (na, nb, cap))
    return best[1], best[2], best[3]

_orig_sgc = mem_model.split_grid_cap
def split_grid_cap_c(na, nb, cap, minpts):
    g = 1
    while (1 << 20) < minpts and False: pass
    ka, kb, _ = split_grid_c(na, nb, cap, 2)     # the same wide search (the plane floor is irrelevant at these sizes)
    return ka, kb

def run(T, design, option=None, capc=None):
    M._PC.clear(); M._BIG.clear() if hasattr(M, '_BIG') else None
    _stats['n3'] = _stats['n4'] = 0
    if option == 'b': M.piece_cost = piece_cost_b
    if option == 'c':
        CAPC[0] = capc; mem_model.mn_cap_log = mn_cap_log_c; M.split_grid = split_grid_c; mem_model.split_grid_cap = split_grid_cap_c
    try:
        r = M.run(M.TARGET, T / G, G, verbose=False, design=design)
        m = M.memory(T / G, G, design=design)
    finally:
        M.piece_cost = _orig_piece; mem_model.mn_cap_log = _orig_cap; M.split_grid = _orig_split; mem_model.split_grid_cap = _orig_sgc; CAPC[0] = None
        M._PC.clear()
    return r, dict(_stats), m

def row(label, T, design, option=None, capc=None, mem_design=None):
    r, st, m = run(T, design, option, capc)
    if mem_design is not None: m = M.memory(T / G, G, design=mem_design)
    return dict(label=label, T=T, wall=r['wall_nowrite'], wallw=r['wall'], pieces=r['pieces'], node=m['node'], planes=m['planes'], dev=m['device'], r=r, st=st)

def main():
    Ts = [float(x) for x in sys.argv[1:]] or [4.25e13]
    for T in Ts:
        d3, d4 = M.DEFAULT15(), M.DEFAULT15(np=4)
        rows = [row('3 primes (the plan today: stops at level 5)', T, d3),
                row('(a) ECALC_NP=4 everywhere', T, d4),
                row('(b) 4 primes for pieces > the 3-prime bound', T, d3, 'b', mem_design=d4),
                row('(c) 3 primes, mn cap 2^35', T, d3, 'c', 35),
                row('(c) 3 primes, mn cap 2^34', T, d3, 'c', 34)]
        print("T = %.4g digits on %d nodes, write %.1f GB/s per node (MODELLED: mn_model DEFAULT15, TARGET fabric)" % (T, G, WBW))
        print("%-46s %9s %9s %7s %8s %8s %s" % ("option", "wall s", "+write s", "pieces", "node GB", "planes", "detail"))
        for x in rows:
            r = x['r']
            det = "levels %.1f recip %.1f div %.1f" % (r['levels'], r['recip'], r['div'])
            print("%-46s %9.1f %9.1f %7d %8.1f %8.1f %s" % (x['label'], x['wall'], x['wallw'], x['pieces'], x['node'], x['planes'], det))

if __name__ == '__main__':
    main()

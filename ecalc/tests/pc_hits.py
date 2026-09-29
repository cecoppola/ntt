#!/usr/bin/env python3
"""pc_hits.py (Phase 15 Batch 3 PC, results/PC15.md): the hits the code took per cx_grid product (node 0's lines) against mn_model's simulation
of RNS_DIST_CACHE_PARTIAL's loop (the longer axis) and slot designation, 18-digit (cache_pieces / cache_pieces_t) and P24 (cache_hits_p24);
and the hit pieces' saving (cache_trace lines: each piece against itself at 0 slots in the same repetition).
   tests/pc_hits.py <cx_grid log> <shapes file> <cap_log> <g> [p24|18] [k: the slot's primes, 0 = full -> the per-piece fit]
   tests/pc_hits.py e2e <procs> <cache-off ecalc log> <log> [...] [p24|18]     (ecalc: the hit pieces against the cache-off run's)"""
import sys, re, os, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mn_model

def sim(na, nb, lo, hi, g, cap, slots, p24, loop='long'):
    hc = None if hi < 0 else hi
    if p24:
        c = min(cap, 1 << mn_model.P24_CAP_LOG)
        if mn_model.p24_pts(na) + mn_model.p24_pts(nb) <= c: return 1, 1, 0
        ka, kb, _ = mn_model.p24_split(na, nb, c, g)
        h = mn_model.cache_hits_p24(na, nb, g, ka, kb, lo, hc, slots, loop)
        return ka, kb, sum(1 for v in h.values() if v)
    if na + nb <= cap: return 1, 1, 0
    ka, kb, _ = mn_model.split_grid(na, nb, cap, g)
    ps = mn_model.cache_pieces_t(na, nb, g, cap, lo, hc, slots) if (loop == 'long' and kb > ka) else mn_model.cache_pieces(na, nb, g, cap, lo, hc, slots)[2]
    return ka, kb, sum(1 for p in ps if p[5] == 'hit' or p[6] == 'hit')

def main():
    log, shp, cl, g = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]); p24 = len(sys.argv) > 5 and sys.argv[5] == 'p24'
    cap = 1 << cl
    shapes = {}
    for l in open(shp):
        a = l.split()
        if len(a) == 5: shapes[a[4]] = (int(a[0]), int(a[1]), int(a[2]), int(a[3]))
    rx = re.compile(r'cx_grid node 0: rep (\d+)( \(warm-up\))? shape (\S+) .* slots (\d+): ([\d.]+) s, cache (\d+) hits (\d+) misses')
    ok = bad = 0; times = collections.defaultdict(dict)
    for l in open(log, errors='replace'):
        m = rx.search(l)
        if not m: continue
        rep, warm, tag, sl, t, h = int(m.group(1)), m.group(2), m.group(3), int(m.group(4)), float(m.group(5)), int(m.group(6))
        if tag not in shapes: continue
        na, nb, lo, hi = shapes[tag]
        ka, kb, hs = sim(na, nb, lo, hi, g, cap, sl, p24) if sl > 0 else (0, 0, 0)
        same = hs == h; ok += same; bad += not same
        if not warm: times[tag][sl] = times[tag].get(sl, []) + [t]
        print('%-16s rep %d slots %d: code %2d hits, model %2d (grid %d x %d) %s' % (tag, rep, sl, h, hs, ka, kb, 'same' if same else 'DIFFERS'))
    print('hits: %d products agree, %d differ' % (ok, bad))
    tot = collections.defaultdict(float)
    for tag, d in times.items():
        row = ' '.join('%d: %.2f s' % (k, sum(v) / len(v)) for k, v in sorted(d.items()))
        for k, v in d.items(): tot[k] += sum(v) / len(v)
        print('%-16s %s' % (tag, row))
    if tot:
        b = tot.get(0)
        print('all shapes: ' + ' | '.join('%d slots %.2f s%s' % (k, v, (' (%+.1f %%)' % (100 * (v / b - 1))) if b and k else '') for k, v in sorted(tot.items())))

RP = re.compile(r'cache_trace node 0: piece \((\d+), (\d+)\) of (\d+) x (\d+), (\d+) \+ (\d+) limbs at (\d+), slots (\d+)(?: x \d primes?)?: A (\S+) B (\S+) \| ([\d.]+) s:')
RG = re.compile(r'cx_grid node 0: rep (\d+)( \(warm-up\))? shape (\S+) .* slots (\d+): ')
def fit(log, g, p24, k):
    """the hit pieces' saving as a fraction of the piece (against the same piece at 0 slots, same repetition) vs mn_model's hit_cost with a k-prime
    slot (k = 0: full) on aac6's loopback fabric (as CX's fit: DEFAULT15B, aac6_fabric('tcp', g))"""
    mn_model.DZ = mn_model.DEFAULT15B().at_g(g); fab = mn_model.aac6_fabric('tcp', g)
    prods = []; cur = []
    for l in open(log, errors='replace'):
        m = RP.search(l)
        if m: cur.append(dict(i=int(m.group(1)), j=int(m.group(2)), la=int(m.group(5)), lb=int(m.group(6)), A=m.group(9), B=m.group(10), t=float(m.group(11)))); continue
        m = RG.search(l)
        if m: prods.append((int(m.group(1)), bool(m.group(2)), m.group(3), int(m.group(4)), cur)); cur = []
    base = {(r, sh, p['i'], p['j']): p for r, w, sh, sl, ps in prods if not w and sl == 0 for p in ps}
    meas, mod = [], []
    saved = mn_model.CACHE_PRIMES; mn_model.CACHE_PRIMES = k or None
    try:
        for r, w, sh, sl, ps in prods:
            if w or sl == 0: continue
            for p in ps:
                if 'HIT' not in (p['A'], p['B']): continue
                b = base.get((r, sh, p['i'], p['j']))
                if not b: continue
                la, lb = (p['la'], p['lb']) if p['B'] == 'HIT' else (p['lb'], p['la'])    # hit_cost prices the B operand's side (t_r on lb): the cached one
                n_ = mn_model.p24_pts(la) + mn_model.p24_pts(lb) if p24 else la + lb
                pts = mn_model.plane_pts(n_, g)
                c2 = mn_model.piece_cost(fab, pts, g, la, lb, la + lb, 2, False, 'grid', grid=True, p24=p24)
                c1 = mn_model.hit_cost(fab, pts, g, la, lb, 'grid', p24=p24, np_=4)
                meas.append((b['t'] - p['t']) / b['t']); mod.append((c2.t - c1.t) / c2.t)
    finally: mn_model.CACHE_PRIMES = saved
    if meas:
        mm, md = sum(meas) / len(meas), sum(mod) / len(mod)
        print('fit: %d hit pieces (k = %s, %s): saved %.1f %% of the piece measured, %.1f %% modelled (hit_cost x CACHE_HIT_F %.2f) -> measured / modelled %.3f'
              % (len(meas), k or 'full', 'P24' if p24 else '18-digit', 100 * mm, 100 * md, mn_model.CACHE_HIT_F, mm / md if md else float('nan')))
    else: print('fit: no hit pieces')

def e2e(procs, base_path, paths, p24=False):
    """ecalc runs against a cache-off run of the same size (tests/cx_fit.py's e2e, the pieces matched by (product, i, j): the switch changes the
    loop order): the hit pieces' saving against themselves measured, and mn_model's hit_cost (the slot's primes from the run's partial note)"""
    import cx_fit
    fab = mn_model.aac6_fabric('tcp', procs); base = cx_fit.products_of(base_path)
    rk = re.compile(r'partial: (\d) primes? per slot')
    for pth in paths:
        ks = [int(m.group(1)) for m in (rk.search(l) for l in open(pth, errors='replace')) if m]   # the traced pieces' slot primes, in order
        pr = cx_fit.products_of(pth); tt = tb = tm = tmb = 0.0; n = 0; kk = iter(ks)
        for (g, na, nb, ps), (g0, _, _, bs) in zip(pr, base):
            mn_model.DZ = mn_model.DEFAULT15B().at_g(g); bd = {(b['i'], b['j']): b for b in bs}
            for p in ps:
                k = next(kk, 0)
                if p['state'] != 'hit': continue
                b = bd.get((p['i'], p['j']))
                if not b: continue
                la, lb = (p['la'], p['lb']) if p['B'] == 'HIT' else (p['lb'], p['la'])
                n_ = mn_model.p24_pts(la) + mn_model.p24_pts(lb) if p24 else la + lb
                pts = mn_model.plane_pts(n_, g)
                saved = mn_model.CACHE_PRIMES; mn_model.CACHE_PRIMES = k if k and k < 4 else None
                try:
                    c2 = mn_model.piece_cost(fab, pts, g, la, lb, la + lb, 2, False, 'grid', grid=True, p24=p24); c1 = mn_model.hit_cost(fab, pts, g, la, lb, 'grid', p24=p24, np_=4)
                finally: mn_model.CACHE_PRIMES = saved
                tt += b['t'] - p['t']; tb += b['t']; tm += c2.t - c1.t; tmb += c2.t; n += 1
        if n: print('%s: %d hit pieces save %.1f s measured (%.1f %% of them) vs %.1f %% modelled (hit_cost x CACHE_HIT_F) -> measured / modelled %.3f'
                    % (os.path.basename(pth), n, tt, 100 * tt / tb, 100 * tm / tmb, (tt / tb) / (tm / tmb)))
        else: print('%s: no hit pieces matched' % os.path.basename(pth))

if __name__ == '__main__':
    if sys.argv[1] == 'e2e': e2e(int(sys.argv[2]), sys.argv[3], sys.argv[4:-1] if sys.argv[-1] in ('p24', '18') else sys.argv[4:], sys.argv[-1] == 'p24'); sys.exit(0)
    main()
    if len(sys.argv) > 6: fit(sys.argv[1], int(sys.argv[4]), len(sys.argv) > 5 and sys.argv[5] == 'p24', int(sys.argv[6]))

#!/usr/bin/env python3
"""pc_hits.py (Phase 15 Batch 3 PC, results/PC15.md): the hits the code took per cx_grid product (node 0's lines) against mn_model's simulation
of RNS_DIST_CACHE_PARTIAL's loop (the longer axis) and slot designation, 18-digit (cache_pieces / cache_pieces_t) and P24 (cache_hits_p24);
and the hit pieces' saving (cache_trace lines: each piece against itself at 0 slots in the same repetition).
   tests/pc_hits.py <cx_grid log> <shapes file> <cap_log> <g> [p24]"""
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

if __name__ == '__main__': main()

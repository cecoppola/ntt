#!/usr/bin/env python3
"""cx_fit.py (Phase 15 CX, results/CX15.md): the per-product and per-piece cache data of tests/cx_grid logs (RNS_DIST_CACHE_TRACE=1) and of
ecalc logs, and the fit of what one hit saves against mn_model's piece_cost on the aac6 loopback fabric.

    tests/cx_fit.py grid <g> <cx_grid log> [...]     products by shape x slots (node 0, the timed repetitions), pieces by cache state, the fit
    tests/cx_fit.py ecalc <log> [...]                 an ecalc run's grid pieces by cache state (node 0) and its phase lines
"""
import sys, os, re, statistics as st
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mn_model as M

RP = re.compile(r'cache_trace node (\d+): piece \((\d+), (\d+)\) of (\d+) x (\d+), (\d+) \+ (\d+) limbs at (\d+), slots (\d+): A (\S+) B (\S+) \| ([\d.]+) s: '
                r'redistribute ([\d.]+) ntt ([\d.]+) crt ([\d.]+) out ([\d.]+) carry ([\d.]+)(?: \| ntt parts rows ([\d.]+) cols ([\d.]+) pack ([\d.]+) xfer ([\d.]+) a2a ([\d.]+))?')
RG = re.compile(r'cx_grid node (\d+): rep (\d+)(?: \(warm-up\))? shape (\S+) \((\d+) x (\d+), cut (\d+) / (-?\d+)\) slots (\d+): ([\d.]+) s, cache (\d+) hits (\d+) misses, C sha256 (\S+) (\S+)')

def pieces_of(lines, node=0):
    """the traced pieces of node `node`, grouped per product (a product ends where the next piece restarts at (0, 0) or the grid changes)"""
    out = []
    for l in lines:
        m = RP.search(l)
        if not m or int(m.group(1)) != node: continue
        v = m.groups()
        d = dict(i=int(v[1]), j=int(v[2]), ka=int(v[3]), kb=int(v[4]), la=int(v[5]), lb=int(v[6]), at=int(v[7]), slots=int(v[8]), A=v[9], B=v[10],
                 t=float(v[11]), red=float(v[12]), ntt=float(v[13]), crt=float(v[14]), out=float(v[15]), carry=float(v[16]))
        if v[17] is not None: d.update(rows=float(v[17]), cols=float(v[18]), pack=float(v[19]), xfer=float(v[20]), a2a=float(v[21]))
        d['state'] = 'hit' if 'HIT' in (d['A'], d['B']) else 'cached' if 'cached' in (d['A'], d['B']) else 'plain'
        out.append(d)
    return out

def model_saving(g, la, lb, pts, fab):
    """mn_model's piece_cost on the fabric: (fwd 2, fwd 1) seconds for this piece"""
    c2 = M.piece_cost(fab, pts, g, la, lb, la + lb, 2, False, 'grid', grid=True); c1 = M.piece_cost(fab, pts, g, la, lb, la + lb, 1, False, 'grid', grid=True)
    return c2.t, c1.t

def grid_report(g, paths):
    M.DZ = M.DEFAULT15B().at_g(g); fab = M.aac6_fabric('tcp', g)
    lines = []
    for p in paths: lines += open(p, errors='replace').read().splitlines()
    prods = [m for m in (RG.search(l) for l in lines) if m and int(m.group(1)) == 0]
    # the pieces in order, attached to their products (the trace lines of one product precede its cx_grid line)
    allp = []; cur = []
    for l in lines:
        m = RP.search(l)
        if m and int(m.group(1)) == 0: cur.extend(pieces_of([l])); continue
        m = RG.search(l)
        if m and int(m.group(1)) == 0:
            allp.append((int(m.group(2)), m.group(3), int(m.group(8)), float(m.group(9)), int(m.group(10)), int(m.group(11)), m.group(13), cur)); cur = []
    timed = [x for x in allp if x[0] > 0]
    shapes = []; [shapes.append(x[1]) for x in timed if x[1] not in shapes]
    slots = sorted(set(x[2] for x in timed))
    print('products (node 0, the timed repetitions; mean s [runs]; hits/misses of the last run; the share hashes %s):' % ('all agree' if all(x[6] == 'same' for x in allp) else 'DIFFER'))
    print('  %-16s' % 'shape' + ''.join('| slots %d               ' % s for s in slots) + '| saved at %s' % ', '.join('%d' % s for s in slots[1:]))
    tot = {s: 0.0 for s in slots}
    for sh in shapes:
        row = '  %-16s' % sh; m = {}
        for s in slots:
            ts = [x[3] for x in timed if x[1] == sh and x[2] == s]; hm = [x for x in timed if x[1] == sh and x[2] == s][-1]
            m[s] = st.mean(ts); tot[s] += m[s]
            row += '| %7.2f [%d] %3dh %3dm ' % (m[s], len(ts), hm[4], hm[5])
        row += '| ' + ', '.join('%+.2f (%+.0f %%)' % (m[s] - m[slots[0]], 100 * (m[s] / m[slots[0]] - 1)) for s in slots[1:])
        print(row)
    print('  %-16s' % 'all' + ''.join('| %7.2f                ' % tot[s] for s in slots) + '| ' + ', '.join('%+.2f (%+.1f %%)' % (tot[s] - tot[slots[0]], 100 * (tot[s] / tot[slots[0]] - 1)) for s in slots[1:]))
    # per piece: the same piece (rep, shape, i, j) at slots 0 against the hit / cached state at s > 0
    base = {}
    for r, sh, s, t, h, mi, _, ps in timed:
        if s == 0:
            for p in ps: base[(r, sh, p['i'], p['j'])] = p
    print('pieces (node 0): a piece against itself at 0 slots in the same repetition (mean s, n)')
    keys = ('t', 'red', 'ntt', 'crt', 'out', 'carry') + (('rows', 'cols', 'pack', 'xfer', 'a2a') if any('rows' in p for x in timed for p in x[7]) else ())
    fitd, fitm = [], []
    for s in slots[1:]:
        for state in ('hit', 'cached', 'plain'):
            pairs = [(p, base.get((r, sh, p['i'], p['j']))) for r, sh, ss, t, h, mi, _, ps in timed if ss == s for p in ps if p['state'] == state]
            pairs = [(p, b) for p, b in pairs if b]
            if not pairs: continue
            print('  slots %d, %-6s n %3d: ' % (s, state, len(pairs)) + '  '.join('%s %.3f -> %.3f' % (k, st.mean(b[k] for p, b in pairs), st.mean(p[k] for p, b in pairs)) for k in keys if k in pairs[0][0]))
            if state == 'hit':
                for p, b in pairs:
                    pts = M.plane_pts(p['la'] + p['lb'], g); t2, t1 = model_saving(g, p['la'], p['lb'], pts, fab)
                    fitd.append(b['t'] - p['t']); fitm.append(t2 - t1)
    if fitd:
        f = sum(fitd) / sum(fitm)
        print('the fit: a hit saves %.3f s per piece measured (median %.3f, n %d) against %.3f modelled on %s -> CACHE_HIT_F %.3f'
              % (st.mean(fitd), st.median(fitd), len(fitd), st.mean(fitm), fab.name, f))
        return f

def ecalc_report(paths):
    for p in paths:
        lines = open(p, errors='replace').read().splitlines()
        ps = pieces_of(lines)
        print('== %s: %d traced pieces (node 0)' % (os.path.basename(p), len(ps)))
        for state in ('plain', 'cached', 'hit'):
            q = [x for x in ps if x['state'] == state]
            if q: print('  %-6s n %3d: sum %.2f s, mean %.3f (redistribute %.3f ntt %.3f crt %.3f out %.3f carry %.3f)' % (state, len(q), sum(x['t'] for x in q), st.mean(x['t'] for x in q),
                        *(st.mean(x[k] for x in q) for k in ('red', 'ntt', 'crt', 'out', 'carry'))))
        for l in lines:
            if l.startswith(('total', 'divmod', 'bs ', 'dm ')) or 'transform cache:' in l or ': tree levels' in l: print('  ' + l.strip()[:200])

if __name__ == '__main__':
    if sys.argv[1] == 'grid': grid_report(int(sys.argv[2]), sys.argv[3:])
    else: ecalc_report(sys.argv[2:])

#!/usr/bin/env python3
"""refit_s48.py - S48: refit the aac7 profile of mn_model.py to measured scaling runs and recompute the 576-node estimate.

    ./refit_s48.py --logs ~/s48/log                       # run logs of s48_run.sh (files n<N>_r<k>.log; ECALC_VERBOSE xstats / wait-stats / tl lines used)
    ./refit_s48.py --table 1:77.9 2:155.7 4:220.3         # nodes:total_seconds[:init_seconds] (measured totals; mixes with --logs)
    ./refit_s48.py --logs ... --write-profile             # also write ecalc/aac7_s48_fit.json, which `estimate.py --fabric aac7_s48` then uses
    ./refit_s48.py --selftest                             # parser + fit on a synthetic log

What is fitted (all other model constants stay as they are; nothing in the code or its defaults changes):
  1 node : INIT_SCALE (the init time, when the logs give it) and NODE_SCALE (the node's compute) so the model's no-write wall = the measured `total`.
  >1 node: bw_apu (GB/s per APU thread) and lat (s per message) by a grid search on sum(log(model / measured)^2) over the multi-node sizes,
           with the 1-node scales held (HIDE_POW2, GEN_HIDE, T_ROUND stay at AAC7_CONSTS).  The logs' xstats lines also give the measured
           exchange rate (GB / t_post per APU thread, by phase and kind) and the wait-stats the blocked time: printed as independent checks of bw_apu.
Then: the 576-node target estimate (M.TARGET_DIGITS, the target fabric, as `estimate.py --target`) with the unchanged target locals and with the
refit local scales (NODE_SCALE, INIT_SCALE: the same MI300A hardware) - modelled, labelled so; the target fabric (100 GB/s per APU) is ASSUMED, not fitted.
Measured quantity = `total` of the run (no digit write), compared to the model's no-write wall.  Labels: measured (logs/table), modelled (the rest).
"""
import argparse, glob, json, math, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mn_model as M
import mem_model

DPN = 6.441e10
TGT = dict(HIDE_POW2=M.HIDE_POW2, T_ROUND=M.T_ROUND, GEN_HIDE1=M.GEN_HIDE_DEPTH[1], GEN_HIDE2=M.GEN_HIDE_DEPTH[2], MAP_RATE=M.MAP_RATE, NODE_SCALE=M.NODE_SCALE, INIT_SCALE=M.INIT_SCALE)   # the target profile's constants (module defaults, before any apply_profile)
FIT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'aac7_s48_fit.json')

# ---------------------------------------------------------------- parsing
LAB = re.compile(r'^\s*\d+: ')
TOT = re.compile(r'^total\s+([\d.]+) s\s+\(bs ([\d.]+) \+ 10dP ([\d.]+) \+ dm ([\d.]+) \+ T1 ([\d.]+) \+ dc ([\d.]+) \+ T2 ([\d.]+) = [\d.]+; init ([\d.]+); other ([\d.]+)\)')
XPH = re.compile(r'^xstats pe (\d+) phase (\w+) kind (\w+): n (\d+) rounds (\d+) GB ([\d.]+) t_post ([\d.]+) s t_call ([\d.]+) s t_wait ([\d.]+) s')
WST = re.compile(r'^wait-stats node (\d+):(.*)$')
WPH = re.compile(r'(\w+) barrier ([\d.]+) s \(\d+\) wait ([\d.]+) s')
TLL = re.compile(r'^tl\s+([\d.]+)\s+(.*)$')

def parse_log(path, nodes=None):
    """one run's log -> dict(nodes, total, bs, dm, init, other, verify, xs{(phase,kind)->[GB, t_post, n_pe]}, wait{phase->[barrier, wait, n]}, tl_end)"""
    r = dict(path=path, nodes=nodes, total=None, init=None, bs=None, dm=None, other=None, verify=False, xs={}, wait={}, tl_end=None, pes=set())
    m = re.search(r'n(\d+)_r', os.path.basename(path))
    if m and nodes is None: r['nodes'] = int(m.group(1))
    for ln in open(path, errors='replace'):
        ln = LAB.sub('', ln.rstrip('\n'))
        if 'VERIFY OK' in ln: r['verify'] = True
        t = TOT.match(ln)
        if t and r['total'] is None:
            r.update(total=float(t.group(1)), bs=float(t.group(2)), dm=float(t.group(4)), init=float(t.group(8)), other=float(t.group(9)))
        x = XPH.match(ln)
        if x:
            pe, ph, kd = int(x.group(1)), x.group(2), x.group(3); r['pes'].add(pe)
            a = r['xs'].setdefault((ph, kd), [0.0, 0.0, set()]); a[0] += float(x.group(6)); a[1] += float(x.group(7)); a[2].add(pe)
        w = WST.match(ln)
        if w:
            for ph, b, wt in WPH.findall(w.group(2)):
                a = r['wait'].setdefault(ph, [0.0, 0.0, 0]); a[0] += float(b); a[1] += float(wt); a[2] += 1
        tl = TLL.match(ln)
        if tl: r['tl_end'] = float(tl.group(1))
    if r['nodes'] is None and r['pes']: r['nodes'] = len(r['pes'])
    return r

def collect(paths):
    files = []
    for p in paths:
        p = os.path.expanduser(p)
        files += sorted(glob.glob(os.path.join(p, '*.log'))) if os.path.isdir(p) else sorted(glob.glob(p))
    return [parse_log(f) for f in files]

# ---------------------------------------------------------------- model
def design():
    """the design of `estimate.py`'s defaults (p15c: auto primes at size > 1, round 1024 MB, packed part file, cache FIT)"""
    M.TWREC_G = True; M.CACHE_FORCE = None
    return M.Design(np=3, strategy='auto', cap=mem_model.CAPS['2^31'], chunk='both', depth=2, modmul=1, chunk_mb=M.CHUNK_MB, p15=True, round_mb=1024,
                    out_overlap='none', p15b=True, np_mn='auto', packed=True, p15c=True, arena_room=None, cache_fit=True)

DES = None
def model(g, D, bw, lat, node_scale, init_scale, consts=None, write_bw=0.116, groups=None, fab_name=None, v_factor=1.0):
    """the model's no-write wall and phases for g nodes at D digits per node on a fabric (bw, lat); constants as set"""
    global DES
    if DES is None: DES = design()
    M._PC.clear()   # T06: the product-cost memo's key has no NODE_SCALE / INIT_SCALE, so a call with other scales read stale entries (the S48 'refit locals' 576 row was 169.9, clean 152.4)
    saved = (M.NODE_SCALE, M.INIT_SCALE, M.HIDE_POW2, M.T_ROUND, dict(M.GEN_HIDE_DEPTH), M.MAP_RATE)
    try:
        M.NODE_SCALE, M.INIT_SCALE = node_scale, init_scale
        if consts:
            M.HIDE_POW2 = consts.get('HIDE_POW2', M.HIDE_POW2); M.T_ROUND = consts.get('T_ROUND', M.T_ROUND)
            M.MAP_RATE = consts.get('MAP_RATE', M.MAP_RATE)
            M.GEN_HIDE_DEPTH[1] = consts.get('GEN_HIDE1', M.GEN_HIDE_DEPTH[1]); M.GEN_HIDE_DEPTH[2] = consts.get('GEN_HIDE2', M.GEN_HIDE_DEPTH[2])
        fab = M.Fabric(fab_name or 'aac7 S48 refit', bw, lat, group=64, layers=2, taper=1.0, write_bw=write_bw, v_factor=v_factor)
        r = M.run(fab, D, g, 'model', verbose=False, groups=groups, form='grid', transport='shmem', staging='code', design=DES)
        return r
    finally:
        M.NODE_SCALE, M.INIT_SCALE, M.HIDE_POW2, M.T_ROUND = saved[:4]; M.GEN_HIDE_DEPTH.update(saved[4]); M.MAP_RATE = saved[5]

def bisect(f, lo, hi, target, it=40, increasing=True):
    """x in [lo, hi] with f(x) = target (f monotone); geometric bisection; clamps at the ends"""
    flo, fhi = f(lo), f(hi)
    if (flo - target) * (fhi - target) > 0: return lo if abs(flo - target) < abs(fhi - target) else hi
    for _ in range(it):
        mid = math.sqrt(lo * hi); fm = f(mid)
        if (fm < target) == increasing: lo = mid
        else: hi = mid
    return math.sqrt(lo * hi)

# ---------------------------------------------------------------- fit
def mean(xs): xs = [x for x in xs if x is not None]; return sum(xs) / len(xs) if xs else None

def summarize(runs, table):
    """per node count: dict(total, init, bs, dm, n, src)  -- measured; logs first, table entries fill in what the logs lack"""
    by = {}
    for r in runs:
        if r['total'] is None or not r['verify'] or r['nodes'] is None: continue
        by.setdefault(r['nodes'], []).append(r)
    out = {}
    for g, rs in sorted(by.items()):
        out[g] = dict(total=mean([x['total'] for x in rs]), init=mean([x['init'] for x in rs]), bs=mean([x['bs'] for x in rs]), dm=mean([x['dm'] for x in rs]),
                      n=len(rs), spread=(max(x['total'] for x in rs) - min(x['total'] for x in rs)), src='logs')
    for g, (tot, ini) in table.items():
        if g not in out: out[g] = dict(total=tot, init=ini, bs=None, dm=None, n=1, spread=0.0, src='table')
    return out

def fit(meas, args):
    ac = dict(M.AAC7_CONSTS)
    ns0, is0 = ac['NODE_SCALE'], ac['INIT_SCALE']
    bw0, lat0 = M.AAC7.bw, M.AAC7.lat
    res = dict(base=dict(bw_apu=bw0, lat=lat0, consts=ac))
    D = DPN
    # --- 1 node: scales
    ns, isc = ns0, is0
    if 1 in meas:
        m1 = meas[1]
        if m1['init'] is not None:
            isc = bisect(lambda s: model(1, D, bw0, lat0, ns0, s)['init'], 0.2, 5.0, m1['init'])
        else:
            isc = ns0 * is0 / ns0   # no init measurement: keep AAC7's ratio below
        f = lambda s: model(1, D, bw0, lat0, s, isc if m1['init'] is not None else s * is0 / ns0)['wall_nowrite']
        ns = bisect(f, 0.2, 5.0, m1['total'])
        if m1['init'] is None: isc = ns * is0 / ns0
    res['node_scale'], res['init_scale'] = ns, isc
    # --- multi-node: bw, lat
    gs = [g for g in sorted(meas) if g > 1]
    best = None
    if gs:
        bws = [bw0 * 2 ** (k / 4.0) for k in range(-8, 13)]
        # S48 finding: the per-message latency moves the modelled walls by < 0.5 % over 2..68 us (the message counts are small), so the totals cannot
        # separate lat from bw.  lat is therefore FIXED at the profile value (a pair fit would just wander); --fix-lat is kept for compatibility.
        lats = [lat0]
        # bw is fitted on the power-of-2 sizes only: a non-power-of-2 size runs the 'layv' (variable-layout) exchange, measured ~2.4x slower per
        # thread than the equal-slab 'layeq' of 2/4 nodes, which one bw cannot describe.  It is reported as an implied bw / factor instead.
        gs_all = gs
        gs = [g for g in gs if M.is_pow2(g)] or gs
        for lat in lats:
            for bw in bws:
                e = 0.0
                for g in gs:
                    w = model(g, D, bw, lat, ns, isc, ac)['wall_nowrite']
                    e += math.log(w / meas[g]['total']) ** 2
                if best is None or e < best[0]: best = (e, bw, lat)
        # refine bw at the best lat by bisection on the mean log error
        _, bw, lat = best
        f = lambda b: sum(math.log(model(g, D, b, lat, ns, isc, ac)['wall_nowrite'] / meas[g]['total']) for g in gs) / len(gs)
        lo, hi = bw / 1.2, bw * 1.2
        try:
            bw = bisect(f, lo, hi, 0.0, it=18, increasing=False)
        except Exception:
            pass
        res['bw_apu'], res['lat'] = bw, lat
        res['fit_sizes'] = gs
        res['implied'] = {}
        for g in gs_all:
            fg = lambda b, g=g: model(g, D, b, lat, ns, isc, ac)['wall_nowrite']
            res['implied'][g] = bisect(fg, 1.0, 200.0, meas[g]['total'], increasing=False)
        npw = [g for g in gs_all if not M.is_pow2(g)]
        res['nonpow2_bw_factor'] = (sum(res['implied'][g] for g in npw) / len(npw)) / bw if npw else 1.0
        # T06: the layv rate factor (the model's per-kind rate, mn_model.Fabric.v_factor): fitted on the non-power-of-2 totals with bw, lat, scales fixed
        res['layv_factor'] = 1.0
        if npw:
            fv = lambda v: sum(math.log(model(g, D, bw, lat, ns, isc, ac, v_factor=v)['wall_nowrite'] / meas[g]['total']) for g in npw) / len(npw)
            res['layv_factor'] = bisect(fv, 0.05, 1.0, 0.0, it=30, increasing=False)
    else:
        res['bw_apu'], res['lat'] = bw0, lat0
    res['consts'] = {k: ac[k] for k in ('HIDE_POW2', 'GEN_HIDE1', 'GEN_HIDE2', 'T_ROUND', 'MAP_RATE')}
    res['consts']['NODE_SCALE'], res['consts']['INIT_SCALE'] = ns, isc
    return res

def rate_checks(runs):
    """measured exchange rates (GB/s per APU thread = GB / t_post) from the xstats lines, and the wait stats; mean over runs with them, by node count"""
    out = {}
    for r in runs:
        if r['nodes'] is None or not r['verify']: continue
        o = out.setdefault(r['nodes'], dict(xs={}, wait={}, n=0, tl=[]))
        if r['xs'] or r['wait']: o['n'] += 1
        for k, (gb, tp, pes) in r['xs'].items():
            a = o['xs'].setdefault(k, [0.0, 0.0, 0]); a[0] += gb / len(pes); a[1] += tp / len(pes); a[2] += 1
        for ph, (b, w, n) in r['wait'].items():
            a = o['wait'].setdefault(ph, [0.0, 0.0, 0]); a[0] += b / n; a[1] += w / n; a[2] += 1
        if r['tl_end'] is not None: o['tl'].append(r['tl_end'])
    return out

def target_rows(res, ns, isc, vf=1.0):
    """the 576-node estimate: the standing target profile (unchanged) and with the refit local scales; groups as estimate.py --target"""
    groups = '2,4,8,16,32,64,192,576'; rows = []
    for T in sorted({3.71e13, 3.82e13, M.TARGET_DIGITS}):
        for label, nsc, isc_ in (('target as is (aac6-calibrated locals)', TGT['NODE_SCALE'], TGT['INIT_SCALE']), ('target + refit local scales', ns, isc)):
            r = model(576, T / 576, M.TARGET.bw, M.TARGET.lat, nsc, isc_, TGT, write_bw=M.TARGET_WRITE_BW, groups=groups, fab_name='target', v_factor=vf)
            rows.append((T, label, nsc, isc_, r['wall_nowrite'], r['wall']))
    return rows

def level_split(res, T, vf):
    """T06: the layv penalty at 576 nodes per level (modelled): the no-write wall with the layv factor vf applied to all non-pow2 levels, to one level only
    (the others at 1), and none; groups as estimate.py --target (2,4,..,64 = layeq; 192 and 576 = layv).  Returns (wall at vf=1, {level: seconds added}, wall at vf)"""
    groups = '2,4,8,16,32,64,192,576'; orig = M.Fabric.a2a; out = {}
    def run(vf_, only):
        def a2a(self, b, g, c=1, v=False): return orig(self, b, g, c, v and (only is None or g == only))
        M.Fabric.a2a = a2a; M._PC.clear()
        try: return model(576, T / 576, M.TARGET.bw, M.TARGET.lat, res['node_scale'], res['init_scale'], TGT, write_bw=M.TARGET_WRITE_BW, groups=groups, fab_name='target', v_factor=vf_)['wall_nowrite']
        finally: M.Fabric.a2a = orig; M._PC.clear()
    w1 = run(1.0, None); wall = run(vf, None)
    for lv in (192, 576): out[lv] = run(vf, lv) - w1
    return w1, out, wall

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--logs', nargs='*', default=[], help='log files / dirs (n<N>_r<k>.log of s48_run.sh)')
    ap.add_argument('--table', nargs='*', default=[], help='nodes:total[:init] measured seconds (mean of the runs)')
    ap.add_argument('--fix-lat', action='store_true', help='keep the profile latency (8.5 us), fit bw only')
    ap.add_argument('--write-profile', action='store_true', help='write ecalc/aac7_s48_fit.json (the profile `estimate.py --fabric aac7_s48` reads)')
    ap.add_argument('--l4', action='store_true', help='L4 (results/L4.md): refit to all of 2026-10-11\'s multi-node classes with the corrected structure; --write-profile writes ecalc/aac7_l4_fit.json')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    M.apply_profile('aac7_c16')
    if a.selftest: return selftest()
    if a.l4: return l4_main(a)
    runs = collect(a.logs)
    table = {}
    for t in a.table:
        p = t.split(':'); table[int(p[0])] = (float(p[1]), float(p[2]) if len(p) > 2 else None)
    meas = summarize(runs, table)
    if not meas: raise SystemExit('refit_s48: no usable run (need a VERIFY OK log with a `total` line, or --table)')
    print('measured (mean per node count; total = the run\'s own clock, no digit write):')
    for g, m in meas.items():
        print('  %d node(s): total %.1f s (n %d, spread %.1f s, %s)%s%s' % (g, m['total'], m['n'], m['spread'], m['src'],
              ', init %.1f' % m['init'] if m['init'] is not None else '', ', bs %.1f dm %.1f' % (m['bs'], m['dm']) if m['bs'] is not None else ''))
    res = fit(meas, a)
    ac = res['base']['consts']
    print('\nfit (modelled on the measured totals above):')
    print('  node_scale %.3f (AAC7 %.3f)   init_scale %.3f (AAC7 %.3f)   bw_apu %.2f GB/s (AAC7 %.2f)   lat %.1f us (AAC7 %.1f)' % (
        res['node_scale'], ac['NODE_SCALE'], res['init_scale'], ac['INIT_SCALE'], res['bw_apu'], res['base']['bw_apu'], res['lat'] * 1e6, res['base']['lat'] * 1e6))
    print('  lat fixed at %.1f us (not identifiable from totals); bw fitted on the power-of-2 sizes %s' % (res['lat'] * 1e6, res.get('fit_sizes')))
    if res.get('implied'):
        print('  implied bw per size (GB/s per APU, lat fixed): ' + ', '.join('%d n %.2f' % (g, b) for g, b in sorted(res['implied'].items())) + '   non-pow2 bw factor %.2f (measured layv vs layeq rate is lower still)' % res['nonpow2_bw_factor'])
    print('  %-6s %10s %10s %7s | %10s %7s' % ('nodes', 'measured', 'model AAC7', 'err %', 'model fit', 'err %'))
    for g, m in meas.items():
        w0 = model(g, DPN, res['base']['bw_apu'], res['base']['lat'], ac['NODE_SCALE'], ac['INIT_SCALE'], ac)['wall_nowrite']
        w1 = model(g, DPN, res['bw_apu'], res['lat'], res['node_scale'], res['init_scale'], ac)['wall_nowrite']
        print('  %-6d %10.1f %10.1f %+7.1f | %10.1f %+7.1f' % (g, m['total'], w0, 100 * (w0 / m['total'] - 1), w1, 100 * (w1 / m['total'] - 1)))
    rc = rate_checks(runs)
    for g, o in sorted(rc.items()):
        if not o['n']: continue
        print('\nindependent checks, %d node(s), %d run(s) with stats (measured): ' % (g, o['n']))
        for (ph, kd), (gb, tp, n) in sorted(o['xs'].items()):
            if tp > 0: print('  xstats %-6s %-7s %8.1f GB/node  t_post %7.1f s (4 threads summed)  -> %.2f GB/s per APU thread' % (ph, kd, gb / n, tp / n, gb / tp * 1.0 / 1))
        for ph, (b, w, n) in sorted(o['wait'].items()): print('  wait-stats %-6s barrier %.1f s  wait %.1f s (mean over nodes, summed over APU threads)' % (ph, b / n, w / n))
        if o['tl']: print('  init timeline last tl mark: %.1f s' % (sum(o['tl']) / len(o['tl'])))
    print('\n576-node standing estimate (modelled; fabric 100 GB/s per APU / 2 us ASSUMED, the part file at %.1f GB/s ASSUMED), T = %.3e digits:' % (M.TARGET_WRITE_BW, M.TARGET_DIGITS))
    print('  layv factor (T06, fitted on the non-pow2 totals): %.3f; measured xstats layv/layeq per-thread rate: %s' % (res.get('layv_factor', 1.0), ', '.join('%s %.2f' % (ph, rc[6]['xs'][(ph, 'layv')][0] / rc[6]['xs'][(ph, 'layv')][1] / (rc[4]['xs'][(ph, 'layeq')][0] / rc[4]['xs'][(ph, 'layeq')][1])) for ph in ('bs', 'dm', 'recip') if 6 in rc and 4 in rc and (ph, 'layv') in rc[6]['xs'] and (ph, 'layeq') in rc[4]['xs']) if 6 in rc else 'n/a'))
    print('  residuals with the per-kind rate (model - measured):')
    for g, m in meas.items():
        w2 = model(g, DPN, res['bw_apu'], res['lat'], res['node_scale'], res['init_scale'], ac, v_factor=res.get('layv_factor', 1.0))['wall_nowrite']
        print('    %d n: model %.1f s vs %.1f s measured: %+.1f %%' % (g, w2, m['total'], 100 * (w2 / m['total'] - 1)))
    for vf_label, vf in (('layv = layeq (factor 1)', 1.0), ('layv ratio as fitted on aac7 (%.3f)' % res.get('layv_factor', 1.0), res.get('layv_factor', 1.0))):
      for T, label, nsc, isc, nw, w in target_rows(res, res['node_scale'], res['init_scale'], vf):
        if 'as is' in label: continue
        print('  [%s] T %.3e  %-40s: no-write %.1f s (%.2f min), with the file %.1f s (%.2f min)' % (vf_label, T, label, nw, nw / 60, w, w / 60))
    for vf_label, vf in (('fitted %.3f' % res.get('layv_factor', 1.0), res.get('layv_factor', 1.0)), ('xstats ratio 0.41 (sensitivity)', 0.41)):
        for T in (3.82e13, 3.71e13):
            w1, lv, wl = level_split(res, T, vf)
            print('  [layv per level, factor %s] T %.3e: layeq-only %.1f s; +%.1f s from the 192 level, +%.1f s from the 576 level (alone each); all layv levels %.1f s (+%.1f s)' % (vf_label, T, w1, lv[192], lv[576], wl, wl - w1))
    for T, label, nsc, isc, nw, w in target_rows(res, res['node_scale'], res['init_scale']):
        print('  T %.3e  %-52s node_scale %.3f init_scale %.3f : no-write %.1f s (%.2f min), with the file %.1f s (%.2f min)' % (T, label, nsc, isc, nw, nw / 60, w, w / 60))
    if a.write_profile:
        prof = dict(bw_apu=res['bw_apu'], lat=res['lat'], nonpow2_bw_factor=res.get('nonpow2_bw_factor', 1.0), layv_factor=res.get('layv_factor', 1.0), consts=res['consts'], write_bw=0.116, fitted_on={str(g): dict(total=m['total'], n=m['n']) for g, m in meas.items()})
        json.dump(prof, open(FIT_FILE, 'w'), indent=1); print('\nwrote', FIT_FILE)

# ---------------------------------------------------------------- L4 (2026-10-11): refit on all multi-node run classes
L4_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'aac7_l4_fit.json')
# (label, nodes, MN_GROUPS or None, NIC group 'B' fast / 'A' slow / '?' unknown, [measured totals, s, no digit write, 6.441e10 digits per node], source)
# Totals are MEASURED (`total` line, VERIFY OK).  Cold-start runs (init >= 36 s) have the excess init over 30 s removed (marked 'cold-adj'); the mode of the
# pow2 classes is the run's own all-to-all rate (fast > 12.3, slow < 11.95 GB/s per APU thread: calib_table.py); '?' classes are priced as group B (the 4 n slow
# mode is group A, A13/T09; for '?' classes both groups are printed).
L4_CLASSES = [
    ('2 n [2] fast',          2, None,  'B', [144.1, 133.8], 'S48'),
    ('4 n [2,4] fast',        4, None,  'B', [181.5], 'S48/S47/S50/S51/f3, 17 clean fast runs, mean'),
    ('4 n [2,4] slow',        4, None,  'A', [208.0], 'S47/S50/S51, 6 clean slow runs, mean'),
    ('4 n [4] fast',         4, '4',   'B', [181.3], 'f3 single4 r5_B (group B, chain-free)'),
    ('4 n [4] slow',          4, '4',   'A', [209.8, 205.7, 208.8], 'f3 single4 r2_B, single4b r4_B, r6_B (group A; r1_B cold 233.7 excluded)'),
    ('3 n [3]',               3, None,  '?', [200.2], 'B06 t_def 210.2, cold-adj -10'),
    ('5 n [5]',               5, None,  '?', [213.2], 'B06 5-node queue item'),
    ('5 n [2,5]',             5, '2,5', '?', [226.2], 'B06'),
    ('5 n [4,5]',             5, '4,5', '?', [232.2], 'B06'),
    ('6 n [2,6]',             6, None,  '?', [223.6, 271.5, 258.0, 271.5, 282.8, 293.8, 335.8], 'f3 single6 r4_A, single6b r2_A r3_A r5_A; S48 n6_r2, n6_r1 (cold-adj -10.3); C09 r1_A'),
    ('6 n [6]',               6, '6',   '?', [231.3, 234.3, 238.2, 252.8, 260.0], 'f3 single6 r2_B r3_B r5_B, single6b r4_B; C09 r1_B'),
    ('6 n [3,6]',             6, '3,6', '?', [263.8, 264.1], 'C09 run6 r1_C, r2_C'),
]
def l4_wall(g, gr, bw, v, consts, ns, isc, ko=1.0, crit=True):
    M.TREE_CRIT = crit
    return model(g, DPN, bw, M.AAC7.lat, ns, isc, consts, groups=gr, v_factor=v)['wall_nowrite']
def _bis(f, lo, hi, it=16):
    for _ in range(it):
        m = math.sqrt(lo * hi)
        if f(m) > 0: lo = m
        else: hi = m
    return math.sqrt(lo * hi)
def l4_fit():
    ac = dict(M.AAC7_CONSTS); j = json.load(open(FIT_FILE)); ac.update(j['consts']); ns, isc = ac['NODE_SCALE'], ac['INIT_SCALE']
    mean_ = lambda xs: sum(xs) / len(xs)
    lr = lambda m, t: math.log(m / t)
    B = [(g, gr, mean_(t)) for (_, g, gr, md, t, _) in L4_CLASSES if md == 'B' and M.is_pow2(g)]
    A = [(g, gr, mean_(t)) for (_, g, gr, md, t, _) in L4_CLASSES if md == 'A']
    N = [(g, gr, mean_(t)) for (_, g, gr, md, t, _) in L4_CLASSES if not M.is_pow2(g)]
    bwB = _bis(lambda b: sum(lr(l4_wall(g, gr, b, 1.0, ac, ns, isc), t) for g, gr, t in B) / len(B), 3.0, 40.0)
    alpha = _bis(lambda a_: sum(lr(l4_wall(g, gr, bwB * a_, 1.0, ac, ns, isc), t) for g, gr, t in A) / len(A), 0.3, 1.2)
    best = None
    v0 = None
    for pen in (0.0, 0.05, 0.10, 0.15, 0.20, 0.30, 0.45, 0.60):        # the multi-node-children level penalty (M.MIX_PEN): v by mean-log = 0 at each pen, pen by least squares
        M.MIX_PEN = pen
        v_ = _bis(lambda v__: sum(lr(l4_wall(g, gr, bwB, v__, ac, ns, isc), t) for g, gr, t in N) / len(N), 0.3, 1.5, 14)
        e = sum(lr(l4_wall(g, gr, bwB, v_, ac, ns, isc), t) ** 2 for g, gr, t in N)
        if v0 is None: v0 = v_
        print('  pen %.2f: v %.3f, sum sq log residual (non-pow2 classes) %.4f' % (pen, v_, e), flush=True)
        if best is None or e < best[0] - 1e-9: best = (e, v_, pen)
    print('  (the penalty is not identified: the residual falls monotonically with it up to the grid edge while v rises to compensate; the adopted fit keeps pen = 0, best-at-edge %.2f is a sensitivity only)' % best[2])
    v, pen = v0, 0.0; M.MIX_PEN = 0.0
    return dict(bw_apu=bwB, alpha_slow=alpha, layv_factor=v, layv_factor_nopen=v0, mix_pen=pen, lat=M.AAC7.lat, k_other=1.0, write_bw=0.116,
                consts={k: ac[k] for k in ('HIDE_POW2', 'GEN_HIDE1', 'GEN_HIDE2', 'T_ROUND', 'MAP_RATE', 'NODE_SCALE', 'INIT_SCALE')},
                fitted_on={c_[0]: dict(nodes=c_[1], groups=c_[2], mode=c_[3], totals=c_[4], src=c_[5]) for c_ in L4_CLASSES}), ac, ns, isc
def l4_main(a):
    f, ac, ns, isc = l4_fit()
    j0 = json.load(open(FIT_FILE)); ac0 = dict(M.AAC7_CONSTS); ac0.update(j0['consts'])
    print('L4 fit (modelled on measured totals): bw_apu(B) %.2f GB/s (S48 %.2f)  alpha(A/B) %.3f  layv factor %.3f (S48 %.3f)  mix_pen %.2f  k_other 1.0 (tested 1.0..0.45: no gain)' % (f['bw_apu'], j0['bw_apu'], f['alpha_slow'], f['layv_factor'], j0['layv_factor'], f['mix_pen']))
    print('%-18s %-8s %9s | %-26s | %-26s' % ('class', 'group', 'measured', 'before (S48, top-only)', 'after (L4)'))
    rows = []
    for (lab, g, gr, md, ts, src) in L4_CLASSES:
        t = sum(ts) / len(ts)
        M.TREE_CRIT = False; _mp = M.MIX_PEN; M.MIX_PEN = 0.0
        b0 = model(g, DPN, j0['bw_apu'], j0['lat'], ac0['NODE_SCALE'], ac0['INIT_SCALE'], ac0, groups=gr, v_factor=j0['layv_factor'])['wall_nowrite']
        M.MIX_PEN = _mp
        out = []
        for grp in (('B',) if md == 'B' else ('A',) if md == 'A' else ('B', 'A')):
            bw = f['bw_apu'] * (f['alpha_slow'] if grp == 'A' else 1.0)
            w1 = l4_wall(g, gr, bw, f['layv_factor'], ac, ns, isc)
            out.append((grp, w1))
        rows.append((lab, g, gr, md, t, b0, out))
        print('%-18s %-8s %9.1f | %7.1f %+7.1f %%          | %s' % (lab, md, t, b0, 100 * (b0 / t - 1), '   '.join('%s %6.1f %+6.1f %%' % (gp, w, 100 * (w / t - 1)) for gp, w in out)))
    M.TREE_CRIT = True
    rb = [math.log(r[5] / r[4]) for r in rows]; ra = [math.log(r[6][0][1] / r[4]) for r in rows]
    print('rms log residual: before %.3f  after %.3f (all %d classes; after = group B for the "?" classes)' % ((sum(x * x for x in rb) / len(rb)) ** .5, (sum(x * x for x in ra) / len(ra)) ** .5, len(rows)))
    if a.write_profile:
        json.dump(f, open(L4_FILE, 'w'), indent=1); print('wrote', L4_FILE)

def selftest():
    import tempfile; M.apply_profile('aac7_c16')
    d = tempfile.mkdtemp()
    for n in (1, 2, 4):
        for k in (1, 2):
            tot = {1: 78.0, 2: 156.0, 4: 221.0}[n] + k * 0.3
            with open(os.path.join(d, 'n%d_r%d.log' % (n, k)), 'w') as f:
                f.write('tl    0.00  rns_init begins\ntl   14.20  join\n')
                for pe in range(n):
                    f.write('%d: all %d nodes: VERIFY OK\n' % (pe, n) if pe == 0 else '')
                    f.write('%d: xstats pe %d phase dm kind layv: n 100 rounds 50 GB 400.000 t_post 100.000 s t_call 90.000 s t_wait 10.000 s roff_wait 0 s sig_wait 0 s (x)\n' % (pe, pe))
                    f.write('%d: wait-stats node %d: bs barrier 1.000 s (3) wait 2.000 s (9) ready 0.000 s (0) | dm barrier 1.0 s (3) wait 20.0 s (9) ready 0.000 s (0)\n' % (pe, pe))
                f.write('0: total %8.2f s   (bs 30.0 + 10dP 0.0 + dm 40.0 + T1 1.0 + dc 1.0 + T2 1.0 = 73.0; init 14.2; other 0.8); VmHWM 1.0 GB\n' % tot)
    runs = collect([d]); assert len(runs) == 6 and all(r['verify'] and r['total'] for r in runs), runs
    assert abs(runs[0]['init'] - 14.2) < 1e-9 and runs[0]['nodes'] == 1
    meas = summarize(runs, {})
    assert sorted(meas) == [1, 2, 4], meas
    rc = rate_checks(runs); assert abs(rc[4]['xs'][('dm', 'layv')][0] / rc[4]['xs'][('dm', 'layv')][2] - 400.0) < 1e-6, rc[4]
    ns = bisect(lambda s: model(1, DPN, 3.6, 8.5e-6, s, 1.2)['wall_nowrite'], 0.2, 5.0, 78.0)
    w = model(1, DPN, 3.6, 8.5e-6, ns, 1.2)['wall_nowrite']; assert abs(w - 78.0) < 0.5, (ns, w)
    print('selftest OK (parser, summary, rate check, node_scale %.3f -> %.1f s at 1 node)' % (ns, w))

if __name__ == '__main__':
    main()

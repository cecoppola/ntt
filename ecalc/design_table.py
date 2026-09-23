#!/usr/bin/env python3
"""design_table.py - the design table for the 576-node target (PLAN.md 31, Phase 13b agent D).

Every combination of the options that still differ -- product strategy S (C, B, B4, auto) x plane cap K (2^30, 3 2^29,
2^31, 3 2^30) x exchange-scratch chunking (off, MDB_SHIFT_CHUNK_MB, both) x uneven-exchange depth (1, 2) = 96 rows, each
on the step-0 defaults (three primes, NTT_MODMUL=1) -- with, per row:
  (a) 4 x 10^10 wall on one node          (b) the node peak at 4 x 10^10 (device + host HWM)
  (c) the 576-node maximum digits at 502 and at 480 GB per node
  (d) the 576-node wall at that maximum (502 GB)
  (e) the 576-node wall at one common size, 4 x 10^13 digits
  (f) (e) at 50 and 200 GB/s per APU of fabric (the one input aac6 cannot measure; 100 GB/s is PLAN 25's)
and the Pareto front on ((e), (c) at 502 GB), the fastest, the largest and a recommended balance.  Every cell is labelled
measured (an aac6 run of that configuration, from the M-run log), modelled (mn_model / mem_model on measured inputs) or
assumed (a target parameter).

    ./design_table.py                          the table (all modelled unless --mrun), written to ../results/DESIGN_TABLE.md
    ./design_table.py --mrun mrun.log          measured per-node inputs from the integrator's campaign replace modelled ones
    ./design_table.py --calibrate [--mrun ..]  model against every measured run (RESULTS 75-78, results/*.md, the M-run): wall, peak, error
    ./design_table.py --make-line LOG KEY=V..  the M-run line of one ecalc log (the integrator's helper; cmp=<verdict> wins)
    ./design_table.py --regen LOGDIR [PROGRESS] > mrun.log   the M-run log rebuilt from the campaign's tagged logs
    ./design_table.py --quick                  a 12-row subset (depth 1, chunking off/both, C/B/auto at 2^31 / 3 2^30) for a fast look

The M-run log: one line per run, fields separated by '|':
    <KEY=VALUE ...> | <ecalc's `total ...` line> | <peak memory>
  e.g.
    digits=40000000000 size=1 RNS_STRATEGY=B ECALC_PLANE_CAP=2^31 | total    66.51 s   (bs 25.9 + 10dP 0.1 + dm 20.9 + T1 0.0 + dc 0.0 + T2 0.0 = 46.9; init 19.6; other 0.0); VmHWM 12.1 GB | device 287.5 GB identical
  KEY=VALUE: digits= (or D=), size= (node-processes; default 1), and the run's environment.  The axes are read from it:
  RNS_STRATEGY (C), the cap from ECALC_PLANE_CAP (2^30, 3*2^29, 2^31, 3*2^30, or 30, 3x29, ...) else POOL_LOG + RNS_PLANES_3Q30
  (the code's rule when unset), chunking from MDB_SHIFT_CHUNK_MB / MN_T_CHUNK_MB, the depth from COMM_ALLTOALLV_DEPTH (any key with DEPTH),
  ECALC_NP (3), NTT_MODMUL (1).  From the total line: total, init, bs, dm; `recip X` if present; VmHWM.  The peak: `device X GB`
  (mem_report's device total; else `peak X GB` = device + host).  A line with DIFFERS or FAILED is reported and not used.
"""
import argparse, math, os, re, sys, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mn_model as M
import mem_model as MM

GB = 1e9
EDGE_GB = 524.0                                  # MEASURED on aac6 (P13b): a node runs while device + host HWM <= ~524 GB (529.6 was OOM-killed)
NODES = 576
COMMON = 4e13                                   # the common size of column (e)
BWS = [50.0, 100.0, 200.0]                      # GB/s per APU: (f) low, (e), (f) high (--bws)
def BE(): return BWS[1]
def BL(): return BWS[0]
def BH(): return BWS[2]
CAPS = [1 << 30, 3 << 29, 1 << 31, 3 << 30]
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'results', 'DESIGN_TABLE.md')
FIT_TROUND = False                              # --fit-tround: refit T_ROUND from the M-run's chunked rows (off: the aac6 sweep shows no trend, M-run 13b)
BUILT = {'C': 'on main', 'B': 'agent B (p13b-B)', 'B4': 'agent B (p13b-B)', 'auto': 'agent B (p13b-B)'}

def fabric(bw):
    t = M.TARGET
    return M.Fabric(t.name, bw, t.lat, group=t.group, layers=t.layers, taper=t.taper, write_bw=t.write_bw)

# ------------------------------------------------------------------------------------------------------------ the M-run log
def parse_cap(v):
    v = v.strip().lower().replace('x', '*')
    m = re.match(r'^(?:(\d)\*)?2\^(\d+)$', v)
    if m: return (int(m.group(1) or 1)) << int(m.group(2))
    m = re.match(r'^3\*(\d+)$', v)
    if m: return 3 << int(m.group(1))
    x = int(float(v))
    return 1 << x if x < 64 else x

def parse_mrun(path):
    """-> list of dict(env, D, g, total, init, bs, dm, recip, hwm, dev, peak, ok, design, line)"""
    runs = []
    for ln, line in enumerate(open(path, errors='replace'), 1):
        s = line.strip()
        if not s or s.startswith('#') or 'total' not in s: continue
        head = s.split('total', 1)[0]
        env = dict(re.findall(r'([A-Za-z_][A-Za-z0-9_]*)=([^\s|]+)', head))
        def f(p, src=s):
            m = re.search(p, src); return float(m.group(1)) if m else None
        r = dict(env=env, line=ln, text=s)
        r['D'] = float(env.get('digits', env.get('D', 'nan')))
        r['g'] = int(env.get('size', env.get('g', env.get('procs', 1))))
        r['total'] = f(r'total\s+([\d.]+) s'); r['init'] = f(r'init ([\d.]+)'); r['bs'] = f(r'\(bs ([\d.]+)')
        r['dm'] = f(r'dm ([\d.]+)'); r['recip'] = f(r'recip ([\d.]+)'); r['hwm'] = f(r'VmHWM ([\d.]+) GB')
        r['dev'] = f(r'device ([\d.]+) GB'); r['peak'] = f(r'peak ([\d.]+) GB')
        r['verdict'], r['ok'] = line_verdict(s.split(' tag=')[0])
        r['k'] = 'NTT_B1R' in env or 'NTT_PLAN' in env                  # agent K's kernels on (the M-run's default rows)
        r['devmax'] = f(r'max in use ([\d.]+)'); r['peaklive'] = f(r'peak live ([\d.]+)')
        np_ = int(env.get('ECALC_NP', 3)); mm = int(env.get('NTT_MODMUL', 1)); st = env.get('RNS_STRATEGY', 'C')
        cap = None
        for k in ('ECALC_PLANE_CAP', 'PLANE_CAP', 'RNS_PLANE_CAP', 'cap'):
            if k in env and env[k].lower() != 'fit': cap = parse_cap(env[k])   # 'fit' (agent P: the largest cap that fits): the code's choice, printed in the log
        if cap is None and ('POOL_LOG' in env or 'RNS_PLANES_3Q30' in env):
            pl = int(env.get('POOL_LOG', 31)); r3 = env.get('RNS_PLANES_3Q30')
            if r3 is None or r3 == 'auto': cap = MM.code_cap(r['D'] * r['g'], pl)
            else: cap = (3 << (pl - 1)) if int(r3) else (1 << pl)
        sh = float(env.get('MDB_SHIFT_CHUNK_MB', 0)); tm = float(env.get('MN_T_CHUNK_MB', 0))
        chunk = 'both' if sh > 0 and tm > 0 else ('shift' if sh > 0 else ('t-only' if tm > 0 else 'off'))
        depth = 1
        for k, v in env.items():
            if 'DEPTH' in k.upper():
                try: depth = int(v)
                except ValueError: pass
        r['design'] = M.Design(np=np_, strategy=st, cap=cap, chunk=chunk if chunk != 't-only' else 'off', depth=depth, modmul=mm, chunk_mb=sh or tm or M.CHUNK_MB)
        r['axes'] = (st, cap, chunk, depth, np_, mm)
        runs.append(r)
    return runs

def run_peak(r):
    """a size-1 run's node peak: the largest device total over the mem summary's phases (B / B4 map their extra planes after init)
    + the host HWM; a line without the maximum takes the device at init"""
    if r['dev'] is None or r['hwm'] is None: return r['peak']
    return max(r['dev'], r['devmax'] or 0.0) + r['hwm']

# ------------------------------------------------------------------------------------------------------------ one row
class Row:
    pass

MEMO = {}
def max_d(design, budget):
    """the largest D per node at 576 that fits `budget` GB (memoised on what the memory depends on)"""
    k = (design.strategy if design.strategy in ('B', 'B4') else 'C', design.cap, design.chunk, design.depth, design.np, budget)   # B and B4 carry extra planes; C and auto share the pools
    if k not in MEMO: MEMO[k] = M.max_digits(NODES, budget, design=design)
    return MEMO[k]

def size1_4e10(design):
    p = M.node_phases(4e10, design)
    wall = sum(v for k, v in p.items() if k != 'label')
    m = MM.mem_per_node(int(4e10), 1, design.mem_opts(4e10))
    return wall, m['node_peak'] / GB, m['dev_init'] / GB, p['label']

def evaluate(design, meas=None, peak_delta=0.0, leaf_scale=1.0, lab_meas=''):
    r = Row(); r.d = design
    r.a, r.b, r.b_dev, lab = size1_4e10(design)
    r.a_lab = r.b_lab = 'modelled'
    if not meas and leaf_scale != 1.0: r.a *= leaf_scale; r.a_lab = 'modelled x K'
    if meas:
        if meas.get('wall') is not None: r.a = meas['wall']; r.a_lab = 'measured (n=%d)' % meas['n']
        if meas.get('peak') is not None: r.b = meas['peak']; r.b_lab = 'measured'
    r.maxd = {}
    for budget in (502.0, 480.0, EDGE_GB):
        r.maxd[budget] = max_d(design, budget - max(0.0, peak_delta))
    r.walls = {}
    D = r.maxd[502.0]
    r.wall_max = M.run(fabric(BE()), D, NODES, verbose=False, design=design, leaf_scale=leaf_scale)['wall'] if D else None
    for bw in BWS:
        rr = M.run(fabric(bw), COMMON / NODES, NODES, verbose=False, design=design, leaf_scale=leaf_scale)
        r.walls[bw] = rr['wall']
        if bw == BE(): r.run4e13 = rr
    r.fits = r.maxd[502.0] * NODES >= COMMON * 0.9999
    r.fits480 = r.maxd[480.0] * NODES >= COMMON * 0.9999
    r.lab576 = 'modelled' + (' on measured per-node input' if lab_meas else '')
    return r

def rows_all(quick=False):
    out = []
    for st in M.STRATEGIES:
        for cap in CAPS:
            for ch in M.CHUNKS:
                for dp in (1, 2):
                    if quick and (dp == 2 or ch == 'shift' or st == 'B4' or cap not in (1 << 31, 3 << 30)): continue
                    out.append(M.Design(np=3, strategy=st, cap=cap, chunk=ch, depth=dp, modmul=1))
    return out

# ------------------------------------------------------------------------------------------------------------ the M-run's use
def mrun_inputs(runs, log):
    """from the M-run: per (strategy, cap) at size 1, 4e10: the measured wall (mean) and peak; per chunk / depth at g > 1: the fitted
    T_ROUND and the two-deep hide (if the size is not a power of two).  Returns (by_sk, notes)."""
    by_sk = {}; notes = []; koff = {}
    kon = any(r['k'] for r in runs if r['g'] == 1)                  # the M-run's rows run agent K's kernels (NTT_B1R, NTT_PLAN): the table is K on
    for r in runs:
        if not r['ok']: notes.append('M-run line %d: digits differ or failed -- not used' % r['line']); continue
        if r['g'] == 1 and abs(r['D'] - 4e10) < 1e6 and r['total'] is not None and r['design'].np == 3 and r['design'].modmul == 1:
            k = (r['design'].strategy, r['design'].cap_at(4e10))
            if r['k'] != kon: koff.setdefault(k, []).append(r['total']); continue
            e = by_sk.setdefault(k, dict(walls=[], peaks=[], devs=[]))
            e['walls'].append(r['total'])
            if r['dev'] is not None and r['hwm'] is not None: e['peaks'].append(run_peak(r)); e['devs'].append(r['devmax'] or r['dev'])
            elif r['peak'] is not None: e['peaks'].append(r['peak'])
    for k, e in by_sk.items():
        e['n'] = len(e['walls']); e['wall'] = sum(e['walls']) / e['n']
        e['sd'] = (sum((x - e['wall']) ** 2 for x in e['walls']) / max(1, e['n'] - 1)) ** 0.5
        e['peak'] = max(e['peaks']) if e['peaks'] else None
    by_sk['_fk'] = 1.0
    for k, ws in koff.items():                                        # agent K's kernels: measured on / off at the same (strategy, cap)
        if k in by_sk:
            fk = by_sk[k]['wall'] / (sum(ws) / len(ws)); by_sk['_fk'] = fk
            notes.append('agent K\'s kernels (NTT_B1R=3 NTT_PLAN=1) at %s %s: %.2f s on (n %d) against %.2f s off (n %d): x %.3f, applied to the rows '
                         'the M-run did not measure (measured)' % (k[0], MM.cap_name(k[1]), by_sk[k]['wall'], by_sk[k]['n'], sum(ws) / len(ws), len(ws), fk))
    # chunking at g > 1: refit T_ROUND on (off, shift, both) at the same D and g
    groups = {}
    for r in runs:
        if r['ok'] and r['g'] > 1 and r['total'] is not None: groups.setdefault((r['D'], r['g'], r['design'].depth), []).append(r)
    num = den = 0.0
    for (D, g, dp), rs in groups.items():
        base = [x['total'] for x in rs if x['axes'][2] == 'off']
        if not base: continue
        b = sum(base) / len(base)
        for x in rs:
            if x['axes'][2] in ('shift', 'both'):
                fab = M.aac6_fabric('tcp', g); saved = M.T_ROUND
                def w(tr, ch):
                    M.T_ROUND = tr
                    return M.run(fab, D / g, g, verbose=False, leaf_scale=0.0, init_override=6.5, dc_exposed=2.4, form='grid', transport='tcp', pool_log=29,
                                 design=M.Design(np=4, legacy=True, chunk=ch, chunk_mb=x['design'].chunk_mb))['wall']
                e0 = w(0.0, x['axes'][2]) - w(0.0, 'off'); rounds = (w(0.01, x['axes'][2]) - w(0.0, 'off') - e0) / 0.01; M.T_ROUND = saved
                num += rounds * (x['total'] - b - e0); den += rounds * rounds
                notes.append('M-run chunking %s at %g MB, %.0e / %d, depth %d: %+.1f s against off (%s)' % (x['axes'][2], x['design'].chunk_mb, D, g, dp, x['total'] - b,
                             '%d extra rounds in the model' % rounds if rounds >= 0.5 else 'no extra round at this size: the chunk is larger than the share, T_ROUND is not refitted from it'))
    # the depth pairs: the same D, g and chunking at depth 1 and 2 (the general map: g not a power of two, or DIST_GEN=1)
    dp_groups = {}
    for r in runs:
        if r['ok'] and r['g'] > 1 and r['total'] is not None:
            dp_groups.setdefault((r['D'], r['g'], r['axes'][2], r['design'].chunk_mb, r['env'].get('DIST_GEN', '0')), {}).setdefault(r['design'].depth, []).append(r['total'])
    for (D, g, ch, mb, gen), byd in sorted(dp_groups.items()):
        if 1 in byd and 2 in byd:
            m1, m2 = sum(byd[1]) / len(byd[1]), sum(byd[2]) / len(byd[2])
            general = (g & (g - 1)) or gen == '1'
            notes.append('M-run depth at %.0e / %d%s, chunking %s: depth 1 %.2f s (n %d), depth 2 %.2f s (n %d): %+.1f %% (measured; %s)' % (
                D, g, ' DIST_GEN=1' if gen == '1' else '', ch, m1, len(byd[1]), m2, len(byd[2]), 100 * (m2 / m1 - 1),
                'the general map' if general else 'a power of two: the switch is not exercised'))
    if den > 0 and FIT_TROUND:
        M.T_ROUND = max(0.0, num / den)
        notes.append('T_ROUND refitted on the M-run: %.3f s per extra round (was 0.030, M13)' % M.T_ROUND)
    elif den > 0:
        notes.append('T_ROUND stays ASSUMED at %.3f s (range 0.01-0.1 s, the table prints both ends for the recommended row): the chunk sweep on aac6 '
                     'loopback shows no trend above its +-10 %% noise (a refit would give %.3f s; --fit-tround takes it)' % (M.T_ROUND, max(0.0, num / den)))
    return by_sk, notes

# ------------------------------------------------------------------------------------------------------------ the table
def pareto(rows):
    """rows on the front of (min wall at 4e13 over the rows that hold 4e13 at 502 GB, max digits at 502 GB)"""
    cand = [r for r in rows if r.fits]
    front = []
    for r in cand:
        dom = any((o.walls[BE()] <= r.walls[BE()] and o.maxd[502.0] >= r.maxd[502.0]) and (o.walls[BE()] < r.walls[BE()] - 1e-9 or o.maxd[502.0] > r.maxd[502.0]) for o in cand)
        if not dom: front.append(r)
    return front

def rank(rows, key):
    order = sorted(rows, key=key)
    return {id(r): i + 1 for i, r in enumerate(order)}

def fmt_d(x): return '%.2f' % (x / 1e13)

def git_head():
    try: return subprocess.check_output(['git', 'rev-parse', '--short', 'HEAD'], cwd=HERE, stderr=subprocess.DEVNULL).decode().strip()
    except Exception: return '?'

def build(args):
    t0 = time.time()
    runs = parse_mrun(args.mrun) if args.mrun else []
    by_sk, notes = mrun_inputs(runs, args.mrun) if runs else ({}, [])
    rows = []
    designs = rows_all(args.quick)
    for i, d in enumerate(designs):
        meas = by_sk.get((d.strategy, d.cap))
        leaf_scale = by_sk.get('_fk', 1.0); peak_delta = 0.0
        if meas:
            wall_m, peak_m, _, _ = size1_4e10(d)
            leaf_scale = meas['wall'] / wall_m                       # the per-node compute: measured / modelled at 4e10, applied to the leaf
            if meas.get('peak') is not None: peak_delta = meas['peak'] - peak_m
        r = evaluate(d, meas, peak_delta, leaf_scale, 'yes' if meas else '')
        r.leaf_scale, r.peak_delta = leaf_scale, peak_delta
        rows.append(r)
        if args.verbose: print('%3d/%d %-22s %.0f s' % (i + 1, len(designs), d.name(), time.time() - t0), file=sys.stderr)
    front = pareto(rows)
    fits = [r for r in rows if r.fits]
    fastest = min(fits, key=lambda r: r.walls[BE()]) if fits else None
    largest = max(rows, key=lambda r: (r.maxd[502.0], -r.walls[BE()]))
    best_w = fastest.walls[BE()] if fastest else 1.0
    best_d = largest.maxd[502.0]
    # the recommended balance: on the front, the best of (safe digits / the largest) + (the fastest wall / its wall), built options first
    def score(r): return r.maxd[480.0] / best_d + best_w / r.walls[BE()]
    rec = max(front, key=score) if front else None
    ranks = {bw: rank(fits, lambda r, bw=bw: r.walls[bw]) for bw in BWS}
    return dict(rows=rows, front=front, fastest=fastest, largest=largest, rec=rec, ranks=ranks, runs=runs, notes=notes, by_sk=by_sk, secs=time.time() - t0)

def write_md(res, args):
    rows, front, F, L, R, ranks = res['rows'], res['front'], res['fastest'], res['largest'], res['rec'], res['ranks']
    fset = set(id(r) for r in front)
    L_ = []
    L_.append('# DESIGN_TABLE — the 576-node design space (PLAN §31, Phase 13b agent D)\n')
    L_.append('Generated by `ecalc/design_table.py%s` at commit `%s`, %s. **%s.** Every row carries step 0 (three primes, `NTT_MODMUL=1`).\n' % (
        (' --mrun ' + os.path.basename(args.mrun)) if args.mrun else '', git_head(), time.strftime('%Y-%m-%d %H:%M'),
        'Per-node inputs measured by the M-run where marked' if res['by_sk'] else 'All cells modelled: no M-run log was given'))
    L_.append('Labels: **measured** = an aac6 run of that configuration (the M-run); **modelled** = `mn_model.py` / `mem_model.py` arithmetic on measured inputs '
              '(the per-product times of S13, the phase table of the current code, X13\'s overlap, M13\'s chunk rounds, the memory formulas of the code); '
              '**assumed** = the target\'s fabric (100 GB/s per APU, 2 µs per message, 64-node dragonfly groups), the part file (2 GB/s per node) '
              'and the chunk rounds\' fixed cost (T_ROUND %.3f s, ASSUMED: range 0.01-0.1 s; the aac6 chunk sweep at 16-1024 MB shows no trend above its +-10 %% loopback noise, '
              'so the chunked rows\' wall cost is unmeasured -- the recommended row\'s sensitivity is printed below). Column (f) varies the fabric bandwidth.\n' % M.T_ROUND)
    L_.append('Columns: S = `RNS_STRATEGY`; K = plane cap; chunk = `MDB_SHIFT_CHUNK_MB` (shift) / + `MN_T_CHUNK_MB` (both), at %d MB; depth = the uneven exchange two deep; '
              '(a) 4 × 10¹⁰ wall at size 1 [s]; (b) node peak at 4 × 10¹⁰ [GB]; (c) the 576-node maximum digits [×10¹³] at 502 / 480 GB per node (the target\'s columns: use them until the target\'s own edge is measured) '
              'and at 524 GB, the edge measured on one aac6 node (P13b: device + host HWM 523.8 GB ran, 529.6 was OOM-killed; not the target\'s figure); '
              '(d) the 576-node wall at the 502-GB maximum [min]; (e) the 576-node wall at 4 × 10¹³ digits [min] at %g GB/s per APU; (f) the same at %g / %g GB/s; '
              'rank = (e)\'s rank at %g / %g / %g GB/s among the rows that hold 4 × 10¹³. Marks: **P** Pareto front on ((e), (c) at 502), **F** fastest, **L** largest, **R** recommended. '
              '(e) in parentheses: the row cannot hold 4 × 10¹³ at 502 GB. Labels column: mod = modelled, meas = measured, (meas. node) = the 576 model scaled to the measured one-node run.\n' % (M.CHUNK_MB, BE(), BL(), BH(), BL(), BE(), BH()))
    L_.append('| # | S | K | chunk | depth | (a) s | (b) GB | (c) 502 | (c) 480 | (c) 524 edge | (d) min | (e) min | (f) %g | (f) %g | rank %g/%g/%g | mark | labels (a) / (b) / 576 |' % (BL(), BH(), BL(), BE(), BH()))
    L_.append('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    order = sorted(rows, key=lambda r: (not r.fits, r.walls[BE()]))
    for i, r in enumerate(order, 1):
        d = r.d; mk = ''.join(c for c, cond in (('P', id(r) in fset), ('F', r is F), ('L', r is L), ('R', r is R)) if cond)
        e = lambda x: ('%.2f' % (x / 60)) if r.fits else '(%.2f)' % (x / 60)
        rk = '/'.join(str(ranks[bw].get(id(r), '-')) for bw in BWS)
        labs = '%s / %s / %s' % (r.a_lab.replace('modelled', 'mod').replace('measured', 'meas'), r.b_lab.replace('modelled', 'mod').replace('measured', 'meas'), r.lab576.replace('modelled', 'mod').replace(' on measured per-node input', ' (meas. node)'))
        L_.append('| %d | %s | %s | %s | %d | %.1f | %.1f | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
            i, d.strategy, MM.cap_name(d.cap), d.chunk, d.depth, r.a, r.b, fmt_d(r.maxd[502.0] * NODES), fmt_d(r.maxd[480.0] * NODES), fmt_d(r.maxd[EDGE_GB] * NODES),
            '%.2f' % (r.wall_max / 60) if r.wall_max else '-', e(r.walls[BE()]), e(r.walls[BL()]), e(r.walls[BH()]), rk, mk, labs))
    L_.append('')
    def desc(r, tag):
        if r is None: return '- %s: none' % tag
        return ('- **%s: S %s, K %s, chunking %s, depth %d** — %.2f × 10¹³ digits at 502 GB (%.2f at 480), %.2f min at its maximum; 4 × 10¹³ in %.2f min '
                '(%.2f at the low, %.2f at the high bandwidth); one node at 4 × 10¹⁰: %.1f s, %.0f GB. Built: %s%s. Environment: `%s`' % (
                tag, r.d.strategy, MM.cap_name(r.d.cap), r.d.chunk, r.d.depth, r.maxd[502.0] * NODES / 1e13, r.maxd[480.0] * NODES / 1e13,
                (r.wall_max or 0) / 60, r.walls[BE()] / 60, r.walls[BL()] / 60, r.walls[BH()] / 60, r.a, r.b, BUILT[r.d.strategy],
                '' if r.d.depth == 1 else ', depth 2 = agent X (p13b-X)', ' '.join('%s=%s' % kv for kv in r.d.env().items())))
    L_.append('## The marked rows\n')
    for r, tag in ((F, 'fastest (F)'), (L, 'largest (L)'), (R, 'recommended balance (R)')): L_.append(desc(r, tag))
    L_.append('- Pareto front (%d rows): %s' % (len(front), '; '.join('%s/%s/%s/d%d' % (r.d.strategy, MM.cap_name(r.d.cap), r.d.chunk, r.d.depth) for r in sorted(front, key=lambda r: r.walls[BE()]))))
    L_.append('\nThe recommended balance maximises (its safe digits at 480 GB / the largest row\'s digits at 502) + (the fastest wall / its wall) over the front. '
              '`auto` never allocates: rns_dist.c\'s b_choose takes the B form (`RNS_STRATEGY_FORM`, default B) only where its whole planes fit '
              'the pools as sized at init (b_place), and its grid (`RNS_STRATEGY_GRID=1`) weighs the pieces that fit B at 0.70 per point, so the products run B '
              'on pieces the pools hold (at 3*2^30: 2^31 pieces; at 2^31: 2^30 pieces). `B` and `B4` forced take what the pools lack from a grow-only '
              'hipMalloc buffer (B at 2^31: 32 GiB on each of three APUs).\n')
    # the ranking against the fabric assumption
    fits = [r for r in rows if r.fits]
    def spearman(a, b):
        n = len(fits)
        if n < 3: return float('nan')
        return 1 - 6 * sum((ranks[a][id(r)] - ranks[b][id(r)]) ** 2 for r in fits) / (n * (n * n - 1))
    L_.append('## Does the ranking survive the fabric assumption?\n')
    top = lambda bw: min(fits, key=lambda r: r.walls[bw]) if fits else None
    L_.append('Spearman rank correlation of (e) over the %d rows that hold 4 × 10¹³: %g vs %g GB/s %.3f, %g vs %g GB/s %.3f. '
              'The fastest row at %g / %g / %g GB/s: %s.\n' % (len(fits), BL(), BE(), spearman(BL(), BE()), BH(), BE(), spearman(BH(), BE()), BL(), BE(), BH(), 
              ' / '.join('%s %s %s d%d' % (t.d.strategy, MM.cap_name(t.d.cap), t.d.chunk, t.d.depth) for t in (top(BL()), top(BE()), top(BH())) if t)))
    if R is not None:
        rr = R.run4e13
        L_.append('## The recommended row at 4 × 10¹³ (576 nodes, 100 GB/s), by phase\n')
        L_.append('init %.1f, batch %.1f, top levels %.1f, distributed levels %.1f, reciprocal %.1f, division %.1f, part file exposed %.1f, other %.1f s = **%.1f s**; '
                  'exposed communication %.1f s; node peak %.0f GB (device %.0f: planes %.0f, arena %.0f, exchange %.0f; host %.0f).\n' % (
                  rr['init'], rr['batch'], rr['top'], rr['levels'], rr['recip'], rr['div'], rr['out'], rr['other'], rr['wall'], rr['exposed'],
                  rr['mem']['node'], rr['mem']['device'], rr['mem']['planes'], rr['mem']['arena'], rr['mem']['exchange'], rr['mem']['host']))
        sens = []
        saved = M.T_ROUND
        for tr in (0.01, 0.1):
            M.T_ROUND = tr; sens.append((tr, M.run(fabric(BE()), COMMON / NODES, NODES, verbose=False, design=R.d, leaf_scale=R.leaf_scale)['wall']))
        M.T_ROUND = saved
        L_.append('Sensitivity of the recommended row to the chunk rounds\' fixed cost (assumed on the target): T_ROUND %s.\n' % ', '.join('%.2f s -> %.2f min' % (tr, w / 60) for tr, w in sens))
    if res['by_sk'] or res['notes']:
        L_.append('## The M-run inputs\n')
        for (st, cap), e in sorted(((k, v) for k, v in res['by_sk'].items() if isinstance(k, tuple)), key=lambda kv: (kv[0][0], kv[0][1])):
            d = M.Design(strategy=st, cap=cap); wm, pm, _, _ = size1_4e10(d); wm *= res['by_sk'].get('_fk', 1.0)   # the M-run's rows run agent K's kernels
            L_.append('- %s at %s: measured %.1f ± %.1f s (n = %d) against modelled %.1f s (%+.1f %%); peak %s against modelled %.1f GB' % (
                st, MM.cap_name(cap), e['wall'], e['sd'], e['n'], wm, 100 * (e['wall'] / wm - 1), ('%.1f GB' % e['peak']) if e['peak'] else '-', pm))
        for n in res['notes']: L_.append('- ' + n)
        L_.append('')
    L_.append('## How to regenerate\n')
    L_.append('`cd ecalc && ./design_table.py [--mrun <log>]` (about %.0f s); `./design_table.py --calibrate [--mrun <log>]` prints the model against every measured run. '
              'On the target: measure the fabric first (docs/TARGET.md §6) and pass `--bws <low,measured,high>`, `--lat`, `--write-bw` (and `--hide-pow2`, `--gen-hide2` from the `COMM_LAYER_STATS` both-busy fractions).\n' % res['secs'])
    open(args.out, 'w').write('\n'.join(L_) + '\n')
    return '\n'.join(L_)

# ------------------------------------------------------------------------------------------------------------ calibration
LOOPBACK = [   # the multi-process runs on one node (TCP loopback): (D total, g, wall, init, switches, POOL_LOG, dc exposed, source)
    (1e10, 4, 113.85, 6.7, 'off', 29, 2.4, 'M13 b1 e10x4_base (job 21008)'),
    (1e10, 4, 111.30, 6.5, 'off', 29, 2.4, 'M13 b2 e10x4_base (job 21015)'),
    (1e10, 4, 118.05, 6.2, 'shift', 29, 2.4, 'M13 b1 e10x4_shift, 64 MB (T_ROUND fit)'),
    (1e10, 4, 125.37, 6.5, 'both', 29, 2.4, 'M13 b1 e10x4_both, 64 MB (T_ROUND fit)'),
    (1e10, 4, 125.55, 6.7, 'both', 29, 2.4, 'M13 b2 e10x4_both, 64 MB (T_ROUND fit)'),
    (1e10, 2, 146.4, 9.3, 'off', 30, 5.0, 'X.md job 20802 (Phase 11 code; the only default-configuration 1e10/2)'),
]
LOOPBACK_EXCLUDED = [   # measured, not modelled: why
    ('1e10 / 2, M13 b2 e10x2_base 205.76 s and _both 207.98 s', 'MN_TREE_LOGN_TEST=26 forces 2^26-point tree pieces (a test setting); the model forms the default grid'),
    ('1e9 / 2, M13 b2 e9x2 37.55 / 36.67 s', 'MN_TREE_LOGN_TEST=23 DIST_LOGN_TEST=24 (forced grids, a test setting)'),
]
EDGE_CEIL = [   # (cap, the largest D that ran, the smallest that failed) -- agent P, results/P13b.md (three primes, one node)
    (1 << 30, 1.44e11, 1.46e11), (3 << 29, 1.40e11, 1.42e11), (1 << 31, 1.30e11, 1.34e11), (3 << 30, 1.14e11, 1.16e11)]
STRATEGY_RUNS = [   # (RNS_STRATEGY, phases s, device peak GB, the same node's C phases, source) -- results/B13b.md, 4e10 size 1
    ('auto', 43.6, 287.5, 48.1, 'job 21062, s24-30, 744df439 (auto grid on): 89 B products'),
    ('B4', 46.7, 377.7, 48.4, 'job 21054, s24-30, 05a7730f: 82 B4 products, 24 GiB/APU extra'),
    ('B', 51.9, 442.1, 48.4, 'job 21054, s24-30, 05a7730f: 82 B products, 48 GiB/APU extra'),
]
LOOPBACK_DEPTH = [   # X13b: 10^9, phases at depth 1 and 2 (two runs each), the general map; (g, force_gen, depth-1 phases, depth-2 phases, source)
    (3, False, 15.6, 14.45, 'X13b size 3 (-7.4 %)'),
    (4, True, 12.65, 11.4, 'X13b size 4, DIST_GEN=1 (-9.9 %)'),
]
LOOPBACK_MEM = [  # (D total, g, measured device per process, host HWM per process, source)
    (1e10, 4, 56.6, 24.8, 'M13 b1 (job 21008), POOL_LOG 29'),
]

def calibrate(args):
    gate_w, gate_p = 0.03, 0.01
    flags = []
    print('== calibration: the model against every measured run it covers (Phase 13b D; gate: wall within %.0f %%, peak within %.0f %%)' % (100 * gate_w, 100 * gate_p))
    print('   size 1: the model = mn_model.node_phases (the phase table of the current code + S13\'s per-product law + the prime and modmul factors)')
    print('   + mem_model (device = the code\'s sizing formulas; node peak = device + the fitted host HWM).  "use": table = an input of the phase')
    print('   table (its error is the run\'s distance from its series), fit = an input of the three-prime factor, check = not an input.\n')
    hdr = '%-6s %-2s %-7s %-5s | %7s %7s %6s | %7s %7s %6s | %7s %7s %6s | %s'
    print(hdr % ('D', 'np', 'cap', 'use', 'wall', 'model', 'err', 'device', 'model', 'err', 'node', 'model', 'err', 'source'))
    fails = []; series = {}
    for r in M.RUNS:
        d = M.Design(np=r['np'], strategy='C', cap=r['cap'], modmul=r.get('mm', 0))
        p = M.node_phases(r['D'], d); mw = sum(v for k, v in p.items() if k != 'label')
        m = MM.mem_per_node(int(r['D']), 1, dict(np=r['np'], cap=r['cap']))
        mdev = m['dev_init'] / GB; mnode = m['node_peak'] / GB
        ew = mw / r['total'] - 1; ed = mdev / r['dev'] - 1
        node = r['dev'] + r['hwm']; en = mnode / node - 1
        key = (r['D'], r['np'], r['cap'], 'P11/12' if ('i12' in r['src'] or 'M11' in r['src']) else '13')
        series.setdefault(key, []).append((r, mw, r.get('n', 1)))
        print(hdr % ('%.3g' % r['D'], r['np'], MM.cap_name(r['cap']), r['use'], '%.2f' % r['total'], '%.2f' % mw, '%+.1f%%' % (100 * ew),
                     '%.1f' % r['dev'], '%.1f' % mdev, '%+.2f%%' % (100 * ed), '%.1f' % node, '%.1f' % mnode, '%+.2f%%' % (100 * en), r['src']))
        if r['D'] < 1e10: continue                                    # below the model's range for the wall (see the note at the end)
        if abs(ed) > gate_p: fails.append('device %s: %+.2f %%' % (r['src'], 100 * ed))
        if abs(en) > gate_p and r['D'] >= 2e10: fails.append('node peak %s: %+.2f %%' % (r['src'], 100 * en))
    print('\n== the series (runs of one configuration, weighted by their count): the gate is on the series mean; single runs scatter by their init (+-1.5 s)')
    print('%-6s %-2s %-7s %-6s | %3s %7s %7s %6s | %s' % ('D', 'np', 'cap', 'code', 'n', 'mean', 'model', 'err', 'runs outside the gate (reason)'))
    for key, lst in sorted(series.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2])):
        n = sum(k for _, _, k in lst); mean = sum(r['total'] * k for r, _, k in lst) / n; mw = lst[0][1]; e = mw / mean - 1
        outs = []
        for r, _, k in lst:
            er = mw / r['total'] - 1
            if abs(er) > gate_w:
                why = ('its init %.1f s against the series\' %.1f' % (r['init'], sum((x['init'] or 0) * kk for x, _, kk in lst) / n)) if (r['init'] and n > 1) else 'a single run'
                if 'lmin8' in r['src']: why = 'RNS_BATCH_LOCAL_MIN=8 run (another switch), init %.1f s' % r['init']
                outs.append('%s %+.1f %% (%s)' % (r['src'].split(' (')[0], 100 * er, why))
        exc = [x for x in lst if 'lmin8' in x[0]['src']]
        if exc and len(lst) > len(exc):                          # the series without the other switch's run
            rest = [x for x in lst if x not in exc]; n2 = sum(k for _, _, k in rest); mean2 = sum(r['total'] * k for r, _, k in rest) / n2
            e = mw / mean2 - 1
        ok = abs(e) <= gate_w or key[0] < 1e10 or key[0] > 1.0001e11   # the walls beyond 1e11 are extrapolated: printed, not gated
        if not ok: fails.append('series %s: %+.1f %%' % (str(key), 100 * e))
        print('%-6s %-2s %-7s %-6s | %3d %7.2f %7.2f %+5.1f%% | %s' % ('%.3g' % key[0], key[1], MM.cap_name(key[2]), key[3], n, mean, mw, 100 * e, '; '.join(outs) or '-'))
    print('\n== leave-one-out (a predictive check, not gated): each table size modelled from the others')
    for D in sorted(M.phase_table()):
        rs = [r for r in M.RUNS if r['D'] == D and r['use'] == 'table']
        mean = sum(r['total'] * r.get('n', 1) for r in rs) / sum(r.get('n', 1) for r in rs)
        try:
            M.phase_table.cache_clear(); M.np_factors.cache_clear()
            d = M.Design(np=4, cap=rs[0]['cap'], modmul=0)
            p = M.node_phases(D, d, exclude=D); mw = sum(v for k, v in p.items() if k != 'label')
            print('  %.0e: measured %.2f, modelled without it %.2f (%+.1f %%)' % (D, mean, mw, 100 * (mw / mean - 1)))
        except Exception as ex:
            print('  %.0e: %s' % (D, ex))
    M.phase_table.cache_clear(); M.np_factors.cache_clear()
    print('\n== the multi-process runs (g node-processes on ONE node over loopback TCP: mn_model\'s aac6 fabric, fitted per g; the leaves share the')
    print('   node).  NOT covered by the 3 % gate: the same configuration spreads +-10 % on this node (M13: 101-125 s at 1e10/4); gate +-10 %')
    for D, g, wall, init, ch, pl, dcx, src in LOOPBACK:
        fab = M.aac6_fabric('tcp', g)
        leaf = 0.5 * (M.phase('batch', D) + M.phase('top', D))
        rr = M.run(fab, D / g, g, verbose=False, leaf_scale=0.0, init_override=init, dc_exposed=dcx, form='flat', transport='tcp', pool_log=pl,
                   design=None if ch == 'off' else M.Design(np=4, legacy=True, chunk=ch, chunk_mb=64))
        mw = rr['wall'] + leaf; e = mw / wall - 1
        if abs(e) > 0.10: fails.append('loopback %s: %+.1f %%' % (src, 100 * e))
        print('  %.0e / %d %-5s: measured %.2f, model %.2f (%+.1f %%)  %s' % (D, g, ch, wall, mw, 100 * e, src))
    for what, why in LOOPBACK_EXCLUDED: print('  not modelled: %s -- %s' % (what, why))
    for g, fg, p1, p2, src in LOOPBACK_DEPTH:                 # the depth's effect on the loopback model: hidden 0.011 -> 0.74 of the xGMI time
        fab = M.aac6_fabric('tcp', g); w = []
        for gh in (M.GEN_HIDE_DEPTH[1], M.GEN_HIDE_DEPTH[2]):
            dz = M.Design(np=3, legacy=True); dz.gen_hide = gh; dz.force_gen = fg
            w.append(M.run(fab, 1e9 / g, g, verbose=False, leaf_scale=0.0, init_override=5.0, dc_exposed=0.2, form='grid', transport='tcp', pool_log=29, design=dz)['wall'])
        print('  1e9 / %d depth 2 against 1: phases measured %.2f -> %.2f (%+.1f %%), model %+.2f s (%+.1f %% of the measured)  %s' % (
              g, p1, p2, 100 * (p2 / p1 - 1), w[1] - w[0], 100 * (w[1] - w[0]) / p1, src))
    for D, g, dev, host, src in LOOPBACK_MEM:
        m = MM.mem_per_node(int(D / g), g, dict(np=4, pool_log=29, planes_3q30=False, host_fit=False))
        print('  %.0e / %d memory per process: device measured %.1f, model %.1f (%+.1f %%: the exchange scratch counted on top of the dm need, which at this size'
              ' it does not stack on -- conservative, M13); host HWM %.1f vs %.1f (the TCP transport\'s copies; the target\'s SHMEM pool is counted separately)' % (
              D, g, dev, m['dev_dm'] / GB, 100 * (m['dev_dm'] / GB / dev - 1), host, m['host_hwm'] / GB))
    print('\n== the one-node ceiling at three primes against agent P\'s edge runs (node budget %.0f GB = device + host HWM, measured on aac6)' % EDGE_GB)
    for cap, ran, failed in EDGE_CEIL:
        Dm = MM.max_digits_per_node(EDGE_GB * GB, 1, dict(np=3, cap=cap)); D5 = MM.max_digits_per_node(502 * GB, 1, dict(np=3, cap=cap))
        ok = ran - 0.021e11 <= Dm <= failed
        if not ok: fails.append('ceiling %s: model %.2e outside [%.2e runs, %.2e fails]' % (MM.cap_name(cap), Dm, ran, failed))
        print('  %-7s model at %.0f GB %.2e (at 502 GB %.2e); measured: %.2e ran, %.2e failed -- %s' % (MM.cap_name(cap), EDGE_GB, Dm, D5, ran, failed, 'inside' if ok else 'OUTSIDE'))
    print('\n== the strategy rows (agent B, results/B13b.md: 4e10 at size 1, three primes, NTT_MODMUL=1, the cap rule = 3 2^30; one run each).')
    print('   The node spreads the C phases by +-3 % between sessions (48.1-48.4 s here against 47.0 in the three-prime series on s24-26), so the gate')
    print('   is on each form\'s phases relative to the same node\'s C run (within 3 % of C\'s phases); the absolute error and the device are printed too.')
    base = {}
    for st, ph, dev, cref, src in STRATEGY_RUNS:
        d = M.Design(strategy=st); p = M.node_phases(4e10, d); mp = sum(v for k, v in p.items() if k not in ('label', 'init'))
        base.setdefault('C', sum(v for k, v in M.node_phases(4e10, M.Design()).items() if k not in ('label', 'init')))
        md = MM.mem_per_node(int(4e10), 1, d.mem_opts(4e10))['dev_init'] / GB
        dm_, dmod = ph - cref, mp - base['C']; e = (dmod - dm_) / cref
        ok = abs(e) <= gate_w
        if not ok: flags.append('strategy %s (one run): its phases against the same node\'s C %+.1f s measured, %+.1f s modelled (%.1f %% of C); '
                                'absolute %.1f measured against %.1f modelled (%+.1f %%). One run each on s24-30, whose C phases run 3-5 %% above s24-26\'s '
                                '(48.1-48.4 against 47.0; M13\'s four-prime 4e10 61.7 against 58.5), and the M-run repeats them' % (st, dm_, dmod, 100 * e, ph, mp, 100 * (mp / ph - 1)))
        print('  %-5s phases %.1f, model %.1f (%+.1f %%); against C: measured %+.1f s, model %+.1f s (%+.1f %% of C); device %.1f, model %.1f (%+.2f %%)  %s' % (
              st, ph, mp, 100 * (mp / ph - 1), dm_, dmod, 100 * e, dev, md, 100 * (md / dev - 1), src))
        if abs(md / dev - 1) > gate_p: fails.append('strategy %s: device %+.2f %%' % (st, 100 * (md / dev - 1)))
    if args.mrun:
        print('\n== the M-run (%s): single runs, then the gate on each configuration\'s mean' % args.mrun)
        grp = {}; mr = parse_mrun(args.mrun); fk = mrun_inputs(mr, args.mrun)[0].get('_fk', 1.0)
        kon = any(r['k'] for r in mr if r['g'] == 1)
        print('   agent K\'s kernels: %s' % ('the rows run them; the model is scaled by the measured on/off factor x %.3f' % fk if fk != 1.0 else
              ('the rows run them and no K-off rows are in the log yet: the K-on rows are printed, not gated' if kon else 'off')))
        for r in mr:
            d = r['design']; tag = '' if r['ok'] else '  [DIGITS DIFFER / FAILED: not used]'
            if r['g'] == 1:
                p = M.node_phases(r['D'], d); mw = sum(v for k, v in p.items() if k != 'label')
                if r['k']: mw *= fk
                m = MM.mem_per_node(int(r['D']), 1, d.mem_opts(r['D']))
                pk = run_peak(r)
                ep = (m['node_peak'] / GB / pk - 1) if pk else None
                print('  line %3d %-22s np %d mm %d %.0e: wall %.2f model %.2f (%+.1f %%); peak %s model %.1f %s%s' % (r['line'], d.name(), d.np, d.modmul, r['D'], r['total'], mw,
                      100 * (mw / r['total'] - 1), ('%.1f' % pk) if pk else '-', m['node_peak'] / GB, ('(%+.2f %%)' % (100 * ep)) if ep is not None else '', tag))
                if r['ok']:
                    e = grp.setdefault((d.key(), r['D'], r['k']), dict(name=d.name() + (' K' if r['k'] else ''), np=d.np, mm=d.modmul, D=r['D'], mw=mw, walls=[], peaks=[], mp=m['node_peak'] / GB,
                                                                       gated=not (r['k'] and fk == 1.0)))
                    e['walls'].append(r['total'])
                    if pk: e['peaks'].append(pk)
            else:
                print('  line %3d %-22s %.0e / %d: wall %.2f (loopback; enters the chunk fit)%s' % (r['line'], d.name(), r['D'], r['g'], r['total'], tag))
        for e in grp.values():
            mean = sum(e['walls']) / len(e['walls']); ew = e['mw'] / mean - 1
            pk = max(e['peaks']) if e['peaks'] else None; ep = (e['mp'] / pk - 1) if pk else None
            fitted = any(e['name'].startswith('%s %s ' % (st, MM.cap_name(c))) for st, c in M.STRAT_FIT)
            print('  config %-22s np %d mm %d %.0e: n %d, mean %.2f, model %.2f (%+.1f %%)%s; peak %s (%s)' % (e['name'], e['np'], e['mm'], e['D'], len(e['walls']), mean, e['mw'], 100 * ew,
                  ' [fitted: STRAT_FIT]' if fitted else '', ('%.1f' % pk) if pk else '-', ('%+.2f %%' % (100 * ep)) if ep is not None else '-'))
            if abs(ew) > gate_w and e['gated']: fails.append('M-run %s at %.0e: wall %+.1f %%' % (e['name'], e['D'], 100 * ew))
            elif abs(ew) > gate_w: flags.append('M-run %s at %.0e: wall %+.1f %% (K on, no K-off rows yet: not gated)' % (e['name'], e['D'], 100 * ew))
            if ep is not None and abs(ep) > gate_p: fails.append('M-run %s at %.0e: peak %+.2f %%' % (e['name'], e['D'], 100 * ep))
    print('\n== gate: %s' % ('PASS' if not fails else 'FAIL on %d item(s):' % len(fails)))
    for f in fails: print('   ' + f)
    if flags: print('   single-run checks outside 3 %, listed as exceptions:')
    for f in flags: print('   - ' + f)
    print('   the depth rows on aac6 loopback are not reproduced (printed above, not gated): the model hides the xGMI LINK time, a few ms per')
    print('   exchange there, while depth 2 on loopback also hides the host-side stage (pack, barrier, sync: 40-100 x the link, X13 3.1); on the')
    print('   target the modelled depth-2 gain (0.74 of the link time, measured on two real nodes) is therefore a lower bound.')
    print('   the model covers size-1 walls from 1e10 to 1e11 digits (the 576 leaf is 5-10 x 10^10); at 1e9 the wall is 75 % init (+-1.5 s per run) and')
    print('   the batch tier is latency-bound (it does not follow the prime factor): printed, not gated.')
    print('   exceptions by design (not gated): the node peak below 2e10 digits (the dm phase\'s host flows, 26-40 GB at 1e10, are not modelled; the device is),')
    print('   single runs whose init deviates from their series (the gate is on the series), the loopback walls (+-10 %: their own spread).')
    return not fails

VERDICTS = ('identical', 'VERIFY', 'DIFFERS')

def line_verdict(s):
    """the run's verdict from an M-run line: its LAST word when that is identical / VERIFY / DIFFERS / *VERIFY-FAILED* (the
    integrator appends its own after make-line's); ok = identical or VERIFY (the run's VERIFY OK, digits not compared)"""
    w = s.split()[-1] if s.split() else ''
    if 'FAILED' in w: return w, False
    if w in VERDICTS: return w, w != 'DIFFERS'
    return '', not re.search(r'\bDIFFERS\b|VERIFY FAILED|VERIFY-FAILED', s)

def log_mem(s):
    """device at init and the maxima of a log: size 1 -> the `mem summary` rows (`mem init  287.5 ...`); several processes ->
    the per-rank lines `mem[r] [phase] device X GB in use ... peak live Y` (per process: the processes share one node).
    Returns dict(dev_init, dev_max, peak_live, nproc) in GB (None where absent)."""
    out = dict(dev_init=None, dev_max=None, peak_live=None, nproc=1)
    m = re.search(r'^mem init\s+([\d.]+)', s, re.M)
    if m:
        out['dev_init'] = float(m.group(1))
        rows = re.findall(r'^mem (\w+)\s+([\d.]+)\s+[\d.]+ \|', s, re.M)
        if rows: out['dev_max'] = max(float(x) for _, x in rows)
        return out
    per = re.findall(r'^mem\[(\d+)\] \[(\w+)\] device ([\d.]+) GB in use.*?peak live ([\d.]+)', s, re.M)
    if per:
        out['nproc'] = len(set(r for r, _, _, _ in per))
        ini = [float(x) for r, ph, x, _ in per if ph == 'init']
        out['dev_init'] = max(ini) if ini else None
        out['dev_max'] = max(float(x) for _, _, x, _ in per)
        out['peak_live'] = max(float(p) for _, _, _, p in per)
    return out

def make_line(log, kv):
    """one M-run line from an ecalc log: the given KEY=VALUEs, the `total` line (node 0's at several processes), the device at init
    (size 1: the mem summary; several processes: per process, the max over the ranks) with the maxima over the phases, and the
    verdict: an explicit cmp=<verdict> first, else DIFFERS / VERIFY FAILED if the log says so exactly, else identical / VERIFY"""
    s = open(log, errors='replace').read()
    tot = re.search(r'^total .*$', s, re.M)
    if not tot: raise SystemExit('%s: no total line' % log)
    env = dict(x.split('=', 1) for x in kv if '=' in x)
    if 'digits' not in env:
        m = re.search(r'== ecalc: e to (\d+) digits', s)
        if m: env['digits'] = m.group(1)
    if 'ECALC_NP' not in env: env['ECALC_NP'] = '3' if 'three primes' in s else '4'   # rns_init prints it at three primes (give ECALC_NP= for an older log)
    if 'NTT_MODMUL' not in env: env['NTT_MODMUL'] = '1'                                           # the default since step 0 (not in the log)
    verdict = env.pop('cmp', None)
    if not verdict:
        if re.search(r'\bDIFFERS\b|VERIFY FAILED', s): verdict = 'DIFFERS'
        elif re.search(r'^identical\s*$', s, re.M): verdict = 'identical'
        elif 'VERIFY OK' in s: verdict = 'VERIFY'
        else: verdict = ''
    mm = log_mem(s)
    f = lambda x: '%.1f' % x if x is not None else '?'
    mem = 'device %s GB' % f(mm['dev_init'])
    if mm['nproc'] > 1:
        mem += ' (per process, %d processes on one node; max in use %s, peak live %s)' % (mm['nproc'], f(mm['dev_max']), f(mm['peak_live']))
    elif mm['dev_max'] is not None:
        mem += ' (max in use %s)' % f(mm['dev_max'])            # the node peak's device (B / B4: the extra planes come after init)
    return '%s | %s | %s %s' % (' '.join('%s=%s' % kv for kv in env.items()), tot.group(0), mem, verdict)

KON = ['NTT_B1R=3', 'NTT_PLAN=1']
def regen(logdir, progress=None):
    """rebuild the M-run log from the per-run logs of the campaign (~/mrun/logs) by their tags:
         s1_<strategy>_<cap>_r<n>       size 1, 4e10, RNS_STRATEGY, ECALC_PLANE_CAP (2_30, 3_2_29, 2_31, 3_2_30), K on
         koff_auto_2_31_r<n>            the same without K (NTT_B1R, NTT_PLAN unset)
         series_auto_2_31_r<n>          the five-run series (K on)
         p4_d<depth>_<off|shift|both>_r<n>   10^10 over 4 processes, DIST_GEN=1, POOL_LOG=29, COMM_ALLTOALLV_DEPTH, chunks at 1024 MB, K on
         p3_d<depth>_r<n>               10^10 over 3 processes, POOL_LOG=29, K on
         p4_d<depth>_both<MB>_r<n>      the chunk sweep: both switches at <MB>
         p3e9_d<depth>_r<n>             10^9 over 3 processes, POOL_LOG=27, K on
       the verdicts from progress.txt (`HH:MM <tag> <verdict> total ...`), passed as cmp=<verdict>"""
    import glob
    ver = {}
    pf = progress or os.path.join(os.path.dirname(os.path.abspath(logdir.rstrip('/'))), 'progress.txt')
    if os.path.exists(pf):
        for ln in open(pf, errors='replace'):
            w = ln.split()
            if len(w) >= 3 and re.match(r'\d\d:\d\d$', w[0]): ver[w[1]] = w[2]
    capname = {'2_30': '2^30', '3_2_29': '3*2^29', '2_31': '2^31', '3_2_30': '3*2^30'}
    out = []
    for fn in sorted(glob.glob(os.path.join(logdir, '*.log'))):
        tag = os.path.basename(fn)[:-4]; kv = None
        m = re.match(r's1_(\w+?)_(2_30|3_2_29|2_31|3_2_30)_r\d+$', tag) or re.match(r'(?:series)_(auto)_(2_31)_r\d+$', tag)
        if m: kv = ['digits=40000000000', 'size=1', 'RNS_STRATEGY=' + m.group(1), 'ECALC_PLANE_CAP=' + capname[m.group(2)]] + KON
        m2 = re.match(r'koff_(\w+?)_(2_31)_r\d+$', tag)
        if m2: kv = ['digits=40000000000', 'size=1', 'RNS_STRATEGY=' + m2.group(1), 'ECALC_PLANE_CAP=' + capname[m2.group(2)]]
        m3 = re.match(r'p4_d(\d)_(off|shift|both)(\d*)_r\d+$', tag)
        if m3:
            mb = m3.group(3) or '1024'
            ch = {'off': [], 'shift': ['MDB_SHIFT_CHUNK_MB=' + mb], 'both': ['MDB_SHIFT_CHUNK_MB=' + mb, 'MN_T_CHUNK_MB=' + mb]}[m3.group(2)]
            kv = ['digits=10000000000', 'size=4', 'DIST_GEN=1', 'POOL_LOG=29', 'COMM_ALLTOALLV_DEPTH=' + m3.group(1)] + ch + KON
        m4 = re.match(r'p3_d(\d)_r\d+$', tag)
        if m4: kv = ['digits=10000000000', 'size=3', 'POOL_LOG=29', 'COMM_ALLTOALLV_DEPTH=' + m4.group(1)] + KON
        m5 = re.match(r'p3e9_d(\d)_r\d+$', tag)
        if m5: kv = ['digits=1000000000', 'size=3', 'POOL_LOG=27', 'COMM_ALLTOALLV_DEPTH=' + m5.group(1)] + KON
        if kv is None: print('# %s: tag not recognised, skipped' % tag, file=sys.stderr); continue
        if tag in ver: kv.append('cmp=' + ver[tag])
        try: out.append('%s tag=%s' % (make_line(fn, kv), tag))
        except SystemExit as e: print('# %s' % e, file=sys.stderr)
    return out

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mrun', help='the M-run log (see the format above)')
    ap.add_argument('--calibrate', action='store_true', help='model against every measured run, with the error; exit 1 if the gate fails')
    ap.add_argument('--out', default=OUT, help='the markdown file (default results/DESIGN_TABLE.md)')
    ap.add_argument('--quick', action='store_true', help='12 rows only')
    ap.add_argument('--bws', default='50,100,200', help='GB/s per APU: (f) low, (e), (f) high (assumed 100 = PLAN 25; on the target the measured value in the middle)')
    ap.add_argument('--lat', type=float, default=2e-6, help='seconds per message (assumed)')
    ap.add_argument('--write-bw', type=float, default=2.0, help='GB/s per node for the part file (assumed)')
    ap.add_argument('--e0', help='an extra t_strategy log (e.g. with B4 lines) for the per-product law')
    ap.add_argument('--hide-pow2', type=float, default=M.HIDE_POW2, help='the equal-slab path\'s hidden fraction of its xGMI time (X13: 0.75)')
    ap.add_argument('--gen-hide2', type=float, default=M.GEN_HIDE_DEPTH[2], help='the general map\'s hidden fraction at depth 2 (modelled 0.75; agent X\'s both-busy measurement replaces it)')
    ap.add_argument('--make-line', nargs='+', metavar=('LOG', 'KEY=VALUE'), help='print the M-run line of one ecalc log (node 0\'s): LOG [digits=.. size=.. ENV=..]; '
                    'the env keys the log does not show must be given; "identical" / "DIFFERS" is taken from the log or from a key cmp=identical')
    ap.add_argument('--regen', nargs='+', metavar=('LOGDIR', 'PROGRESS'), help='rebuild the M-run log from the campaign\'s per-run logs by their tags '
                    '(s1_*, koff_*, series_*, p4_*, p3_*) with the verdicts of progress.txt (default: LOGDIR/../progress.txt); prints it')
    ap.add_argument('--fit-tround', action='store_true', help='refit T_ROUND from the M-run\'s chunked rows (default: assumed, see the notes)')
    ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args()
    BWS[:] = [float(x) for x in a.bws.split(',')]
    M.TARGET = M.Fabric(M.TARGET.name, BE(), a.lat, group=M.TARGET.group, layers=M.TARGET.layers, taper=M.TARGET.taper, write_bw=a.write_bw)
    if a.e0: M._E0 = None; M.e0_table(a.e0)
    M.HIDE_POW2 = a.hide_pow2; M.GEN_HIDE_DEPTH[2] = a.gen_hide2
    global FIT_TROUND; FIT_TROUND = a.fit_tround
    if a.make_line:
        print(make_line(a.make_line[0], a.make_line[1:])); return
    if a.regen:
        print('\n'.join(regen(a.regen[0], a.regen[1] if len(a.regen) > 1 else None))); return
    if a.calibrate:
        sys.exit(0 if calibrate(a) else 1)
    res = build(a)
    txt = write_md(res, a)
    print(txt)
    print('\nwrote %s (%d rows, %.0f s)' % (a.out, len(res['rows']), res['secs']), file=sys.stderr)

if __name__ == '__main__':
    main()

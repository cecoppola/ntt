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
    ./design_table.py --make-line LOG KEY=V..  the M-run line of one ecalc log (the integrator's helper)
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
NODES = 576
COMMON = 4e13                                   # the common size of column (e)
BWS = [50.0, 100.0, 200.0]                      # GB/s per APU: (f) low, (e), (f) high (--bws)
def BE(): return BWS[1]
def BL(): return BWS[0]
def BH(): return BWS[2]
CAPS = [1 << 30, 3 << 29, 1 << 31, 3 << 30]
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'results', 'DESIGN_TABLE.md')
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
        r['ok'] = not re.search(r'DIFFERS|FAILED|differs', s)
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

# ------------------------------------------------------------------------------------------------------------ one row
class Row:
    pass

MEMO = {}
def max_d(design, budget):
    """the largest D per node at 576 that fits `budget` GB (memoised on what the memory depends on)"""
    k = ('B' if design.strategy == 'B' else 'C', design.cap, design.chunk, design.depth, design.np, budget)
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
    if meas:
        if meas.get('wall') is not None: r.a = meas['wall']; r.a_lab = 'measured (n=%d)' % meas['n']
        if meas.get('peak') is not None: r.b = meas['peak']; r.b_lab = 'measured'
    r.maxd = {}
    for budget in (502.0, 480.0):
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
    by_sk = {}; notes = []
    for r in runs:
        if not r['ok']: notes.append('M-run line %d: digits differ or failed -- not used' % r['line']); continue
        if r['g'] == 1 and abs(r['D'] - 4e10) < 1e6 and r['total'] is not None and r['design'].np == 3 and r['design'].modmul == 1:
            k = (r['design'].strategy, r['design'].cap_at(4e10))
            e = by_sk.setdefault(k, dict(walls=[], peaks=[], devs=[]))
            e['walls'].append(r['total'])
            if r['dev'] is not None and r['hwm'] is not None: e['peaks'].append(r['dev'] + r['hwm']); e['devs'].append(r['dev'])
            elif r['peak'] is not None: e['peaks'].append(r['peak'])
    for k, e in by_sk.items():
        e['n'] = len(e['walls']); e['wall'] = sum(e['walls']) / e['n']
        e['sd'] = (sum((x - e['wall']) ** 2 for x in e['walls']) / max(1, e['n'] - 1)) ** 0.5
        e['peak'] = max(e['peaks']) if e['peaks'] else None
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
                notes.append('M-run chunking %s at %.0e / %d: %+.1f s against off (%d extra rounds in the model)' % (x['axes'][2], D, g, x['total'] - b, rounds))
        if not (g & (g - 1)) and any(x['axes'][3] == 2 for x in rs):
            notes.append('M-run depth 2 at size %d: a power of two -- the equal-slab path, the depth switch is not exercised (use 3 node-processes or DIST_GEN=1)' % g)
    if den > 0:
        M.T_ROUND = max(0.0, num / den)
        notes.append('T_ROUND refitted on the M-run: %.3f s per extra round (was 0.030, M13)' % M.T_ROUND)
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
        leaf_scale = 1.0; peak_delta = 0.0
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
              'and the chunk rounds\' fixed cost on the target (T_ROUND %.3f s, fitted on aac6 loopback). Column (f) varies the fabric bandwidth.\n' % M.T_ROUND)
    L_.append('Columns: S = `RNS_STRATEGY`; K = plane cap; chunk = `MDB_SHIFT_CHUNK_MB` (shift) / + `MN_T_CHUNK_MB` (both), at %d MB; depth = the uneven exchange two deep; '
              '(a) 4 × 10¹⁰ wall at size 1 [s]; (b) node peak at 4 × 10¹⁰ [GB]; (c) the 576-node maximum digits [×10¹³] at 502 / 480 GB per node; '
              '(d) the 576-node wall at the 502-GB maximum [min]; (e) the 576-node wall at 4 × 10¹³ digits [min] at %g GB/s per APU; (f) the same at %g / %g GB/s; '
              'rank = (e)\'s rank at %g / %g / %g GB/s among the rows that hold 4 × 10¹³. Marks: **P** Pareto front on ((e), (c) at 502), **F** fastest, **L** largest, **R** recommended. '
              '(e) in parentheses: the row cannot hold 4 × 10¹³ at 502 GB. Labels column: mod = modelled, meas = measured, (meas. node) = the 576 model scaled to the measured one-node run.\n' % (M.CHUNK_MB, BE(), BL(), BH(), BL(), BE(), BH()))
    L_.append('| # | S | K | chunk | depth | (a) s | (b) GB | (c) 502 | (c) 480 | (d) min | (e) min | (f) %g | (f) %g | rank %g/%g/%g | mark | labels (a) / (b) / 576 |' % (BL(), BH(), BL(), BE(), BH()))
    L_.append('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    order = sorted(rows, key=lambda r: (not r.fits, r.walls[BE()]))
    for i, r in enumerate(order, 1):
        d = r.d; mk = ''.join(c for c, cond in (('P', id(r) in fset), ('F', r is F), ('L', r is L), ('R', r is R)) if cond)
        e = lambda x: ('%.2f' % (x / 60)) if r.fits else '(%.2f)' % (x / 60)
        rk = '/'.join(str(ranks[bw].get(id(r), '-')) for bw in BWS)
        labs = '%s / %s / %s' % (r.a_lab.replace('modelled', 'mod').replace('measured', 'meas'), r.b_lab.replace('modelled', 'mod').replace('measured', 'meas'), r.lab576.replace('modelled', 'mod').replace(' on measured per-node input', ' (meas. node)'))
        L_.append('| %d | %s | %s | %s | %d | %.1f | %.1f | %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
            i, d.strategy, MM.cap_name(d.cap), d.chunk, d.depth, r.a, r.b, fmt_d(r.maxd[502.0] * NODES), fmt_d(r.maxd[480.0] * NODES),
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
              '`auto` equals `B4` in every column at three primes: rns_dist.c\'s auto takes the B form (`RNS_STRATEGY_FORM`, default B4 at P = 3) wherever its planes fit '
              'the pools as sized at init, and B4\'s 12 n bytes per APU are exactly the C pools; the two differ only where a product is not an owning dbig or uses the transform cache (then C).\n')
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
        for (st, cap), e in sorted(res['by_sk'].items(), key=lambda kv: (kv[0][0], kv[0][1])):
            d = M.Design(strategy=st, cap=cap); wm, pm, _, _ = size1_4e10(d)
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
LOOPBACK = [   # the multi-process runs of the current code on one node (TCP loopback): (D total, g, wall, init, switches, source)
    (1e10, 4, 113.85, 6.7, 'off', 'M13 b1 e10x4_base (job 21008)'),
    (1e10, 4, 111.30, 6.5, 'off', 'M13 b2 e10x4_base (job 21015)'),
    (1e10, 4, 118.05, 6.2, 'shift', 'M13 b1 e10x4_shift, 64 MB (T_ROUND fit)'),
    (1e10, 4, 125.37, 6.5, 'both', 'M13 b1 e10x4_both, 64 MB (T_ROUND fit)'),
    (1e10, 4, 125.55, 6.7, 'both', 'M13 b2 e10x4_both, 64 MB (T_ROUND fit)'),
]
LOOPBACK_MEM = [  # (D total, g, measured device per process, host HWM per process, source)
    (1e10, 4, 56.6, 24.8, 'M13 b1 (job 21008), POOL_LOG 29'),
]

def calibrate(args):
    gate_w, gate_p = 0.03, 0.01
    print('== calibration: the model against every measured run it covers (Phase 13b D; gate: wall within %.0f %%, peak within %.0f %%)' % (100 * gate_w, 100 * gate_p))
    print('   size 1: the model = mn_model.node_phases (the phase table of the current code + S13\'s per-product law + the prime and modmul factors)')
    print('   + mem_model (device = the code\'s sizing formulas; node peak = device + the fitted host HWM).  "use": table = an input of the phase')
    print('   table (its error is the run\'s distance from its series), fit = an input of the three-prime factor, check = not an input.\n')
    hdr = '%-6s %-2s %-7s %-5s | %7s %7s %6s | %7s %7s %6s | %7s %7s %6s | %s'
    print(hdr % ('D', 'np', 'cap', 'use', 'wall', 'model', 'err', 'device', 'model', 'err', 'node', 'model', 'err', 'source'))
    fails = []; series = {}
    for r in M.RUNS:
        d = M.Design(np=r['np'], strategy='C', cap=r['cap'], modmul=0)
        p = M.node_phases(r['D'], d); mw = sum(v for k, v in p.items() if k != 'label')
        m = MM.mem_per_node(int(r['D']), 1, dict(np=r['np'], cap=r['cap']))
        mdev = m['dev_init'] / GB; mnode = m['node_peak'] / GB
        ew = mw / r['total'] - 1; ed = mdev / r['dev'] - 1
        node = r['dev'] + r['hwm']; en = mnode / node - 1
        key = (r['D'], r['np'], r['cap'], 'P11/12' if ('i12' in r['src'] or 'M11' in r['src']) else '13')
        series.setdefault(key, []).append((r, mw, r.get('n', 1)))
        print(hdr % ('%.0e' % r['D'], r['np'], MM.cap_name(r['cap']), r['use'], '%.2f' % r['total'], '%.2f' % mw, '%+.1f%%' % (100 * ew),
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
        ok = abs(e) <= gate_w or key[0] < 1e10
        if not ok: fails.append('series %s: %+.1f %%' % (str(key), 100 * e))
        print('%-6s %-2s %-7s %-6s | %3d %7.2f %7.2f %+5.1f%% | %s' % ('%.0e' % key[0], key[1], MM.cap_name(key[2]), key[3], n, mean, mw, 100 * e, '; '.join(outs) or '-'))
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
    for D, g, wall, init, ch, src in LOOPBACK:
        fab = M.aac6_fabric('tcp', g)
        leaf = 0.5 * (M.phase('batch', D) + M.phase('top', D))
        rr = M.run(fab, D / g, g, verbose=False, leaf_scale=0.0, init_override=init, dc_exposed=2.4, form='flat', transport='tcp', pool_log=29,
                   design=None if ch == 'off' else M.Design(np=4, legacy=True, chunk=ch, chunk_mb=64))
        mw = rr['wall'] + leaf; e = mw / wall - 1
        if abs(e) > 0.10: fails.append('loopback %s: %+.1f %%' % (src, 100 * e))
        print('  %.0e / %d %-5s: measured %.2f, model %.2f (%+.1f %%)  %s' % (D, g, ch, wall, mw, 100 * e, src))
    for D, g, dev, host, src in LOOPBACK_MEM:
        m = MM.mem_per_node(int(D / g), g, dict(np=4, pool_log=29, planes_3q30=False, host_fit=False))
        print('  %.0e / %d memory per process: device measured %.1f, model %.1f (%+.1f %%: the exchange scratch counted on top of the dm need, which at this size'
              ' it does not stack on -- conservative, M13); host HWM %.1f vs %.1f (the TCP transport\'s copies; the target\'s SHMEM pool is counted separately)' % (
              D, g, dev, m['dev_dm'] / GB, 100 * (m['dev_dm'] / GB / dev - 1), host, m['host_hwm'] / GB))
    if args.mrun:
        print('\n== the M-run (%s): single runs, then the gate on each configuration\'s mean' % args.mrun)
        grp = {}
        for r in parse_mrun(args.mrun):
            d = r['design']; tag = '' if r['ok'] else '  [DIGITS DIFFER / FAILED: not used]'
            if r['g'] == 1:
                p = M.node_phases(r['D'], d); mw = sum(v for k, v in p.items() if k != 'label')
                m = MM.mem_per_node(int(r['D']), 1, d.mem_opts(r['D']))
                pk = (r['dev'] + r['hwm']) if r['dev'] is not None and r['hwm'] is not None else r['peak']
                ep = (m['node_peak'] / GB / pk - 1) if pk else None
                print('  line %3d %-22s np %d mm %d %.0e: wall %.2f model %.2f (%+.1f %%); peak %s model %.1f %s%s' % (r['line'], d.name(), d.np, d.modmul, r['D'], r['total'], mw,
                      100 * (mw / r['total'] - 1), ('%.1f' % pk) if pk else '-', m['node_peak'] / GB, ('(%+.2f %%)' % (100 * ep)) if ep is not None else '', tag))
                if r['ok']:
                    e = grp.setdefault((d.key(), r['D']), dict(name=d.name(), np=d.np, mm=d.modmul, D=r['D'], mw=mw, walls=[], peaks=[], mp=m['node_peak'] / GB))
                    e['walls'].append(r['total'])
                    if pk: e['peaks'].append(pk)
            else:
                print('  line %3d %-22s %.0e / %d: wall %.2f (loopback; enters the chunk fit)%s' % (r['line'], d.name(), r['D'], r['g'], r['total'], tag))
        for e in grp.values():
            mean = sum(e['walls']) / len(e['walls']); ew = e['mw'] / mean - 1
            pk = max(e['peaks']) if e['peaks'] else None; ep = (e['mp'] / pk - 1) if pk else None
            print('  config %-22s np %d mm %d %.0e: n %d, mean %.2f, model %.2f (%+.1f %%); peak %s (%s)' % (e['name'], e['np'], e['mm'], e['D'], len(e['walls']), mean, e['mw'], 100 * ew,
                  ('%.1f' % pk) if pk else '-', ('%+.2f %%' % (100 * ep)) if ep is not None else '-'))
            if abs(ew) > gate_w: fails.append('M-run %s at %.0e: wall %+.1f %%' % (e['name'], e['D'], 100 * ew))
            if ep is not None and abs(ep) > gate_p: fails.append('M-run %s at %.0e: peak %+.2f %%' % (e['name'], e['D'], 100 * ep))
    print('\n== gate: %s' % ('PASS' if not fails else 'FAIL on %d item(s):' % len(fails)))
    for f in fails: print('   ' + f)
    print('   the model covers size-1 walls from 1e10 to 1e11 digits (the 576 leaf is 5-10 x 10^10); at 1e9 the wall is 75 % init (+-1.5 s per run) and')
    print('   the batch tier is latency-bound (it does not follow the prime factor): printed, not gated.')
    print('   exceptions by design (not gated): the node peak below 2e10 digits (the dm phase\'s host flows, 26-40 GB at 1e10, are not modelled; the device is),')
    print('   single runs whose init deviates from their series (the gate is on the series), the loopback walls (+-10 %: their own spread).')
    return not fails

def make_line(log, kv):
    """one M-run line from an ecalc log: the given KEY=VALUEs, the `total` line, the device total at init (mem summary) and the
    comparison's verdict"""
    s = open(log, errors='replace').read()
    tot = re.search(r'^total .*$', s, re.M); dev = re.search(r'^mem init\s+([\d.]+)', s, re.M)
    if not tot: raise SystemExit('%s: no total line' % log)
    env = dict(x.split('=', 1) for x in kv if '=' in x)
    if 'digits' not in env:
        m = re.search(r'== ecalc: e to (\d+) digits', s)
        if m: env['digits'] = m.group(1)
    if 'ECALC_NP' not in env: env['ECALC_NP'] = '3' if 'three primes' in s else '4'   # rns_init prints it at three primes (give ECALC_NP= for an older log)
    if 'NTT_MODMUL' not in env: env['NTT_MODMUL'] = '1'                                           # the default since step 0 (not in the log)
    verdict = env.pop('cmp', None) or ('DIFFERS' if re.search(r'DIFFERS|differ|VERIFY FAILED', s) else ('identical' if re.search(r'^identical', s, re.M) else ''))
    return '%s | %s | device %s GB %s' % (' '.join('%s=%s' % kv for kv in env.items()), tot.group(0), dev.group(1) if dev else '?', verdict)

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
    ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args()
    BWS[:] = [float(x) for x in a.bws.split(',')]
    M.TARGET = M.Fabric(M.TARGET.name, BE(), a.lat, group=M.TARGET.group, layers=M.TARGET.layers, taper=M.TARGET.taper, write_bw=a.write_bw)
    if a.e0: M._E0 = None; M.e0_table(a.e0)
    M.HIDE_POW2 = a.hide_pow2; M.GEN_HIDE_DEPTH[2] = a.gen_hide2
    if a.make_line:
        print(make_line(a.make_line[0], a.make_line[1:])); return
    if a.calibrate:
        sys.exit(0 if calibrate(a) else 1)
    res = build(a)
    txt = write_md(res, a)
    print(txt)
    print('\nwrote %s (%d rows, %.0f s)' % (a.out, len(res['rows']), res['secs']), file=sys.stderr)

if __name__ == '__main__':
    main()

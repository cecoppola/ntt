#!/usr/bin/env python3
"""calib_table.py - B07: ecalc run logs -> one calibration row per run, the rate regression total = a + b / r, and a rate-adjusted A/B comparison.

    ./calib_table.py LOGDIR_OR_FILES... --tsv calib.tsv            # one row per run (all *.log under the given dirs; N: label prefixes stripped)
    ./calib_table.py DIRS... --fit                                 # + per (nodes, digits/node, config) group: total = a + b/r (and bs, dm, recip), residual sd
    ./calib_table.py DIRS... --fit --oos                           # + leave-one-source-out check (fit on the other sources, test on this one)
    ./calib_table.py DIRS... --ab                                  # + rate-adjusted paired A/B (arm B - arm A) per experiment, with a rate-shift warning
    ./calib_table.py DIRS... --driver-tsv ab.tsv pairs.tsv ...     # attach driver wall / start stamp by label (ab.tsv, pairs.tsv, summary.tsv forms)
    ./calib_table.py --selftest

Row columns (TSV): src, label, exp, round, arm, config (hash of env words if the log prints them, else --config REGEX=NAME match, else '-'),
nodes, digits, dpn (digits per node), rc-like verify (OK/FAIL/none), total, init, bs, tree (max node tree levels), recip, dm, dm sub-stages
(rcp_prod, s1, s2, xlo), dc, other, chain (single-node reciprocal chain, s), r (mean over PEs of the `comm_shmem: ... all-to-all ... GB/s per
APU thread`), rmin, rmax, mode (slow < 11.95, fast > 12.3 GB/s, mid between), flags (chain spike > 2.6 s, cold start init >= 36 s, no-verify),
clean (1 = usable in a fit), memory (per phase max over ranks of device in use and peak live, host HWM; budget peak; live window peak,
peak/pool %, min MemAvailable), xstats (per phase GB per PE and rate GB / t_post), wait-stats (sum of wait / barrier s per phase), wall/start from
a driver file, and `resid` (total - fit prediction) after --fit.
Labels: every figure is MEASURED from the logs; fit results and CIs are MODELLED (statistics on measured values).
Stdlib only.
"""
import argparse, glob, hashlib, math, os, re, sys

SLOW, FAST = 11.95, 12.3          # GB/s per APU thread (T05)
CHAIN_MAX, INIT_MAX = 2.6, 36.0   # clean-run limits (T05)

LAB = re.compile(r'^\s*\d+: ')
TOT = re.compile(r'^total\s+([\d.]+) s\s+\(bs ([\d.]+) \+ 10dP ([\d.]+) \+ dm ([\d.]+) \+ T1 ([\d.]+) \+ dc ([\d.]+) \+ T2 ([\d.]+) = [\d.]+; init ([\d.]+); other ([\d.]+)\)(?:; VmHWM ([\d.]+) GB)?')
RCP = re.compile(r'^recip\s+([\d.]+) s')
DMS = re.compile(r'^divmod\(mn, DKM\) ([\d.]+) s:.*?reciprocal ([\d.]+) \(products ([\d.]+)\).*?step 1 \([^)]*\) ([\d.]+), step 2 A mu \+ assembly ([\d.]+), X_lo Q ([\d.]+)')
CHN = re.compile(r'single-node chain to \d+ limbs \(([\d.]+) s\)')
A2A = re.compile(r'^comm_shmem: pe (\d+): all-to-all \d+ exchanges.*?= ([\d.]+) GB/s per APU thread')
PLAN = re.compile(r'MN_PLAN_ONLY=(\d+):(\d+)')
TRE = re.compile(r'^mn: node (\d+): tree levels ([\d.]+) s')
MEMD = re.compile(r'^mem\[(\d+)\] \[([\w ]+)\] device ([\d.]+) GB in use .*?peak live ([\d.]+)')
MEMH = re.compile(r'^mem\[(\d+)\] \[([\w ]+)\] host ([\d.]+) GB RSS \(HWM ([\d.]+)\)')
BUD = re.compile(r'^budget: rank (\d+): peak ([\d.]+) GB \(device ([\d.]+), host ([\d.]+)\)')
LIV = re.compile(r'^live: .*?window peak ([\d. ]+)\|.*?peak/pool ([\d.]+) %; MemAvailable ([\d.]+) GB')
XPH = re.compile(r'^xstats pe (\d+) phase (\w+) kind (\w+): n (\d+) rounds (\d+) GB ([\d.]+) t_post ([\d.]+) s')
WST = re.compile(r'^wait-stats node (\d+):(.*)$')
WPH = re.compile(r'(\w+) barrier ([\d.]+) s \(\d+\) wait ([\d.]+) s')
ENVL = re.compile(r'^(?:env|config|LINE)\s*[:|]\s*(.*)$')
ENVW = re.compile(r'\b((?:COMM|MN|RNS|DM|ECALC|DBIG|BS|DIST|NEWTON)_[A-Z0-9_]+)=(\S+)')
FNAMES = [re.compile(r'^(?P<exp>[A-Za-z]+)_r(?P<round>\d+)_(?P<arm>[A-Za-z])$'),      # S47: ii_r3_A
          re.compile(r'^r(?P<round>\d+)_(?P<pos>\d+)(?P<arm>[A-Za-z])$'),             # S50/S51: r1_2B
          re.compile(r'^n(?P<nn>\d+)_r(?P<round>\d+)$'),                              # S47/S48 scaling: n4_r1
          re.compile(r'^(?P<exp>[A-Za-z0-9]+)_(?P<round>\d+)_(?P<arm>[A-Za-z]+)$')]

PH = ['init', 'tree', 'bs', 'recip', 'dm', 'end']

def num(x):
    return '' if x is None else (('%.4g' % x) if isinstance(x, float) else str(x))

def parse_log(path):
    r = dict(path=path, total=None, bs=None, dm=None, init=None, other=None, dc=None, vmhwm=None, recip=None, dmsub=None, chain=None,
             digits=None, nodes=None, verify=None, a2a={}, tree=None, mem={}, host={}, bud=None, live_peak=None, live_pct=None, live_min=None,
             xs={}, wait={}, envs={}, pes=set())
    for ln in open(path, errors='replace'):
        ln = LAB.sub('', ln.rstrip('\n'))
        if ln.startswith('total'):
            t = TOT.match(ln)
            if t and r['total'] is None:
                g = [float(v) if v else None for v in t.groups()]
                r.update(total=g[0], bs=g[1], dm=g[3], dc=g[5], init=g[7], other=g[8], vmhwm=g[9])
            continue
        if ln.startswith('recip'):
            m = CHN.search(ln)
            if m and r['chain'] is None: r['chain'] = float(m.group(1))
            m = RCP.match(ln)
            if m and r['recip'] is None: r['recip'] = float(m.group(1))
            continue
        if ln.startswith('divmod(mn'):
            m = DMS.match(ln)
            if m and r['dmsub'] is None: r['dmsub'] = tuple(float(v) for v in m.groups())
            continue
        if 'VERIFY' in ln:
            if re.search(r'VERIFY (FAIL|MISMATCH|BAD)', ln): r['verify'] = 'FAIL'
            elif 'VERIFY OK' in ln and r['verify'] != 'FAIL': r['verify'] = 'OK'
        m = CHN.search(ln)
        if m and r['chain'] is None: r['chain'] = float(m.group(1))
        if ln.startswith('comm_shmem: pe'):
            m = A2A.match(ln)
            if m: r['a2a'][int(m.group(1))] = float(m.group(2))
            continue
        if r['digits'] is None:
            m = PLAN.search(ln)
            if m: r['digits'], r['nodes'] = int(m.group(1)), int(m.group(2))
        if ln.startswith('mn: node'):
            m = TRE.match(ln)
            if m: r['tree'] = max(r['tree'] or 0.0, float(m.group(2)))
            continue
        if ln.startswith('mem['):
            m = MEMD.match(ln)
            if m:
                d = r['mem'].setdefault(m.group(2), [0.0, 0.0]); d[0] = max(d[0], float(m.group(3))); d[1] = max(d[1], float(m.group(4)))
                continue
            m = MEMH.match(ln)
            if m: r['host'][m.group(2)] = max(r['host'].get(m.group(2), 0.0), float(m.group(4)))
            continue
        if ln.startswith('budget:'):
            m = BUD.match(ln)
            if m:
                b = r['bud'] or [0.0, 0.0, 0.0]
                r['bud'] = [max(b[0], float(m.group(2))), max(b[1], float(m.group(3))), max(b[2], float(m.group(4)))]
            continue
        if ln.startswith('live:'):
            m = LIV.match(ln)
            if m:
                w = max(float(v) for v in m.group(1).split())
                r['live_peak'] = max(r['live_peak'] or 0.0, w); r['live_pct'] = max(r['live_pct'] or 0.0, float(m.group(2)))
                r['live_min'] = float(m.group(3)) if r['live_min'] is None else min(r['live_min'], float(m.group(3)))
            continue
        if ln.startswith('xstats'):
            m = XPH.match(ln)
            if m:
                r['pes'].add(int(m.group(1)))
                a = r['xs'].setdefault((m.group(2), m.group(3)), [0.0, 0.0, set()]); a[0] += float(m.group(6)); a[1] += float(m.group(7)); a[2].add(int(m.group(1)))
            continue
        if ln.startswith('wait-stats'):
            m = WST.match(ln)
            if m:
                for ph, b, wt in WPH.findall(m.group(2)):
                    a = r['wait'].setdefault(ph, [0.0, 0.0]); a[0] += float(b); a[1] += float(wt)
            continue
        m = ENVL.match(ln)
        if m:
            for k, v in ENVW.findall(m.group(1)): r['envs'][k] = v
    if r['nodes'] is None:
        n = len(r['a2a']) or len(r['pes'])
        m = re.search(r'(?:^|[/_])n(\d+)_r', path)
        r['nodes'] = int(m.group(1)) if m else (n or None)
    return r

def label_of(path):
    b = os.path.basename(path)
    return re.sub(r'\.(log|plain)$', '', b)

def src_of(path):
    d = os.path.dirname(os.path.abspath(path))
    if os.path.basename(d) == 'log': d = os.path.dirname(d)
    return os.path.basename(d) or d

def row_of(p, cfgmap, drv):
    r = parse_log(p)
    lab = label_of(p)
    row = dict(src=src_of(p), label=lab, exp='', round='', arm='', config='-')
    for rx in FNAMES:
        m = rx.match(lab)
        if m:
            gd = m.groupdict(); row['exp'] = gd.get('exp') or ''; row['round'] = gd.get('round') or ''; row['arm'] = (gd.get('arm') or '').upper()
            if r['nodes'] is None and gd.get('nn'): r['nodes'] = int(gd['nn'])
            break
    if r['envs']:
        row['config'] = hashlib.md5(' '.join('%s=%s' % kv for kv in sorted(r['envs'].items())).encode()).hexdigest()[:8]
    for rx, name in cfgmap:
        if rx.search(p): row['config'] = name; break
    rates = [r['a2a'][k] for k in sorted(r['a2a'])]
    rm = sum(rates) / len(rates) if rates else None
    n = r['nodes']
    row.update(nodes=n, digits=r['digits'], dpn=(r['digits'] // n if r['digits'] and n else None), verify=r['verify'] or 'none',
               total=r['total'], init=r['init'], bs=r['bs'], tree=r['tree'], recip=r['recip'], dm=r['dm'], dc=r['dc'], other=r['other'], vmhwm=r['vmhwm'],
               chain=r['chain'], r=rm, rmin=min(rates) if rates else None, rmax=max(rates) if rates else None)
    ds = r['dmsub']
    row.update(rcp_prod=ds[2] if ds else None, s1=ds[3] if ds else None, s2=ds[4] if ds else None, xlo=ds[5] if ds else None)
    row['mode'] = '' if rm is None else ('slow' if rm < SLOW else 'fast' if rm > FAST else 'mid')
    fl = []
    if r['chain'] is not None and r['chain'] > CHAIN_MAX: fl.append('chain')
    if r['init'] is not None and r['init'] >= INIT_MAX: fl.append('cold')
    if row['verify'] != 'OK': fl.append('noverify')
    if r['total'] is None: fl.append('nototal')
    row['flags'] = ','.join(fl)
    row['clean'] = int(not fl)
    for ph in PH + sorted(set(r['mem']) - set(PH)):
        if ph in r['mem']: row['dev_' + ph], row['live_' + ph] = r['mem'][ph]
        if ph in r['host']: row['hwm_' + ph] = r['host'][ph]
    if r['bud']: row['bud_peak'], row['bud_dev'], row['bud_host'] = r['bud']
    row.update(live_win=r['live_peak'], live_pct=r['live_pct'], avail_min=r['live_min'])
    for (ph, kd), (gb, tp, pes) in sorted(r['xs'].items()):
        if tp > 0 and len(pes): row['xs_%s_%s_gbpe' % (ph, kd)] = gb / len(pes); row['xs_%s_%s_rate' % (ph, kd)] = gb / tp * 1.0 if len(pes) else None
    for ph, (b, w) in sorted(r['wait'].items()): row['wait_%s' % ph] = w; row['bar_%s' % ph] = b
    d = drv.get(lab)
    if d: row['wall'], row['start'] = d
    return row

def read_driver(files):
    out = {}
    for f in files:
        for ln in open(f, errors='replace'):
            c = ln.rstrip('\n').split('\t')
            if len(c) < 5: continue
            stamp = c[-1] if re.match(r'\d\d-\d\d_\d\d:\d\d', c[-1]) else ''
            try:
                if len(c) >= 7 and c[2] in ('A', 'B') and re.match(r'^[A-Za-z]+$', c[0]):   # ab.tsv: exp round arm rc wall verify total stamp
                    out['%s_r%s_%s' % (c[0], c[1], c[2])] = (float(c[4]), stamp)
                elif c[1] in ('A', 'B') and len(c) >= 5:                                     # pairs.tsv: label arm rc VERIFY total stamp (no wall)
                    out[c[0]] = ('', stamp)
                elif re.match(r'^\d+$', c[0]) and re.match(r'^n\d+_r\d+$', c[1]):            # summary.tsv: nodes label rc wall verify total stamp
                    out[c[1]] = (float(c[3]), stamp)
            except ValueError:
                pass
    return out

# ---------------------------------------------------------------- statistics (stdlib)
T975 = [0, 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
        2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042]
def tq(df):
    return T975[df] if 1 <= df <= 30 else (2.021 if df <= 40 else 2.000 if df <= 60 else 1.980 if df <= 120 else 1.960) if df > 0 else float('nan')

def mean(v): return sum(v) / len(v)
def sd(v):
    if len(v) < 2: return float('nan')
    m = mean(v); return math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1))

def ols(x, y):
    """y = a + b x -> (a, b, residual sd with n-2 dof, n)"""
    n = len(x)
    if n < 3: return None
    mx, my = mean(x), mean(y)
    sxx = sum((u - mx) ** 2 for u in x)
    if sxx <= 0: return None
    b = sum((u - mx) * (v - my) for u, v in zip(x, y)) / sxx
    a = my - b * mx
    res = [v - (a + b * u) for u, v in zip(x, y)]
    return a, b, math.sqrt(sum(e * e for e in res) / (n - 2)), n

def ci_mean(d):
    n = len(d)
    if n < 2: return float('nan'), float('nan'), float('nan')
    m = mean(d); h = tq(n - 1) * sd(d) / math.sqrt(n)
    return m, m - h, m + h

def welch_t(u, v):
    if len(u) < 2 or len(v) < 2: return float('nan')
    se = math.sqrt(sd(u) ** 2 / len(u) + sd(v) ** 2 / len(v))
    return (mean(v) - mean(u)) / se if se > 0 else float('nan')

# ---------------------------------------------------------------- fits
STAGES = ['total', 'bs', 'dm', 'recip']

def fit_rows(rows):
    return [x for x in rows if x['clean'] and x['r'] and x['total'] is not None and (x['nodes'] or 0) > 1]

def groups_of(rows):
    g = {}
    for x in fit_rows(rows): g.setdefault((x['nodes'], x['dpn'], x['config']), []).append(x)
    return g

def do_fit(rows, oos):
    out = []
    for key in sorted(groups_of(rows), key=lambda k: (k[0] or 0, k[1] or 0, k[2])):
        g = groups_of(rows)[key]
        out.append('group nodes=%s digits/node=%s config=%s : n=%d clean runs with a rate' % (key[0], key[1], key[2], len(g)))
        for st in STAGES:
            pts = [(1.0 / x['r'], x[st]) for x in g if x.get(st) is not None]
            f = ols([p[0] for p in pts], [p[1] for p in pts])
            if not f: out.append('  %-6s fit not possible (n=%d)' % (st, len(pts))); continue
            a, b, s, n = f
            raw = sd([p[1] for p in pts])
            out.append('  %-6s %s = %.1f + %.0f / r   residual sd %.2f s (raw sd %.2f s, n=%d)   [modelled]' % (st, st, a, b, s, raw, n))
            if st == 'total':
                for x in g: x['resid'] = x['total'] - (a + b / x['r'])
                if oos and len(set(x['src'] for x in g)) > 1:
                    for sname in sorted(set(x['src'] for x in g)):
                        tr = [x for x in g if x['src'] != sname]; te = [x for x in g if x['src'] == sname]
                        f2 = ols([1 / x['r'] for x in tr], [x['total'] for x in tr])
                        if f2 and te:
                            e = [x['total'] - (f2[0] + f2[1] / x['r']) for x in te]
                            out.append('    out-of-sample: fit on others (n=%d: a=%.1f b=%.0f), test %s (n=%d): bias %+.2f s, sd %.2f s' % (len(tr), f2[0], f2[1], sname, len(te), mean(e), sd(e)))
    nf = len([x for x in rows if not x['clean']])
    out.append('(%d of %d runs not clean: chain > %.1f s, init >= %.0f s, no VERIFY OK or no total; excluded from the fits)' % (nf, len(rows), CHAIN_MAX, INIT_MAX))
    return out

def ols2(x, arm, y):
    """y = c_arm + b x (common slope, arm intercept): -> (effect B-A, se, b, dof)"""
    n = len(x);
    xa = [u for u, a in zip(x, arm) if a == 0]; xb = [u for u, a in zip(x, arm) if a == 1]
    ya = [v for v, a in zip(y, arm) if a == 0]; yb = [v for v, a in zip(y, arm) if a == 1]
    if len(xa) < 2 or len(xb) < 2: return None
    ma, mb, mya, myb = mean(xa), mean(xb), mean(ya), mean(yb)
    sxx = sum((u - ma) ** 2 for u in xa) + sum((u - mb) ** 2 for u in xb)
    if sxx <= 0: return None
    sxy = sum((u - ma) * (v - mya) for u, v in zip(xa, ya)) + sum((u - mb) * (v - myb) for u, v in zip(xb, yb))
    b = sxy / sxx
    res = [v - mya - b * (u - ma) for u, v in zip(xa, ya)] + [v - myb - b * (u - mb) for u, v in zip(xb, yb)]
    dof = n - 3
    if dof < 1: return None
    s2 = sum(e * e for e in res) / dof
    eff = (myb - mya) - b * (mb - ma)
    se = math.sqrt(s2 * (1.0 / len(xa) + 1.0 / len(xb) + (mb - ma) ** 2 / sxx))
    return eff, se, b, dof

def do_ab(rows, desc, target, xexp=()):
    out = []
    byexp = {}
    for x in rows:
        if x['arm'] in ('A', 'B') and (x['nodes'] or 0) > 1: byexp.setdefault((x['src'], x['exp'], x['nodes'], x['dpn']), []).append(x)
    grp = groups_of(rows)
    for key in sorted(byexp, key=str):
        xs = byexp[key]
        src, exp, nodes, dpn = key
        out.append('experiment src=%s exp=%s nodes=%s digits/node=%s%s' % (src, exp or '-', nodes, dpn, ('  [' + desc[exp] + ']') if exp in desc else ''))
        pairs = {}
        for x in xs: pairs.setdefault(x['round'], {}).setdefault(x['arm'], []).append(x)
        full = [rd for rd, d in pairs.items() if 'A' in d and 'B' in d]
        ok = [rd for rd in full if all(x['clean'] and x['r'] and x['total'] is not None for a in 'AB' for x in pairs[rd][a])]
        out.append('  rounds with both arms: %d; usable (all runs clean, with rate): %d; dropped: %s' % (len(full), len(ok), ', '.join('r' + str(r) for r in full if r not in ok) or 'none'))
        if len(ok) < 2: out.append('  too few usable pairs'); continue
        ref = grp.get((nodes, dpn, xs[0]['config']), [])
        f = ols([1 / x['r'] for x in ref], [x['total'] for x in ref])
        if not f: out.append('  no fit'); continue
        a, b, s, n = f
        def resid(x): return x['total'] - (a + b / x['r'])
        dr, dres, dra = [], [], []
        for rd in ok:
            A, B = pairs[rd]['A'], pairs[rd]['B']
            dr.append(mean([x['total'] for x in B]) - mean([x['total'] for x in A]))
            dres.append(mean([resid(x) for x in B]) - mean([resid(x) for x in A]))
            dra.append(mean([x['r'] for x in B]) - mean([x['r'] for x in A]))
        m0, l0, h0 = ci_mean(dr); m1, l1, h1 = ci_mean(dres); mr, lr, hr = ci_mean(dra)
        out.append('  fit used for the residuals: total = %.1f + %.0f / r over %d clean %d-node runs (residual sd %.2f s) [modelled]' % (a, b, n, nodes, s))
        out.append('  raw paired difference B - A (s):            %+7.2f  95%% CI [%+.2f, %+.2f]  (n=%d pairs, sd %.2f)' % (m0, l0, h0, len(dr), sd(dr)))
        out.append('  rate-adjusted difference of residuals (s):  %+7.2f  95%% CI [%+.2f, %+.2f]  (sd %.2f)  [fit treated as known]' % (m1, l1, h1, sd(dres)))
        sel = [x for rd in ok for a_ in 'AB' for x in pairs[rd][a_]]
        an = ols2([1 / x['r'] for x in sel], [0 if x['arm'] == 'A' else 1 for x in sel], [x['total'] for x in sel])
        if an:
            h = tq(an[3]) * an[1]
            out.append('  ANCOVA cross-check (arm intercepts, common slope b=%.0f, fit on these runs only): %+7.2f  95%% CI [%+.2f, %+.2f]  (dof %d)  <- the wider, more conservative CI' % (an[2], an[0], an[0] - h, an[0] + h, an[3]))
        for st in STAGES[1:]:
            fs = ols([1 / x['r'] for x in ref if x.get(st) is not None], [x[st] for x in ref if x.get(st) is not None])
            if not fs: continue
            dd = [mean([x[st] - (fs[0] + fs[1] / x['r']) for x in pairs[rd]['B']]) - mean([x[st] - (fs[0] + fs[1] / x['r']) for x in pairs[rd]['A']]) for rd in ok]
            dd0 = [mean([x[st] for x in pairs[rd]['B']]) - mean([x[st] for x in pairs[rd]['A']]) for rd in ok]
            m2, l2, h2 = ci_mean(dd); m3, l3, h3 = ci_mean(dd0)
            out.append('    stage %-5s raw %+6.2f [%+.2f, %+.2f]   rate-adjusted %+6.2f [%+.2f, %+.2f]' % (st, m3, l3, h3, m2, l2, h2))
        ra = [x['r'] for rd in ok for x in pairs[rd]['A']]; rb = [x['r'] for rd in ok for x in pairs[rd]['B']]
        t = welch_t(ra, rb); tp = mr / (sd(dra) / math.sqrt(len(dra))) if sd(dra) > 0 else float('nan')
        rel = (mean(rb) - mean(ra)) / mean(ra)
        out.append('  mean r: arm A %.2f, arm B %.2f GB/s (B - A paired %+.2f, CI [%+.2f, %+.2f]; Welch t %.2f; %+.1f%%)' % (mean(ra), mean(rb), mr, lr, hr, t, 100 * rel))
        sig = abs(tp) > tq(len(dra) - 1) or abs(t) > 2.0
        if sig or abs(rel) > 0.02:
            out.append('  WARNING: the arms differ in mean exchange rate r (%s). If the arm changes the exchange itself, r is not an arm-independent covariate: the adjustment' % ('significant' if sig else 'not significant, n=%d pairs' % len(dra)))
            out.append('           then removes part of the effect, and the estimate needs an environment probe (B05 rank 4). If the difference is only an unbalanced rate draw, the adjustment is right.')
        if exp in xexp:
            out.append('  CAVEAT (--exchange-exp): this arm changes the exchange itself; read the adjusted effect as conditional on the arm\'s own rate until a probe covariate exists.')
        if sd(dres) > 0:
            need = math.ceil((tq(max(1, len(dres) - 1)) * sd(dres) / target) ** 2)
            out.append('  pairs needed for a 95%% CI half-width of %.1f s at this sd: about %d (have %d) [modelled]' % (target, need, len(dres)))
    return out

COLS = ['src', 'label', 'exp', 'round', 'arm', 'config', 'nodes', 'digits', 'dpn', 'verify', 'total', 'init', 'bs', 'tree', 'recip', 'dm', 'rcp_prod', 's1', 's2', 'xlo',
        'dc', 'other', 'vmhwm', 'chain', 'r', 'rmin', 'rmax', 'mode', 'flags', 'clean', 'resid', 'wall', 'start']

def write_tsv(rows, path):
    extra = sorted(set(k for x in rows for k in x) - set(COLS))
    cols = COLS + extra
    with open(path, 'w') as f:
        f.write('\t'.join(cols) + '\n')
        for x in rows: f.write('\t'.join(num(x.get(c)) for c in cols) + '\n')

def collect(paths):
    files = []
    for p in paths:
        p = os.path.expanduser(p)
        if os.path.isdir(p): files += sorted(glob.glob(os.path.join(p, '*.log')) + glob.glob(os.path.join(p, 'log', '*.log')))
        else: files += sorted(glob.glob(p))
    seen, out = set(), []
    for f in files:
        k = os.path.abspath(f)
        if k not in seen: seen.add(k); out.append(f)
    return out

def selftest():
    import tempfile, random
    random.seed(1)
    d = tempfile.mkdtemp()
    os.makedirs(os.path.join(d, 'log'))
    for rd in range(1, 9):
        for arm, eff in (('A', 0.0), ('B', -5.0)):
            r = random.choice([11.0, 13.8])
            tot = 80.8 + 1388 / r + eff + random.gauss(0, 1.0)
            with open(os.path.join(d, 'log', 'x_r%d_%s.log' % (rd, arm)), 'w') as f:
                f.write('mnrun.sh: COMM_SHMEM_POOL_MB=512 from MN_PLAN_ONLY=257640000000:4 (x)\n')
                for pe in range(4): f.write('comm_shmem: pe %d: all-to-all 10 exchanges, 1.0 s post to completion (summed over the APU threads), 10.0 GB received = %.2f GB/s per APU thread; x\n' % (pe, r))
                f.write('total   %.2f s   (bs 70.0 + 10dP 0.0 + dm 80.0 + T1 0.4 + dc 3.0 + T2 0.0 = 153.4; init 29.0; other 1.0); VmHWM 29.0 GB\n' % tot)
                f.write('recip    24.00 s   (over 4 nodes; 32 iterations)\nmn: all 4 nodes: VERIFY OK\n')
    rows = [row_of(p, [], {}) for p in collect([d])]
    assert len(rows) == 16 and all(x['clean'] for x in rows) and rows[0]['nodes'] == 4 and rows[0]['dpn'] == 64410000000
    o = do_fit(rows, False); o2 = do_ab(rows, {}, 2.0)
    assert any(l.strip().startswith('total') and 'residual sd' in l for l in o), o
    assert any('-5.' in l or '-4.' in l for l in o2), o2
    print('selftest OK'); print('\n'.join(o + o2))

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='*', help='run-log directories (*.log, log/*.log) or files')
    ap.add_argument('--tsv', help='write the per-run table here')
    ap.add_argument('--fit', action='store_true'); ap.add_argument('--oos', action='store_true')
    ap.add_argument('--ab', action='store_true'); ap.add_argument('--target-hw', type=float, default=2.0, help='target CI half-width (s) for the pairs-needed line')
    ap.add_argument('--desc', action='append', default=[], metavar='EXP=TEXT', help='describe an experiment (exp label from the file names) in the A/B output')
    ap.add_argument('--exchange-exp', action='append', default=[], metavar='EXP', help='experiments whose arm changes the exchange itself (adds the probe caveat)')
    ap.add_argument('--config', action='append', default=[], metavar='REGEX=NAME', help='config name for log paths matching REGEX (when the log does not print its env words)')
    ap.add_argument('--driver-tsv', nargs='*', default=[], help='driver ab.tsv / pairs.tsv / summary.tsv files: attaches wall and start stamp by label')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest: return selftest()
    cfg = [(re.compile(k), v) for k, v in (c.split('=', 1) for c in a.config)]
    drv = read_driver(a.driver_tsv)
    files = collect(a.paths)
    rows = [row_of(p, cfg, drv) for p in files]
    print('# %d logs read' % len(rows))
    if not rows: return 1
    lines = []
    if a.fit or a.ab: lines += do_fit(rows, a.oos)
    if a.ab: lines += [''] + do_ab(rows, dict(d.split('=', 1) for d in a.desc), a.target_hw, a.exchange_exp)
    if a.tsv: write_tsv(rows, a.tsv); print('# table: %s (%d rows)' % (a.tsv, len(rows)))
    print('\n'.join(lines))

if __name__ == '__main__':
    sys.exit(main() or 0)

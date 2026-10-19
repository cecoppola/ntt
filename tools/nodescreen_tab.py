#!/usr/bin/env python3
"""nodescreen_tab.py -- tabulate and apply the exclusion rule for tools/nodescreen.sh (results/NODE_SLOWMODE.md §B04).
Reads OUT/local/<host>.log (lines 'NS ...' incl. 'NS nics have= want=' (fewer NICs than wanted = failed), 'NODECHK ...', 'NODECHK_COLD ...', ecalc 'tl' lines), OUT/mn.log + OUT/mn.meta (initchk.py
parses the labelled multi-node log).  Rule: a time metric > THR x reference and >= MINEX s above it, or a failed step, flags the node.
Reference = median over nodes (>= 3 values), else the smaller value.  Writes OUT/screen_table.txt and OUT/EXCLUDE.
Exit 0 none flagged, 3 flagged, 2 nothing could be parsed."""
import os, re, sys, argparse, statistics, importlib.util

TIME = ['nc_up', 'cold', 'touch', 'loc_init', 'loc_post', 'loc_wall', 'mn_init', 'mn_up']


def kv(line):
    d = {}
    for w in line.split():
        if '=' in w:
            k, v = w.split('=', 1); d[k] = v
    return d


def parse_local(path):
    r = {'fail': [], 'v': {}, 'avail': None, 'host': None, 'steps': {}}
    seen_done = False
    for l in open(path, errors='replace'):
        l = l.rstrip('\n')
        if l.startswith('NS host '):
            r['host'] = l.split()[2]
        elif l.startswith('NS nics '):
            d = kv(l); r['nics'] = int(d['have'])
            if int(d['have']) < int(d['want']): r['fail'].append('NICs %s < %s wanted (SHMEM_OFI_NUM_NICS would abort)' % (d['have'], d['want']))
        elif l.startswith('NS done'):
            seen_done = True
        elif l.startswith('NS step '):
            w = l.split(); name = w[2]; d = kv(l)
            r['steps'][name] = (int(d.get('rc', -1)), float(d.get('wall', 0)))
            if d.get('rc') != '0':
                r['fail'].append('%s rc=%s' % (name, d.get('rc')))
        elif l.startswith('NODECHK_COLD '):
            d = kv(l); t = [float(v) for k, v in d.items() if re.match(r'apu\d+_total_s$', k) and v not in ('ERR',)]
            if t: r['v']['cold'] = max(t)
            if 'touch_s' in d:
                try: r['v']['touch'] = float(d['touch_s'])
                except ValueError: pass
        elif l.startswith('NODECHK '):
            d = kv(l)
            if 'ERROR' in d: r['fail'].append('nodechk ' + d['ERROR'])
            t = []
            for k, v in d.items():
                if re.match(r'apu\d+_s$', k):
                    if v == 'ERR': r['fail'].append('upload ERR ' + k)
                    else: t.append(float(v))
            if t: r['v']['nc_up'] = max(t)
            if 'MemAvailable_kB' in d: r['avail'] = int(d['MemAvailable_kB']) / 1e6
        else:
            m = re.match(r'^tl\s+([\d.]+)\s+peer access done', l)
            if m and 'loc_init' not in r['v']: r['v']['loc_init'] = float(m.group(1))
    if 'ecalc' in r['steps']: r['v']['loc_wall'] = r['steps']['ecalc'][1]
    if 'loc_wall' in r['v'] and 'loc_init' in r['v']: r['v']['loc_post'] = r['v']['loc_wall'] - r['v']['loc_init']   # compute + first uploads after init (a 20 s upload stall is 2x loc_wall but 3x+ loc_post)
    if 'loc_init' not in r['v'] and 'ecalc' in r['steps'] and r['steps']['ecalc'][0] == 0:
        r['fail'].append('no init line (ECALC_INIT_TL) in the ecalc output')
    if not seen_done: r['fail'].append('node script did not finish (timeout or lost node)')
    return r


def ref(vals):
    return statistics.median(vals) if len(vals) >= 3 else min(vals)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True); ap.add_argument('--nodes', required=True)
    ap.add_argument('--thr', type=float, default=3.0); ap.add_argument('--min-excess', type=float, default=2.0)
    ap.add_argument('--min-avail-gb', type=float, default=0.0); ap.add_argument('--stage', default='all')
    ap.add_argument('--quiet', action='store_true'); ap.add_argument('--initchk'); ap.add_argument('--flagged-file')
    a = ap.parse_args()
    nodes = [l.strip() for l in open(a.nodes) if l.strip()]
    short = lambda h: h.split('.')[0]
    rows = {short(h): {'v': {}, 'fail': [], 'avail': None} for h in nodes}
    ldir = os.path.join(a.out, 'local'); have_local = os.path.isdir(ldir) and any(f.endswith('.log') for f in os.listdir(ldir))
    if have_local:
        for h in rows:
            p = os.path.join(ldir, h + '.log')
            if os.path.exists(p):
                r = parse_local(p); rows[h].update(r)
            else:
                rows[h]['fail'].append('no local log')
    mn_note = ''
    mlog = os.path.join(a.out, 'mn.log')
    if a.stage != 'local' and os.path.exists(mlog):
        meta = open(os.path.join(a.out, 'mn.meta')).read().strip() if os.path.exists(os.path.join(a.out, 'mn.meta')) else ''
        sp = importlib.util.spec_from_file_location('initchk', a.initchk); ic = importlib.util.module_from_spec(sp); sp.loader.exec_module(ic)
        nd = {}; ic.parse_file(mlog, nd)
        got = 0
        for rk, n in nd.items():
            h = short(n.host or '')
            if h in rows:
                if n.v['rns_init'] is not None: rows[h]['v']['mn_init'] = n.v['rns_init']; got += 1
                if n.v['upload'] is not None: rows[h]['v']['mn_up'] = n.v['upload']
        d = kv(meta)
        mn_note = 'mn stage: %s; per-node init lines for %d node(s)' % (meta or 'no meta', got)
        if d.get('rc') not in (None, '0'): mn_note += ' -- THE MN RUN FAILED (rc %s): a fabric-wide state, a hung node or a shmem start problem; see mn.log; no node is excluded for it' % d.get('rc')
    flags = {h: [] for h in rows}
    refs = {}
    for m in TIME:
        vals = [r['v'][m] for r in rows.values() if m in r['v']]
        if not vals: continue
        refs[m] = ref(vals)
        for h, r in rows.items():
            x = r['v'].get(m)
            if x is not None and len(vals) >= 2 and x > a.thr * refs[m] and x - refs[m] >= a.min_excess:
                flags[h].append('%s %.2f s = %.1fx ref %.2f' % (m, x, x / max(refs[m], 1e-9), refs[m]))
    avs = [r['avail'] for r in rows.values() if r['avail']]
    for h, r in rows.items():
        for f in r['fail']: flags[h].append('FAILED: ' + f)
        if a.min_avail_gb > 0 and r['avail'] is not None and r['avail'] < a.min_avail_gb:
            flags[h].append('MemAvailable %.0f GB < %.0f' % (r['avail'], a.min_avail_gb))
    if a.stage == 'local' and a.quiet:
        print('KEEP ' + ','.join(h for h in rows if not flags[h])); return 0
    f = lambda x: '-' if x is None else '%.2f' % x
    out = ['%-16s %7s %6s %6s %7s %8s %8s %8s %7s %7s %8s  %s' % ('node', 'avail', 'nc_up', 'cold', 'touch', 'loc_init', 'loc_post', 'loc_wall', 'mn_init', 'mn_up', 'step_s', 'verdict')]
    for h, r in rows.items():
        st = r.get('steps', {})
        out.append('%-16s %7s %6s %6s %7s %8s %8s %8s %7s %7s %8s  %s' % (h, ('%.0f' % r['avail']) if r['avail'] else '-', f(r['v'].get('nc_up')), f(r['v'].get('cold')), f(r['v'].get('touch')),
                   f(r['v'].get('loc_init')), f(r['v'].get('loc_post')), f(r['v'].get('loc_wall')), f(r['v'].get('mn_init')), f(r['v'].get('mn_up')),
                   ('%.1f' % st['nodechk'][1] if 'nodechk' in st else '-'), ('EXCLUDE: ' + '; '.join(flags[h])) if flags[h] else 'ok'))
    out.append('%-16s %7s %6s %6s %7s %8s %8s %8s %7s %7s' % ('reference', ('%.0f' % ref(avs)) if avs else '-', f(refs.get('nc_up')), f(refs.get('cold')), f(refs.get('touch')), f(refs.get('loc_init')), f(refs.get('loc_post')),
               f(refs.get('loc_wall')), f(refs.get('mn_init')), f(refs.get('mn_up'))))
    out.append('rule: > %.1fx reference and >= %.1f s above it, or a failed step; reference = median (>= 3 values) else the smaller; times in s; avail = MemAvailable GB' % (a.thr, a.min_excess))
    if mn_note: out.append(mn_note)
    exc = [h for h in rows if flags[h]]
    open(os.path.join(a.out, 'screen_table.txt'), 'w').write('\n'.join(out) + '\n')
    open(os.path.join(a.out, 'EXCLUDE'), 'w').write(''.join(h + '\n' for h in exc))
    print('\n'.join(out))
    if exc and a.flagged_file:
        with open(a.flagged_file, 'a') as fh: fh.write(''.join(h + '\n' for h in exc))
    print('nodescreen: %d node(s), %d to EXCLUDE: %s' % (len(rows), len(exc), ','.join(exc) or '-'))
    if not have_local and not any(r['v'] for r in rows.values()): return 2
    return 3 if exc else 0


sys.exit(main())

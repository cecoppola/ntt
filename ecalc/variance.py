#!/usr/bin/env python3
# summarise results/variance: per-run phase times, mean/sd, per-APU clock and power while running
import re, json, glob, statistics as st, sys
import os
D = 'results/variance' + ('_b' + os.environ['LIMB_BASE'] if os.environ.get('LIMB_BASE') else '')
runs = sorted(glob.glob(D + '/run*.log'))
ph = ['bs', '10dP', 'dm', 'T1', 'dc', 'T2', 'total']
print('dir', D)
tab = {}
for r in runs:
    t = open(r).read()
    tab[r] = {p: float(m.group(1)) for p in ph if (m := re.search(r'^%s\s+([0-9.]+) s' % re.escape(p), t, re.M))}
print('%-8s' % 'run' + ''.join('%9s' % p for p in ph))
for r in runs: print('%-8s' % r.split('/')[-1][:-4] + ''.join('%9.1f' % tab[r].get(p, float('nan')) for p in ph))
if len(runs) > 1:
    print('%-8s' % 'mean' + ''.join('%9.1f' % st.mean(tab[r][p] for r in runs if p in tab[r]) for p in ph))
    print('%-8s' % 'sd' + ''.join('%9.1f' % st.stdev(tab[r][p] for r in runs if p in tab[r]) for p in ph))
    print('%-8s' % 'sd %' + ''.join('%9.1f' % (100 * st.stdev(tab[r][p] for r in runs) / st.mean(tab[r][p] for r in runs)) for p in ph))
for r in runs:
    smi = r[:-4] + '.smi'
    clk = {}; pw = {}
    for line in open(smi):
        parts = line.split(' ', 1)
        if len(parts) < 2 or not parts[1].startswith('['): continue
        try: js = json.loads(parts[1])
        except Exception: continue
        for g in js.get('gpu_data', js if isinstance(js, list) else []):
            gid = g.get('gpu'); c = g.get('clock', {}); p = g.get('power', {})
            vals = [c[k]['clk']['value'] for k in c if k.startswith('gfx_') and isinstance(c[k], dict) and isinstance(c[k].get('clk'), dict)]
            if vals: clk.setdefault(gid, []).append(st.mean(vals))
            w = p.get('socket_power')
            if isinstance(w, dict) and isinstance(w.get('value'), (int, float)): pw.setdefault(gid, []).append(w['value'])
    if clk: print(r.split('/')[-1][:-4], 'sclk MHz mean per APU:', {g: round(st.mean(v)) for g, v in sorted(clk.items())},
                  'power W:', {g: round(st.mean(v)) for g, v in sorted(pw.items())} if pw else '')

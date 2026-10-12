#!/usr/bin/env python3
"""o9_sum.py <rundir>: O9 summary (results/O9.md).  Reads <rundir>/runs.tsv (seq, label, nodes, arm, rc, wall, group) and the
logs <rundir>/<label>.log; prints per run the total / VERIFY / NIC group and, per (phase, kind) of the layered exchanges and the
direct kinds, the per-node means of the COMM_XSTATS lines (summed over the 4 APU threads by the engine):
  GB (received per node), rate_post = GB / t_post (GB/s per APU thread; double-counts under COMM_LAYER_INTER2),
  rate_span = GB / t_call (post-to-post span rate, the INTER2-proof rate of results/C02.md), and per APU thread
  t_wait, roff (blocked on a peer's offset word = skew), sig (blocked on signal words = transfer / tail) in seconds.
Then the layer-stats (COMM_LAYER_STATS=1) fabric rate per node and the per-arm means of the totals."""
import re, sys, os, statistics as st
D = sys.argv[1]
XS = re.compile(r'xstats pe (\d+) phase (\S+) kind (\S+): n (\d+) rounds (\d+) GB ([\d.]+) t_post ([\d.]+) s t_call ([\d.]+) s '
                r't_wait ([\d.]+) s roff_wait ([\d.]+) s sig_wait ([\d.]+) s')
LS = re.compile(r'layer-stats \(at exit\) node (\d+): .*fabric ([\d.]+) GB at ([\d.]+) GB/s per APU')
LH = re.compile(r'layer-stats \(at exit\) node (\d+): both/span ([\d.]+) %.*held ([\d.]+) s, of it fabric idle ([\d.]+) s')
rows = []
try:
    for ln in open(os.path.join(D, 'runs.tsv')):
        f = ln.rstrip('\n').split('\t')
        if len(f) >= 7 and f[0] != 'seq': rows.append(f)
except OSError:
    print('no runs.tsv'); sys.exit(0)
tot = {}
for seq, lab, nodes, arm, rc, wall, grp in (r[:7] for r in rows):
    p = os.path.join(D, lab + '.log'); txt = open(p, errors='replace').read() if os.path.exists(p) else ''
    txt = re.sub(r'(?m)^ *\d+: ', '', txt)
    m = re.search(r'(?m)^total +([\d.]+) s', txt); total = float(m.group(1)) if m else None
    nver = len(re.findall(r'VERIFY OK', txt)); init = re.search(r'init ([\d.]+);', txt)
    print(f'== {lab}: nodes {nodes} arm {arm} rc {rc} wall {wall} s NIC group {grp} | total {total} s, init {init.group(1) if init else "NA"} s, VERIFY OK lines {nver}')
    if rc == '0' and total and nver >= 1: tot.setdefault((nodes, arm), []).append(total)
    agg = {}
    for m in XS.finditer(txt):
        pe, ph, k = int(m.group(1)), m.group(2), m.group(3)
        v = [float(x) for x in m.group(4, 6, 7, 8, 9, 10, 11)]
        a = agg.setdefault((ph, k), [set(), [0.0] * 7]); a[0].add(pe); a[1] = [x + y for x, y in zip(a[1], v)]
    if agg:
        print(f'   {"phase":6s} {"kind":7s} {"n":>6s} {"GB/node":>9s} {"rate_post":>9s} {"rate_span":>9s} {"t_wait/thr":>10s} {"roff/thr":>9s} {"sig/thr":>8s}')
    for (ph, k), (pes, v) in sorted(agg.items()):
        npe = len(pes); n, gb, tp, tc, tw, ro, sg = (x / npe for x in v)
        if gb < 1: continue
        sp = '%9.2f' % (gb / tc) if tc > 0.05 * tp else '%9s' % '-'
        print(f'   {ph:6s} {k:7s} {n:6.0f} {gb:9.1f} {gb / tp if tp else 0:9.2f} {sp} {tw / 4:10.1f} {ro / 4:9.1f} {sg / 4:8.1f}')
    fr = {int(m.group(1)): (float(m.group(2)), float(m.group(3))) for m in LS.finditer(txt)}
    hd = {int(m.group(1)): (float(m.group(2)), float(m.group(3)), float(m.group(4))) for m in LH.finditer(txt)}
    if fr:
        print('   layer-stats fabric GB/s per APU by node: ' + ' '.join(f'n{k}:{v[1]:.2f}' for k, v in sorted(fr.items()))
              + (' | both/span %: ' + ' '.join(f'{v[0]:.0f}' for k, v in sorted(hd.items())) if hd else ''))
print('== totals per (nodes, arm): mean (runs)')
for (nodes, arm), v in sorted(tot.items()):
    print(f'   {nodes} n arm {arm}: {st.mean(v):.2f} s ({", ".join(f"{x:.2f}" for x in v)})')
for nodes in sorted({k[0] for k in tot}):
    a, b = tot.get((nodes, 'A'), []), tot.get((nodes, 'B'), [])
    if a and b: print(f'   {nodes} n B - A = {st.mean(b) - st.mean(a):+.2f} s (diagnostic only: {len(a)}+{len(b)} runs, not a timing claim)')

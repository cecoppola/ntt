#!/usr/bin/env python3
"""o7b_table.py <rocprofv3 csv dir> <wall_s> [topN]  -- top-N kernel table from rocprofv3 --kernel-trace CSV output.
Columns: kernel (arguments stripped), calls, total ms (sum over all agents), % of GPU time (share of the summed kernel
durations), % of wall (total ms / (agents x wall)), then seconds per agent (S39 listed APU0 = first agent column).
Also prints per-agent busy time (sum of durations) and the trace window. Read-only."""
import csv, glob, os, re, sys, collections
d, wall = sys.argv[1], float(sys.argv[2]); top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
files = sorted(glob.glob(os.path.join(d, '**', '*kernel_trace.csv'), recursive=True))
if not files: print("no kernel_trace.csv under", d); sys.exit(1)
def norm(n):
    n = n.strip().strip('"'); i = n.find('(')           # drop the argument list (template args use < > only)
    n = n[:i] if i > 0 else n
    return re.sub(r'^(void\s+)', '', n)
agg = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))   # name -> agent -> [calls, ns]
busy = collections.defaultdict(int); t0 = None; t1 = None; ncall = 0
for f in files:
    with open(f, newline='') as fh:
        for r in csv.DictReader(fh):
            try: s, e = int(r['Start_Timestamp']), int(r['End_Timestamp'])
            except (KeyError, ValueError): continue
            a = r.get('Agent_Id', '?'); k = norm(r.get('Kernel_Name', '?'))
            x = agg[k][a]; x[0] += 1; x[1] += e - s; busy[a] += e - s; ncall += 1
            t0 = s if t0 is None or s < t0 else t0; t1 = e if t1 is None or e > t1 else t1
agents = sorted(busy, key=lambda a: (len(a), a)); na = len(agents)
tot = sum(busy.values())
rows = sorted(((sum(v[1] for v in per.values()), sum(v[0] for v in per.values()), k, per) for k, per in agg.items()), reverse=True)
print(f"files {len(files)}, dispatches {ncall}, agents {na}, trace window {(t1-t0)/1e9:.1f} s, run wall {wall:.1f} s (measured, from the ecalc 'total' line)")
print("busy s per agent (sum of kernel durations): " + "  ".join(f"{a}:{busy[a]/1e9:.1f}" for a in agents))
print(f"{'#':>2} {'kernel':<46} {'calls':>7} {'total ms':>10} {'%GPU':>6} {'%wall':>6} | s per agent: " + " ".join(f"{a:>7}" for a in agents))
for i, (ns, c, k, per) in enumerate(rows[:top], 1):
    print(f"{i:>2} {k[:46]:<46} {c:>7} {ns/1e6:>10.1f} {100*ns/tot:>6.2f} {100*ns/1e9/(na*wall):>6.2f} | " + " ".join(f"{per[a][1]/1e9 if a in per else 0:>7.2f}" for a in agents))
rest = sum(r[0] for r in rows[top:])
print(f"   other {max(0,len(rows)-top)} kernels{'':<28} {sum(r[1] for r in rows[top:]):>7} {rest/1e6:>10.1f} {100*rest/tot:>6.2f} {100*rest/1e9/(na*wall):>6.2f}")
print(f"   all kernels{'':<35} {ncall:>7} {tot/1e6:>10.1f} {100.0:>6.2f} {100*tot/1e9/(na*wall):>6.2f}")

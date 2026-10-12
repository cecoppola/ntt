#!/usr/bin/env python3
"""Per-agent GPU busy timeline from a rocprofv3 kernel_trace.csv: union of kernel intervals per time bin.
usage: o7b_timeline.py <kernel_trace.csv> <run_wall_s> [bin_s]   (t=0 is the first dispatch; the wall tail is the run end)"""
import csv, sys, collections
f, wall = sys.argv[1], float(sys.argv[2]); b = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
iv = collections.defaultdict(list); t0 = None
for r in csv.DictReader(open(f)):
    s, e = int(r["Start_Timestamp"]), int(r["End_Timestamp"])
    iv[r["Agent_Id"]].append((s, e)); t0 = s if t0 is None else min(t0, s)
ag = sorted(iv); nb = int(wall / b) + 1; busy = {a: [0.0] * nb for a in ag}
for a in ag:
    cur_s = cur_e = None; segs = []
    for s, e in sorted(iv[a]):
        if cur_e is None or s > cur_e:
            if cur_e is not None: segs.append((cur_s, cur_e))
            cur_s, cur_e = s, e
        else: cur_e = max(cur_e, e)
    segs.append((cur_s, cur_e))
    for s, e in segs:
        x, y = (s - t0) / 1e9, (e - t0) / 1e9
        while x < y and int(x / b) < nb:
            k = int(x / b); z = min(y, (k + 1) * b); busy[a][k] += z - x; x = z
print("t0 = first dispatch; bins of %.1f s; busy s per agent (union of kernel intervals)" % b)
print("bin_start " + " ".join("%8s" % a for a in ag))
for k in range(nb):
    print("%8.1f " % (k * b) + " ".join("%8.2f" % busy[a][k] for a in ag))
print("total     " + " ".join("%8.2f" % sum(busy[a]) for a in ag))
print("last kernel end at %.2f s after first dispatch" % (max(e for a in ag for _, e in iv[a]) / 1e9 - t0 / 1e9))

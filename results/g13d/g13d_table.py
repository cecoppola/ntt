#!/usr/bin/env python3
"""G13d: one table row per run log in ~/g13d (tags given, or all): digits, wall, phases, pieces, verdicts, device peak."""
import os, re, sys, glob
D = os.path.expanduser("~/g13d")
summ = open(os.path.join(D, "summary.txt")).read()
tags = sys.argv[1:] or sorted(os.path.basename(f)[:-4] for f in glob.glob(D + "/*.log"))
print("| tag | digits | env | wall s | total s | init | bs | recip | dm | phases | big products (count*ka x kb) | dist calls (recip) | device peak GB | verdict |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for t in tags:
    f = os.path.join(D, t + ".log")
    if not os.path.exists(f): continue
    s = open(f, errors="replace").read()
    m = re.search(r"\] " + re.escape(t) + r" np=(\d+) D=(\d+) \[([^\]]*)\]: rc (\d+), VERIFY OK x(\d+), (\S*) ?wall (\d+) s", summ)
    np_, dg, env, rc, nv, cmp_, wall = m.groups() if m else ("?", "?", "?", "?", "?", "", "?")
    tot = re.findall(r"^total\s+([\d.]+) s\s+\(bs ([\d.]+) \+ 10dP [\d.]+ \+ dm ([\d.]+).*?= ([\d.]+); init ([\d.]+)", s, re.M)
    rec = re.findall(r"^recip\s+([\d.]+) s", s, re.M)
    rdb = re.findall(r"^recip\(db\) [\d.]+ s: dist (\d+) calls", s, re.M)
    pcs = {}
    for a, b, na, nb in re.findall(r"dist_db (\d+) x (\d+) limbs: (\d+) x (\d+) pieces", s):
        if int(a) + int(b) > 2 ** 28: pcs[(int(na), int(nb))] = pcs.get((int(na), int(nb)), 0) + 1
    pc = " ".join(f"{c}*{a}x{b}" for (a, b), c in sorted(pcs.items(), key=lambda x: x[0][0] * x[0][1]))
    dev = max([float(x) for x in re.findall(r"^mem \[.*?\] device ([\d.]+) GB", s, re.M)] or [0])
    ver = f"VERIFY OK x{nv}" + (f", {cmp_}" if cmp_ else "") + ("" if rc == "0" else f", rc {rc}")
    if tot:
        T, bs, dm, ph, ini = tot[-1]
        print(f"| {t} | {int(dg)/1e9:.1f}e9 | {env} | {wall} | {T} | {ini} | {bs} | {rec[-1] if rec else ''} | {dm} | {ph} | {pc} | {rdb[-1] if rdb else ''} | {dev:.1f} | {ver} |")
    else:
        print(f"| {t} | {dg} | {env} | {wall} | - | | | | | | {pc} | | {dev:.1f} | {ver} |")

#!/usr/bin/env python3
"""c10_chain.py - C10: per-run chain / init / mode table in time order from calib_table.py's TSV plus driver stamps and the
`live:` MemAvailable / Cached lines.  usage: c10_chain.py LOGROOT CALIB.tsv   (stdlib only; all figures measured)"""
import sys, re, os, csv, datetime as dt, statistics as st
root, tsv = sys.argv[1], sys.argv[2]
def stamp(s):  # 10-11_02:11:19 (end of run) -> datetime
    return dt.datetime.strptime("2026-" + s, "%Y-%m-%d_%H:%M:%S")
meta = {}  # (src,label) -> (end datetime, wall, hold)
def add(src, label, end, wall, hold): meta[(src, label)] = (end, float(wall), hold)
def rd(p): return [l.rstrip("\n").split("\t") for l in open(p)] if os.path.exists(p) else []
for r in rd(f"{root}/s47/run/ab.tsv"):
    add("run", f"{r[0]}_r{r[1]}_{r[2]}", stamp(r[7]), r[4], "12471")
for r in rd(f"{root}/s47/run/summary.tsv"):
    if r[1].startswith("n4"): add("run", r[1], stamp(r[6]), r[3], "12471")
for sub, hold in (("s50/abba4_0024", "12471"), ("s51/abba4", "12471"), ("s51/abba4b", "12496")):
    for r in rd(f"{root}/batch1011/T5/{sub}/pairs.tsv"):
        add(sub.split("/")[1], r[0], stamp(r[5]), r[4] and 0 or 0, hold) if False else None
        # pairs.tsv: label arm rc verify total stamp [hosts]; wall unknown -> use total + 12 s
        add(sub.split("/")[1], r[0], stamp(r[5]), float(r[4]) + 12, hold)
for r in rd(f"{root}/a13/A/runs.tsv")[1:]:
    e = dt.datetime.strptime("2026-" + r[11], "%Y-%m-%d_%H:%M:%S") + dt.timedelta(seconds=float(r[4]))
    add("A", r[0], e, r[4], "12531")
rows = list(csv.DictReader(open(tsv), delimiter="\t"))
DIRS = {"run": "s47/run/log", "A": "a13/A/log", "abba4_0024": "batch1011/T5/s50/abba4_0024/log",
        "abba4": "batch1011/T5/s51/abba4/log", "abba4b": "batch1011/T5/s51/abba4b/log"}
def memmin(src, label):
    p = f"{root}/{DIRS[src]}/{label}.log"
    if not os.path.exists(p): return (None, None)
    a = []; c = []
    for l in open(p, errors="replace"):
        if l.startswith("live: recip"):
            m = re.search(r"MemAvailable ([\d.]+) GB, Cached ([\d.]+) GB", l)
            if m: a.append(float(m[1])); c.append(float(m[2]))
    return (min(a), min(c)) if a else (None, None)
out = []
for r in rows:
    if r["nodes"] != "4" or not r["total"]: continue
    k = (r["src"] if r["src"] in ("run", "A") else r["src"], r["label"])
    if r["src"] == "abba4_0024": k = ("abba4_0024", r["label"])
    m = meta.get(k)
    if not m: continue
    a, c = memmin(*k)
    out.append(dict(src=r["src"], label=r["label"], end=m[0], hold=m[2], total=float(r["total"]), init=float(r["init"] or 0),
                    chain=float(r["chain"] or 0), r=float(r["r"] or 0), mode=r["mode"], ma=a, cached=c, rcp=float(r["recip"] or 0)))
out.sort(key=lambda d: d["end"])
prev = None; idx = {}
print("hold\tidx\tend\tlabel\ttotal\tinit\tchain\ta2a\tmode\tmemavail_min\tcached_min\tgap_s\tprev_chain\tprev_mode")
for d in out:
    idx[d["hold"]] = idx.get(d["hold"], 0) + 1
    gap = (d["end"] - dt.timedelta(seconds=d["total"] + 12) - prev["end"]).total_seconds() if prev and prev["hold"] == d["hold"] else float("nan")
    print("\t".join(str(x) for x in (d["hold"], idx[d["hold"]], d["end"].strftime("%m-%d %H:%M"), d["label"], d["total"], d["init"], d["chain"], d["r"], d["mode"],
          d["ma"], d["cached"], "%.0f" % gap, prev["chain"] if prev else "", prev["mode"] if prev else "")))
    d["gap"] = gap; d["idx"] = idx[d["hold"]]; prev = d
import pickle; pickle.dump(out, open(os.environ.get("C10_PKL", "/dev/null") if False else "/dev/null", "wb")) if False else None

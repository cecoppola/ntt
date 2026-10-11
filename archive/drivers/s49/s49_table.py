#!/usr/bin/env python3
# T4 (S49): stdin = "<cluster> <file> NODECHK k=v ..." lines (+ marker lines) -> host table with flags.
import sys, re, statistics as st
rows = {}; marks = []
for ln in sys.stdin:
    ln = ln.rstrip("\n")
    m = re.match(r"(\S+) (\S+) NODECHK (.*)", ln)
    if not m: marks.append(ln); continue
    cl, f, rest = m.groups(); kv = dict(x.split("=", 1) for x in rest.split() if "=" in x)
    rows[(cl, kv.get("host", "?"))] = (f, kv)   # later files overwrite
for cl in ("aac7", "aac6"):
    hs = sorted(h for c, h in rows if c == cl)
    if not hs: print(cl, ": no data"); continue
    up = {}; av = {}
    for h in hs:
        kv = rows[(cl, h)][1]; a = [float(v) for k, v in kv.items() if re.match(r"apu\d+_s$", k) and v != "ERR"]
        up[h] = max(a) if a else -1; av[h] = float(kv.get("MemAvailable_kB", 0)) / 1e6
    mu = st.median(u for u in up.values() if u >= 0); ma = st.median(av.values())
    print("== %s: %d hosts; median slowest-APU upload %.3f s, median MemAvailable %.1f GB" % (cl, len(hs), mu, ma))
    print("%-18s %11s %9s %-34s %s" % ("host", "MemAvail GB", "Cached GB", "upload s per APU (apu0..3)", "flags"))
    for h in hs:
        kv = rows[(cl, h)][1]; ap = " ".join(kv.get("apu%d_s" % i, "-") for i in range(4)); fl = []
        if kv.get("ERROR") or "ERR" in ap: fl.append("ERROR")
        if mu > 0 and up[h] > 3 * mu: fl.append("SLOW(%.1fx)" % (up[h] / mu))
        if av[h] < 0.9 * ma: fl.append("LOWMEM(%.0f%%)" % (100 * av[h] / ma))
        print("%-18s %11.1f %9.1f %-34s %s" % (h, av[h], float(kv.get("Cached_kB", 0)) / 1e6, ap, " ".join(fl)))
print("\n".join(marks))

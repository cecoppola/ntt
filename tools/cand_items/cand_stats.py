#!/usr/bin/env python3
"""cand_stats.py -- copy of tools/o2_stats.py (branch o2) for CAND: A = main binary, B = cand with all three switches on; --stop uses the TOTAL (>= 4 pairs, p < 0.05).

  tools/o2_stats.py LOGDIR [--stop]

Logs: LOGDIR/<tag>_p<k>_<A|B>.log (mnrun output with MNRUN_LABEL=1; A = switch off, B = DBIG_SMALL_BOUNCE=1), printed with
COMM_HS_TS=1 NEWTON_MN_RECIP_TS=2 DBIG_BOUNCE_TS=1.  Per run: total, the recip "single-node chain" interval (rank 0), per node
db_to_bi in the gather prelude (stamps alloc_host -> db_to_bi_done), the late PE (allgather entry = h2d_done stamp, minus the
earliest), and the summed first-use pinned allocation time (bounce_init lines).  Paired differences B - A per pair k with mean,
95 % CI and two-sided p (Student t).  --stop: exit 0 when the stop rule holds (>= 4 complete pairs and p < 0.05 on the chain
interval), else 1.
"""
import os, re, sys, math, glob, collections

def betacf(a, b, x):
    qab, qap, qam = a + b, a + 1, a - 1; c, d = 1.0, 1 - qab * x / qap
    d = 1 / (d if abs(d) > 1e-30 else 1e-30); h = d
    for m in range(1, 300):
        m2 = 2 * m; aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1 + aa * d; d = 1 / (d if abs(d) > 1e-30 else 1e-30); c = 1 + aa / c; c = c if abs(c) > 1e-30 else 1e-30; h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1 + aa * d; d = 1 / (d if abs(d) > 1e-30 else 1e-30); c = 1 + aa / c; c = c if abs(c) > 1e-30 else 1e-30; de = d * c; h *= de
        if abs(de - 1) < 3e-12: break
    return h

def betai(a, b, x):
    if x <= 0 or x >= 1: return 0.0 if x <= 0 else 1.0
    bt = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log(1 - x))
    return bt * betacf(a, b, x) / a if x < (a + 1) / (a + b + 2) else 1 - bt * betacf(b, a, 1 - x) / b

def t_p(t, df): return betai(df / 2, 0.5, df / (df + t * t))           # two-sided p
def t_crit(df):                                                        # 97.5 % quantile by bisection
    lo, hi = 0.0, 100.0
    for _ in range(100):
        m = (lo + hi) / 2
        if t_p(m, df) > 0.05: lo = m
        else: hi = m
    return hi

def paired(ds):
    n = len(ds)
    if n < 2: return None
    m = sum(ds) / n; sd = math.sqrt(sum((d - m) ** 2 for d in ds) / (n - 1)); se = sd / math.sqrt(n)
    if se == 0: return m, m, m, 0.0, n
    tc = t_crit(n - 1); return m, m - tc * se, m + tc * se, t_p(abs(m / se), n - 1), n

def parse(path):
    r = {"total": None, "chain": None, "verify": 0, "nodes": 0, "dbtobi": {}, "entry": {}, "pin": collections.defaultdict(float)}
    st = collections.defaultdict(dict)
    for line in open(path, errors="replace"):
        line = re.sub(r"^ *\d+: ", "", line.rstrip("\n"))
        m = re.match(r"total\s+([\d.]+) s", line)
        if m and r["total"] is None: r["total"] = float(m.group(1))
        m = re.search(r"single-node chain to \d+ limbs \(([\d.]+) s\)", line)
        if m and r["chain"] is None: r["chain"] = float(m.group(1))
        m = re.match(r"mn: all (\d+) nodes: VERIFY OK", line)
        if m: r["verify"] = 1; r["nodes"] = int(m.group(1))
        m = re.match(r"hs_ts pre node (\d+) step (\S+) t_us (\d+)", line)
        if m and m.group(2) not in st[int(m.group(1))]: st[int(m.group(1))][m.group(2)] = int(m.group(3))
        m = re.match(r"bounce_init dev \d+: .* ([\d.]+) s", line)
        if m: r["pin"][path] += float(m.group(1))
    for n, s in st.items():
        if "alloc_host" in s and "db_to_bi_done" in s: r["dbtobi"][n] = (s["db_to_bi_done"] - s["alloc_host"]) / 1e6
        if "h2d_done" in s: r["entry"][n] = s["h2d_done"]
    r["pinsum"] = sum(r["pin"].values())   # summed over the 4 ranks' printed lines (each rank prints its own)
    return r

def main():
    d = sys.argv[1]; stop = "--stop" in sys.argv
    runs = {}
    for f in sorted(glob.glob(os.path.join(d, "*_p*_[AB].log"))):
        m = re.search(r"_p(\d+)_([AB])\.log$", f); runs[(int(m.group(1)), m.group(2))] = (f, parse(f))
    print("pair arm verify total chain db_to_bi[min,max] late_PE(+s) pin_sum(s, all ranks)")
    for (k, a), (f, r) in sorted(runs.items()):
        db = r["dbtobi"]; e = r["entry"]
        late = "-"
        if len(e) > 1: t0 = min(e.values()); n = max(e, key=e.get); late = "node%d+%.3f" % (n, (e[n] - t0) / 1e6)
        dbs = "[%.3f,%.3f]" % (min(db.values()), max(db.values())) if db else "-"
        print("%d %s %s %s %s %s %s %.3f" % (k, a, "OK" if r["verify"] else "NOVERIFY", r["total"], r["chain"], dbs, late, r["pinsum"]))
    res = {}
    for key in ("chain", "total"):
        ds = [runs[(k, "B")][1][key] - runs[(k, "A")][1][key] for k in sorted({k for k, _ in runs})
              if (k, "A") in runs and (k, "B") in runs and runs[(k, "A")][1][key] is not None and runs[(k, "B")][1][key] is not None
              and runs[(k, "A")][1]["verify"] and runs[(k, "B")][1]["verify"]]
        p = paired(ds); res[key] = p
        if p: print("paired B-A %s: n=%d mean %+.3f s, 95%% CI [%+.3f, %+.3f], p=%.4f" % (key, p[4], p[0], p[1], p[2], p[3]))
        else: print("paired B-A %s: n=%d (too few)" % (key, len(ds)))
    if stop:
        c = res.get("total"); ok = bool(c and c[4] >= 4 and c[3] < 0.05)
        print("STOP_RULE", "met" if ok else "not met"); sys.exit(0 if ok else 1)

if __name__ == "__main__": main()

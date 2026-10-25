#!/usr/bin/env python3
"""parse_run.py - read an ecalc run log (one mnrun.sh launch) and gather ladder replicas (target/FORMATS.md section 10).

  parse_run.py parse LOG [--nodes N] [--pernode OUT.tsv]   KEY=VALUE lines on stdout (times s, memory GB)
  parse_run.py collect DIR... [--expect N] [--model-nowrite S] [--model-write S] [--md FILE --label TEXT]
        DIR = ladder replica dirs (results/ladder/<level>/<run>/ with summary.txt): per path mean/min/max/spread of
        the wall (`total`), verify, digit-hash agreement; the table goes to stdout (and is appended to FILE with --md)

What is read (the real ecalc output): `RESULT ecalc <name> <unit> <value>` (init bs 10dP dm T1 dc T2 total vmhwm), the
`mem <phase>` summary rows (device / largest APU / host HWM), `mnrun.sh: plan fit ... FITS` (planned device GB), the
`VERIFY OK|FAILED` lines and `mn: all N nodes: VERIFY ...`, the `digits: first...last` line of every node (hash and
the head of e), `mn: <host> <rank>: dc|T1 <s>` per node (slow-node check), `wrote ... write X s` (writer time).
Pure Python 3.6+, standard library only.
"""
import argparse
import hashlib
import re
import statistics
import sys

E_PREFIX = "27182818284590452353602874713526624977572470936999595749669676"   # first digits of e, a VERIFY OK run's own output

PFX = re.compile(r"^(?:\s*\d+: )?(?:mn: (\S+) (\d+): )?")


def _num(x):
    try:
        return float(x)
    except ValueError:
        return None


def parse(text, nodes=None, hosts=None):
    r = {}
    res = {}
    pernode = {}                       # (host, rank) -> {dc, t1, write}
    dig = set()
    penode = {}                        # PE index -> node index (one process per GPU: the lines say `pe N`, not the host)
    ok_lines = fail_lines = 0
    mn_all = None
    dev_peak = apu_peak = host_hwm = 0.0
    wmax = wafter = 0.0
    first_err = []
    for raw in text.splitlines():
        m = PFX.match(raw)
        host, rank, line = m.group(1), m.group(2), raw[m.end():]
        mm = re.match(r"mn: pe (\d+) of \d+ \(node (\d+) of", raw)
        if mm:
            penode[int(mm.group(1))] = int(mm.group(2))
        mm = re.match(r"RESULT ecalc (\S+) (\S+) (\S+)", line)
        if mm:
            k, v = mm.group(1), _num(mm.group(3))
            if k not in res:
                res[k] = v
            elif k in ("bs", "dm") and v is not None and (res[k] is None or v > res[k]):
                res[k] = v        # per-PE phases: the critical path is the max over PEs (total stays rank 0's)
            continue
        mm = re.match(r"mnrun\.sh: plan fit +FITS: .*?device ([0-9.]+) / ([0-9.]+) GB \(([-+0-9.]+)\), node ([0-9.]+) / ([0-9.]+) GB \(([-+0-9.]+)\)", raw)
        if mm and "plan_device_gb" not in r:
            r["plan_device_gb"], r["plan_device_limit_gb"], r["plan_device_margin_gb"] = mm.group(1), mm.group(2), mm.group(3)
            r["plan_node_gb"], r["plan_node_margin_gb"] = mm.group(4), mm.group(6)
        if re.match(r"mnrun\.sh: .*(REFUSED)", raw) and "refused" not in r:
            r["refused"] = raw[:200]
        if line.startswith("mem ") and not line.startswith("mem summary") and not line.startswith("mem phase"):
            t = line.replace("|", " ").split()
            if len(t) >= 19 and _num(t[2]) is not None:
                dev_peak = max(dev_peak, _num(t[2]))
                apu_peak = max(apu_peak, _num(t[3]))
                host_hwm = max(host_hwm, _num(t[18]) or 0.0)
        if line.startswith("VERIFY OK"):
            ok_lines += 1
        elif line.startswith("VERIFY FAILED"):
            fail_lines += 1
        mm = re.match(r"mn: all (\d+) (?:nodes|PEs): VERIFY (OK|FAILED)", line)
        if mm:
            mn_all = mm.group(2)
        mm = re.match(r"digits: (\S*?)\.\.\.(\S*)", line)
        if mm:
            dig.add("%s %s...%s" % (rank if rank is not None else "0", mm.group(1), mm.group(2)))
        if rank is not None:
            mm = re.match(r"(dc|T1)\s+([0-9.]+) s", line)
            if mm:
                pernode.setdefault((host, int(rank)), {})[mm.group(1)] = float(mm.group(2))
        mm = re.search(r"wrote \S+ .*?write ([0-9.]+)(?: \+ ([0-9.]+))? s in the writer thread, ([0-9.]+) s after", line)
        if mm:
            wmax = max(wmax, float(mm.group(1)), float(mm.group(2) or 0))
            wafter = max(wafter, float(mm.group(3)))
        if re.search(r"FATAL|Segmentation|SIGSEGV|SIGBUS|SIGABRT|out of memory|REFUSED", line) and len(first_err) < 2:
            first_err.append(line[:160])
    for k, key in (("total", "total_s"), ("init", "init_s"), ("bs", "bs_s"), ("dm", "dm_s"), ("T1", "t1_s"), ("dc", "dc_s"),
                   ("T2", "t2_s"), ("phases", "phases_s"), ("other", "other_s"), ("vmhwm", "vmhwm_gb"), ("10dP", "dp10_s")):
        if k in res and res[k] is not None:
            r[key] = "%.2f" % res[k]
    tot = res.get("total")
    if tot:
        for k in ("bs", "dm", "init"):
            if res.get(k) is not None:
                r[k + "_share"] = "%.3f" % (res[k] / tot)
    if dev_peak:
        r["dev_peak_gb"], r["dev_peak_apu_gb"], r["host_hwm_gb"] = "%.1f" % dev_peak, "%.1f" % apu_peak, "%.1f" % host_hwm
    if wmax or wafter:
        r["write_max_s"], r["write_after_checks_max_s"] = "%.2f" % wmax, "%.2f" % wafter
    # verify: OK only when nothing failed and the all-node line (multi) or at least one OK line (one node) says OK
    if fail_lines or mn_all == "FAILED":
        v = "FAIL"
    elif mn_all == "OK" or (nodes in (None, 1) and ok_lines >= 1 and mn_all is None):
        v = "OK"
    else:
        v = "NONE"
    r["verify"], r["verify_ok_lines"] = v, str(ok_lines)
    if dig:
        r["digits_hash"] = hashlib.sha256("\n".join(sorted(dig)).encode()).hexdigest()[:16]
        r["digits_lines"] = str(len(dig))
        heads = [d.split(" ", 1)[1].split("...")[0] for d in dig if d.split(" ", 1)[1].startswith("2718")]
        if heads:
            h = heads[0]
            n = min(len(h), len(E_PREFIX))
            r["e_head"] = h[:24]
            r["e_prefix_check"] = ("PASS" if h[:n] == E_PREFIX[:n] else "FAIL") + " (%d digits)" % n
        else:
            r["e_prefix_check"] = "NONE (no node line starts at digit 1)"
    if pernode:
        agg = {}
        for (h, rk), d in pernode.items():
            nk = (hosts[penode.get(rk, -1)] if hosts and 0 <= penode.get(rk, -1) < len(hosts) else "node%d" % penode.get(rk, -1)) if h == "pe" else h
            a = agg.setdefault((nk, 0), {})
            for k2, v2 in d.items():
                a[k2] = max(a.get(k2, 0.0), v2)
        pernode = agg
        for key, nm in (("dc", "dc"), ("T1", "t1")):
            vals = sorted((d[key], h, rk) for (h, rk), d in pernode.items() if key in d)
            if vals:
                med = statistics.median([v[0] for v in vals])
                r[nm + "_node_median_s"] = "%.2f" % med
                r[nm + "_node_max_s"] = "%.2f at %s" % (vals[-1][0], vals[-1][1])
        slow = sorted(set(h for (h, rk), d in pernode.items() if "dc" in d and
                          d["dc"] > 1.3 * statistics.median([x["dc"] for x in pernode.values() if "dc" in x]) and
                          d["dc"] > 5 + statistics.median([x["dc"] for x in pernode.values() if "dc" in x])))
        r["slow_hosts"] = ",".join(slow) if slow else "none"
        r["nodes_reporting"] = str(len(set(h for h, _ in pernode)))
    if first_err:
        r["first_error"] = " / ".join(first_err)
    r["_pernode"] = pernode
    return r


def write_pernode(path, pernode):
    with open(path, "w") as f:
        f.write("host\trank\tdc_s\tT1_s\n")
        for (h, rk), d in sorted(pernode.items(), key=lambda kv: (kv[0][1], kv[0][0])):
            f.write("%s\t%d\t%s\t%s\n" % (h, rk, d.get("dc", ""), d.get("T1", "")))


# ---------------------------------------------------------------------------------------------------- collect
def read_summary(path):
    d = {}
    with open(path) as f:
        for line in f:
            if "=" in line:
                k, v = line.rstrip("\n").split("=", 1)
                d[k] = v
    return d


def collect(dirs, model_nw=None, model_w=None, expect=None):
    rows = {}                                   # path -> list of (dir, runtag, dict)
    meta = {}
    for d in dirs:
        try:
            s = read_summary(d.rstrip("/") + "/summary.txt")
        except OSError:
            continue
        for k, v in s.items():
            m = re.match(r"(p\d)r(\d+)\.(\w+)$", k)
            if m:
                rows.setdefault(m.group(1), {}).setdefault((d, m.group(2)), {})[m.group(3)] = v
        meta.setdefault("hosts", []).append(s.get("hosts", ""))
        meta["level"] = s.get("level", meta.get("level", "?"))
        meta["nodes"] = s.get("nodes", "?")
        meta["digits"] = s.get("digits", "?")
    out = []
    allok = True
    heads = set()
    allhash = set()
    for p in sorted(rows):
        runs = rows[p]
        tot = [float(v["total_s"]) for v in runs.values() if "total_s" in v and v.get("verify") == "OK"]
        ver = [v.get("verify", "NONE") for v in runs.values()]
        hashes = set(v["digits_hash"] for v in runs.values() if "digits_hash" in v)
        for v in runs.values():
            if "e_head" in v:
                heads.add(v["e_head"])
        allhash |= hashes
        nok = sum(1 for x in ver if x == "OK")
        nbadrc = sum(1 for v in runs.values() if v.get("rc", "0") != "0")
        allok = allok and nok == len(ver) and len(ver) > 0 and nbadrc == 0
        line = "path %s: runs %d, VERIFY OK %d" % (p[1], len(ver), nok) + (", rc != 0 in %d" % nbadrc if nbadrc else "")
        if tot:
            mean = sum(tot) / len(tot)
            line += ", total mean %.1f min %.1f max %.1f s, spread %.1f %%" % (mean, min(tot), max(tot), 100.0 * (max(tot) - min(tot)) / mean)
            for lab, mv in (("no-write", model_nw), ("write", model_w)):
                if mv:
                    line += ", %s model %.1f s (mod): measured/model %.2f" % (lab, mv, mean / mv)
        line += ", digit hash %s" % ("agree (%s)" % next(iter(hashes)) if len(hashes) == 1 else ("none" if not hashes else "DIFFER %s" % ",".join(sorted(hashes))))
        extras = []
        for key in ("init_s", "bs_s", "dm_s", "dc_s", "dev_peak_gb", "host_hwm_gb"):
            xs = [float(v[key]) for v in runs.values() if key in v]
            if xs:
                extras.append("%s %.1f" % (key, sum(xs) / len(xs)))
        if extras:
            line += "; mean " + ", ".join(extras)
        slow = sorted(set(h for v in runs.values() for h in v.get("slow_hosts", "none").split(",") if h not in ("none", "")))
        if slow:
            line += "; slow hosts " + ",".join(slow)
        out.append(line)
    if len(allhash) != 1:                       # one digit string across all runs of all paths
        out.append("digit hash across paths: %s" % ("none (no digit lines)" if not allhash else "DIFFER %s" % ",".join(sorted(allhash))))
        allok = False
    if expect and len(dirs) < expect:
        out.append("replicas finished %d of %d" % (len(dirs), expect))
        allok = False
    if len(heads) > 1:
        out.append("e head DIFFERS across runs: %s" % ",".join(sorted(heads)))
        allok = False
    out.append("level %s: %s nodes, %s digits, %d replica dirs, overall %s" % (
        meta.get("level", "?"), meta.get("nodes", "?"), meta.get("digits", "?"), len(dirs), "PASS" if allok else "NOT PASS"))
    return out, allok


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd")
    sub.required = True
    s = sub.add_parser("parse")
    s.add_argument("log")
    s.add_argument("--nodes", type=int)
    s.add_argument("--pernode")
    s.add_argument("--hosts", help="comma list of the run's hosts in node order (names the slow node of a one-process-per-GPU run)")
    s = sub.add_parser("collect")
    s.add_argument("dirs", nargs="+")
    s.add_argument("--model-nowrite", type=float)
    s.add_argument("--model-write", type=float)
    s.add_argument("--expect", type=int, help="replica dirs of the tag (finished or not): fewer finished = NOT PASS")
    s.add_argument("--md")
    s.add_argument("--label", default="")
    a = p.parse_args(argv)
    if a.cmd == "parse":
        with open(a.log, errors="replace") as f:
            r = parse(f.read(), a.nodes, a.hosts.split(",") if a.hosts else None)
        pn = r.pop("_pernode")
        if a.pernode and pn:
            write_pernode(a.pernode, pn)
        for k, v in r.items():
            print("%s=%s" % (k, v))
        return 0
    lines, ok = collect(a.dirs, a.model_nowrite, a.model_write, a.expect)
    print("\n".join(lines))
    if a.md:
        with open(a.md, "a") as f:
            f.write("\n### %s\n\n" % a.label + "\n".join("- " + l for l in lines) + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

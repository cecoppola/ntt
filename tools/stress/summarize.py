#!/usr/bin/env python3
"""tools/stress/summarize.py <outdir>: the arms table of a stress.sh output directory (runs.tsv) as markdown.
Columns per arm: runs, clean runs, anomalies (rc != 0 / no VERIFY OK), CRT flags, P24 flag count, VERIFY failures, digit differences, remaps (all, forced), mean wall."""
import csv, os, sys

def main(d):
    p = os.path.join(d, "runs.tsv")
    if not os.path.exists(p):
        print("no runs.tsv"); return 1
    rows = list(csv.DictReader(open(p), delimiter="\t"))
    arms = {}
    for r in rows:
        a = arms.setdefault(r["arm"], dict(runs=0, ok=0, rc=0, crt=0, p24=0, vf=0, dd=0, rem=0, frc=0, wall=[]))
        a["runs"] += 1
        a["rc"] += r["rc"] != "0"
        a["vf"] += r["verify"] != "OK" and r["rc"] == "0"
        a["crt"] += r["crtflag"] != "0"
        a["p24"] += int(r["p24flags"])
        a["dd"] += r["digits"] == "DIFFERS"
        a["rem"] += int(r["remaps"]); a["frc"] += int(r["forced"])
        a["wall"].append(float(r["wall_s"]))
        a["ok"] += (r["rc"] == "0" and r["verify"] == "OK" and r["crtflag"] == "0" and r["p24flags"] == "0" and r["digits"] in ("identical", "nofile"))
    print("| arm | runs | clean | rc!=0 | VERIFY missing/failed | CRT flags | P24 flags | digit diffs | remaps (forced) | mean wall s |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    T = dict(runs=0, ok=0, rc=0, crt=0, p24=0, vf=0, dd=0, rem=0, frc=0)
    for k in sorted(arms):
        a = arms[k]
        print("| %s | %d | %d | %d | %d | %d | %d | %d | %d (%d) | %.1f |" % (k, a["runs"], a["ok"], a["rc"], a["vf"], a["crt"], a["p24"], a["dd"], a["rem"], a["frc"], sum(a["wall"]) / len(a["wall"])))
        for f in T: T[f] += a[f]
    print("| total | %d | %d | %d | %d | %d | %d | %d | %d (%d) | |" % (T["runs"], T["ok"], T["rc"], T["vf"], T["crt"], T["p24"], T["dd"], T["rem"], T["frc"]))
    kp = os.path.join(d, "keep")
    if os.path.isdir(kp) and os.listdir(kp):
        print("\nlogs kept (anomalies): " + ", ".join(sorted(os.listdir(kp))))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1]) if len(sys.argv) > 1 else 2)

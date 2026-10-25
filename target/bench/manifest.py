#!/usr/bin/env python3
"""manifest.py - manifest reader of the bench runner (contract: target/FORMATS.md §4, §5).

Used by run.sh (pick / validate / status / plan) and aggregate.py (load, state).
  manifest.py validate
  manifest.py pick --nhosts N --remaining MIN|- --max-min M [--only a,b] [--tier T] [--assume-done a,b]
        stdout: eligible ids, lowest prio first.  exit 0 = some eligible, 3 = none now but some item fits and waits for deps of
        other workers, 4 = nothing left that fits.  stderr: one line saying what is left and why.
  manifest.py get ID COLUMN
  manifest.py status
  manifest.py plan N [--max-nodes K]
Environment overrides (self-test): TGT_BENCH_DIR (holds manifest.tsv, manifest.d/, items/), TGT_RES_DIR (results root).
"""
import itertools, os, re, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.environ.get("TGT_BENCH_DIR", os.path.join(ROOT, "bench"))
RES = os.environ.get("TGT_RES_DIR", os.path.join(ROOT, "results"))
RB = os.path.join(RES, "bench")
COLS = "id tier prio nodes est_min deps questions cmd".split()
AREAS = "sys bld mem gpu ntt net io ini scl prm cal cor".split()
OK = ("PASS", "PARTIAL")


def files():
    fs = []
    m = os.path.join(BENCH, "manifest.tsv")
    if os.path.exists(m):
        fs.append(m)
    d = os.path.join(BENCH, "manifest.d")
    if os.path.isdir(d):
        fs += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith(".tsv")]
    return fs


def load(errors=None):
    """-> list of dicts in file order (ids may repeat only when errors is a list, which then collects the problems)."""
    items, err = [], errors if errors is not None else []
    for f in files():
        hdr = False
        for n, line in enumerate(open(f), 1):
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            c = line.split("\t")
            if not hdr:
                hdr = True
                if c[:8] != COLS:
                    err.append("%s:%d: header must be %s" % (f, n, "\t".join(COLS)))
                continue
            where = "%s:%d" % (os.path.basename(f), n)
            if len(c) != 8:
                err.append("%s: %d columns, need 8" % (where, len(c)))
                continue
            it = dict(zip(COLS, [x.strip() for x in c]))
            it["where"] = where
            try:
                it["tier"], it["prio"], it["nodes"] = int(it["tier"]), int(it["prio"]), int(it["nodes"])
                it["est_min"] = float(it["est_min"])
            except ValueError:
                err.append("%s: tier/prio/nodes/est_min must be numbers" % where)
                continue
            it["deps_l"] = [] if it["deps"] in ("-", "") else it["deps"].split(",")
            it["q_l"] = [] if it["questions"] in ("-", "") else it["questions"].split(",")
            items.append(it)
    if errors is None and err:
        sys.exit("manifest: " + "; ".join(err))
    return items


def validate():
    err = []
    items = load(err)
    ids = {}
    for it in items:
        if it["id"] in ids:
            err.append("%s: duplicate id %s (also %s)" % (it["where"], it["id"], ids[it["id"]]["where"]))
        ids[it["id"]] = it
        if not re.fullmatch(r"[a-z]+[0-9]+", it["id"]) or not re.match("|".join(AREAS), it["id"]):
            err.append("%s: id %s is not <area><nn> (areas %s)" % (it["where"], it["id"], ",".join(AREAS)))
        if it["tier"] not in (1, 2):
            err.append("%s: tier must be 1 or 2" % it["where"])
        if it["nodes"] < 0 or it["est_min"] <= 0:
            err.append("%s: nodes >= 0 and est_min > 0 required" % it["where"])
        prog = it["cmd"].split()[0] if it["cmd"] else ""
        if not prog or not os.path.isfile(os.path.join(BENCH, "items", prog)):
            err.append("%s: item script items/%s missing" % (it["where"], prog))
        if not it["q_l"] and it["questions"] != "-":
            err.append("%s: questions column empty (use -)" % it["where"])
    for it in items:
        for d in it["deps_l"]:
            if d not in ids:
                err.append("%s: %s depends on unknown id %s" % (it["where"], it["id"], d))
            elif d == it["id"]:
                err.append("%s: %s depends on itself" % (it["where"], it["id"]))
    # cycles
    seen = {}
    def visit(i, path):
        if seen.get(i) == 2:
            return
        if seen.get(i) == 1:
            err.append("dependency cycle: " + " -> ".join(path + [i]))
            return
        seen[i] = 1
        for d in ids[i]["deps_l"]:
            if d in ids:
                visit(d, path + [i])
        seen[i] = 2
    for i in ids:
        visit(i, [])
    return items, err


def state(i, est_min=0.0):
    """-> dict(state=unclaimed|claimed|done, status, worker, wall_s, stale). done: status = last status= of summary.txt."""
    d = os.path.join(RB, i)
    r = {"state": "unclaimed", "status": "", "worker": "", "wall_s": "", "stale": False}
    if os.path.isfile(os.path.join(d, "DONE")):
        r["state"] = "done"
        r["status"] = "FAIL"
        sf = os.path.join(d, "summary.txt")
        if os.path.isfile(sf):
            for l in open(sf, errors="replace"):
                if l.startswith("status="):
                    r["status"] = l.split("=", 1)[1].strip()
                elif l.startswith("wall_s="):
                    r["wall_s"] = l.split("=", 1)[1].strip()
    if os.path.exists(os.path.join(d, "claim.lock")):
        who = os.path.join(d, "claim.d", "who")
        try:
            w = open(who).read().split()
            r["worker"] = w[4] if len(w) > 4 else (w[0] if w else "")
        except OSError:
            pass
        age = time.time() - os.path.getmtime(os.path.join(d, "claim.lock"))
        if r["state"] != "done":
            r["state"] = "claimed"
            r["wall_s"] = str(int(age))
            r["stale"] = age > (2 * est_min + 10) * 60
    return r


def is_ok(i, assume, st):
    return i in assume or (st[i]["state"] == "done" and st[i]["status"] in OK)


def pick(args):
    nh, rem, maxm = args["nhosts"], args["remaining"], args["max_min"]
    only, tier, assume = args["only"], args["tier"], args["assume_done"]
    items = load()
    byid = {i["id"]: i for i in items}
    st = {i["id"]: state(i["id"], i["est_min"]) for i in items}
    elig, waiting, left, why = [], 0, 0, {}
    for it in sorted(items, key=lambda x: (x["prio"], x["id"])):
        i = it["id"]
        if i in assume or st[i]["state"] != "unclaimed":
            continue
        if only and i not in only:
            continue
        if tier and it["tier"] != tier:
            continue
        left += 1
        if it["nodes"] > nh:
            why[i] = "needs %d nodes (have %d)" % (it["nodes"], nh)
            continue
        if rem is not None and it["est_min"] > rem:
            why[i] = "est %g min > %g min left in the allocation" % (it["est_min"], rem)
            continue
        if maxm is not None and it["est_min"] > maxm:
            why[i] = "est %g min > --max-min %g" % (it["est_min"], maxm)
            continue
        bad = [d for d in it["deps_l"] if not is_ok(d, assume, st)]
        if bad:
            # waits only if every blocking dep is claimed (running elsewhere) or itself waiting on such; failed deps block for good
            dead = [d for d in bad if (st[d]["state"] == "done" and st[d]["status"] not in OK)]
            if dead:
                why[i] = "dep %s ended %s" % (dead[0], st[dead[0]]["status"])
            elif any(st[d]["state"] == "claimed" for d in bad):
                waiting += 1
                why[i] = "waits for " + ",".join(bad)
            else:
                why[i] = "waits for unclaimed " + ",".join(bad)
            continue
        elig.append(i)
    # unclaimed deps chain: an item whose dep is unclaimed but fits this worker is not "waiting for others": we will run the dep
    if elig:
        print("\n".join(elig))
        return 0
    msg = "%d item(s) left: " % left + "; ".join("%s %s" % (k, v) for k, v in list(why.items())[:12])
    sys.stderr.write(msg + ("" if len(why) <= 12 else " ...") + "\n")
    return 3 if waiting else 4


def cmd_status():
    items, err = validate()
    rows = [("id", "prio", "nodes", "est", "state", "worker", "wall_s")]
    cnt = {}
    for it in sorted(items, key=lambda x: (x["prio"], x["id"])):
        s = state(it["id"], it["est_min"])
        label = s["state"] if s["state"] != "done" else "done-" + (s["status"] or "?")
        if s["stale"]:
            label += "(STALE)"
        k = label.split("(")[0]
        cnt[k] = cnt.get(k, 0) + 1
        rows.append((it["id"], str(it["prio"]), str(it["nodes"]), "%g" % it["est_min"], label, s["worker"], s["wall_s"]))
    w = [max(len(r[c]) for r in rows) for c in range(7)]
    for r in rows:
        print("  ".join(r[c].ljust(w[c]) for c in range(7)).rstrip())
    print("total %d: %s" % (len(items), ", ".join("%s %d" % kv for kv in sorted(cnt.items()))))
    for e in err:
        print("MANIFEST PROBLEM:", e)


def simulate(items, sizes, st):
    """list scheduling of the not-yet-done items on workers of the given allocation sizes -> (makespan, worker sequences, left)."""
    fin = {}
    pend = {}
    for it in items:
        s = st[it["id"]]
        if s["state"] == "done":
            fin[it["id"]] = 0.0 if s["status"] in OK else None
        else:
            pend[it["id"]] = it
    free = [0.0] * len(sizes)
    seq = [[] for _ in sizes]
    alive = list(range(len(sizes)))
    while pend and alive:
        w = min(alive, key=lambda k: (free[k], k))
        t = free[w]
        best = None
        for i, it in sorted(pend.items(), key=lambda x: (x[1]["prio"], x[0])):
            if it["nodes"] > sizes[w] or t + it["est_min"] > 1e9:
                continue
            if all(fin.get(d) is not None and d in fin and fin[d] <= t for d in it["deps_l"]):
                best = i
                break
        if best:
            it = pend.pop(best)
            fin[best] = t + it["est_min"]
            free[w] = fin[best]
            seq[w].append((best, t))
        else:
            nxt = [v for v in fin.values() if v is not None and v > t]
            if nxt:
                free[w] = min(nxt)
            else:
                alive.remove(w)
    span = max([f for f in fin.values() if f is not None] + [0.0])
    return span, seq, sorted(pend)


def cmd_plan(n, maxn):
    items, err = validate()
    if err:
        sys.exit("manifest problems: " + "; ".join(err))
    st = {i["id"]: state(i["id"], i["est_min"]) for i in items}
    todo = [i for i in items if st[i["id"]]["state"] != "done"]
    use = [i for i in items if maxn is None or i["nodes"] <= maxn]
    sizes_avail = sorted({i["nodes"] for i in todo if i["nodes"] > 0 and (maxn is None or i["nodes"] <= maxn)}) or [1]
    best = None
    for combo in itertools.combinations_with_replacement(sizes_avail, n):
        if max(combo) < max([i["nodes"] for i in todo if maxn is None or i["nodes"] <= maxn] + [0]):
            continue
        span, seq, left = simulate(use, list(combo), st)
        key = (len(left), span, sum(combo))
        if best is None or key < best[0]:
            best = (key, combo, span, seq, left)
    if not best:
        sys.exit("no feasible worker set")
    _, combo, span, seq, left = best
    tot = sum(i["est_min"] for i in todo)
    print("plan: %d workers, allocation sizes (nodes) %s; estimated wall to drain: %.0f min (sum of est_min %.0f, node-min %.0f)" % (
        n, ",".join(map(str, sorted(combo, reverse=True))), span, tot, sum(i["est_min"] * i["nodes"] for i in todo)))
    order = sorted(range(n), key=lambda k: -combo[k])
    for k in order:
        busy = sum(next(i["est_min"] for i in items if i["id"] == s) for s, _ in seq[k])
        print("  worker %d: %d-node allocation, %d items, busy %.0f min: %s" % (
            k + 1, combo[k], len(seq[k]), busy, " ".join("%s@%.0f" % (s, t) for s, t in seq[k][:40])))
    if left:
        print("  not placed: " + ",".join(left))
    print("  allocation length to ask: ~%.0f min each (the runner skips items that no longer fit its remaining time)" % (span + 10))
    print("  (list-scheduling estimate from est_min; the real run differs by queue waits and by items that fail)")


def main(a):
    if not a:
        sys.exit(__doc__)
    c = a[0]
    if c == "validate":
        items, err = validate()
        for e in err:
            print("ERROR", e)
        print("manifest: %d items, %d problems" % (len(items), len(err)))
        return 1 if err else 0
    if c == "pick":
        o = {"nhosts": 0, "remaining": None, "max_min": None, "only": set(), "tier": 0, "assume_done": set()}
        k = 1
        while k < len(a):
            f, v = a[k], a[k + 1]
            k += 2
            if f == "--nhosts": o["nhosts"] = int(v)
            elif f == "--remaining": o["remaining"] = None if v == "-" else float(v)
            elif f == "--max-min": o["max_min"] = None if v == "-" else float(v)
            elif f == "--only": o["only"] = set(v.split(","))
            elif f == "--tier": o["tier"] = int(v)
            elif f == "--assume-done": o["assume_done"] = set(x for x in v.split(",") if x)
        return pick(o)
    if c == "get":
        for it in load():
            if it["id"] == a[1]:
                print(it[a[2]])
                return 0
        return 1
    if c == "status":
        cmd_status()
        return 0
    if c == "plan":
        mn = int(a[a.index("--max-nodes") + 1]) if "--max-nodes" in a else None
        cmd_plan(int(a[1]), mn)
        return 0
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

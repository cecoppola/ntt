#!/usr/bin/env python3
"""tools/plan_values.py -- the size-dependent launch values of a run, computed in one place (TASKS 27 part A, results/T27.md).

  tools/plan_values.py [--profile NAME|PATH] [--line FILE] [--ecalc PATH] [--exports] DIGITS NODES

What it computes, from (digits, node-processes, machine profile + the line's settings), with nothing typed:
  - the SHMEM pool COMM_SHMEM_POOL_MB, the library heap (SHMEM_SYMMETRIC_SIZE / XT_SYMMETRIC_HEAP_SIZE or
    SHMEM_SYMMETRIC_HEAP_SIZE, by implementation) and FI_UNIVERSE_SIZE: ecalc/mnrun.sh with MNRUN_PRINT_ENV=1 (mnrun.sh asks the
    planner, `MN_PLAN_ONLY=<digits>:<nodes>`, for the pool and adds 512 MiB for the heap -- the rule every launch through mnrun.sh
    already uses);
  - the tree schedule MN_GROUPS, the plane pools' pool_log and the plan's verdict: the planner's own header and `plan check` line.
Nothing is launched: the planner touches no device (a login node is enough; the binary must be the SHMEM build of the run).

  --profile P   load profiles/P.conf first (tools/profile_load.py: explicit environment > profile > code default)
  --line FILE   a hand-typed launch line (K=V or `export K=V ...` lines, `#` comments, `unset K ...`; other lines ignored): its
                settings are applied over the profile (they shape the plan), its size-dependent values are removed, computed
                again and compared: MATCH / DIFF per key, NO-CONSUMER for a key nothing reads.  Exit 1 on any DIFF.
  --ecalc PATH  the binary (default ecalc/ecalc of this tree)
  --exports     print only `export K=V` lines of the computed launcher values (for an srun launch without mnrun.sh:
                eval "$(tools/plan_values.py --profile target --exports 38200000000000 576)")
Exit: 0 values computed (and every compared key matches), 1 a DIFF or `plan REFUSED`, 2 usage or planner failure.
"""
import os, re, sys, shlex, subprocess, decimal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import profile_load as PL                                           # registry() and script_words(): who reads a key

# the values a launch line used to type per size; plan_values computes them (MN_GROUPS: the code's default schedule)
LAUNCH_KEYS = ["COMM_SHMEM_POOL_MB", "SHMEM_SYMMETRIC_SIZE", "XT_SYMMETRIC_HEAP_SIZE", "SHMEM_SYMMETRIC_HEAP_SIZE", "FI_UNIVERSE_SIZE"]
PLAN_KEYS = ["MN_GROUPS"]
COMPUTED = LAUNCH_KEYS + PLAN_KEYS
PRE = '[ -n "${MNRUN_UNLOAD:-}" ] && module unload $MNRUN_UNLOAD > /dev/null 2>&1; module load $MNRUN_MODULES > /dev/null 2>&1; exec "$@"'


def die(msg, rc=2):
    print("plan_values: " + msg, file=sys.stderr)
    sys.exit(rc)


def digits_int(s):
    try:
        v = decimal.Decimal(s)
    except decimal.InvalidOperation:
        die("digits must be a number (e.g. 3.82e13): %s" % s)
    if v != v.to_integral_value() or v < 1:
        die("digits must be a positive integer: %s" % s)
    return int(v)


def read_line_file(path):
    """-> (ordered list of (K, V), set of unset keys); K=V words of `export` / bare assignment lines only"""
    sets, unsets = [], set()
    for raw in open(os.path.expanduser(path), errors="replace"):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        try:
            w = shlex.split(line)
        except ValueError:
            continue
        if w[0] == "unset":
            unsets.update(w[1:]); continue
        if w[0] == "export":
            w = w[1:]
        if not w or not re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", w[0]):
            continue                                                # srun / bash lines of a launch block
        for t in w:
            m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$", t)
            if m:
                sets.append((m.group(1), m.group(2)))
    return sets, unsets


def profile_env(prof, env):
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "profile_load.py"), prof], env=env, capture_output=True, text=True)
    if r.returncode:
        die("profile %s: %s" % (prof, (r.stderr or r.stdout).strip()[:300]))
    r2 = subprocess.run(["bash", "-c", r.stdout + "\nenv -0"], env=env, capture_output=True, text=True)
    return dict(kv.split("=", 1) for kv in r2.stdout.split("\0") if "=" in kv)


def parse_exports(txt):
    out = {}
    for line in txt.splitlines():
        if line.startswith("export "):
            k, v = line[7:].split("=", 1)
            out[k] = shlex.split(v)[0] if v else ""
    return out


def mib(v):
    """heap / pool sizes compared in MiB: 2048M, 2G, 2048 (MiB)"""
    m = re.fullmatch(r"\s*([0-9]+)\s*([KkMmGg]?)[Bb]?\s*", v or "")
    if not m:
        return None
    n, u = int(m.group(1)), m.group(2).upper()
    return n // 1024 if u == "K" else n * 1024 if u == "G" else n


def main(argv):
    a = {"profile": None, "line": None, "ecalc": os.path.join(ROOT, "ecalc", "ecalc"), "exports": False}
    pos = []
    i = 0
    while i < len(argv):
        x = argv[i]
        if x in ("--profile", "--line", "--ecalc"):
            if i + 1 >= len(argv):
                die("%s needs a value" % x)
            a[x[2:]] = argv[i + 1]; i += 2; continue
        if x == "--exports":
            a["exports"] = True
        elif x in ("-h", "--help"):
            print(__doc__); return 0
        else:
            pos.append(x)
        i += 1
    if len(pos) != 2:
        die("usage: plan_values.py [--profile P] [--line FILE] [--ecalc PATH] [--exports] DIGITS NODES")
    d, g = digits_int(pos[0]), int(pos[1])
    ecalc = os.path.abspath(os.path.expanduser(a["ecalc"]))
    if not os.access(ecalc, os.X_OK):
        die("no executable %s (build ecalc first; the SHMEM build for more than one node)" % ecalc)

    env = dict(os.environ)
    caller = {k: env[k] for k in COMPUTED if k in env}
    for k in COMPUTED:                                              # computed here: an inherited value would be taken as typed
        env.pop(k, None)
    hand, unsets = ({}, set())
    if a["line"]:
        sets, unsets = read_line_file(a["line"])
        for k, v in sets:
            if k in COMPUTED:
                hand[k] = v
            else:
                env[k] = v                                          # the line's settings shape the plan (explicit > profile)
        for k in unsets:
            env.pop(k, None)
    if a["profile"]:
        env = profile_env(a["profile"], env)
        for k in COMPUTED:
            env.pop(k, None)

    # 1. the launcher values: mnrun.sh's own computation, printed instead of launched
    cmd = [os.path.join(ROOT, "ecalc", "mnrun.sh"), str(g), ecalc, str(d)]
    r = subprocess.run(cmd, env=dict(env, MNRUN_PRINT_ENV="1"), capture_output=True, text=True)
    if r.returncode:
        die("mnrun.sh MNRUN_PRINT_ENV failed (rc %d): %s" % (r.returncode, r.stderr.strip()[-300:]))
    ex = parse_exports(r.stdout)
    note = [l for l in r.stderr.splitlines() if "COMM_SHMEM_POOL_MB" in l and "MN_PLAN_ONLY" in l]
    if g > 1 and not note:
        die("mnrun.sh did not size the pool from the plan (is %s the SHMEM build?): %s" % (ecalc, r.stderr.strip()[-300:]))

    # 2. the plan itself: schedule, pool_log, verdict (same environment, same modules as mnrun.sh's plan call)
    penv = dict(env)
    penv.update({k: v for k, v in ex.items() if k in ("COMM_TRANSPORT", "COMM_SHMEM_POOL_MB", "COMM_OFI_PLAN_CXI", "MNRUN_MODULES", "MNRUN_UNLOAD")})
    if g > 1:
        penv["COMM_TRANSPORT"] = "shmem"
    penv["MN_PLAN_ONLY"] = "%d:%d" % (d, g)
    r = subprocess.run(["bash", "-lc", PRE, "_", "env", ecalc], env=penv, capture_output=True, text=True)
    out = r.stdout
    head = re.search(r"^== MN_PLAN_ONLY: .*?pool_log (\d+),.*?MN_GROUPS (.*?) ==$", out, re.M)
    chk = [l for l in out.splitlines() if l.startswith("plan check")]
    if not head or not chk:
        die("the planner gave no plan (rc %d): %s" % (r.returncode, (r.stderr or out).strip()[-300:]))
    groups = head.group(2).strip()
    groups = "-" if groups.startswith("-") else groups
    verdict = chk[-1]
    comp = {k: ex[k] for k in LAUNCH_KEYS if k in ex}
    comp["MN_GROUPS"] = groups

    if a["exports"]:
        print("# tools/plan_values.py: %d digits on %d node-process%s%s; %s" % (d, g, "es" if g > 1 else "", " (profile %s)" % a["profile"] if a["profile"] else "", verdict))
        for k, v in ex.items():
            if k not in ("MNRUN_MODULES", "MNRUN_UNLOAD", "COMM_OFI_PLAN_CXI"):
                print("export %s=%s" % (k, shlex.quote(v)))
        print("# MN_GROUPS: the code's default schedule (%s) -- nothing to export" % groups)
        return 1 if "REFUSED" in verdict else 0

    print("plan values: %d digits on %d node-process%s%s%s" % (d, g, "es" if g > 1 else "", "; profile " + a["profile"] if a["profile"] else "",
                                                             "; line " + a["line"] if a["line"] else ""))
    print("  %-28s %-26s %-26s %s" % ("key", "computed", "line" if a["line"] else "", "verdict" if a["line"] else ""))
    diffs = 0
    reg, words = PL.registry(), None
    for k in COMPUTED:
        c = comp.get(k)
        h = hand.get(k)
        if c is None and h is None:
            continue
        v = ""
        if a["line"]:
            if h is None:
                v = "computed (not on the line)"
            elif c is None and k in LAUNCH_KEYS[1:4]:
                v = "EXTRA (a heap name this SHMEM implementation's launch does not set)"
            elif c is None:
                v = "DIFF (the line sets it; this launch computes none)"; diffs += 1
            elif k == "MN_GROUPS":
                v = "MATCH" if h.replace(" ", "") == c.replace(" ", "") else "DIFF"
                diffs += v == "DIFF"
            else:
                v = "MATCH" if mib(h) is not None and mib(h) == mib(c) else "DIFF"
                diffs += v == "DIFF"
        print("  %-28s %-26s %-26s %s" % (k, c if c is not None else "-", h if h is not None else "", v))
    if a["line"]:
        for k, val in read_line_file(a["line"])[0]:
            if k in COMPUTED or k in reg or PL.EXTERNAL.match(k):
                continue
            words = words if words is not None else PL.script_words()
            if k not in words:
                print("  %-28s %-26s %-26s %s" % (k, "-", val, "NO-CONSUMER (no code or script reads it: drop it from the line)"))
    print("  pool_log %s; %s" % (head.group(1), verdict))
    for l in note:
        print("  " + l.strip())
    if caller:
        print("  note: the caller's environment set %s; ignored here (computed), a launch would take the typed value" % ",".join(sorted(caller)))
    return 1 if diffs or "REFUSED" in verdict else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

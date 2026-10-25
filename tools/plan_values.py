#!/usr/bin/env python3
"""tools/plan_values.py -- the size-dependent launch values of a run, computed in one place (TASKS 27 part A, results/TOOLS_PROFILES.md §T27).

  tools/plan_values.py [--profile NAME|PATH] [--env "K=V ..."] [--ecalc PATH] [--exports | --fit] DIGITS NODES

What it computes, from (digits, node-processes, machine profile + the line's settings), with nothing typed:
  - the pool COMM_SHMEM_POOL_MB (the planner's pool figure, also the XSHMEM_HEAP_MB sizing), the library heap (SHMEM_SYMMETRIC_SIZE = XT_SYMMETRIC_HEAP_SIZE, Cray) and FI_UNIVERSE_SIZE: ecalc/mnrun.sh with MNRUN_PRINT_ENV=1 (mnrun.sh asks the
    planner, `MN_PLAN_ONLY=<digits>:<nodes>`, for the pool and adds 512 MiB for the heap -- the rule every launch through mnrun.sh
    already uses);
  - the tree schedule MN_GROUPS, the plane pools' pool_log and the plan's verdict: the planner's own header and `plan check` line.
Nothing is launched: the planner touches no device (a login node is enough; the binary must be the xshmem build of the run).

  --profile P   load profiles/P.conf first (tools/profile_load.py: explicit environment > profile > code default)
  --env WORDS   K=V words (a launch line) applied before the profile: they shape the plan (explicit > profile > code default)
  --ecalc PATH  the binary (default ecalc/ecalc of this tree)
  --fit         FIT (user decision D26, results/LAUNCH_DEFAULTS.md §FIT): the pre-launch memory-fit report only -- the planner's `plan fit` lines
                (device GB per node and per APU against the device limit ECALC_DEVICE_GB, the node against ECALC_NODE_GB, the margins,
                the largest consumers, the DM_MN_WRAP decision and why).  Exit 0 = fits, 1 = does not fit (or a plan REFUSED): a
                launch is refused.  ecalc/mnrun.sh and tools/gate.sh print the same lines before every launch.
  --exports     print only `export K=V` lines of the computed launcher values (for an srun launch without mnrun.sh:
                eval "$(tools/plan_values.py --profile target --exports 38200000000000 576)", once target/config/target.conf is filled by target/probe/gen_conf.py)
Exit: 0 values computed (and the memory fits), 1 `plan REFUSED` or a memory that does not fit,
2 usage or planner failure.
"""
import os, re, sys, shlex, subprocess, decimal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# the values a launch line used to type per size; plan_values computes them (MN_GROUPS: the code's default schedule)
LAUNCH_KEYS = ["COMM_SHMEM_POOL_MB", "SHMEM_SYMMETRIC_SIZE", "XT_SYMMETRIC_HEAP_SIZE", "FI_UNIVERSE_SIZE"]
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


def main(argv):
    a = {"profile": None, "env": "", "ecalc": os.path.join(ROOT, "ecalc", "ecalc"), "exports": False, "fit": False}
    pos = []
    i = 0
    while i < len(argv):
        x = argv[i]
        if x in ("--profile", "--env", "--ecalc"):
            if i + 1 >= len(argv):
                die("%s needs a value" % x)
            a[x[2:]] = argv[i + 1]; i += 2; continue
        if x == "--exports":
            a["exports"] = True
        elif x == "--fit":
            a["fit"] = True
        elif x in ("-h", "--help"):
            print(__doc__); return 0
        else:
            pos.append(x)
        i += 1
    if len(pos) != 2:
        die("usage: plan_values.py [--profile P] [--env WORDS] [--ecalc PATH] [--exports | --fit] DIGITS NODES")
    d, g = digits_int(pos[0]), int(pos[1])
    ecalc = os.path.abspath(os.path.expanduser(a["ecalc"]))
    if not os.access(ecalc, os.X_OK):
        die("no executable %s (build ecalc first; the SHMEM build for more than one node)" % ecalc)

    env = dict(os.environ)
    caller = {k: env[k] for k in COMPUTED if k in env}
    for k in COMPUTED:                                              # computed here: an inherited value would be taken as typed
        env.pop(k, None)
    for w in shlex.split(a["env"]):
        if "=" in w:
            k, v = w.split("=", 1)
            if k not in COMPUTED:
                env[k] = v                                          # the line's settings shape the plan (explicit > profile)
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
    note = [l for l in r.stderr.splitlines() if "POOL_MB" in l and "MN_PLAN_ONLY" in l]
    if g > 1 and not ex:
        die("mnrun.sh printed no launcher values (is %s the multi-node build?): %s" % (ecalc, r.stderr.strip()[-300:]))

    # 2. the plan itself: schedule, pool_log, verdict (same environment, same modules as mnrun.sh's plan call)
    penv = dict(env)
    penv.update({k: v for k, v in ex.items() if k in ("COMM_TRANSPORT", "COMM_SHMEM_POOL_MB", "MNRUN_MODULES", "MNRUN_UNLOAD")})
    if g > 1:
        penv.setdefault("COMM_TRANSPORT", "xshmem")   # (mnrun.sh's print-env gave the transport it picks: xshmem with the engine, Q73)
    penv["MN_PLAN_ONLY"] = "%d:%d" % (d, g)
    if env.get("ECALC_PE_PER_GPU") == "1":                          # one process per GPU: the engine reads the PEs per node from srun, which a plan-only call has not (as mnrun.sh gives its own plan call)
        penv["SLURM_NTASKS_PER_NODE"] = env.get("MNRUN_PPG_PPN") if (env.get("MNRUN_PPG_PPN") or "").isdigit() else "4"
    r = subprocess.run(["bash", "-lc", PRE, "_", "env", ecalc], env=penv, capture_output=True, text=True)
    out = r.stdout
    head = re.search(r"^== MN_PLAN_ONLY: .*?pool_log (\d+),.*?MN_GROUPS (.*?) ==$", out, re.M)
    chk = [l for l in out.splitlines() if l.startswith("plan check") or l.startswith("plan REFUSED")]
    if not head or not chk:
        die("the planner gave no plan (rc %d): %s" % (r.returncode, (r.stderr or out).strip()[-300:]))
    groups = head.group(2).strip()
    groups = "-" if groups.startswith("-") else groups
    verdict = chk[-1]
    fit = [l for l in out.splitlines() if l.startswith("plan fit") or (l.startswith("plan REFUSED") and "memory" in l)]
    fits = any(re.match(r"^plan fit +FITS:", l) for l in fit)
    fit_last = next((l for l in reversed(fit) if re.match(r"^plan fit +(FITS|REFUSED)", l)), None)
    if not fit:
        die("the planner printed no `plan fit` lines (a binary before FIT? rebuild %s)" % ecalc)

    if a["fit"]:
        print("memory fit: %d digits on %d node-process%s%s%s" % (d, g, "es" if g > 1 else "", "; profile " + a["profile"] if a["profile"] else "",
                                                             "; env " + a["env"] if a["env"] else ""))
        for l in fit:
            print("  " + l)
        if "REFUSED" in verdict and verdict not in fit:
            print("  " + verdict)
        print("  verdict: %s" % ("FITS -- the launch may proceed" if fits and "REFUSED" not in verdict else "DOES NOT FIT -- a launch is refused"))
        return 0 if fits and "REFUSED" not in verdict else 1
    comp = {k: ex[k] for k in LAUNCH_KEYS if k in ex}
    comp["MN_GROUPS"] = groups

    if a["exports"]:
        print("# tools/plan_values.py: %d digits on %d node-process%s%s; %s" % (d, g, "es" if g > 1 else "", " (profile %s)" % a["profile"] if a["profile"] else "", verdict))
        for k, v in ex.items():
            if k not in ("MNRUN_MODULES", "MNRUN_UNLOAD"):
                print("export %s=%s" % (k, shlex.quote(v)))
        print("# MN_GROUPS: the code's default schedule (%s) -- nothing to export" % groups)
        print("# %s" % (fit_last or "plan fit: -"))
        return 1 if "REFUSED" in verdict or not fits else 0

    print("plan values: %d digits on %d node-process%s%s" % (d, g, "es" if g > 1 else "", "; profile " + a["profile"] if a["profile"] else ""))
    for k in COMPUTED:
        if comp.get(k) is not None:
            print("  %-28s %s" % (k, comp[k]))
    print("  pool_log %s; %s" % (head.group(1), verdict))
    print("  %s" % (fit_last or "plan fit: -").strip())
    for l in note:
        print("  " + l.strip())
    if caller:
        print("  note: the caller's environment set %s; ignored here (computed), a launch would take the typed value" % ",".join(sorted(caller)))
    return 1 if "REFUSED" in verdict or not fits else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

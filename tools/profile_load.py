#!/usr/bin/env python3
"""tools/profile_load.py -- load and validate a machine profile (profiles/*.conf), step P1-2.

  eval "$(tools/profile_load.py aac7)"          export the profile's values that the caller has not set (stdout = sh lines)
  tools/profile_load.py aac7 --validate         validate only (exit 1 on any error); NAME or a path to a .conf
  tools/profile_load.py --validate-all          validate every profiles/*.conf (used by tools/gate.sh --build)
  tools/profile_load.py aac7 --show             table: key, value, origin (profile / env / default-changed), read-only
  tools/profile_load.py aac7 --run [--] CMD...  run CMD with the profile loaded; prints one provenance line first:
                                                config-src: profile=NAME from-profile=K,K env-over=K,K (code defaults: all other switches)
  tools/profile_load.py aac7 --line             the profile's launch line: KEY=value words of the keys named in LAUNCH_LINE_KEYS, in that order
                                                (tools/gate.sh --line aac7 runs them; the one source of the aac7 launch line, SCR2/D15)
  tools/profile_load.py PROFILE --check [-q] [--strict]
                                                drift check: keys that repeat a script default, unknown or unfilled keys (CHECK_DOC below; read-only)

Precedence per key: explicit environment  >  profile  >  built-in default (what the code does when the variable is unset).
A key already present in the environment is never overwritten (an explicit value, even empty, wins).
Validation errors (exit 1): a key that is not a registered switch (ecalc/switches.tsv), not an external-library variable
(FI_ SHMEM_ XT_ OMPI_ ...), and not a name used by a script (ecalc/*.sh, Makefile, tools/*) -- a typo is reported with the
closest name; a value still `__FILL__`; a line that is not KEY=value; a duplicate key; a profile that cannot be sourced.
Nothing is launched (except CMD with --run) and no file is written.
"""
import os, re, sys, glob, shlex, subprocess, difflib, locale

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTERNAL = re.compile(r"^(SLURM_|PMI_|SHMEM_|FI_|OMPI_|XT_|HIP_|ROC|HSA_|OMP_|MPICH_|CRAY_|LD_)")
UNSET_MARK = "__UNSET__"
# profile keys that describe the profile or the site (read by people and the target kit docs, by no script): accepted, never exported
META_KEYS = ("LAUNCH_LINE_KEYS", "TARGET_PARTITION", "TARGET_NODES", "ECALC_OUT_DIR")


def registry():
    names = {}
    p = os.path.join(ROOT, "ecalc", "switches.tsv")
    for line in open(p):
        if line.startswith("#") or line.startswith("name\t") or not line.strip():
            continue
        c = line.rstrip("\n").split("\t")
        names[c[0]] = c[2]            # name -> effective default
    return names


def script_words():
    words = set()
    files = glob.glob(os.path.join(ROOT, "ecalc", "*.sh")) + glob.glob(os.path.join(ROOT, "ecalc", "Makefile")) + \
        [f for f in glob.glob(os.path.join(ROOT, "tools", "*")) if os.path.isfile(f) and os.path.basename(f) != "profile_load.py"]
    for f in files:
        try:
            txt = open(f, errors="replace").read()
        except OSError:
            continue
        words.update(re.findall(r"\b[A-Z][A-Z0-9_]{2,}\b", txt))
    return words


def resolve(arg):
    if os.path.isfile(arg):
        return arg
    p = os.path.join(ROOT, "profiles", arg if arg.endswith(".conf") else arg + ".conf")
    return p if os.path.isfile(p) else None


def parse(path):
    """-> (keys in order, errors, values dict). Values come from sourcing the file in a clean shell ($HOME expanded)."""
    errs, keys = [], []
    for n, line in enumerate(open(path, errors="replace"), 1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        m = re.match(r"([A-Za-z_][A-Za-z0-9_]*)=", s)
        if not m:
            errs.append("%s:%d: not KEY=value: %s" % (os.path.basename(path), n, s[:60]))
            continue
        if m.group(1) in keys:
            errs.append("%s:%d: duplicate key %s" % (os.path.basename(path), n, m.group(1)))
        keys.append(m.group(1))
    vals = {}
    if not errs or keys:
        script = '. "$1"; shift; for k in "$@"; do if [ -n "${!k+x}" ]; then printf "%s\\0%s\\0" "$k" "${!k}"; fi; done'
        r = subprocess.run(["env", "-i", "HOME=" + os.environ.get("HOME", ""), "PATH=" + os.environ.get("PATH", ""),
                            "bash", "-c", script, "_", path] + sorted(set(keys)), capture_output=True)
        if r.returncode != 0:
            errs.append("%s: cannot be sourced: %s" % (os.path.basename(path), r.stderr.decode(errors="replace").strip()[:120]))
        else:
            parts = r.stdout.decode(errors="replace").split("\0")
            vals = dict(zip(parts[0::2], parts[1::2]))
    return keys, errs, vals


def validate(path, reg=None, words=None):
    reg = reg if reg is not None else registry()
    words = words if words is not None else script_words()
    keys, errs, vals = parse(path)
    base = os.path.basename(path)
    for k in dict.fromkeys(keys):
        if k in reg or EXTERNAL.match(k) or k in words or k in META_KEYS:
            pass
        else:
            near = difflib.get_close_matches(k, list(reg), n=2, cutoff=0.8)
            errs.append("%s: unknown switch %s%s" % (base, k, " (did you mean %s?)" % " or ".join(near) if near else ""))
        if vals.get(k) == "__FILL__":
            errs.append("%s: %s is still __FILL__" % (base, k))
    for k in vals.get("LAUNCH_LINE_KEYS", "").split():
        if k not in vals or vals[k] == "__FILL__":
            errs.append("%s: LAUNCH_LINE_KEYS names %s, which the profile does not set" % (base, k))
    return keys, errs, vals


def origin_table(path, env=None):
    env = os.environ if env is None else env
    reg = registry()
    keys, errs, vals = validate(path, reg)
    rows = []
    for k in dict.fromkeys(keys):
        v = vals.get(k, "")
        if k in env:
            rows.append((k, env[k], "env" if env[k] != v else "env(=profile)", v))
        else:
            rows.append((k, v, "profile", reg.get(k, "-")))
    return rows, errs


CHECK_DOC = """profile_load.py PROFILE --check [-q] [--strict]   (read-only drift check of a machine profile against the launch scripts)
    -q        only the problems (SAME-AS-SCRIPT, UNKNOWN, UNFILLED), no OVERRIDE / PROFILE-ONLY lines
    --strict  exit 1 if any UNKNOWN or SAME-AS-SCRIPT is found (default exit 0: it reports, it does not gate)
  Per profile key (value read by sourcing the profile in an `env -i` subshell):
    SAME-AS-SCRIPT  a literal ${NAME:-value} default in ecalc/mnrun.sh, aac7env.sh or target/kit/target_kit.sh equals the profile value: delete the key (Q107)
    OVERRIDE        the scripts default to another value; the profile wins when it is loaded
    PROFILE-ONLY    no script defaults it (the code default, or a launch-line value)
    UNFILLED        the value is __FILL__
    UNKNOWN         a name found nowhere in ecalc/*.c|*.h|*.sh, ecalc/Makefile, tools/ (FI_* SHMEM_* XT_* OMPI_* SLURM_* are EXTERNAL)"""
CHECK_EXT = ("FI_", "SHMEM_", "XT_", "OMPI_", "SLURM_")


def _lsort(lines):
    """sort -u in the caller's locale (the order the shell version printed)"""
    try:
        locale.setlocale(locale.LC_COLLATE, "")
        key = locale.strxfrm
    except locale.Error:
        key = None
    return sorted(set(lines), key=key)


def _bash(script, *args, env_i=False):
    env = {"HOME": os.environ.get("HOME", ""), "PATH": os.environ.get("PATH", "")} if env_i else None
    r = subprocess.run(["bash", "-c", script, "_"] + list(args), capture_output=True, env=env)
    return r.returncode, r.stdout.decode(errors="replace"), r.stderr.decode(errors="replace")


def launch_line(path):
    """the launch line of a profile: KEY=value for every key of LAUNCH_LINE_KEYS, in that order (None: the profile has no line)"""
    _, errs, vals = parse(path)
    if errs or "LAUNCH_LINE_KEYS" not in vals:
        return None
    return ["%s=%s" % (k, vals[k]) for k in vals["LAUNCH_LINE_KEYS"].split()]


def check(argv):
    E = os.path.join(ROOT, "ecalc")
    prof, q, strict = None, False, False
    for a in argv:
        if a == "--check":
            continue
        elif a == "-q":
            q = True
        elif a == "--strict":
            strict = True
        elif a in ("-h", "--help"):
            print(CHECK_DOC)
            return 0
        else:
            prof = a
    path = resolve(prof) if prof else None
    if not path or not os.access(path, os.R_OK):
        print("usage: profile_load.py <profile> --check [-q] [--strict]", file=sys.stderr)
        return 2
    keys, errs, vals = parse(path)
    if errs and not keys:
        print("profile_check: cannot read %s: %s" % (prof, errs[0]))
        return 2
    # the literal defaults the scripts apply: ${NAME:-value} / ${NAME-value} (values containing $ are computed, never compared)
    dflt = {}
    for f in ("mnrun.sh", "aac7env.sh", os.path.join("..", "target", "kit", "target_kit.sh")):
        p = os.path.join(E, f)
        if os.access(p, os.R_OK):
            for m in set(re.findall(r"\$\{[A-Z][A-Z0-9_]*:?-[^}]*\}", open(p, errors="replace").read())):
                inner = m[2:-1]
                k = re.match(r"[^:-]*", inner).group(0)
                v = inner[len(k):].lstrip(":")[1:]
                if v and "$" not in v and v not in ("<unset>", "<system>"):
                    dflt.setdefault(k, []).append((f, v))
    words = script_words() | set(registry())
    nsame = nunk = nfill = nover = nonly = 0
    print("profile_check: %s" % prof)
    for k in dict.fromkeys(keys):
        v = vals.get(k, "")
        ext = k.startswith(CHECK_EXT) or k in META_KEYS
        if v == "__FILL__":
            nfill += 1
            print("UNFILLED      %s" % k)
        if k not in words and not ext:
            nunk += 1
            print("UNKNOWN       %s=%s : no such name in ecalc/ or tools/ (new key without a consumer, or a typo)" % (k, v))
        if v == "__FILL__":
            continue
        d = dflt.get(k, [])
        if d and all(x[1] == v for x in d):
            nsame += 1
            print("SAME-AS-SCRIPT %s=%s : the script default (%s) is the same: delete the key" % (k, v, d[0][0]))
        elif d:
            nover += 1
            if not q:
                print("OVERRIDE      %s=%s ; script defaults: %s" % (k, v, " ".join("%s=%s" % x for x in d)))
        else:
            nonly += 1
            if not q:
                print("PROFILE-ONLY  %s=%s" % (k, v))
    print("summary: %d keys; SAME-AS-SCRIPT %d, OVERRIDE %d, PROFILE-ONLY %d, UNFILLED %d, UNKNOWN %d"
          % (len(dict.fromkeys(keys)), nsame, nover, nonly, nfill, nunk))
    return 1 if strict and nsame + nunk > 0 else 0


def main(argv):
    if "--check" in argv:
        return check(argv)
    if "--run" in argv:
        i = argv.index("--run")
        cmd = argv[i + 1:]
        cmd = cmd[1:] if cmd[:1] == ["--"] else cmd
        if len(argv[:i]) != 1 or not cmd:
            print("usage: profile_load.py PROFILE --run [--] CMD...", file=sys.stderr)
            return 2
        path = resolve(argv[0])
        if not path:
            print("profile_load: no such profile: %s" % argv[0], file=sys.stderr)
            return 1
        rows, errs = origin_table(path)
        for e in errs:
            print("profile_load: " + e, file=sys.stderr)
        if errs:
            return 1
        rows = [r for r in rows if r[0] not in META_KEYS]
        os.environ.update({k: v for k, v, o, d in rows if o == "profile"})
        fromp = ",".join(k for k, v, o, d in rows if o == "profile")
        over = ",".join(k for k, v, o, d in rows if o.startswith("env"))
        os.environ.update(ECALC_PROFILE_NAME=os.path.basename(path)[:-5], ECALC_PROFILE_FROM=fromp, ECALC_PROFILE_OVER=over)
        print("config-src: profile=%s from-profile=%s env-over=%s (code defaults: all other switches)"
              % (os.path.basename(path)[:-5], fromp or "-", over or "-"), flush=True)
        os.execvp(cmd[0], cmd)
    if "--validate-all" in argv:
        reg, words, bad = registry(), script_words(), 0
        for f in sorted(glob.glob(os.path.join(ROOT, "profiles", "*.conf"))):
            _, errs, _ = validate(f, reg, words)
            for e in errs:
                print("profile_load: " + e)
            bad += len(errs)
        print("profile_load --validate-all: %d problems" % bad)
        return 1 if bad else 0
    args = [a for a in argv if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        return 2
    path = resolve(args[0])
    if not path:
        print("profile_load: no such profile: %s" % args[0], file=sys.stderr)
        return 2
    rows, errs = origin_table(path)
    if errs:
        for e in errs:
            print("profile_load: " + e, file=sys.stderr)
        return 1
    if "--line" in argv:
        w = launch_line(path)
        if not w:
            print("profile_load: %s has no LAUNCH_LINE_KEYS" % os.path.basename(path), file=sys.stderr)
            return 1
        print(" ".join(w))
        return 0
    if "--validate" in argv:
        print("profile_load: %s ok (%d keys)" % (os.path.basename(path), len(rows)))
        return 0
    if "--show" in argv:
        for k, v, o, d in rows:
            print("%-32s %-28s %-14s code-default=%s" % (k, v, o, d))
        return 0
    rows = [r for r in rows if r[0] not in META_KEYS]
    prof = os.path.basename(path)[:-5]
    fromp = [k for k, v, o, d in rows if o == "profile"]
    over = [k for k, v, o, d in rows if o.startswith("env")]
    for k, v, o, d in rows:
        if o == "profile":
            print("export %s=%s" % (k, shlex.quote(v)))
    print("export ECALC_PROFILE_NAME=%s" % shlex.quote(prof))
    print("export ECALC_PROFILE_FROM=%s" % shlex.quote(",".join(fromp)))
    print("export ECALC_PROFILE_OVER=%s" % shlex.quote(",".join(over)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

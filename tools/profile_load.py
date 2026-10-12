#!/usr/bin/env python3
"""tools/profile_load.py -- load and validate a machine profile (profiles/*.conf), step P1-2.

  eval "$(tools/profile_load.py aac7)"          export the profile's values that the caller has not set (stdout = sh lines)
  tools/profile_load.py aac7 --validate         validate only (exit 1 on any error); NAME or a path to a .conf
  tools/profile_load.py --validate-all          validate every profiles/*.conf (used by tools/gate.sh --build)
  tools/profile_load.py aac7 --show             table: key, value, origin (profile / env / default-changed), read-only

Precedence per key: explicit environment  >  profile  >  built-in default (what the code does when the variable is unset).
A key already present in the environment is never overwritten (an explicit value, even empty, wins).
Validation errors (exit 1): a key that is not a registered switch (ecalc/switches.tsv), not an external-library variable
(FI_ SHMEM_ XT_ OMPI_ ...), and not a name used by a script (ecalc/*.sh, Makefile, tools/*) -- a typo is reported with the
closest name; a value still `__FILL__`; a line that is not KEY=value; a duplicate key; a profile that cannot be sourced.
Nothing is launched and no file is written.  tools/with_profile.sh runs a command under a profile and prints the provenance line.
"""
import os, re, sys, glob, shlex, subprocess, difflib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTERNAL = re.compile(r"^(SLURM_|PMI_|SHMEM_|FI_|OMPI_|XT_|HIP_|ROC|HSA_|OMP_|MPICH_|CRAY_|LD_)")
UNSET_MARK = "__UNSET__"


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
        if k in reg or EXTERNAL.match(k) or k in words:
            pass
        else:
            near = difflib.get_close_matches(k, list(reg), n=2, cutoff=0.8)
            errs.append("%s: unknown switch %s%s" % (base, k, " (did you mean %s?)" % " or ".join(near) if near else ""))
        if vals.get(k) == "__FILL__":
            errs.append("%s: %s is still __FILL__" % (base, k))
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


def main(argv):
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
    if "--validate" in argv:
        print("profile_load: %s ok (%d keys)" % (os.path.basename(path), len(rows)))
        return 0
    if "--show" in argv:
        for k, v, o, d in rows:
            print("%-32s %-28s %-14s code-default=%s" % (k, v, o, d))
        return 0
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

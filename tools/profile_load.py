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
  tools/profile_load.py PROFILE --check [-q] [--strict] [--launch=aac7|none]
                                                compare the profile with what the launch scripts set (see check() below; read-only)

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


CHECK_DOC = """profile_load.py PROFILE --check [-q] [--strict] [--launch=aac7|none]  (formerly tools/profile_check.sh; same output)
  READ-ONLY comparison of a machine profile with what the launch scripts set today.
    -q        only the problems (MISMATCH, UNKNOWN, UNFILLED, REMOVAL-SLATED, SCRIPT-ONLY), no MATCH / PROFILE-ONLY lines
    --launch=aac7|none  compare with the aac7 launch lines (default: only for aac7*.conf)
    --strict  exit 1 if any MISMATCH or UNKNOWN is found (default exit 0: it reports, it does not gate)
  1. Sources the profile in an `env -i` subshell and reads the value of every KEY=value line.
  2. Collects what the scripts set: aac7env = the variables ecalc/aac7env.sh exports when sourced (module stubbed, the /opt/cray/pe/sma test forced true);
     default = the literal ${NAME:-value} / ${NAME-value} defaults in mnrun.sh, aac7env.sh, target_kit.sh
     (values containing $ are COMPUTED, never compared).
  3. Per key: MATCH / MISMATCH (against aac7env), PROFILE-ONLY (script defaults shown), UNFILLED (__FILL__).
  4. UNKNOWN: a key named nowhere in ecalc/*.c|*.h|*.sh, ecalc/Makefile, tools/ (FI_* SHMEM_* XT_* OMPI_* SLURM_* are EXTERNAL).
  5. SCRIPT-ONLY: keys that aac7env.sh sets but the profile lacks.
  6. REMOVAL-SLATED: profile keys named in the tier A / B tables of results/A11.md."""
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
    prof, q, strict, launch = None, False, False, "auto"
    for a in argv:
        if a == "--check":
            continue
        elif a == "-q":
            q = True
        elif a == "--strict":
            strict = True
        elif a.startswith("--launch="):
            launch = a.split("=", 1)[1]
        elif a in ("-h", "--help"):
            print(CHECK_DOC)
            return 0
        else:
            prof = a
    path = resolve(prof) if prof else None
    if not path or not os.access(path, os.R_OK):
        print("usage: profile_load.py <profile> --check [-q] [--strict] [--launch=aac7|none]", file=sys.stderr)
        return 2
    # 1. the profile: keys (file order) and values
    keys = []
    for line in open(path, errors="replace"):
        m = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)=", line)
        if m and m.group(1) not in keys:
            keys.append(m.group(1))
    rc, out, err = _bash('. "$1"; shift; for k in "$@"; do printf "%s\\t%s\\n" "$k" "${!k-}"; done', path, *keys, env_i=True)
    if rc != 0:
        print("profile_check: cannot source %s:" % prof)
        sys.stdout.write(err)
        return 2
    profv = []
    for line in out.splitlines():
        k, _, v = line.partition("\t")
        profv.append((k, v.strip("\t")))
    # 2. what the scripts set: (key, source, kind set|default|computed, value)
    src = []
    ae = os.path.join(E, "aac7env.sh")
    if os.access(ae, os.R_OK):
        forced = open(ae, errors="replace").read().replace("/opt/cray/pe/sma", "/")
        rc, out, _ = _bash('module() { :; }; b=$(env | sort); eval "$1" > /dev/null 2>&1; a=$(env | sort); '
                           'comm -13 <(echo "$b") <(echo "$a")', forced, env_i=True)
        for l in out.splitlines():
            if "=" in l:
                src.append((l.split("=", 1)[0], "aac7env", "set", l.split("=", 1)[1]))
    for f in ("mnrun.sh", "aac7env.sh", "target_kit.sh"):
        p = os.path.join(E, f)
        if not os.access(p, os.R_OK):
            continue
        for m in set(re.findall(r"\$\{[A-Z][A-Z0-9_]*:?-[^}]*\}", open(p, errors="replace").read())):
            m = m[2:-1]
            k = re.match(r"[^:-]*", m).group(0)
            v = m[len(k):]
            v = v[1:] if v.startswith(":") else v
            v = v[1:] if v.startswith("-") else v
            src.append((k, f[:-3], "computed" if "$" in v else "default", v))
    if launch == "auto":
        launch = "aac7" if os.path.basename(path).startswith("aac7") else "none"
    if launch != "aac7":
        src = [r for r in src if r[2] != "set"]
    srcs = [tuple(l.split("\t")) for l in _lsort("\t".join(r) for r in src)]
    # code-known names (binary files skipped, as grep does)
    known = set()
    files = [f for pat in ("*.c", "*.h", "*.sh", "Makefile") for f in glob.glob(os.path.join(E, pat))]
    for d, _, fs in os.walk(os.path.join(ROOT, "tools")):
        files += [os.path.join(d, f) for f in fs]
    for f in files:
        try:
            b = open(f, "rb").read()
        except OSError:
            continue
        if b"\0" not in b:
            known.update(x.decode() for x in re.findall(rb"\b[A-Z][A-Z0-9_]{2,}\b", b))
    # removal-slated (results/A11.md tier A / B tables)
    slated = {}
    a11 = os.path.join(ROOT, "results", "A11.md")
    if os.access(a11, os.R_OK):
        on = False
        for l in open(a11, errors="replace"):
            if re.match(r"### Tier [AB]", l):
                on = True
                continue
            if re.match(r"### Tier C", l):
                on = False
            if on and re.match(r"\| [AB][0-9]+ \|", l):
                c = l.split("|")
                sid = None
                for t in re.findall(r"[A-B][0-9]+ *\||`[A-Z][A-Z0-9_]*`", c[1] + "|" + (c[3] if len(c) > 3 else "")):
                    if "|" in t:
                        sid = t.split()[0] if t.split() else t
                    else:
                        slated.setdefault(t.strip("`"), sid)
    # 3-6. report
    nmis = nunk = nfill = nslate = nmatch = nonly = 0
    print("profile_check: %s  (scripts under %s)" % (prof, E))
    for k, v in profv:
        if v == "__FILL__":
            nfill += 1
            print("UNFILLED      %s" % k)
            if k not in known and not k.startswith(CHECK_EXT):
                nunk += 1
                print("UNKNOWN       %s : no consumer in ecalc/ or tools/ (profile-only meta key: needs the entry point to read it)" % k)
            continue
        vals = dv = ""
        bad = n = dsame = dn = 0
        for r in srcs:
            if len(r) < 3 or r[0] != k:
                continue
            ss, kind, sv = r[1], r[2], (r[3] if len(r) > 3 else "")
            if kind == "set":
                n += 1
                vals += " %s=%s" % (ss, sv)
                bad = bad or sv != v
            elif kind == "default":
                if not sv or sv in ("<unset>", "<system>"):
                    continue
                dn += 1
                dv += " %s=%s" % (ss, sv)
                dsame += sv == v
            elif kind == "computed":
                dv += " %s=COMPUTED" % ss
        ext = ""
        if k not in known:
            if k.startswith(CHECK_EXT):
                ext = " [EXTERNAL library variable]"
            else:
                nunk += 1
                print("UNKNOWN       %s=%s : no such name in ecalc/ or tools/ (new key without a consumer, or a typo)" % (k, v))
        if slated.get(k):
            nslate += 1
            print("REMOVAL-SLATED %s : tier item %s of results/A11.md" % (k, slated[k]))
        if bad:
            nmis += 1
            print("MISMATCH      %s profile=%s ; launch script sets:%s" % (k, v, vals))
        elif n > 0:
            nmatch += 1
            if not q:
                print("MATCH         %s=%s ; launch script sets:%s%s" % (k, v, vals, ext))
        else:
            nonly += 1
            if not q:
                note = ""
                if dv:
                    note = (" ; equals the script default:%s" if dn > 0 and dsame == dn else " ; script defaults (the profile overrides):%s") % dv
                print("PROFILE-ONLY  %s=%s%s%s" % (k, v, note, ext))
    keyset = set(keys)
    so = ["SCRIPT-ONLY   %s=%s  (set by %s, not in the profile)" % (k, v, s)
          for k, s, v in (l.split("\t") for l in _lsort("%s\t%s\t%s" % (r[0], r[1], r[3]) for r in src
                                                          if r[2] == "set" and r[1] == "aac7env"))
          if k not in keyset]
    for l in so:
        print(l)
    print("summary: %d keys; MATCH %d, MISMATCH %d, PROFILE-ONLY %d, UNFILLED %d, UNKNOWN %d, REMOVAL-SLATED %d, SCRIPT-ONLY %d"
          % (len(profv), nmatch, nmis, nonly, nfill, nunk, nslate, len(so)))
    return 1 if strict and nmis + nunk > 0 else 0


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

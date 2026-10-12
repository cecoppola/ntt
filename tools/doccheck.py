#!/usr/bin/env python3
"""doccheck.py - fast read-only check that the docs match the repository.

Usage: tools/doccheck.py [--warn] [--root DIR]
  Prints each failure as `file:line: [check] reason`; exit 1 on any failure (--warn: print, exit 0).
Checks:
  registry  tools/switch_registry.py --check passes (called, not duplicated)
  scope     central docs have a "Scope:" line in their first 8 lines
  path      relative paths / links / results/X.md / tools/x / ecalc/x cited in the central docs exist (results/*.md are dated records, not checked)
  plan      "PLAN §N" / "PLAN.md §N" outside results/ names a section PLAN.md has
  paper     no "the paper" / "epaper" in tracked files outside results/
  archive   no `archive/` path references (use `archive:`)
"""
import os, re, subprocess, sys

CENTRAL = ["README.md", "ALGORITHM.md", "TESTING.md", "PLAN.md", "RESULTS.md", "TASKS.md", "ecalc/README.md", "profiles/README.md"]
TEXT_EXT = {".md", ".txt", ".sh", ".py", ".conf", ".template", ".c", ".h", ".cpp", ".hpp", ".cu", ".hip", ".cc", ".mk", ".csv", ""}


def main():
    warn = "--warn" in sys.argv
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if "--root" in sys.argv:
        root = os.path.abspath(sys.argv[sys.argv.index("--root") + 1])
    fails = {}

    def bad(kind, f, ln, msg):
        fails.setdefault(kind, []).append(f"{f}:{ln}: [{kind}] {msg}")

    def lines(rel):
        try:
            with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as fh:
                return fh.read().split("\n")
        except OSError:
            return None

    # tracked files
    files = None
    if os.path.exists(os.path.join(root, ".git")):
        r = subprocess.run(["git", "-C", root, "ls-files"], capture_output=True, text=True)
        if r.returncode == 0:
            files = [x for x in r.stdout.split("\n") if x]
    if files is None:
        files = []
        for d, ds, fs in os.walk(root):
            ds[:] = [x for x in ds if x != ".git"]
            files += [os.path.relpath(os.path.join(d, x), root) for x in fs]
    files = [f for f in files if os.path.isfile(os.path.join(root, f))]
    fileset = set(files)
    dirset = {os.path.dirname(f) for f in files}
    for f in list(dirset):
        while f:
            f = os.path.dirname(f)
            dirset.add(f)

    # a. registry
    sr = os.path.join(root, "tools", "switch_registry.py")
    if os.path.exists(sr):
        r = subprocess.run([sys.executable, sr, "--check"], capture_output=True, text=True, cwd=root)
        if r.returncode != 0:
            out = (r.stdout + r.stderr).strip().split("\n")
            for l in out[:15]:
                bad("registry", "ecalc/README.md", 1, l.strip())
    else:
        bad("registry", "tools/switch_registry.py", 1, "missing")

    # b. scope lines
    for f in CENTRAL:
        L = lines(f)
        if L is None:
            bad("scope", f, 1, "central doc missing")
        elif not any(l.lstrip("#*_ >").startswith("Scope:") for l in L[:8]):
            bad("scope", f, 1, 'no "Scope:" line in the first 8 lines')

    # c. cited paths
    pat = re.compile(r"(?<![\w/.:~-])((?:results|tools|ecalc|profiles|tests)/[A-Za-z0-9_./+-]*[A-Za-z0-9_+/-])")
    link = re.compile(r"\]\(([^)\s]+)\)")
    # results/*.md are dated evidence records (frozen when written); only the central docs must cite live paths.
    targets = [f for f in CENTRAL if f in fileset]
    # files the docs name before they exist (planned) or that runs create (generated)
    PLANNED = {"profiles/target.conf", "tools/target_probe.sh", "ecalc/results/mnaccept"}
    for f in targets:
        L = lines(f) or []
        d = os.path.dirname(f)
        for i, l in enumerate(L, 1):
            cands = []
            for m in link.finditer(l):
                t = m.group(1).split("#")[0]
                if t and not re.match(r"[a-z]+:", t) and not t.startswith("~"):
                    cands.append((t, True))
            for m in pat.finditer(l):
                cands.append((m.group(1), False))
            seen = set()
            for t, islink in cands:
                if t in seen:
                    continue
                seen.add(t)
                if any(c in t for c in "<>*{}$?|") or t.startswith("archive") or "..." in t or "/." in t or t.endswith((".out", ".log", ".bin", ".o")):
                    continue
                t = t.rstrip("/")
                if t in PLANNED or t.endswith("_") or "NAME" in t:
                    continue
                opts = {os.path.normpath(os.path.join(d, t)), os.path.normpath(t)}
                if not islink:
                    opts.add(os.path.normpath(os.path.join("ecalc", t)))
                # built products (binaries) are accepted when a tracked source of the same stem exists
                stems = {o + e for o in opts for e in (".c", ".cpp", ".cu", ".hip", ".py", ".sh")}
                if not any(o in fileset or o in dirset for o in opts) and not (stems & fileset):
                    bad("path", f, i, f"cited path does not exist: {t}")

    # d. PLAN section references
    secs = set()
    for l in lines("PLAN.md") or []:
        m = re.match(r"#{2,}\s+(\d+(?:\.\d+)*)\.?\s", l)
        if m:
            secs.add(m.group(1))
    ref = re.compile(r"(?<!archive:)PLAN(?:\.md)?\s*§\s*(\d+(?:\.\d+)*)")
    # e, f scan
    for f in files:
        if os.path.splitext(f)[1] not in TEXT_EXT or f.startswith("tools/doccheck"):
            continue
        L = lines(f)
        if L is None:
            continue
        inres = f.startswith("results/")
        for i, l in enumerate(L, 1):
            if not inres:
                for m in ref.finditer(l):
                    n = m.group(1).rstrip(".")
                    if n not in secs:
                        bad("plan", f, i, f"PLAN §{n} does not exist (PLAN.md has {', '.join(sorted(secs, key=lambda x: [int(p) for p in x.split('.')]))})")
                if re.search(r"\bthe paper\b|\bepaper\b", l, re.I):
                    bad("paper", f, i, '"the paper"/"epaper" reference')
            if not inres and re.search(r"(?<![\w:])archive/(?!<)", l):
                bad("archive", f, i, "`archive/` reference (write `archive:<path>`)")

    total = 0
    for kind in ("registry", "scope", "path", "plan", "paper", "archive"):
        for s in fails.get(kind, []):
            print(s)
        total += len(fails.get(kind, []))
    summ = " ".join(f"{k}={len(fails.get(k, []))}" for k in ("registry", "scope", "path", "plan", "paper", "archive"))
    print(f"doccheck: {total} problem(s) ({summ})")
    sys.exit(0 if (total == 0 or warn) else 1)


main()

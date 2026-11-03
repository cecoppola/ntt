#!/usr/bin/env python3
"""citenum.py - every decimal on a line that cites `results/X.md` (with or without §ID) must appear in that results file
(or in another results file when the line cites several, or, as a derived figure, on another line of the citing doc). Prints the misses; exit 1 if any.
Usage: tools/citenum.py [--archive DIR]   (DIR = the pre-merge results/*.md: numbers that were never in them are doc-derived and not reported)"""
import re, sys, os, glob
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = ["TASKS.md", "RESULTS.md", "PLAN.md", "ALGORITHM.md", "ecalc/README.md", "TESTING.md"]
cite = re.compile(r"results/([A-Za-z0-9_]+)\.md`?(?:,?\s*§\s*([A-Za-z0-9_-]+))?")
num = re.compile(r"(?<![\w.])\d+\.\d+(?![\w.]*\()")

def sections(text):
    out = []; cur = None
    for l in text.split("\n"):
        m = re.match(r"(#+) ", l)
        if m:
            if cur: out.append(cur)
            cur = [len(m.group(1)), l, [l]]
        elif cur: cur[2].append(l)
    if cur: out.append(cur)
    return out

def find(secs, ident):
    for i, (lv, h, body) in enumerate(secs):
        if re.search(r"(?<![A-Za-z0-9_])" + re.escape(ident) + r"(?![A-Za-z0-9_])", h):
            txt = list(body)
            for lv2, h2, b2 in secs[i + 1:]:
                if lv2 <= lv: break
                txt += b2
            return "\n".join(txt)
    return None

_docs = {}
def doc_other(d, ln):
    if d not in _docs: _docs[d] = open(os.path.join(ROOT, d)).read().split("\n")
    return "\n".join(l for k, l in enumerate(_docs[d], 1) if k != ln)

def main():
    arch = sys.argv[sys.argv.index("--archive") + 1] if "--archive" in sys.argv else None
    olds = "\n".join(open(f).read() for f in glob.glob(arch + "/*.md")) if arch else None
    res = {os.path.basename(f)[:-3]: open(f).read() for f in glob.glob(ROOT + "/results/*.md")}
    old = {}
    if arch:
        for f in glob.glob(arch + "/*.md"): old[os.path.basename(f)[:-3]] = open(f).read()
    everything = "\n".join(res.values())
    miss = {}; n = 0
    for d in DOCS:
        for i, l in enumerate(open(os.path.join(ROOT, d)).read().split("\n"), 1):
            ms = list(cite.finditer(l))
            if not ms or not num.search(l): continue
            for m in ms:
                fn, sid = m.groups()
                if fn not in res: continue
                body = res[fn]
                if sid and find(sections(body), sid) is None: continue
                # numbers of this line that are near this citation (same clause: up to the next citation or line end)
                end = ms[ms.index(m) + 1].start() if ms.index(m) + 1 < len(ms) else len(l)
                start = ms[ms.index(m) - 1].end() if ms.index(m) > 0 else 0
                for x in set(num.findall(l[start:end])):
                    n += 1
                    if x not in body and x not in everything and x not in doc_other(d, i) and (olds is None or x in olds): miss.setdefault((fn, sid), []).append((d, i, x))
    for (fn, sid), v in sorted(miss.items(), key=lambda t: (t[0][0], t[0][1] or "")):
        print("%s §%s: %s" % (fn, sid, " ".join("%s:%d:%s" % t for t in v[:6])))
    print("citenum: %d numbers checked, %d missing in %d sections" % (n, sum(len(v) for v in miss.values()), len(miss)))
    return miss, res, old

if __name__ == "__main__":
    sys.exit(1 if main()[0] else 0)

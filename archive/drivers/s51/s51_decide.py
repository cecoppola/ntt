#!/usr/bin/env python3
# s51_decide.py <probe-dir> <gate-summary-dir>  -> prints the ARM_B words (kernels kept) and writes a table to stdout (stderr)
# keeps K2 (DBIG_ADDSUB3=1) / K3 (DBIG_MODQ_MONT=1) when the probe's checksum lines equal the base's AND the median time is < 0.97 of base (mean of the repeat runs);
# K4 (RNS_CRT_SUB=5) is kept when its gate passed (no probe: its gain is read from the S51STAT crtb times of the ABBA).  A kernel whose gate failed is dropped.
import sys,re,glob,os,statistics
pd,gd=sys.argv[1],sys.argv[2]
def parse(f):
    t=open(f).read(); d={}
    for m in re.finditer(r"^(\S+)\s+median ([0-9.]+) s",t,re.M): d[m.group(1)]=float(m.group(2))
    chk=[l for l in t.splitlines() if l.startswith("CHK")]
    return d,chk
def runs(tag):
    out=[]
    for f in sorted(glob.glob(os.path.join(pd,"probe_%s_*.log"%tag))): out.append(parse(f))
    return out
base0=[r for r in runs("base")]; res={}
def gate_ok(name):
    f=os.path.join(gd,"gate_%s.txt"%name)
    return os.path.exists(f) and open(f).read().startswith("gate PASS")
words=["DBIG_S51_STATS=1"]
def mean_of(rs,keys):
    v=[sum(r[0][k] for k in keys) for r in rs if all(k in r[0] for k in keys)]
    return statistics.mean(v) if v else None
for tag,sw,keys in (("k2","DBIG_ADDSUB3=1",("add","sub")),("k3","DBIG_MODQ_MONT=1",("modqs",))):
    a=runs(tag); tb=mean_of(base0,keys); tk=mean_of(a,keys)
    if tb is None or tk is None: print("%s: no probe data -> dropped"%tag,file=sys.stderr); continue
    ok=True   # checksum lines of the same file suffix (rep, kind) must equal the base's
    for f in sorted(glob.glob(os.path.join(pd,"probe_%s_*.log"%tag))):
        g=os.path.join(pd,"probe_base_%s"%f.rsplit("_",1)[1])
        if not os.path.exists(g) or parse(f)[1]!=parse(g)[1] or not parse(f)[1]: ok=False
    ratio=tk/tb; g=gate_ok(tag)
    print("%s: probe ratio %.3f (kernel/base), checksums %s, gate %s"%(tag,ratio,"equal" if ok else "DIFFER","PASS" if g else "not passed"),file=sys.stderr)
    if ok and ratio<0.97 and g: words.append(sw)
if gate_ok("k4"): words.append("RNS_CRT_SUB=5"); print("k4: gate PASS -> kept (gain judged from S51STAT crtb)",file=sys.stderr)
else: print("k4: gate not passed -> dropped",file=sys.stderr)
print(" ".join(words))

#!/usr/bin/env python3
"""C06: which levels of a MN_GROUPS schedule have a cut last group, and is the cut group a layered (non-power-of-two) one?
usage: cut_groups.py [g ...]   (default 576 and 575..560)"""
import sys
def default_sched(size):
    out=[]; v=1
    while v*2<=size and size%(v*2)==0: v*=2; out.append(v)
    rest=size//v; f=3
    while rest>1:
        while rest%f==0: v*=f; out.append(v); rest//=f
        f+=2
    if not out or out[-1]!=size: out.append(size)
    return out
def b04_sched(size):
    s=[2,4,8,16,32,64,192]
    s=[x for x in s if x<size]+[size]
    return s
pow2=lambda x:x>0 and not x&(x-1)
def rows(size,sched,name):
    r=[]
    for g in sched:
        cut=size%g if g<size else 0
        r.append((g,cut))
    lay_cut=[(g,c) for g,c in r if c>1 and not pow2(c)]
    pl_cut=[(g,c) for g,c in r if c>1 and pow2(c)]
    return f"{size:4d} {name:8s} levels {','.join(map(str,sched))} | cut groups (level:size) " + (' '.join(f"{g}:{c}{'L' if not pow2(c) else 'p'}" for g,c in r if c) or 'none') + f" | layered cut {len(lay_cut)}, plain cut {len(pl_cut)}"
gs=[int(a) for a in sys.argv[1:]] or [576]+list(range(575,559,-1))
for g in gs:
    print(rows(g,default_sched(g),'default'))
    if g!=576: print(rows(g,b04_sched(g),'MN_GROUPS'))

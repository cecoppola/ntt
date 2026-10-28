#!/usr/bin/env python3
"""Host-only audit of the xshmem PE-communicator ids (ecalc/mn.c sub_id, vpe_sub_slot).

Reproduces the id rule and every engine partition (mn_group_at levels, the MN_GROUPS tree schedule, the node group, the layered self-test run) for PE
counts 1..N and reports (a) two different runs that share a PE and get the same id (the fatal "communicator id used twice") and (b) runs outside the id
domain (the fatal "supports g < 64 and slot < 4").  Usage: comm_id_audit.py [max_pe] [ppn ...]    (default 2304; ppn 1 2 4 8)
"""
import sys

MAXID_VPE = 544 if "--old" not in sys.argv else 512
INTERNAL = MAXID_VPE - 1                    # comm_xshmem_burst's block (tests/t_vpe hs)

OLD = "--old" in sys.argv                    # the rule before 2026-10-28 (g >= 64 non-aligned: fatal, every engine run on slot 0)
if OLD: sys.argv.remove("--old")

def sub_id(g0, g, slot):
    lg = 0
    while (1 << lg) < g: lg += 1
    if (1 << lg) == g and g0 % g == 0: return 1 + lg + 16 * slot if slot < 16 else None
    if g >= 64:
        return None if OLD or slot >= 32 else 512 + slot
    if slot >= 4: return None
    return 256 + 64 * slot + g

def big_slot(g0, g, base, npe):             # mn.c big_slot
    if OLD: return 0
    return base if g >= 64 and ((g & (g - 1)) | (g0 % g)) != 0 and not (g0 == 0 and g == npe) else 0

def gs_default(size):                       # mn_groups_parse without MN_GROUPS
    out = []; v = 1
    while v * 2 <= size and size % (v * 2) == 0: v *= 2; out.append(v)
    rest = size // v; f = 3
    while rest > 1:
        while rest % f == 0: v *= f; out.append(v); rest //= f
        f += 2
    if not out or out[-1] != size: out.append(size)
    return out

def runs_of(npe, ppn, gs):
    """(what, g0, g, slot) for every run any PE creates"""
    R = set()
    if npe <= 1: return R
    for l in range(1, 16):                  # mn_group_at(l): recip / div / x1 levels (any level up to the top)
        if (1 << (l - 1)) >= npe: break
        for g0 in range(0, npe, 1 << l):
            g = min(1 << l, npe - g0)
            if g > 1: R.add(("at%d" % l, g0, g, big_slot(g0, g, l, npe)))
    for l, Gl in enumerate(gs, 1):          # mn_group_span: the tree schedule
        Gp = gs[l - 2] if l > 1 else 1
        for g0 in range(0, npe, Gl):
            g = min(Gl, npe - g0)
            if (g + Gp - 1) // Gp < 2: continue
            lv = 0
            while (1 << lv) < g: lv += 1
            if ((1 << lv) == g and g0 & (g - 1) == 0) or (g0 == 0 and g == npe):
                if g0 == 0 and g == npe: lv = 0
                continue                    # the binary group = mn_group_at's: counted above
            R.add(("sched%d" % l, g0, g, big_slot(g0, g, 16 + l, npe)))
    if ppn >= 2 and npe % ppn == 0 and ppn < npe:   # vpe_node_barrier: the node group
        al = (ppn & (ppn - 1)) == 0
        for n in {0, npe // ppn - 1}: R.add(("node", n * ppn, ppn, 15 if al else 3))
    gt = 1
    while 2 * gt <= npe: gt *= 2
    R.add(("selftest", 0, gt, 0))
    return R

def main():
    mx = int(sys.argv[1]) if len(sys.argv) > 1 else 2304
    ppns = [int(x) for x in sys.argv[2:]] or [1, 2, 4, 8]
    coll = []; dom = {}; internal = []
    for npe in range(2, mx + 1):
        gs = gs_default(npe)
        for ppn in ppns + ([0] if 0 not in ppns else []):
            if ppn and (npe % ppn): continue
            byid = {}
            for (w, g0, g, slot) in runs_of(npe, ppn, gs):
                if g0 == 0 and g == npe and slot == 0: continue          # the flat communicator (id 0)
                i = sub_id(g0, g, slot)
                if i is None:
                    dom.setdefault((w[:2] if w[:2] in ("at","no") else w[:5], g, slot), []).append(npe); continue
                if i == INTERNAL: internal.append((npe, g0, g, slot))
                byid.setdefault(i, {})[(g0, g, slot)] = w
            for i, d in byid.items():
                L = sorted(d)
                for a in range(len(L)):
                    for b in range(a + 1, len(L)):
                        (a0, ag, _), (b0, bg, _) = L[a], L[b]
                        if a0 < b0 + bg and b0 < a0 + ag: coll.append((npe, ppn, i, L[a], d[L[a]], L[b], d[L[b]]))
    print("comm_id_audit: PE counts 2..%d, ppn %s (+ none): id collisions %d, out-of-domain run classes %d, ids equal to the internal burst block %d" % (mx, ppns, len(coll), len(dom), len(internal)))
    for c in coll[:20]: print("  COLLISION npe %d ppn %d id %d: %s (%s) vs %s (%s)" % c)
    kinds = {}
    for (k, g, slot), ns in dom.items(): kinds.setdefault(k, []).append((g, slot, ns))
    for k, v in sorted(kinds.items()):
        allp = sorted({n for _, _, ns in v for n in ns})
        print("  OUT OF DOMAIN (%s): %d (g, slot) classes at %d PE counts (smallest PE count %d; e.g. g=%s)" % ({"at": "mn_group_at level (truncated top group)", "sched": "MN_GROUPS tree schedule group", "node": "node group"}.get(k, k), len(v), len(allp), allp[0], sorted((g for g, _, _ in v))[:6]))
    for x in internal[:5]: print("  INTERNAL-ID", x)
    return 1 if coll else 0

if __name__ == "__main__": sys.exit(main())

#!/usr/bin/env python3
"""S52 part (b): messages per APU per phase at 576 nodes, taken from the model (ecalc/mn_model.py) without changing it.

Method: the model's product memo is switched off, Fabric.a2a is wrapped to log every all-to-all (phase tag, group g, chunks,
messages per APU, bytes per APU), and the phase functions (tree_cost, recip_cost, division_cost) set the tag.  The logged
totals per phase are checked against the model's own `msgs/APU` (Cost.msgs) -- they must agree.
Run:  python3 msg_count.py [--lat 2e-6] [--layers 2|3] [--bw 47] [--D 6.4409722e10]   (from ecalc/ or with PYTHONPATH=ecalc)
"""
import sys, os, argparse, collections, io, contextlib, math
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "ecalc"))
os.environ.setdefault("COMM_LAYER_VSLOT_SHARE", "1")
import mn_model as M
import estimate as E

LOG = []          # (g, chunks, msgs, bytes_per_message) of every all-to-all, in call order (used only inside piece_cost / hit_cost)

# every Cost carries bd = [transform msgs, transform msg*bytes, in-piece single-chunk msgs, their msg*bytes]; Cost.add sums it, so the
# model's own memo (product_cost) keeps working and the totals are the model's, not a re-evaluation of the rule search
_add = M.Cost.add
def add(self, o):
    _add(self, o)
    bd = getattr(self, "bd", [0, 0, 0, 0]); ob = getattr(o, "bd", [0, 0, 0, 0])
    self.bd = [x + y for x, y in zip(bd, ob)]
    h = getattr(self, "hist", collections.Counter()); h.update(getattr(o, "hist", {})); self.hist = h
    return self
M.Cost.add = add

_a2a = M.Fabric.a2a
COAL = [None]          # None: the code (K chunks per transform exchange); else SMAX bytes: memory-neutral coalescing, chunks' = max(1, ceil(bytes_apu / SMAX))
GENONLY = [False]      # coalesce only on the general map (g not a power of two: 192, 576), where the xGMI stage is hidden by 1.1 % only (the equal-slab path hides 75 %)
POW2 = [False]         # K' rounded up to a power of two (ntt_dist.c needs K | rows, K a power of two)
BUNDLE = [None]        # n_tr: the n_tr transform exchanges of one piece sent as ONE bundle of at most SMAX bytes per chunk slab (memory-neutral); n_tr is ASSUMED (6..12)
KFIX = [None]          # a uniform K for every transform exchange (NOT memory-neutral: DIST_CHUNKS 8 -> 4 costs +13.4 GB per node, README)
SEEN = [0.0]           # the largest slab (bytes_apu / chunks) over the run's transform exchanges
def a2a(self, bytes_apu, g, chunks=1):
    if chunks > 1:
        SEEN[0] = max(SEEN[0], bytes_apu / chunks)
        if COAL[0] and (not GENONLY[0] or (g & (g - 1))):
            k = max(1, math.ceil(bytes_apu / COAL[0])); chunks = min(chunks, 2 ** math.ceil(math.log2(k)) if POW2[0] else k)
        if BUNDLE[0] and (not GENONLY[0] or (g & (g - 1))):
            k = max(1, math.ceil(BUNDLE[0] * bytes_apu / COAL[0])); chunks = min(chunks, (2 ** math.ceil(math.log2(k)) if POW2[0] else k) / BUNDLE[0])
        if KFIX[0]: chunks = KFIX[0]
    r = _a2a(self, bytes_apu, g, chunks)
    if g > 1: LOG.append((g, chunks, r[3], bytes_apu / g / chunks))
    return r
M.Fabric.a2a = a2a

def wrap_piece(name):
    f = getattr(M, name)
    def w(*a, **k):
        n0 = len(LOG); c = f(*a, **k)
        bd = [0, 0, 0, 0]; bm8 = 0.0
        for g, ch, mg, bm in LOG[n0:]:                       # the transform exchange is logged once and multiplied by n_tr inside piece_cost
            if ch > 1: bm8 = bm
            else: bd[2] += mg; bd[3] += mg * bm              # redistributions and the spill exchange: once each
        bd[0] = c.msgs - bd[2]; bd[1] = bd[0] * bm8          # transform-chunk messages = the piece's msgs minus the single-chunk ones
        c.bd = [x + y for x, y in zip(getattr(c, "bd", [0, 0, 0, 0]), bd)]
        if bd[0] and bm8 > 0:
            gg = [e[0] for e in LOG[n0:] if e[1] > 1][0]; h = getattr(c, "hist", collections.Counter()); h[(gg, 2 ** math.ceil(math.log2(bm8)))] += bd[0]; c.hist = h
        return c
    setattr(M, name, w)
wrap_piece("piece_cost"); wrap_piece("hit_cost")

ap = argparse.ArgumentParser()
ap.add_argument("--lat", type=float, default=2e-6); ap.add_argument("--layers", type=int, default=2)
ap.add_argument("--bw", type=float, default=47.0); ap.add_argument("--D", type=float, default=6.4409722e10)
ap.add_argument("--group", type=int, default=64)
ap.add_argument("--dump", action="store_true")
a = ap.parse_args()
sys.argv = ["estimate.py", "--fabric", "target-m", "--bw", str(a.bw), "--lat", str(a.lat), "--layers", str(a.layers), "--group", str(a.group),
            "--g", "576", "--D", str(a.D), "--groups", "2,4,8,16,32,64,192,576", "--verbose"]
fab = None
def run(lat, layers, group=64):
    prof = M.apply_profile("target-m")
    M.TWREC_G = True; M._PC.clear() if hasattr(M._PC, "clear") else None
    design = M.Design(np=3, strategy="auto", cap=E.mem_model.CAPS["2^31"], chunk="both", depth=2, modmul=1, chunk_mb=M.CHUNK_MB, p15=True, round_mb=1024, out_overlap="none",
                      p15b=True, np_mn="auto", packed=True, p15c=True, arena_room=0.16, cache_fit=True)
    fab = M.Fabric(M.TARGET.name, a.bw, lat, group=group, layers=layers, taper=1.0, write_bw=prof.write_bw)
    return M.run(fab, a.D, 576, "model", verbose=False, groups="2,4,8,16,32,64,192,576", form="grid", transport="shmem", staging="code", design=design)
def line(name, c):
    bd = getattr(c, "bd", [0, 0, 0, 0]); other = c.msgs - sum([bd[0], bd[2]])
    mt = bd[1] / max(bd[0], 1); m1 = bd[3] / max(bd[2], 1)
    print("%-24s %9d | transform-chunk %9d (%.2f MB/msg) | redistribute/spill %7d (%.2f MB/msg) | shifts/adds/other %6d | exposed %.1f s | t %.1f s"
          % (name, c.msgs, bd[0], mt / 1e6, bd[2], m1 / 1e6, other, c.t_exposed, c.t))
def report(r, hist=False):
    for k, size, c in r["levels_rows"]: line("tree level g=%d (%d-way)" % (size, k), c)
    line("reciprocal", r["rc"]); line("division", r["dc"])
    print("total %d messages per APU (model's tot.msgs %d); exposed communication %.1f s; wall without write %.1f s" % (sum(c.msgs for _, _, c in r["levels_rows"]) + r["rc"].msgs + r["dc"].msgs, r["msgs"], r["exposed"], r["wall_nowrite"]))
    if hist:
        for name, c in (("tree 192+576 levels", None), ("reciprocal", r["rc"]), ("division", r["dc"])):
            if c is None:
                c = M.Cost()
                for k, size, cc in r["levels_rows"]:
                    if size >= 192: c.add(cc)
            h = getattr(c, "hist", {}); print("  transform messages by group and size class (MB): %s" % name)
            for (g, bm), n in sorted(h.items()): print("     g %4d  <= %7.3f MB/msg : %9d msgs" % (g, bm / 1e6, n))
r0 = run(a.lat, a.layers, a.group)
print("== baseline (the code: K=8 chunks per transform exchange, 2-layer); lat %.1f us, bw %.0f GB/s, D/node %.4e" % (a.lat * 1e6, a.bw, a.D))
report(r0, hist=True)
SMAX = SEEN[0]
print("largest slab per chunk over the run: %.1f MB (= the memory-neutral bound for coalescing)" % (SMAX / 1e6))
print()
print("== schemes: wall without write (model), by per-message cost")
base = {}
rows = []
for lat in (2e-6, 8.5e-6, 20e-6):
    COAL[0] = None; KFIX[0] = None
    rb = run(lat, 2); base[lat] = rb
    row = {"base": (rb["msgs"], rb["wall_nowrite"], rb["exposed"])}
    COAL[0] = SMAX; r1 = run(lat, 2); row["coalesce"] = (r1["msgs"], r1["wall_nowrite"], r1["exposed"]); COAL[0] = None
    KFIX[0] = 4; r4 = run(lat, 2); row["K=4"] = (r4["msgs"], r4["wall_nowrite"], r4["exposed"])
    KFIX[0] = 2; r2 = run(lat, 2); row["K=2"] = (r2["msgs"], r2["wall_nowrite"], r2["exposed"]); KFIX[0] = None
    r3 = run(lat, 3, 64); row["3-layer (G=64)"] = (r3["msgs"], r3["wall_nowrite"], r3["exposed"])
    COAL[0] = SMAX; r13 = run(lat, 3, 64); row["coalesce + 3-layer"] = (r13["msgs"], r13["wall_nowrite"], r13["exposed"])
    GENONLY[0] = True; POW2[0] = True; COAL[0] = SMAX; rg = run(lat, 2); row["coalesce, K' power of 2, general map only (recommended)"] = (rg["msgs"], rg["wall_nowrite"], rg["exposed"])
    for nt in (6, 9, 12):
        BUNDLE[0] = nt; rbn = run(lat, 2); row["bundle the piece's n_tr=%d exchanges, K' pow 2, general map only (n_tr assumed)" % nt] = (rbn["msgs"], rbn["wall_nowrite"], rbn["exposed"])
    GENONLY[0] = False; POW2[0] = False
    BUNDLE[0] = 9; rbn = run(lat, 2); row["bundle n_tr=9, any K', all groups (idealised bound)"] = (rbn["msgs"], rbn["wall_nowrite"], rbn["exposed"])
    BUNDLE[0] = None; COAL[0] = None
    rows.append((lat, row))
    if lat == 2e-6:
        COAL[0] = SMAX; rr = run(lat, 2); print("-- coalescing detail at 2 us"); report(rr, hist=True); COAL[0] = None
print()
print("| scheme | msgs/APU | wall no-write s @2us | saving s | @8.5us | saving s | @20us | saving s |")
for sch in rows[0][1]:
    cells = []
    for lat, row in rows:
        m, w, e = row[sch]; cells.append("%.1f | %+.1f" % (w, w - row["base"][1]))
    print("| %s | %d | %s |" % (sch, rows[0][1][sch][0], " | ".join(cells)))
print("NIC bytes per node (TB): base %.2f, 3-layer %.2f" % (r0["nic"] / 1e12, r3["nic"] / 1e12))

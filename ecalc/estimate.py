#!/usr/bin/env python3
"""estimate.py - the standing estimate for a run of ecalc over g nodes at D digits per node (Phase 12 agent Q; PLAN.md 26's
standing rule and 27 row Q).  Imports mn_model.py (the fabric and phase model, X2 + Q) and mem_model.py (the per-node
memory model, M + Q):

    estimate(g, D, tree='grid', groups=None, fabric=None) -> dict      the numbers
    ./estimate.py                                                      the table for g in {1, 4, 64, 576}, D in {4e10, 7.7e10, 1e11}
    ./estimate.py --g 576 --D 6e10 --tree grid --groups 2,4,8,16,32,64,192,576 --bw 100 --lat 2e-6 --write-bw 2 --group 64
    ./estimate.py --max                                                the largest D per node that fits 502 / 480 GB at each g

Every number is labelled: measured (a recorded aac6 run), modelled (this arithmetic on measured inputs), assumed (a
target parameter no aac6 measurement can give: the fabric's bandwidth and per-message cost, the part-file bandwidth,
the general map's exchange overlap).  docs/TARGET.md says what to measure first on the target and how to feed it in
(--bw, --lat, --write-bw; the constants at the top of mn_model.py).

Phase 13b (agent D): the estimate is of the code after step 0 (three primes, NTT_MODMUL=1) and of a design -- --np, --strategy
(C | B | B4 | auto), --cap (2^30 | 3*2^29 | 2^31 | 3*2^30; default the code's rule), --chunk (off | shift | both), --depth (1 | 2),
--modmul; --legacy gives the Phase 12 model (four primes, the Phase 10/11 phase table).  design_table.py prints every design.

Phase 15 (agent MD): the default is the code's defaults since Phase 14 on the target's launch line (mn_model.DEFAULT15: auto, 2^31, both chunks
at 1024 MB, depth 2, DM_TIGHT, MN_TREE_EARLY_FREE, NEWTON_RECIP_CUT, the SHMEM pool from the plan, COMM_SHMEM_ROUND_MB=1024 = the user's D2;
--round-mb 0 without it; --p13 = the Phase 13/14 model).  Every estimate prints two walls (the user's D3): without the disk write (the digits
computed and verified) and with it, the part file at --write-bw GB/s per node (default 0.6: the target's /ssd0 is Lustre, 0.58-0.64 GB/s
single-stream, measured there; --target prints 2.0 (the old assumption), 0.8 and 0.6).  ./estimate.py --target is the standing estimate.

Phase 15 (agent DOC, the user's decisions of 2026-09-27): the default is mn_model.DEFAULT15B -- the code's defaults of 2026-09-27 (BS_SEED_FILL=128,
BI_MUL1_FAST, NEWTON_RECIP_MID, DIST_TWREC, RNS_AUTO_PIECE_COST, ECALC_CORR_PATCH=2, ECALC_OUT_PACKED, MN_OUT_EARLY, ECALC_ODIRECT=auto) on the target's
launch line (ECALC_NP=4 at size > 1: --np-mn; COMM_SHMEM_ROUND_MB=1024): the part file packed (0.444 B/digit; --ascii for 1 B/digit) and, at size > 1,
started at the division's hook (MN_OUT_EARLY: it overlaps the low product as the size-1 writer does).  --b0 gives the Phase 14 defaults (DEFAULT15).

Phase 15 (agent DOC2, the user's decisions of 2026-09-28): the default is mn_model.DEFAULT15C -- main B2 (B1 + BS_ARENA_ROOM=0.16, DIST_TWREC_G=1,
RNS_POOL1_4Q=1) on the target's launch line with ECALC_NP=auto (--np-mn auto) and RNS_DIST_CACHE_FIT=1: the mn transform cache priced at the slots FIT
allows (0 at 5.1e13 and 4.74e13: the code's rule, mem_model.cache_fit_slots); --cache-slots n prices n slots whatever FIT allows (their bytes added to the
node: "does not fit" over 480 GB); --room F (BS_ARENA_ROOM), --no-twrec-g; --b1 gives B1 (DEFAULT15B: ECALC_NP=4, no room, TWREC_G off, the code's
default 2 cache slots as the time assumed before TC / CX).  --target prints the standing figures, the cache at 1 / 2 slots and ECALC_NP=4 beside them.
"""
import argparse, math, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mn_model as M
import mem_model

LABELS = {
    "digits": "modelled (D x g; D is the request per node, the run computes to the next multiple of 18)",
    "walls": "modelled, two (D3): without the disk write (the digits computed and verified: + the formatting and the digit residues, 0.104 s per 1e9 digits, measured on 2 nodes) and with it (the part file after T1, as the code writes it at size > 1)",
    "p15": "modelled (Phase 15, mn_model.CAL15): the one-node compute refitted on the Phase 14 defaults (the five-run 1e11 series, V2's and V3's runs at 4e10-1.3e11: `total` within -1.3..+4.2 %, `mn_model.py --calib15`); the seed wait as its own term (the top node's terms all above 2^33: x 1.29 per limb-step, measured by V3); NEWTON_RECIP_CUT in the fabric's reciprocal (67 pieces = the C plan)",
    "wall": "modelled: the per-node compute recalibrated on the Phase 13c defaults (Phase 13d D2, mn_model.CAL13: G13d's ten one-node runs 6.6e10-1.16e11 + the 4e10 series + the 7.64e10 share run, `mn_model.py --calib13`; the pipeline's per-product law fitted on G's logs); the top node's leaf (the code's equal-term layout: 1.036 x the average digits at 576) and each tree level's largest group; the distributed levels, reciprocal and division built from S13's 2^31 piece x the pipeline factor (1.22, assumed to carry) and the fabric",
    "steps": "modelled = the C code's own plan (agent L's MN_PLAN_ONLY; mn_model.plan equals it at all 401 sizes 2.0-6.0e13): at the 2^31 cap the tree levels' largest groups step 88 -> 124 pieces over 4.30-4.39e13",
    "exposed": "modelled: the fabric time not hidden under the local passes -- assumes 100 GB/s per APU, 2 us per message (--bw, --lat), the general map's overlap (GEN_HIDE)",
    "fabric": "modelled: bytes through the node's eight NICs and over the dragonfly's global links (MN_TOPO_GROUP = 64, two-layer all-to-all)",
    "device": "modelled: mem_model (the code's own sizing formulas; measured to 0.05 % at size 1: 4e10 313.3 GB at four primes / 287.5 at three, 8e10 369.1, 1e11 431.2; at 10^10/4 19.9 GB arena)",
    "host": "modelled: 7 GB runtime + 4 GiB staging + the seed buffers at init + 6 GB per process of transport + the SHMEM pool ('pool': the larger of COMM_SHMEM_POOL_MB (8192) and the pool the run needs -- Phase 14 P2's law (--staging code, the default): 4 APU threads x the largest staged exchange's send + receive (the division's A_h mu result exchange: my rows of the piece + a quarter of my share of C inside it; MN_T_CHUNK_MB bounds it) + the control blocks; measured to 0.01 % at 1e8-1e10 on 2 nodes and 1e9-1e10 on 4 processes, results/P214.md)",
    "fits": "modelled against 502 GB per node (the MI300A's usable HBM; 480 GB = the safe budget with a 5 % margin)",
    "output": "the part file at --write-bw GB/s per node: 0.6 = the target's Lustre single-stream rate, MEASURED there (the apumult catalog: 0.58-0.64 write, 0.78-0.86 read), ASSUMED to hold with 576 nodes writing at once; 2.0 = the node-local NVMe assumed before.  Phase 15 (2026-09-27): packed, 8 bytes per 18 digits (ECALC_OUT_PACKED=1: 32.8 GB per node at the target, exact), started at the division's hook (MN_OUT_EARLY=1) and hidden under 0.577 of the division (OVL1, FITTED at size 1 and ASSUMED at size > 1); the process's exit 2.4 s (MEASURED) on both walls.  --b0: the ASCII file after T1, exposed whole",
    "p15b": "modelled (Phase 15, the defaults of 2026-09-27, mn_model DEFAULT15B, calibrated on RESULTS 86's paired 1e11 series within -3.1..+2.0 %: `mn_model.py --calib15b`): bs without the seed wait x 0.769 (BS_SEED_FILL, MEASURED at 1e11, ASSUMED at the target's leaf); the seeds' end on SEED15B (BI_MUL1_FAST, FITTED on ten runs; no slow path above 2^33); NEWTON_RECIP_MID in the fabric's reciprocal (66 pieces = the C plan); DIST_TWREC x 0.98 on the pieces' local passes (MODELLED from C2's -2.8 s at 1e11); ECALC_NP=4 at size > 1 (the plan check refuses three primes at 4.25e13 on 576: results/P15.md)",
    "p15c": "modelled (Phase 15 DOC2, main B2 of 2026-09-28): BS_ARENA_ROOM=0.16 -- the node's arena in whole VMM chunks + 0.16 x the hole (mem_model, exact against BS_LAYOUT_ONLY: +16.5 GB of arena at 5.1e13), at size 1 the division x 0.785 (P15C_DIV1, MEASURED at 1e11: no remaps); DIST_TWREC_G=1 (the general map's packs x 0.95, G5, MODELLED); ECALC_NP=auto (four primes only for pieces over the three-prime bound; pool 0 at four planes at 576, pool 1 at three); RNS_DIST_CACHE_FIT=1: the mn cache at the slots the code's budget rule allows (0 at the target), each slot's hit x CACHE_HIT_F 0.96 (FITTED by CX on aac6); `mn_model.py --calib15c` against the fin15e 1e11 pairs (-2.1..-3.9 % on B2)",
    "host15": "modelled (Phase 15): + DB_POOL_VMM's two 8 GiB pinned seed buffers at init (measured 21.5 GB pinned on one node), the bs phase's +6 GB device (measured 2.1-5.8), the SHMEM pool = the plan's need (256 MiB steps) = the C plan exactly (43008 MiB; 9472 with COMM_SHMEM_ROUND_MB=1024)",
}

def estimate(g, D, tree="grid", groups=None, fabric=None, rule="model", transport="shmem", staging="code", verbose=False, design=M.DEFAULT, corrections=0):
    """the estimate for g nodes at D digits per node: a dict of the numbers (seconds, GB, bytes) and their labels.
    design: an mn_model.Design (default: DEFAULT15, the code's defaults + the target's launch line); None = the legacy (Phase 12) model.
    Phase 15 (D3): wall_s = the wall with the part file written (at the fabric's write_bw), nowrite_s without it."""
    fab = fabric or M.TARGET
    r = M.run(fab, D, g, rule, verbose=verbose, groups=groups, form=tree, transport=transport, staging=staging, design=design, corrections=corrections)
    m = r["mem"]
    out = dict(g=g, D=D, digits=r["digits"], wall_s=r["wall"], minutes=r["wall"] / 60, nowrite_s=r["wall_nowrite"], nowrite_min=r["wall_nowrite"] / 60,
               t1_wait=r["t1_wait"], seed_wait=r["seed_wait"], total_line=r["total_line"], write_bw=fab.write_bw, out_write=r["out_write"],
               other=r["other"], init=r["init"], batch=r["batch"], top=r["top"], levels=r["levels"], recip=r["recip"], div=r["div"], out=r["out"],
               exposed_s=r["exposed"], exposed_pct=100 * r["exposed"] / r["wall"],
               nic_bytes=r["nic"], nic_per_nic=r["nic"] / 8, global_bytes=r["glob"], msgs_apu=r["msgs"], pieces=r["pieces"],
               device_gb=m["device"], host_gb=m["host"], node_gb=m["node"], shmem_pool_gb=m["shmem_pool"], fits=m["node"] <= M.NODE_GB, fits_margin=m["node"] <= M.NODE_GB_MARGIN,
               tree=tree, staging=staging, schedule=r["schedule"], mem=m, cache_slots=r.get("cache_slots", 0), cache_gb=m.get("cache", 0.0), node_cache_gb=m.get("node_cache", m["node"]))
    return out

def fmt_b(b): return M.fmt_b(b)

def table(gs, Ds, tree, groups, fab, rule, staging="code", design=M.DEFAULT, corrections=0):
    print("%-4s %-8s %-10s | %8s %7s %6s | %6s %6s %5s | %6s %6s %6s %6s | %8s %8s %8s | %s" % ("g", "D/node", "digits", "no-write", "write", "min", "expo s", "file s", "%", "dev GB", "hst GB", "pool", "node", "NIC/node", "per NIC", "global", "fits 502 / 480 GB"))
    rows = []
    for g in gs:
        for D in Ds:
            e = estimate(g, D, tree, groups, fab, rule, staging=staging, design=design, corrections=corrections)
            rows.append(e)
            print("%-4d %-8.1e %-10.3e | %8.1f %7.1f %6.1f | %6.1f %6.1f %5.0f | %6.0f %6.0f %6.0f %6.0f | %8s %8s %8s | %s / %s" % (
                g, D, e["digits"], e["nowrite_s"], e["wall_s"], e["minutes"], e["exposed_s"], e["out"], e["exposed_pct"], e["device_gb"], e["host_gb"], e["shmem_pool_gb"], e["node_gb"],
                fmt_b(e["nic_bytes"]), fmt_b(e["nic_per_nic"]), fmt_b(e["global_bytes"]), "yes" if e["fits"] else "NO", "yes" if e["fits_margin"] else "no"))
    return rows

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--g", type=int, nargs="*", default=[1, 4, 64, 576])
    ap.add_argument("--D", type=float, nargs="*", default=[4e10, 7.7e10, 1e11])
    ap.add_argument("--tree", default="grid", choices=("grid", "flat"), help="the top product's memory form: grid (Phase 12 G) or flat (the code at 7aded87)")
    ap.add_argument("--groups", default=None, help="MN_GROUPS (e.g. 2,4,8,16,32,64,192,576); default = the code's schedule")
    ap.add_argument("--staging", default="code", choices=("code", "sym", "resident", "per_exchange", "cached"), help="Phase 14 P2: code (the default: the measured law, mem_model.shmem_pool -- the staged exchanges' largest send + receive x 4 APU threads + the control blocks; results/P214.md), sym (DIST_MN_SYM_SLABS=1: + 3 q per APU of resident slabs); the hypotheses before: the SHMEM transport's staging in its pool: resident (Phase 12 S: the callers' slabs in the pool, no staging), per_exchange (the staging freed after each wait), cached (the code at 7aded87: kept per live communicator -- 345 GB per node at 576)")
    ap.add_argument("--as-is", action="store_true", help="the code at main 7aded87: --tree flat --staging cached")
    ap.add_argument("--rule", default="model", choices=("model", "full"))
    ap.add_argument("--fabric", default=os.environ.get("MN_MODEL_PROFILE", "target"), choices=("target", "aac7"), help="Phase 16 C: the fabric profile -- target (the default: the constants as before) or aac7 (mn_model.AAC7: the measured Slingshot-11 / Cray OpenSHMEMX rates and AAC7_CONSTS; --bw / --lat / --write-bw given explicitly still win)")
    ap.add_argument("--bw", type=float, default=None, help="GB/s per APU injection (default 100: assumed, PLAN 25's two 400 Gb/s NICs; the profile's value under --fabric aac7)")
    ap.add_argument("--lat", type=float, default=None, help="seconds per message (default 2e-6 assumed; the profile's under --fabric aac7)")
    ap.add_argument("--group", type=int, default=64, help="nodes per dragonfly group (MN_TOPO_GROUP)")
    ap.add_argument("--layers", type=int, default=2, choices=(2, 3))
    ap.add_argument("--taper", type=float, default=1.0)
    ap.add_argument("--write-bw", type=float, default=None, help="GB/s per node for the part file (Phase 15: 0.6 = the target's Lustre /ssd0 single-stream, measured there; 2.0 was assumed before)")
    ap.add_argument("--round-mb", type=float, default=1024, help="COMM_SHMEM_ROUND_MB on the launch line (D2: 1024; 0 = the code's default, off)")
    ap.add_argument("--out-overlap", default="none", choices=("none", "half"), help="size > 1: the part file after T1 (none: the code) or under half the division (half: the model before Phase 15)")
    ap.add_argument("--corrections", type=int, default=0, help="size 1: the division's corrections (data-dependent; 2 at 1e11 on the defaults)")
    ap.add_argument("--p13", action="store_true", help="the Phase 13/14 model (no Phase 15 terms: the old memory forms, no recip cut, no CAL15, the part file under half the division)")
    ap.add_argument("--max", action="store_true", help="the largest D per node that fits 502 and 480 GB at each g, with its wall")
    ap.add_argument("--target", action="store_true", help="the standing estimate at 576 nodes -- the target (mn_model.TARGET_DIGITS: 5.276e13 since B3 2026-09-29; both test sizes 5.276e13 and 5.167e13 are shown (the user, 2026-10-03); 5.1e13 from the user's decision of 2026-09-27 23:50 EDT; 4.25e13 from Phase 13d, 4.4e13 in Phase 13c), its steps, the runtime one step below (4.74e13) and its step, the previous target")
    ap.add_argument("--verbose", action="store_true", help="the per-phase, per-level breakdown of every run")
    ap.add_argument("--np", type=int, default=3, choices=(3, 4), help="ECALC_NP at size 1 (Phase 13b step 0: 3)")
    ap.add_argument("--np-mn", type=lambda v: v if v == 'auto' else int(v), default='auto', choices=(3, 4, 'auto'), help="ECALC_NP at size > 1 (Phase 15, the user's decision 1 of 2026-09-28: auto on the target's launch line -- Phase 15 NP's per-product count, four only over the three-prime bound; 4 was the launch line of 2026-09-27 (+17.2 GB per node: 489 GB at 5.1e13 with the room, over 480); 3 is refused by the plan check at 4.25e13 and 5.1e13 on 576)")
    ap.add_argument("--b1", action="store_true", help="Phase 15 DOC2: B1 (mn_model.DEFAULT15B: the defaults of 2026-09-27 on their launch line -- ECALC_NP=4, no arena room, DIST_TWREC_G=0, the code's 2 cache slots priced as before TC / CX)")
    ap.add_argument("--room", type=float, default=None, help="BS_ARENA_ROOM (default 0.16: the code's default since 2026-09-28; 0 = off)")
    ap.add_argument("--no-twrec-g", action="store_true", help="DIST_TWREC_G=0 (the default is 1 since 2026-09-28)")
    ap.add_argument("--cache-slots", type=int, default=None, choices=(0, 1, 2), help="price n mn transform-cache slots whatever RNS_DIST_CACHE_FIT allows (default: what FIT allows -- 0 at the target; the slots' bytes are added to the node)")
    ap.add_argument("--no-cache-fit", action="store_true", help="without RNS_DIST_CACHE_FIT on the launch line: the code's default 2 slots priced (137.4 GB per node at the target that no budget holds: TC15)")
    ap.add_argument("--ascii", action="store_true", help="ECALC_OUT_PACKED=0: the ASCII part file (1 B/digit) instead of the packed default (0.444 B/digit)")
    ap.add_argument("--b0", action="store_true", help="the Phase 14 defaults (B0, mn_model.DEFAULT15: three primes, the ASCII part file after T1, no fill / fast mul_1 / middle product / TWREC)")
    ap.add_argument("--strategy", default="auto", choices=M.STRATEGIES, help="RNS_STRATEGY (agent B, Phase 13b)")
    ap.add_argument("--cap", default="2^31", choices=list(mem_model.CAPS) + ["rule"], help="the plane cap (default 2^31 since Phase 13c; rule = the pre-13c size rule)")
    ap.add_argument("--chunk", default="both", choices=M.CHUNKS, help="off | shift (MDB_SHIFT_CHUNK_MB) | both (+ MN_T_CHUNK_MB, the default since Phase 14), at --chunk-mb")
    ap.add_argument("--chunk-mb", type=float, default=M.CHUNK_MB)
    ap.add_argument("--depth", type=int, default=2, choices=(1, 2), help="the uneven exchange's depth (agent X, Phase 13b)")
    ap.add_argument("--modmul", type=int, default=1, choices=(0, 1), help="NTT_MODMUL (step 0: 1)")
    ap.add_argument("--legacy", action="store_true", help="the Phase 12 model: four primes, the Phase 10/11 phase table")
    a = ap.parse_args()
    prof = M.apply_profile(a.fabric)                                      # Phase 16 C: the profile's fabric and constants (target = as before)
    if a.bw is None: a.bw = prof.bw
    if a.lat is None: a.lat = prof.lat
    if a.write_bw is None: a.write_bw = prof.write_bw
    if a.as_is: a.tree, a.staging = "flat", "cached"
    p15b = not (a.p13 or a.b0)
    p15c = p15b and not a.b1                                              # Phase 15 DOC2: B2 on the launch line (the default)
    if a.b1 and a.np_mn == 'auto': a.np_mn = 4                            # (B1's launch line)
    M.TWREC_G = not (a.no_twrec_g or a.b1 or not p15b)                    # DIST_TWREC_G (a global of the model)
    M.CACHE_FORCE = a.cache_slots
    design = None if a.legacy else M.Design(np=a.np, strategy=a.strategy, cap=mem_model.CAPS[a.cap] if a.cap and a.cap != "rule" else None, chunk=a.chunk, depth=a.depth, modmul=a.modmul, chunk_mb=a.chunk_mb,
                                            p15=not a.p13, round_mb=a.round_mb, out_overlap=a.out_overlap if not a.p13 else None,
                                            p15b=p15b, np_mn=a.np_mn if p15b else None, packed=(not a.ascii) if p15b else False,
                                            p15c=p15c, arena_room=a.room if p15c else None, cache_fit=p15c and not a.no_cache_fit)
    fab = M.Fabric(M.TARGET.name, a.bw, a.lat, group=a.group, layers=a.layers, taper=a.taper, write_bw=a.write_bw)
    print("ecalc estimate -- %s; tree form %s, SHMEM staging %s, MN_GROUPS %s, fabric %.0f GB/s per APU, %.1f us per message, dragonfly group %d, %d layers, taper %.2f, part files %.2f GB/s per node%s"
          % ("legacy (Phase 12: four primes)" if design is None else "design %s, ECALC_NP=%d%s, NTT_MODMUL=%d%s" % (design.name(), design.np,
             " (%s at size > 1)" % design.np_mn if design.np_mn and design.np_mn != design.np else "", design.modmul,
             (", COMM_SHMEM_ROUND_MB=%g, the part file %s" % (design.round_mb, "%s, from the division's hook (MN_OUT_EARLY)" % ("packed" if design.packed else "ASCII") if design.p15b else
              ("after T1" if design.out_overlap == 'none' else "under half the division"))) if design.p15 else " (the Phase 13/14 model)"),
             a.tree, a.staging, a.groups or "(default)", a.bw, a.lat * 1e6, a.group, a.layers, a.taper, a.write_bw,
             "" if design is None or not design.p15b else "; BS_ARENA_ROOM %.2f, DIST_TWREC_G=%d, the mn cache %s" % (design.arena_room, M.TWREC_G,
                 ("%d slot(s) forced (--cache-slots)" % a.cache_slots) if a.cache_slots is not None else ("as RNS_DIST_CACHE_FIT=1 allows" if design.cache_fit else "the code's default 2 slots (no FIT)"))))
    if a.target:
        target(a, design); return
    if a.max:
        print("%-4s | %14s %10s %8s %8s | %14s %10s %8s %8s   (min: without / with the part file at %.2f GB/s)" % ("g", "max D @502 GB", "digits", "no-write", "write", "max D @480 GB", "digits", "no-write", "write", a.write_bw))
        for g in a.g:
            cells = []
            for budget in (M.NODE_GB, M.NODE_GB_MARGIN):
                D = M.max_digits(g, budget, a.tree, a.groups, staging=a.staging, design=design)
                e = estimate(g, D, a.tree, a.groups, fab, a.rule, staging=a.staging, design=design, corrections=a.corrections) if D else None
                cells.append("%14.2e %10.3e %8.1f %8.1f" % (D, e["digits"], e["nowrite_min"], e["minutes"]) if e else "%14s %10s %8s %8s" % ("-", "-", "-", "-"))
            print("%-4d | %s | %s" % (g, cells[0], cells[1]))
        return
    if a.verbose:
        for g in a.g:
            for D in a.D: estimate(g, D, a.tree, a.groups, fab, a.rule, staging=a.staging, verbose=True, design=design, corrections=a.corrections)
    table(a.g, a.D, a.tree, a.groups, fab, a.rule, a.staging, design, a.corrections)
    print()
    print("labels:")
    for k, v in LABELS.items(): print("  %-8s %s" % (k, v))

def target(a, design):
    """Phase 15: the standing estimate at 576 nodes -- two walls (D3), the part file at 2.0 / 0.8 / 0.6 GB/s, the node memory, the grid step, the
    memory ceiling at 480 / 502 GB per node"""
    groups = a.groups or '2,4,8,16,32,64,192,576'
    fabs = [(bw, M.Fabric(M.TARGET.name, a.bw, a.lat, group=a.group, layers=a.layers, taper=a.taper, write_bw=bw)) for bw in M.TARGET_WRITE_BWS]
    print("the standing estimate, 576 nodes, MN_GROUPS %s (modelled; the fabric assumed: %.0f GB/s per APU, %.1f us per message; the part file at %s GB/s per node --"
          " 2.0 the old assumption, 0.6 / 0.8 the target's Lustre prior: 0.58-0.64 GB/s single-stream write measured there, 0.78-0.86 read):" % (groups, a.bw, a.lat * 1e6, ' / '.join('%g' % b for b, f in fabs)))
    print("  %-10s %-44s | %9s | %s | %s | %s" % ("digits", "", "no write", " | ".join("write @%.1f" % b for b, f in fabs), "pieces tree_max + recip + div", "node GB (device + host; pool) [mn cache slots]"))
    for T, what in ((M.TARGET_DIGITS, "the target (the last size below the step)"), (5.167e13, "the second test size (the fallback; the user, 2026-10-03)"),
                    (5.28e13, "the step above the target (recip 51 -> 53, div 20 -> 28)"), (5.39e13, "the next step (tree 86 -> 88)"), (5.74e13, "the next (tree 88 -> 108)"),
                    (M.TARGET_BELOW, "one step below (test after the headline)"), (4.75e13, "its step (4.74 -> 4.75e13)"),
                    (4.25e13, "the target until 2026-09-27 23:50 EDT")):
        es = [estimate(576, T / 576, a.tree, groups, f, a.rule, staging=a.staging, design=design) for b, f in fabs]
        p = M.plan(576, T, design); e = es[0]
        print("  %.3e %-44s | %5.1f s %s | %s | %3d + %2d + %2d = %3d | %5.1f (%5.1f + %4.1f; %.1f)%s" % (
            T, what, e["nowrite_s"], "(%.2f min)" % e["nowrite_min"], " | ".join("%5.1f s (%.2f min)" % (x["wall_s"], x["minutes"]) for x in es), p["tree_max"], p["recip"], p["div"],
            p["tree_max"] + p["recip"] + p["div"], e["node_gb"], e["device_gb"], e["host_gb"], e["shmem_pool_gb"], "" if e["fits_margin"] else "  (over 480 GB)")
            + (" [%d slot%s%s]" % (e["cache_slots"], "" if e["cache_slots"] == 1 else "s", ", + %.1f GB -> node %.1f GB%s" % (e["cache_gb"], e["node_cache_gb"], "" if e["node_cache_gb"] <= M.NODE_GB_MARGIN else ": does not fit 480") if e["cache_gb"] else "") if design is not None and design.p15b else ""))
    for TT in (M.TARGET_DIGITS, M.TARGET_BELOW):
        by_phase(a, design, groups, fabs, TT)
    if design is not None and design.p15c and a.cache_slots is None:
        cache_rows(a, design, groups, fabs)
        partial_row(a)
    print("  the ceiling by memory (the largest D per node whose node peak fits; the walls at that size, which is past the grid steps):")
    for budget in (M.NODE_GB_MARGIN, M.NODE_GB):
        D = M.max_digits(576, budget, a.tree, groups, staging=a.staging, design=design)
        es = [estimate(576, D, a.tree, groups, f, a.rule, staging=a.staging, design=design) for b, f in fabs]
        p = M.plan(576, D * 576, design)
        print("    %.0f GB: D %.2e per node -> %.3e digits: %.1f s without the write; %s with it; pieces %d + %d + %d; node %.1f GB" % (
            budget, D, D * 576, es[0]["nowrite_s"], " / ".join("%.1f s @%.1f" % (x["wall_s"], b) for x, (b, f) in zip(es, fabs)), p["tree_max"], p["recip"], p["div"], es[0]["node_gb"]))

def partial_row(a):
    """Phase 15 int15j (2026-09-29, the user's decisions 2 and 3): the launch line carries RNS_DIST_CACHE_PARTIAL=1 (PC: the slots' primes per
    grid product from the block pool's free bytes, P24 cached, the loop along the longer axis) and MN_OUT_DKM_HI=1 (EW: the writer on
    X_hi after step 1; the model's DKM_HI term is read from the MN_OUT_DKM_HI / MN_MODEL_DKM_HI environment -- set it for the launch
    line's figure).  The row is mn_model.cache_partial's 'P24 cached yes, loop long, the pool rule' line at the target: the standing estimate."""
    import io, contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf): (b0, bw), rows = M.cache_partial(wbs=(2.0, a.write_bw, 0.6))
    r = [x for x in rows if x[0] and x[1] == 'long' and x[4] == 'the pool rule']
    if not r: return
    p24c, lp, kt, kd, tag, r0, rw = r[0]
    print("  the launch line's RNS_DIST_CACHE_PARTIAL=1 (int15j; the pool rule: tree %d / division %d primes per slot, P24 cached, the loop along the longer axis)%s:"
          % (kt, kd, " with MN_OUT_DKM_HI=1" if M.DKM_HI else " (MN_OUT_DKM_HI=1 not set in the environment: its -7 s with the write is not in this row)"))
    print("    %.3e the target                               | %5.1f s (%.2f min) | %s | gain %+.1f s without the write, %s with it"
          % (M.TARGET_DIGITS, r0, r0 / 60, " | ".join("%5.1f s (%.2f min)" % (x, x / 60) for x in rw), r0 - b0, " / ".join("%+.1f" % (x - y) for x, y in zip(rw, bw))))

def cache_rows(a, design, groups, fabs):
    """Phase 15 DOC2: at the target and one step below -- the mn transform cache at 0 (what RNS_DIST_CACHE_FIT allows), 1 and 2 slots (forced: their bytes on
    the node), the launch line without FIT (the code's default 2 slots, in no budget: TC15), and ECALC_NP=4 in place of auto"""
    print("  the mn transform cache and the prime count (modelled; a slot = 4 APUs x 4 primes x 2^29 limbs x 8 B = 68.7 GB per node; FIT's room = 480 - the layout node - 32 GB):")
    print("    %-10s %-58s | %9s | %s | %s" % ("digits", "", "no write", " | ".join("write @%.1f" % b for b, f in fabs), "node GB (with the slots)"))
    import copy
    np4 = copy.copy(design); np4.np_mn = 4
    for T in (M.TARGET_DIGITS, M.TARGET_BELOW):
        rows = []
        for name, force, dz in (("0 slots: what RNS_DIST_CACHE_FIT=1 allows (the launch line)", None, design),
                                ("1 slot (forced; does not fit)", 1, design), ("2 slots (forced; does not fit)", 2, design),
                                ("ECALC_NP=4 instead of auto (0 slots)", None, np4)):
            M.CACHE_FORCE = force; M._PC.clear()
            try: es = [estimate(576, T / 576, a.tree, groups, f, a.rule, staging=a.staging, design=dz) for b, f in fabs]
            finally: M.CACHE_FORCE = None; M._PC.clear()
            e = es[0]; nc = e["node_cache_gb"]
            if T == M.TARGET_DIGITS and force is None and dz is design:
                print("    (FIT at %.3ge13: room %.2f GB = 480 - the layout node %.2f - 32 -> %d slots)" % (T / 1e13, e["mem"]["cache_fit_room"], e["mem"]["layout_node"], e["mem"]["cache_fit_slots"]))
            print("    %.3e %-58s | %5.1f s (%.2f min) | %s | %5.1f%s" % (T, name, e["nowrite_s"], e["nowrite_min"], " | ".join("%5.1f s" % x["wall_s"] for x in es), nc,
                  "" if nc <= M.NODE_GB_MARGIN else "  (over 480 GB)"))

def by_phase(a, design, groups, fabs, T):
    """the by-phase line at T total digits on 576 nodes (Phase 15 TGT: the target and the size one step below)"""
    e = estimate(576, T / 576, a.tree, groups, fabs[-1][1], a.rule, staging=a.staging, design=design)
    fm = M.DC_FMT_MN * T / 576 / 1e9
    b15 = design is not None and design.p15b; bpd = M.PACKED_BPD if (b15 and design.packed) else 1.0; gbn = T / 576 * bpd / 1e9
    print("  %.3ge13 by phase (modelled): init %.1f + seed wait %.1f + batch %.1f + top %.1f + distributed levels %.1f + reciprocal %.1f + division %.1f + other %.1f + the digits' formatting"
          " and residues %.1f%s = %.1f s without the write; the part file %.1f GB per node (%s), %s: %s s of writing" % (T / 1e13, e["init"], e["seed_wait"], e["batch"],
          e["top"], e["levels"], e["recip"], e["div"], e["other"], fm, " + the exit %.1f" % M.EXIT_S if b15 else "", e["nowrite_s"], gbn, "packed, 0.444 B/digit" if bpd < 1 else "ASCII",
          "started at the division's hook, %.1f s of it hidden under the division (MN_OUT_EARLY)" % (M.OVL1 * e["div"]) if b15 and design.early else "written after T1 with the formatting in a pipeline",
          " / ".join("%.1f @%.1f" % (gbn / b, b) for b, f in fabs)))

if __name__ == "__main__":
    main()

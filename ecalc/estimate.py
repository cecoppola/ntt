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
    "output": "the part file at --write-bw GB/s per node: 0.6 = the target's Lustre single-stream rate, MEASURED there (the apumult catalog: 0.58-0.64 write, 0.78-0.86 read), ASSUMED to hold with 576 nodes writing at once; 2.0 = the node-local NVMe assumed before.  Exposed whole at size > 1 (mn_out_run after T1: nothing under the division -- the model before Phase 15 hid half the division)",
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
               tree=tree, staging=staging, schedule=r["schedule"], mem=m)
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
    ap.add_argument("--bw", type=float, default=100.0, help="GB/s per APU injection (assumed: PLAN 25's two 400 Gb/s NICs)")
    ap.add_argument("--lat", type=float, default=2e-6, help="seconds per message (assumed)")
    ap.add_argument("--group", type=int, default=64, help="nodes per dragonfly group (MN_TOPO_GROUP)")
    ap.add_argument("--layers", type=int, default=2, choices=(2, 3))
    ap.add_argument("--taper", type=float, default=1.0)
    ap.add_argument("--write-bw", type=float, default=M.TARGET_WRITE_BW, help="GB/s per node for the part file (Phase 15: 0.6 = the target's Lustre /ssd0 single-stream, measured there; 2.0 was assumed before)")
    ap.add_argument("--round-mb", type=float, default=1024, help="COMM_SHMEM_ROUND_MB on the launch line (D2: 1024; 0 = the code's default, off)")
    ap.add_argument("--out-overlap", default="none", choices=("none", "half"), help="size > 1: the part file after T1 (none: the code) or under half the division (half: the model before Phase 15)")
    ap.add_argument("--corrections", type=int, default=0, help="size 1: the division's corrections (data-dependent; 2 at 1e11 on the defaults)")
    ap.add_argument("--p13", action="store_true", help="the Phase 13/14 model (no Phase 15 terms: the old memory forms, no recip cut, no CAL15, the part file under half the division)")
    ap.add_argument("--max", action="store_true", help="the largest D per node that fits 502 and 480 GB at each g, with its wall")
    ap.add_argument("--target", action="store_true", help="Phase 13d D2: the standing estimate at 576 nodes -- 4.25e13 (the target since Phase 13d; 4.4e13 was the Phase 13c target), the proposed 4.25e13, and the step")
    ap.add_argument("--verbose", action="store_true", help="the per-phase, per-level breakdown of every run")
    ap.add_argument("--np", type=int, default=3, choices=(3, 4), help="ECALC_NP (Phase 13b step 0: 3)")
    ap.add_argument("--strategy", default="auto", choices=M.STRATEGIES, help="RNS_STRATEGY (agent B, Phase 13b)")
    ap.add_argument("--cap", default="2^31", choices=list(mem_model.CAPS) + ["rule"], help="the plane cap (default 2^31 since Phase 13c; rule = the pre-13c size rule)")
    ap.add_argument("--chunk", default="both", choices=M.CHUNKS, help="off | shift (MDB_SHIFT_CHUNK_MB) | both (+ MN_T_CHUNK_MB, the default since Phase 14), at --chunk-mb")
    ap.add_argument("--chunk-mb", type=float, default=M.CHUNK_MB)
    ap.add_argument("--depth", type=int, default=2, choices=(1, 2), help="the uneven exchange's depth (agent X, Phase 13b)")
    ap.add_argument("--modmul", type=int, default=1, choices=(0, 1), help="NTT_MODMUL (step 0: 1)")
    ap.add_argument("--legacy", action="store_true", help="the Phase 12 model: four primes, the Phase 10/11 phase table")
    a = ap.parse_args()
    if a.as_is: a.tree, a.staging = "flat", "cached"
    design = None if a.legacy else M.Design(np=a.np, strategy=a.strategy, cap=mem_model.CAPS[a.cap] if a.cap and a.cap != "rule" else None, chunk=a.chunk, depth=a.depth, modmul=a.modmul, chunk_mb=a.chunk_mb,
                                            p15=not a.p13, round_mb=a.round_mb, out_overlap=a.out_overlap if not a.p13 else None)
    fab = M.Fabric(M.TARGET.name, a.bw, a.lat, group=a.group, layers=a.layers, taper=a.taper, write_bw=a.write_bw)
    print("ecalc estimate -- %s; tree form %s, SHMEM staging %s, MN_GROUPS %s, fabric %.0f GB/s per APU, %.1f us per message, dragonfly group %d, %d layers, taper %.2f, part files %.2f GB/s per node"
          % ("legacy (Phase 12: four primes)" if design is None else "design %s, ECALC_NP=%d, NTT_MODMUL=%d%s" % (design.name(), design.np, design.modmul,
             ", COMM_SHMEM_ROUND_MB=%g, the part file %s" % (design.round_mb, "after T1" if design.out_overlap == 'none' else "under half the division") if design.p15 else " (the Phase 13/14 model)"),
             a.tree, a.staging, a.groups or "(default)", a.bw, a.lat * 1e6, a.group, a.layers, a.taper, a.write_bw))
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
    print("  %-10s %-44s | %9s | %s | %s | %s" % ("digits", "", "no write", " | ".join("write @%.1f" % b for b, f in fabs), "pieces tree_max + recip + div", "node GB (device + host; pool)"))
    for T, what in ((4.25e13, "the target (1.2 % below the step)"), (4.29e13, "the last size below the step"), (4.30e13, "the step's first size (4.29 -> 4.30e13)"),
                    (4.4e13, "the Phase 13c target, past two steps")):
        es = [estimate(576, T / 576, a.tree, groups, f, a.rule, staging=a.staging, design=design) for b, f in fabs]
        p = M.plan(576, T, design); e = es[0]
        print("  %.3e %-44s | %5.1f s %s | %s | %3d + %2d + %2d = %3d | %5.1f (%5.1f + %4.1f; %.1f)%s" % (
            T, what, e["nowrite_s"], "(%.2f min)" % e["nowrite_min"], " | ".join("%5.1f s (%.2f min)" % (x["wall_s"], x["minutes"]) for x in es), p["tree_max"], p["recip"], p["div"],
            p["tree_max"] + p["recip"] + p["div"], e["node_gb"], e["device_gb"], e["host_gb"], e["shmem_pool_gb"], "" if e["fits_margin"] else "  (over 480 GB)"))
    e = estimate(576, 4.25e13 / 576, a.tree, groups, fabs[-1][1], a.rule, staging=a.staging, design=design)
    fm = M.DC_FMT_MN * 4.25e13 / 576 / 1e9
    print("  4.25e13 by phase (modelled): init %.1f + seed wait %.1f + batch %.1f + top %.1f + distributed levels %.1f + reciprocal %.1f + division %.1f + other %.1f + the digits' formatting"
          " and residues %.1f = %.1f s without the write; the part file %.1f GB per node, written after T1 with the formatting in a pipeline: %s s" % (e["init"], e["seed_wait"], e["batch"],
          e["top"], e["levels"], e["recip"], e["div"], e["other"], fm, e["nowrite_s"], 4.25e13 / 576 / 1e9, " / ".join("%.1f @%.1f" % (4.25e13 / 576 / 1e9 / b, b) for b, f in fabs)))
    print("  the ceiling by memory (the largest D per node whose node peak fits; the walls at that size, which is past the grid steps at 4.30 / 4.40e13):")
    for budget in (M.NODE_GB_MARGIN, M.NODE_GB):
        D = M.max_digits(576, budget, a.tree, groups, staging=a.staging, design=design)
        es = [estimate(576, D, a.tree, groups, f, a.rule, staging=a.staging, design=design) for b, f in fabs]
        p = M.plan(576, D * 576, design)
        print("    %.0f GB: D %.2e per node -> %.3e digits: %.1f s without the write; %s with it; pieces %d + %d + %d; node %.1f GB" % (
            budget, D, D * 576, es[0]["nowrite_s"], " / ".join("%.1f s @%.1f" % (x["wall_s"], b) for x, (b, f) in zip(es, fabs)), p["tree_max"], p["recip"], p["div"], es[0]["node_gb"]))

if __name__ == "__main__":
    main()

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
"""
import argparse, math, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mn_model as M
import mem_model

LABELS = {
    "digits": "modelled (D x g; D is the request per node, the run computes to the next multiple of 18)",
    "wall": "modelled: the per-node compute recalibrated on the Phase 13c defaults (Phase 13d D2, mn_model.CAL13: G13d's ten one-node runs 6.6e10-1.16e11 + the 4e10 series + the 7.64e10 share run, `mn_model.py --calib13`; the pipeline's per-product law fitted on G's logs); the top node's leaf (the code's equal-term layout: 1.036 x the average digits at 576) and each tree level's largest group; the distributed levels, reciprocal and division built from S13's 2^31 piece x the pipeline factor (1.22, assumed to carry) and the fabric",
    "steps": "modelled = the C code's own plan (agent L's MN_PLAN_ONLY; mn_model.plan equals it at all 401 sizes 2.0-6.0e13): at the 2^31 cap the tree levels' largest groups step 88 -> 124 pieces over 4.30-4.39e13",
    "exposed": "modelled: the fabric time not hidden under the local passes -- assumes 100 GB/s per APU, 2 us per message (--bw, --lat), the general map's overlap (GEN_HIDE)",
    "fabric": "modelled: bytes through the node's eight NICs and over the dragonfly's global links (MN_TOPO_GROUP = 64, two-layer all-to-all)",
    "device": "modelled: mem_model (the code's own sizing formulas; measured to 0.05 % at size 1: 4e10 313.3 GB at four primes / 287.5 at three, 8e10 369.1, 1e11 431.2; at 10^10/4 19.9 GB arena)",
    "host": "modelled: 7 GB runtime + 4 GiB staging + the seed buffers at init + 6 GB per process of transport + the SHMEM pool ('pool': the larger of COMM_SHMEM_POOL_MB and the staging the transport needs -- --staging resident 0 / per_exchange one exchange / cached every live communicator's)",
    "fits": "modelled against 502 GB per node (the MI300A's usable HBM; 480 GB = the safe budget with a 5 % margin)",
    "output": "assumed: the part file at --write-bw GB/s per node (2 GB/s), exposed beyond half the division",
}

def estimate(g, D, tree="grid", groups=None, fabric=None, rule="model", transport="shmem", staging="resident", verbose=False, design=M.DEFAULT):
    """the estimate for g nodes at D digits per node: a dict of the numbers (seconds, GB, bytes) and their labels.
    design: an mn_model.Design (default: the code after Phase 13b step 0); None = the legacy (Phase 12) model"""
    fab = fabric or M.TARGET
    r = M.run(fab, D, g, rule, verbose=verbose, groups=groups, form=tree, transport=transport, staging=staging, design=design)
    m = r["mem"]
    out = dict(g=g, D=D, digits=r["digits"], wall_s=r["wall"], minutes=r["wall"] / 60,
               init=r["init"], batch=r["batch"], top=r["top"], levels=r["levels"], recip=r["recip"], div=r["div"], out=r["out"],
               exposed_s=r["exposed"], exposed_pct=100 * r["exposed"] / r["wall"],
               nic_bytes=r["nic"], nic_per_nic=r["nic"] / 8, global_bytes=r["glob"], msgs_apu=r["msgs"], pieces=r["pieces"],
               device_gb=m["device"], host_gb=m["host"], node_gb=m["node"], shmem_pool_gb=m["shmem_pool"], fits=m["node"] <= M.NODE_GB, fits_margin=m["node"] <= M.NODE_GB_MARGIN,
               tree=tree, staging=staging, schedule=r["schedule"], mem=m)
    return out

def fmt_b(b): return M.fmt_b(b)

def table(gs, Ds, tree, groups, fab, rule, staging="resident", design=M.DEFAULT):
    print("%-4s %-8s %-10s | %7s %6s %6s %5s | %6s %6s %6s %6s | %8s %8s %8s | %s" % ("g", "D/node", "digits", "wall s", "min", "expo s", "%", "dev GB", "hst GB", "pool", "node", "NIC/node", "per NIC", "global", "fits 502 / 480 GB"))
    rows = []
    for g in gs:
        for D in Ds:
            e = estimate(g, D, tree, groups, fab, rule, staging=staging, design=design)
            rows.append(e)
            print("%-4d %-8.1e %-10.3e | %7.1f %6.1f %6.1f %5.0f | %6.0f %6.0f %6.0f %6.0f | %8s %8s %8s | %s / %s" % (
                g, D, e["digits"], e["wall_s"], e["minutes"], e["exposed_s"], e["exposed_pct"], e["device_gb"], e["host_gb"], e["shmem_pool_gb"], e["node_gb"],
                fmt_b(e["nic_bytes"]), fmt_b(e["nic_per_nic"]), fmt_b(e["global_bytes"]), "yes" if e["fits"] else "NO", "yes" if e["fits_margin"] else "no"))
    return rows

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--g", type=int, nargs="*", default=[1, 4, 64, 576])
    ap.add_argument("--D", type=float, nargs="*", default=[4e10, 7.7e10, 1e11])
    ap.add_argument("--tree", default="grid", choices=("grid", "flat"), help="the top product's memory form: grid (Phase 12 G) or flat (the code at 7aded87)")
    ap.add_argument("--groups", default=None, help="MN_GROUPS (e.g. 2,4,8,16,32,64,192,576); default = the code's schedule")
    ap.add_argument("--staging", default="resident", choices=("resident", "per_exchange", "cached"), help="the SHMEM transport's staging in its pool: resident (Phase 12 S: the callers' slabs in the pool, no staging), per_exchange (the staging freed after each wait), cached (the code at 7aded87: kept per live communicator -- 345 GB per node at 576)")
    ap.add_argument("--as-is", action="store_true", help="the code at main 7aded87: --tree flat --staging cached")
    ap.add_argument("--rule", default="model", choices=("model", "full"))
    ap.add_argument("--bw", type=float, default=100.0, help="GB/s per APU injection (assumed: PLAN 25's two 400 Gb/s NICs)")
    ap.add_argument("--lat", type=float, default=2e-6, help="seconds per message (assumed)")
    ap.add_argument("--group", type=int, default=64, help="nodes per dragonfly group (MN_TOPO_GROUP)")
    ap.add_argument("--layers", type=int, default=2, choices=(2, 3))
    ap.add_argument("--taper", type=float, default=1.0)
    ap.add_argument("--write-bw", type=float, default=2.0, help="GB/s per node for the part file (assumed)")
    ap.add_argument("--max", action="store_true", help="the largest D per node that fits 502 and 480 GB at each g, with its wall")
    ap.add_argument("--target", action="store_true", help="Phase 13d D2: the standing estimate at 576 nodes -- 4.4e13 (the Phase 13c target), the proposed 4.25e13, and the step")
    ap.add_argument("--verbose", action="store_true", help="the per-phase, per-level breakdown of every run")
    ap.add_argument("--np", type=int, default=3, choices=(3, 4), help="ECALC_NP (Phase 13b step 0: 3)")
    ap.add_argument("--strategy", default="auto", choices=M.STRATEGIES, help="RNS_STRATEGY (agent B, Phase 13b)")
    ap.add_argument("--cap", default="2^31", choices=list(mem_model.CAPS) + ["rule"], help="the plane cap (default 2^31 since Phase 13c; rule = the pre-13c size rule)")
    ap.add_argument("--chunk", default="shift", choices=M.CHUNKS, help="off | shift (MDB_SHIFT_CHUNK_MB) | both (+ MN_T_CHUNK_MB), at --chunk-mb")
    ap.add_argument("--chunk-mb", type=float, default=M.CHUNK_MB)
    ap.add_argument("--depth", type=int, default=2, choices=(1, 2), help="the uneven exchange's depth (agent X, Phase 13b)")
    ap.add_argument("--modmul", type=int, default=1, choices=(0, 1), help="NTT_MODMUL (step 0: 1)")
    ap.add_argument("--legacy", action="store_true", help="the Phase 12 model: four primes, the Phase 10/11 phase table")
    a = ap.parse_args()
    if a.as_is: a.tree, a.staging = "flat", "cached"
    design = None if a.legacy else M.Design(np=a.np, strategy=a.strategy, cap=mem_model.CAPS[a.cap] if a.cap and a.cap != "rule" else None, chunk=a.chunk, depth=a.depth, modmul=a.modmul, chunk_mb=a.chunk_mb)
    fab = M.Fabric(M.TARGET.name, a.bw, a.lat, group=a.group, layers=a.layers, taper=a.taper, write_bw=a.write_bw)
    print("ecalc estimate -- %s; tree form %s, SHMEM staging %s, MN_GROUPS %s, fabric %.0f GB/s per APU, %.1f us per message, dragonfly group %d, %d layers, taper %.2f, part files %.1f GB/s per node"
          % ("legacy (Phase 12: four primes)" if design is None else "design %s, ECALC_NP=%d, NTT_MODMUL=%d" % (design.name(), design.np, design.modmul),
             a.tree, a.staging, a.groups or "(default)", a.bw, a.lat * 1e6, a.group, a.layers, a.taper, a.write_bw))
    if a.target:
        print("the standing estimate, 576 nodes (modelled; the fabric assumed: %.0f GB/s per APU, %.1f us per message):" % (a.bw, a.lat * 1e6))
        for T, what in ((4.25e13, "proposed target, 1.2 % below the step"), (4.29e13, "the last size below the step"), (4.30e13, "the step's first size"), (4.4e13, "the Phase 13c target")):
            e = estimate(576, T / 576, a.tree, a.groups, fab, a.rule, staging=a.staging, design=design)
            p = M.plan(576, T, design)
            print("  %.3e digits (%s): %.1f s = %.2f min; pieces tree_max %d + recip %d + div %d = %d; node %.0f GB (device %.0f + host %.0f)%s" % (
                T, what, e["wall_s"], e["minutes"], p["tree_max"], p["recip"], p["div"], p["tree_max"] + p["recip"] + p["div"], e["node_gb"], e["device_gb"], e["host_gb"],
                "" if e["fits_margin"] else "  (over 480 GB)"))
        return
    if a.max:
        print("%-4s | %14s %10s %8s | %14s %10s %8s" % ("g", "max D @502 GB", "digits", "min", "max D @480 GB", "digits", "min"))
        for g in a.g:
            cells = []
            for budget in (M.NODE_GB, M.NODE_GB_MARGIN):
                D = M.max_digits(g, budget, a.tree, a.groups, staging=a.staging, design=design)
                e = estimate(g, D, a.tree, a.groups, fab, a.rule, staging=a.staging, design=design) if D else None
                cells.append("%14.2e %10.3e %8.1f" % (D, e["digits"], e["minutes"]) if e else "%14s %10s %8s" % ("-", "-", "-"))
            print("%-4d | %s | %s" % (g, cells[0], cells[1]))
        return
    if a.verbose:
        for g in a.g:
            for D in a.D: estimate(g, D, a.tree, a.groups, fab, a.rule, staging=a.staging, verbose=True, design=design)
    table(a.g, a.D, a.tree, a.groups, fab, a.rule, a.staging, design)
    print()
    print("labels:")
    for k, v in LABELS.items(): print("  %-8s %s" % (k, v))

if __name__ == "__main__":
    main()

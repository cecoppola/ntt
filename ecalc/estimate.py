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
"""
import argparse, math, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mn_model as M
import mem_model

LABELS = {
    "digits": "modelled (D x g; D is the request per node, the run computes to the next multiple of 18)",
    "wall": "modelled: init/batch/top levels measured at size 1 (interpolated: 4e10 81.5 s, 7e10 153.5, 8e10 195.5, 1e11 262.9); the distributed levels, reciprocal and division built from the measured 2^31 piece (1.11 s) and the fabric",
    "exposed": "modelled: the fabric time not hidden under the local passes -- assumes 100 GB/s per APU, 2 us per message (--bw, --lat), the general map's overlap (GEN_HIDE)",
    "fabric": "modelled: bytes through the node's eight NICs and over the dragonfly's global links (MN_TOPO_GROUP = 64, two-layer all-to-all)",
    "device": "modelled: mem_model (the code's own sizing formulas; measured to the byte at size 1: 4e10 253 GB, 8e10 369, 1e11 431; at 10^10/4 20.3 GB arena)",
    "host": "modelled: 7 GB runtime + 4 GiB staging + the seed buffers at init + 6 GB per process of transport + the SHMEM pool ('pool': the larger of COMM_SHMEM_POOL_MB and the staging the transport needs -- --staging resident 0 / per_exchange one exchange / cached every live communicator's)",
    "fits": "modelled against 502 GB per node (the MI300A's usable HBM; 480 GB = the safe budget with a 5 % margin)",
    "output": "assumed: the part file at --write-bw GB/s per node (2 GB/s), exposed beyond half the division",
}

def estimate(g, D, tree="grid", groups=None, fabric=None, rule="model", transport="shmem", staging="resident", verbose=False):
    """the estimate for g nodes at D digits per node: a dict of the numbers (seconds, GB, bytes) and their labels"""
    fab = fabric or M.TARGET
    r = M.run(fab, D, g, rule, verbose=verbose, groups=groups, form=tree, transport=transport, staging=staging)
    m = r["mem"]
    out = dict(g=g, D=D, digits=r["digits"], wall_s=r["wall"], minutes=r["wall"] / 60,
               init=r["init"], batch=r["batch"], top=r["top"], levels=r["levels"], recip=r["recip"], div=r["div"], out=r["out"],
               exposed_s=r["exposed"], exposed_pct=100 * r["exposed"] / r["wall"],
               nic_bytes=r["nic"], nic_per_nic=r["nic"] / 8, global_bytes=r["glob"], msgs_apu=r["msgs"], pieces=r["pieces"],
               device_gb=m["device"], host_gb=m["host"], node_gb=m["node"], shmem_pool_gb=m["shmem_pool"], fits=m["node"] <= M.NODE_GB, fits_margin=m["node"] <= M.NODE_GB_MARGIN,
               tree=tree, staging=staging, schedule=r["schedule"], mem=m)
    return out

def fmt_b(b): return M.fmt_b(b)

def table(gs, Ds, tree, groups, fab, rule, staging="resident"):
    print("%-4s %-8s %-10s | %7s %6s %6s %5s | %6s %6s %6s %6s | %8s %8s %8s | %s" % ("g", "D/node", "digits", "wall s", "min", "expo s", "%", "dev GB", "hst GB", "pool", "node", "NIC/node", "per NIC", "global", "fits 502 / 480 GB"))
    rows = []
    for g in gs:
        for D in Ds:
            e = estimate(g, D, tree, groups, fab, rule, staging=staging)
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
    ap.add_argument("--verbose", action="store_true", help="the per-phase, per-level breakdown of every run")
    a = ap.parse_args()
    if a.as_is: a.tree, a.staging = "flat", "cached"
    fab = M.Fabric(M.TARGET.name, a.bw, a.lat, group=a.group, layers=a.layers, taper=a.taper, write_bw=a.write_bw)
    print("ecalc estimate -- tree form %s, SHMEM staging %s, MN_GROUPS %s, fabric %.0f GB/s per APU, %.1f us per message, dragonfly group %d, %d layers, taper %.2f, part files %.1f GB/s per node"
          % (a.tree, a.staging, a.groups or "(default)", a.bw, a.lat * 1e6, a.group, a.layers, a.taper, a.write_bw))
    if a.max:
        print("%-4s | %14s %10s %8s | %14s %10s %8s" % ("g", "max D @502 GB", "digits", "min", "max D @480 GB", "digits", "min"))
        for g in a.g:
            cells = []
            for budget in (M.NODE_GB, M.NODE_GB_MARGIN):
                D = M.max_digits(g, budget, a.tree, a.groups, staging=a.staging)
                e = estimate(g, D, a.tree, a.groups, fab, a.rule, staging=a.staging) if D else None
                cells.append("%14.2e %10.3e %8.1f" % (D, e["digits"], e["minutes"]) if e else "%14s %10s %8s" % ("-", "-", "-"))
            print("%-4d | %s | %s" % (g, cells[0], cells[1]))
        return
    if a.verbose:
        for g in a.g:
            for D in a.D: estimate(g, D, a.tree, a.groups, fab, a.rule, staging=a.staging, verbose=True)
    table(a.g, a.D, a.tree, a.groups, fab, a.rule, a.staging)
    print()
    print("labels:")
    for k, v in LABELS.items(): print("  %-8s %s" % (k, v))

if __name__ == "__main__":
    main()

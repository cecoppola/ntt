#!/usr/bin/env python3
"""mn_model.py - the fabric and phase model of the multi-node run; the standing estimate is `mn_model.py estimate`.

Per node, for D digits per node over g nodes: the wall by phase, the bytes on the fabric (per NIC and on the dragonfly's global
links), the exposed (un-hidden) communication, the memory, and the total digits.  The single-node phases come from the measured
table below; the distributed levels, the sharded reciprocal and the sharded division are built product by product (the same grids,
chains and cuts the code forms) from the measured 2^31-point piece product and the fabric profile.

    ./mn_model.py estimate --target          the standing estimate at 576 nodes (profile target_l4)
    ./mn_model.py estimate --g 576 --D 6e10 --groups 2,4,8,16,32,64,192,576 --verbose
    ./mn_model.py estimate --max             the largest D per node that fits at each g
    ./mn_model.py estimate --help            every option (fabric profile, design, cache, write bandwidth)

The default design is DEFAULT15C (the code's defaults of 2026-09-28 on the target's launch line: ECALC_NP=auto, COMM_SHMEM_ROUND_MB=1024,
BS_ARENA_ROOM=0.16, RNS_DIST_CACHE_FIT=1); Two walls everywhere: without
the disk write and with it (part file at --write-bw GB/s per node).  Everything printed as "modelled" is this script's arithmetic;
"measured" cites the RESULTS section; the fabric figures of the target are assumed until measured there.
"""
import argparse, math, sys, os, functools
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mem_model

# ------------------------------------------------------------------------------------------------------------
# MEASURED INPUTS (one MI300A node, four APUs; the code of main @ 7aded87 unless noted)
# ------------------------------------------------------------------------------------------------------------
# single-node phases by digits (seconds): init, batch tier, top levels (device tier), reciprocal, division, wall.
#   1e9, 1e10: X's calibration batch (job 20802, s24-26; archive:results_merged_2026-10-24/TIME_MODEL.md §X)
#   4e10: archive:results_merged_2026-10-24/SINGLE_NODE.md §P gate (81.5 +- 1.4 s, phases 66.0);  7e10: archive:results_merged_2026-10-24/SINGLE_NODE.md §P (153.5 s, ECALC_DM_POOL on)
#   8e10: archive:results_merged_2026-10-24/MEMORY.md §M11 v3/v4 (195.5 s, s24-16; zero hipMalloc)  1e11: archive:results_merged_2026-10-24/MEMORY.md §M11 v4 (262.9 s, s24-26)
PHASES = {          #  init   batch   top    recip   div    wall
    1e9:    dict(init=9.3,  batch=1.5,  top=0.1,  recip=2.76, div=0.38, wall=14.4),
    1e10:   dict(init=10.8, batch=7.1,  top=1.1,  recip=4.78, div=2.98, wall=29.0),
    4e10:   dict(init=15.5, batch=22.5, top=13.1, recip=15.2, div=15.1, wall=81.5),
    7e10:   dict(init=18.4, batch=37.7, top=24.9, recip=32.3, div=35.4, wall=153.5),
    8e10:   dict(init=20.7, batch=44.0, top=38.8, recip=37.9, div=45.2, wall=195.5),
    1e11:   dict(init=25.9, batch=65.5, top=51.4, recip=52.9, div=66.9, wall=262.9),   # M11 v4 log: bs 117.0 (seeds 10.9 + batch 54.6 + mdev 51.4), dm 119.8
}
# the distributed piece product on one node (archive:results_merged_2026-10-24/GRID_GROUPS.md §G, RESULTS 75): a 2^31-point piece (4 primes, 2^29 points
# per APU): load 0.33 + ntt 0.67 + crt/out 0.11 = 1.11 s; with the B operand's transform cached 0.72 s.
T_PIECE_31 = 1.11
T_PIECE_31_BHIT = 0.72
# the four-step transform at 2^31 over the four APUs (results/EXCHANGE.md §C, DIST_STATS, per THREE transforms of one prime):
# rows 0.0624 cols 0.0429 packs 0.0385 exchange (xGMI push) 0.0571, exposed 0.0089 (84 % hidden).
T_XGMI_31 = 0.0571 / 3          # the xGMI stage per transform per prime, 2^29 points per APU
HIDDEN_XGMI = 0.84
NODE_GB = 502.0
NODE_GB_MARGIN = 480.0          # the safe budget (DECISIONS2 7: 480 GB)
LIMB_DIGITS = 18                # decimal limbs (RESULTS 67)
EC_NP = 4
K_CHUNKS = 8                    # the slab pipeline (M7; DIST_CHUNKS)
CACHE_MN_SLOTS = int(os.environ.get('MN_MODEL_CACHE_SLOTS', '2'))   # RNS_DIST_CACHE_MN default over shares (archive:results_merged_2026-10-24/GRID_GROUPS.md §G); Phase 15 CX: MN_MODEL_CACHE_SLOTS=n prices n
                                # slots (0: the cache off -- what the 480 GB budget allows at the target, results/CACHE.md §TC15, CX15.md)
CACHE_MODEL = os.environ.get('MN_MODEL_CACHE', 'code')   # Phase 15 CX (results/CACHE.md §CX15): 'code' = the hits as the code takes them (cache_pieces: the slots'
                                # designation, the cuts' skips, the planes' q) x CACHE_HIT_F; 'old' = the term before CX (at >= 2 slots every piece after
                                # the first with one operand cached; nothing at 1 slot)
CACHE_HIT_F = 0.96              # Phase 15 CX: a hit's saving as a fraction of its piece, measured over modelled (piece_cost fwd=2 - fwd=1 over fwd=2 on the aac6
                                # fabric, the same pieces) -- FITTED on aac6, results/CACHE.md §CX15: tests/cx_grid (the target's 9 grid shapes at cap 2^27, cache
                                # 0/1/2 interleaved; 489 hit pieces) 1.043 at 2 node-processes, 0.976 at 4; ecalc 1e10 end to end (the hit pieces against
                                # themselves in the cache-off run) 0.965 at 2 (4 runs), 0.840 at 4 (4 runs); mean 0.956.  The model's structure (1 of the 3
                                # transforms per prime, one redistribution) holds within -16..+4 %; ASSUMED to carry to the target's fabric
# Phase 15 CX proposals (results/CACHE.md §CX15 section 3; off unless set): CACHE_PRIMES = k -> a slot holds k of the piece's primes (k / np of a hit's transform
# saving, no redistribution saved); CACHE_LOOP = 'long' -> mn_grid's loop along the grid's longer axis (the slots follow); CACHE_SLOTS_PHASE =
# {'tree': n, 'dm': n} -> the slots per phase (the tree's arena slack, the division's room); cache_slots_now() is what _product_cost prices
CACHE_PRIMES = None
CACHE_LOOP = 'code'
CACHE_SLOTS_PHASE = None
CACHE_PHASE_NOW = None
# Phase 15 DOC2 (the user's decision 2 of 2026-09-28: RNS_DIST_CACHE_FIT=1 on the launch line): a design with cache_fit prices the slots FIT allows
# (run() sets CACHE_RUN_SLOTS = min(CACHE_MN_SLOTS, mem_model's cache_fit_slots) for the run: 0 at the target); CACHE_FORCE = n prices n slots
# whatever FIT allows (mn_model.py estimate --cache-slots n: what a slot would be worth; its bytes are added to the node and it is labelled "does not fit")
CACHE_RUN_SLOTS = None
CACHE_FORCE = None
# Phase 15 Batch 3 PC (archive:results_merged_2026-10-24/CACHE.md §PC15; RNS_DIST_CACHE_PARTIAL=1): CACHE_P24 -> the P24 products (MN_P24) use the cache too (PC builds it: the slot's
# key carries the 24-digit form); CACHE_PRIMES_PHASE = {'tree': k, 'dm': k} -> the primes a slot holds per phase (PC: the slot's planes drawn per grid
# product from the block pool's free bytes -- the arena's slack at the tree, DKM's unused hole in the division; cache_partial() derives k per phase)
CACHE_P24 = os.environ.get('MN_MODEL_CACHE_P24', '0') == '1'
CACHE_PART_F = float(os.environ.get('MN_MODEL_CACHE_PART_F', '1.08'))   # PC: a partial hit's measured saving over hit_cost's k / np term -- FITTED on aac6
                                # (archive:results_merged_2026-10-24/CACHE.md §PC15 2.1: cx_grid at 2 procs, k = 1 / 2, 18- and 24-digit: 1.069, 1.093, 1.075, 1.087; the full hits 1.017 / 1.039)
CACHE_PRIMES_PHASE = None
def cache_primes_now():
    if CACHE_PRIMES_PHASE is not None and CACHE_PHASE_NOW in CACHE_PRIMES_PHASE: return CACHE_PRIMES_PHASE[CACHE_PHASE_NOW]
    return CACHE_PRIMES
def cache_slots_now():
    if CACHE_SLOTS_PHASE is not None and CACHE_PHASE_NOW in CACHE_SLOTS_PHASE: return CACHE_SLOTS_PHASE[CACHE_PHASE_NOW]
    if CACHE_FORCE is not None: return CACHE_FORCE
    if CACHE_RUN_SLOTS is not None: return CACHE_RUN_SLOTS
    return CACHE_MN_SLOTS
GEN_HIDE = 0.5                  # ASSUMED: the general map (g not a power of two) keeps one v-exchange in flight (L's open
                                # issue), so only half of the xGMI stage hides the fabric stage (the equal path: all of it)

# ------------------------------------------------------------------------------------------------------------
# the fabric
# ------------------------------------------------------------------------------------------------------------
class Fabric:
    """bw_apu: injection per APU (GB/s); lat: per-message latency (s); group: nodes per dragonfly group (the
    MN_TOPO_GROUP parameter); layers: 2 = the layered all-to-all (APU x node), 3 = APU x node-in-group x group;
    taper: the global tier's bandwidth as a fraction of the group's aggregate injection to each peer group;
    gpu_share: node-processes sharing one node's APUs (aac6 calibration; 1 on the target);
    fixed: a fixed cost per exchange (s) beyond the latency model (the loopback transports' staging and syncs);
    coll_fixed: a fixed cost per small collective (s); write_bw: the part file (GB/s per node).
    staging_bw: GB/s HBM copy rate for a D2H + H2D staging copy of every inter-node byte, None = off (06 EVALUATION item 13 /
    04 open item: ASSUMED -- no transport on the target stages through host memory today; CLI --staging, default off so the
    standing estimate does not move). fall_off: exponent a on a per-group-size fabric fall-off, rate x (g/2)^(-a), None/0 = off
    (CLI --fall-off <a>; WISHLIST / B2's open item 9 -- no aac7 t_comm fall-off measurement is fed in by default)."""
    def __init__(self, name, bw_apu, lat, group=64, layers=2, taper=1.0, gpu_share=1, fixed=0.0, write_bw=2.0, tcp_exp=0.0, coll_fixed=None, target=True,
                 staging_bw=None, fall_off=0.0, v_factor=1.0, k_other=1.0):
        self.k_other = k_other   # L4: the operand-redistribution / result / spill all-to-alls' rate as a fraction of the bulk (transform) exchange's; 1.0 = off (all profiles but aac7_l4)
        self.v_factor = v_factor   # T06: the layv (variable-layout, general-map) transform exchange's rate as a fraction of layeq's; 1.0 = off (all profiles but aac7_s48)
        self.name, self.bw, self.lat, self.group, self.layers, self.taper = name, bw_apu, lat, group, layers, taper
        self.gpu_share, self.fixed, self.write_bw, self.tcp_exp, self.target = gpu_share, fixed, write_bw, tcp_exp, target
        self.coll_fixed = fixed if coll_fixed is None else coll_fixed
        self.staging_bw = staging_bw; self.fall_off = fall_off or 0.0

    def rate(self, bytes_apu, g=None, v=False, other=False):
        """GB/s per APU: constant on the target; the loopback transports' single stream per mesh is slower on small
        exchanges (fitted: rate = bw x (bytes / 1 GiB per APU)^tcp_exp, capped at bw); --fall-off <a>: x (g/2)^(-a) when g
        is given (the per-APU rate falling off with the group size, off by default: a = 0)"""
        r = self.bw if not self.tcp_exp else self.bw * min(1.0, (max(bytes_apu, 1.0) / (1 << 30)) ** self.tcp_exp)
        if self.fall_off and g: r *= (max(g, 2) / 2.0) ** (-self.fall_off)
        if other: r *= self.k_other
        return r * self.v_factor if v else r

    def staging_cost(self, node_bytes):
        """D2H + H2D of every inter-node byte at --staging's HBM copy rate (GB/s); 0 without it (CLI --staging, off by default)"""
        return 2.0 * node_bytes / (self.staging_bw * 1e9) if self.staging_bw else 0.0

    def a2a(self, bytes_apu, g, chunks=1, v=False, other=False):
        """one all-to-all over g nodes with bytes_apu bytes per APU in the slab buffer (all destinations).
        Returns (time, nic_bytes_per_node, global_bytes_per_node, messages_per_apu)."""
        if g <= 1: return 0.0, 0.0, 0.0, 0
        G = self.group
        out = bytes_apu * (g - 1) / g                       # leaves the node
        if self.layers == 3 and g > G:
            ng = math.ceil(g / G)
            in_group = bytes_apu * (G - 1) / g
            cross = bytes_apu * (g - G) / g
            nic = in_group + 3 * cross                      # stage 2 in-group (own + relayed), global, stage 3 in-group
            msgs = chunks * (2 * (G - 1) + (ng - 1))
        else:
            cross = bytes_apu * max(0, g - G) / g if g > G else 0.0
            nic = out
            msgs = chunks * (g - 1)
        t_nic = nic / (self.rate(bytes_apu, g, v, other) * 1e9)
        t_glob = cross / (self.rate(bytes_apu, g, v, other) * 1e9 * self.taper) if cross else 0.0   # the group's aggregate to each peer group = G x 4 x bw x taper / (ng - 1), shared by its G nodes evenly
        t = max(t_nic, t_glob) + msgs * self.lat + self.fixed * chunks + self.staging_cost(nic * 4)
        return t, nic * 4, cross * 4, msgs

    def allgather(self, bytes_apu, g):
        """an all-gather over g nodes: bytes_apu received from every peer (the spills)"""
        if g <= 1: return 0.0, 0.0, 0.0, 0
        recv = bytes_apu * (g - 1)
        cross = bytes_apu * max(0, g - self.group) if g > self.group else 0.0
        t = recv / (self.rate(bytes_apu, g) * 1e9) + (g - 1) * self.lat + self.fixed + self.staging_cost(recv * 4)
        return t, recv * 4, cross * 4, g - 1

    def coll(self, g):
        """a small collective (all-gather of a few words, a max-reduction) over g nodes"""
        return 0.0 if g <= 1 else (g - 1) * self.lat + self.coll_fixed + 20e-6

TARGET_WRITE_BW = 1.0                  # 2026-09-29: the user's Lustre test on the target: ~1 GB/s write per node (ASSUMED to hold with 576 nodes writing at once); before: 0.6 (the apumult catalog's 0.58-0.64 single-stream)
                                       # catalog); ASSUMED to hold with 576 writers at once (the aggregate, ~350 GB/s, is not measured); was 2.0 (node-local NVMe, assumed)
TARGET_WRITE_BWS = (2.0, 1.0, 0.6)     # the rates every target estimate prints: the old assumption, the catalog's read rate as an upper write prior, the write prior
# Target facts (user-supplied 2026-10-12): 576 nodes x 4 MI300A (2304 APUs); 8 Slingshot NICs per node at 400 Gb/s (2 per APU); the file system writes
# ~1 TB/s cumulative into ONE file with 32 processes per node.  TARGET_WRITE_BW stays ecalc's writer (one process per node,
# MN_OUT_THREADS=8 pwrite threads, two part files per node: not the measured mode); TARGET_WRITE_BW_FS is the file system's
# measured ceiling per node (1000 / 576 = 1.736 GB/s), reached only by a writer of that shape (OPPORTUNITIES N8).
TARGET_FS_SINGLE_FILE_GBS = 1000.0     # user-supplied: aggregate GB/s into one file at 32 writers per node (all 576 nodes)
TARGET_WRITE_BW_FS = TARGET_FS_SINGLE_FILE_GBS / 576   # 1.736 GB/s per node (modelled from the user-supplied aggregate)
TARGET_NICS_PER_APU = 2                # user-supplied (8 NICs per node): two NIC endpoints per APU; comm_ofi stripes over the NUMA node's NICs by default
TARGET_NIC_GBS = 50.0                  # user-supplied: 400 Gb/s per NIC (100 GB/s injection peak per APU, 400 GB/s per node); the achieved rate is still for A3 to measure
TARGET_BW_APU = TARGET_NICS_PER_APU * TARGET_NIC_GBS   # 100 GB/s per APU: now the user-supplied peak (was the assumed design value; number unchanged)
TARGET_WRITE_BWS = (TARGET_WRITE_BW_FS,) + TARGET_WRITE_BWS   # every target estimate also prints the file system ceiling (user-supplied 1 TB/s / 576)
# T06 (adopted by the user 2026-10-11): the layv exchange (general-map groups, g not a power of 2) at 65.5 % of the layeq rate, part of the
# standing 576-node estimate (aac7 fit, aac7_fit.json (s48); one parameter, modelled). Only the non-power-of-2 groups are affected.
TARGET_LAYV_FACTOR = 0.655
TARGET = Fabric("Slingshot-2 dragonfly (PLAN 25)", bw_apu=TARGET_BW_APU, lat=2e-6, group=64, layers=2, taper=1.0, write_bw=TARGET_WRITE_BW, v_factor=TARGET_LAYV_FACTOR)
TARGET_DIGITS = mem_model.TARGET_DIGITS   # Phase 15 TGT (the user's decision of 2026-09-27 23:50 EDT): 5.1e13 on 576 nodes (was 4.25e13); mem_model.py holds it
TARGET_BELOW = mem_model.TARGET_BELOW     # the runtime one step below: 3.99e13 (B7ACCT; was 4.74e13, below the 2026-09-29 target)
TARGET_NODES = mem_model.TARGET_NODES
# Phase 16 C (results/TIME_MODEL.md §C16): aac7 -- HPE Cray EX, 4 x MI300A, 4 x Slingshot-11 (one 200 Gb/s Cassini per socket), Cray OpenSHMEMX 11.8.0, ROCm
# 7.0.3, one process per node (one NIC per PE: C16 2.1).  MEASURED on 2 nodes: 3.6 GB/s per APU thread through the put path at the exchange
# sizes ecalc sends (13.5-14.8 GB/s per node aggregate), 8.5 us per put + signal + wait, the part file on NFS at 0.116 GB/s (A16 4).
# `mn_model.py estimate --fabric aac7_c16` (or MN_MODEL_PROFILE=aac7_c16; plain `aac7` is the S48 refit since 2026-10-11) selects it and applies AAC7_CONSTS to the constants above; the TARGET profile and
# every default stay as they are.
AAC7 = Fabric("aac7 Slingshot-11 / Cray OpenSHMEMX (C16)", bw_apu=3.6, lat=8.5e-6, group=64, layers=2, taper=1.0, write_bw=0.116)
# TGTBENCH2 (2026-10-06, results/TARGET.md §TGTBENCH2): constants MEASURED ON THE TARGET by the user (their benchmark harness, not ecalc), applied only by
# apply_profile('target-m') (mn_model.py estimate --fabric target-m); the 'target' profile and every default stay as they are (no silent change).
#   MAP_RATE 0.010 s/GB (m, target, A6/D4, n = 5): the harness's VMM map rate; the unit (per APU-GB as mapped by one APU, or per node-GB) is
#     not stated -- this model's MAP_RATE multiplies node GB (node_phases), so the profile ASSUMES the same unit.  Only the device term's
#     difference from the measured reference moves with it; init + the seed wait is bound by the seeds (the CPU), so the wall does not move.
#   TARGET_DEVICE_EDGE_GB 373.44 (m, target, A6, n = 5, 0 % spread): hipMalloc stops at 93.36 GB per APU; VMM shares the edge (m, aac7, RESULTS 107).
#   TARGET_HOST_PIN_APU_GB 256.0 (m, target): the hipHostMalloc limit per APU -- informational, no ecalc pool is host-pinned at that scale.
# The fabric stays the TARGET's (100 GB/s per APU ASSUMED unless --bw): A3 (injection) and A4 (64-node scaling) are not measured.
TARGET_M_CONSTS = dict(MAP_RATE=0.010)
TARGET_DEVICE_EDGE_GB = 373.44
TARGET_M = Fabric("Slingshot-2 dragonfly (PLAN 25), TGTBENCH2 measured constants", bw_apu=TARGET_BW_APU, lat=2e-6, group=64, layers=2, taper=1.0, write_bw=TARGET_WRITE_BW, v_factor=TARGET_LAYV_FACTOR)
# S48 (2026-10-11): 'aac7_s48' = the aac7 profile refit to the 1/2/4/6-node scaling runs by archive:scripts_2026-10-23/refit_s48.py; it reads ecalc/aac7_fit.json (s48)
# (written by `archive:scripts_2026-10-23/refit_s48.py --write-profile`; absent = the profile is refused).  Selected by --fabric aac7_s48 or its alias --fabric aac7 / MN_MODEL_PROFILE (B01, 2026-10-11: the aac7 default; the old one is aac7_c16).  The estimate.py default (no --fabric) stays the target profile.
AAC7_FIT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'aac7_fit.json')   # {"s48": ..., "l4": ...}: the two refits
AAC7_S48 = Fabric("aac7 S48 refit (placeholder until aac7_fit.json (s48) is read)", bw_apu=AAC7.bw, lat=AAC7.lat, group=64, layers=2, taper=1.0, write_bw=0.116)
# B01 (the user's decision of 2026-10-11): 'aac7' now means the S48 refit (an alias of 'aac7_s48'); the previous aac7 profile (C16: 3.6 GB/s per
# APU thread, AAC7_CONSTS) stays selectable by the name 'aac7_c16'.  No code default of ecalc changes (this is the model only).
# L4 (2026-10-11, results/TIME_MODEL.md §L4): 'aac7_l4' / 'aac7_l4a' = the aac7 profile refit to ALL of 2026-10-11's multi-node runs with the corrected structure
# (tree level = its slowest group, TREE_CRIT; operand/result/spill exchanges at k_other of the bulk rate; layv factor on the non-pow2 groups), per NIC
# launch group: aac7_l4 = the fast group B (bulk rate bw_apu), aac7_l4a = the slow group A (bw_apu x alpha_slow).  Read from ecalc/aac7_fit.json (l4)
# (archive:scripts_2026-10-23/refit_s48.py --l4 --write-profile).  'target_l4' = the target fabric (100 GB/s ASSUMED, as 'target') with the L4 layv factor instead of T06's 0.655.
# aac7_s48 and every other profile stay selectable and unchanged.
AAC7_L4 = Fabric("aac7 L4 refit, NIC group B (placeholder until aac7_fit.json (l4) is read)", bw_apu=AAC7.bw, lat=AAC7.lat, group=64, layers=2, taper=1.0, write_bw=0.116)
AAC7_L4A = Fabric("aac7 L4 refit, NIC group A (placeholder)", bw_apu=AAC7.bw, lat=AAC7.lat, group=64, layers=2, taper=1.0, write_bw=0.116)
TARGET_L4 = Fabric("Slingshot-2 dragonfly (PLAN 25), L4 layv factor (placeholder)", bw_apu=TARGET_BW_APU, lat=2e-6, group=64, layers=2, taper=1.0, write_bw=TARGET_WRITE_BW, v_factor=TARGET_LAYV_FACTOR)
PROFILES = {'target': TARGET, 'aac7_c16': AAC7, 'target-m': TARGET_M, 'aac7_s48': AAC7_S48, 'aac7_l4': AAC7_L4, 'aac7_l4a': AAC7_L4A, 'target_l4': TARGET_L4}
PROFILE_ALIASES = {'aac7': 'aac7_s48'}
def apply_profile(name):
    """Phase 16 C: select the fabric profile and set the measured constants of that machine (an env override, MN_MODEL_*, still wins).
    Returns the Fabric.  TGTBENCH2: 'target-m' = the target fabric with TARGET_M_CONSTS (measured on the target).
"""
    global HIDE_POW2, T_ROUND, MAP_RATE, NODE_SCALE, INIT_SCALE
    name = (name or 'target').lower()
    name = PROFILE_ALIASES.get(name, name)                                # B01: aac7 = aac7_s48
    if name not in PROFILES: raise SystemExit('mn_model: unknown profile %s (target, aac7 = aac7_s48, aac7_c16 (the previous aac7), target-m, aac7_s48, aac7_l4, aac7_l4a, target_l4)' % name)
    if name == 'target-m':
        if 'MN_MODEL_MAP_RATE' not in os.environ: MAP_RATE = TARGET_M_CONSTS['MAP_RATE']
    if name == 'aac7_s48':
        import json
        if not os.path.exists(AAC7_FIT_FILE): raise SystemExit('mn_model: profile aac7_s48 needs %s (restore archive:scripts_2026-10-23/refit_s48.py and run it with --write-profile)' % AAC7_FIT_FILE)
        j = json.load(open(AAC7_FIT_FILE))['s48']
        PROFILES[name] = Fabric("aac7 S48 refit (archive:scripts_2026-10-23/refit_s48.py)", bw_apu=j['bw_apu'], lat=j['lat'], group=64, layers=2, taper=1.0, write_bw=j.get('write_bw', 0.116), v_factor=j.get('layv_factor', 1.0))
    if name in ('aac7_l4', 'aac7_l4a', 'target_l4'):
        import json
        if not os.path.exists(AAC7_FIT_FILE): raise SystemExit('mn_model: profile %s needs %s (restore archive:scripts_2026-10-23/refit_s48.py and run it with --l4 --write-profile)' % (name, AAC7_FIT_FILE))
        j = json.load(open(AAC7_FIT_FILE))['l4']
        if name == 'target_l4':
            PROFILES[name] = Fabric("Slingshot-2 dragonfly (PLAN 25), L4 layv factor", bw_apu=TARGET_BW_APU, lat=2e-6, group=64, layers=2, taper=1.0, write_bw=TARGET_WRITE_BW, v_factor=j.get('layv_factor_nopen', j['layv_factor']))   # (the factor fitted without the multi-node-children level penalty: the target runs without it)
            return PROFILES[name]                                          # the target's constants stay as they are; only the layv factor comes from the aac7 fit
        bw = j['bw_apu'] * (j['alpha_slow'] if name == 'aac7_l4a' else 1.0)
        global MIX_PEN
        if 'MN_MODEL_MIX_PEN' not in os.environ: MIX_PEN = j.get('mix_pen', 0.0)
        PROFILES[name] = Fabric("aac7 L4 refit, NIC group %s" % ('A' if name == 'aac7_l4a' else 'B'), bw_apu=bw, lat=j['lat'], group=64, layers=2, taper=1.0, write_bw=j.get('write_bw', 0.116), v_factor=j['layv_factor'], k_other=j.get('k_other', 1.0))
    if name in ('aac7_c16', 'aac7_s48', 'aac7_l4', 'aac7_l4a'):
        c = dict(AAC7_CONSTS)
        if name in ('aac7_s48', 'aac7_l4', 'aac7_l4a'): c.update(j['consts'])
        e = os.environ
        if 'MN_MODEL_HIDE_POW2' not in e: HIDE_POW2 = c['HIDE_POW2']
        if 'MN_MODEL_GEN_HIDE1' not in e: GEN_HIDE_DEPTH[1] = c['GEN_HIDE1']
        if 'MN_MODEL_GEN_HIDE2' not in e: GEN_HIDE_DEPTH[2] = c['GEN_HIDE2']
        if 'MN_MODEL_T_ROUND' not in e: T_ROUND = c['T_ROUND']
        if 'MN_MODEL_MAP_RATE' not in e: MAP_RATE = c['MAP_RATE']
        if 'MN_MODEL_NODE_SCALE' not in e: NODE_SCALE = c['NODE_SCALE']
        if 'MN_MODEL_INIT_SCALE' not in e: INIT_SCALE = c['INIT_SCALE']
    return PROFILES[name]

# aac6: g node-processes on ONE node over a loopback transport (correctness transports; every product's local passes
# shared g-way over the four APUs).  Fitted on the recorded walls (archive:results_merged_2026-10-24/TIME_MODEL.md §X, L.md, M11.md, S.md; --calib):
#   TCP (comm_tcp.c): one stream per mesh pair; per process ~3.3-3.7 GB/s aggregate at 1 GiB exchanges, slower below
#   (exponent), plus a fixed cost per chunk exchange that grows with the process count (the threads of g x 4 meshes x
#   (g - 1) peers contend on one node: 1 ms at 2, 8 ms at 9 processes) and a per-collective cost of the same kind.
#   OSHMEM (comm_shmem.c on OpenMPI 4.1.6, the serial lock, staging through the host-registered pool): the puts are
#   memcpys through shared memory, slower per byte (the D2H/H2D staging on both sides), with a larger fixed cost.

# ------------------------------------------------------------------------------------------------------------
# the product tier over a group (rns_dist.c: mn_core / mn_grid), as cost
# ------------------------------------------------------------------------------------------------------------
def is_pow2(g): return g > 0 and (g & (g - 1)) == 0

@functools.lru_cache(maxsize=None)
def plane_pts(nc, g):
    """the piece's plane for nc limbs over g nodes (mn_shape: rows >= 32 per rank, R, C >= 2^10)"""
    n = 1 << 20
    while n < nc: n <<= 1
    return max(n, 1 << mem_model.mn_shape(nc, g)[0])

@functools.lru_cache(maxsize=None)
def split_grid(na, nb, cap, g):
    """(ka, kb, pieces_pts): the fewest plane points in total, then the fewest pieces (split_grid_cap)"""
    best = None
    for i in range(1, 65):
        for j in range(1, 65):
            pa, pb = -(-na // i), -(-nb // j)
            if pa + pb > cap: continue
            pts = plane_pts(pa + pb, g)
            cost = i * j * pts
            if best is None or cost < best[0] or (cost == best[0] and i * j < best[1] * best[2]): best = (cost, i, j, pts)
    if best is None: raise ValueError("no grid for %d x %d at cap %d" % (na, nb, cap))
    return best[1], best[2], best[3]

def cache_pieces(na, nb, g, cap, lowcut=0, highcut=None, slots=2):
    """Phase 15 CX (results/CACHE.md §CX15): mn_grid's pieces and the transform cache's state per operand, as the code decides them (rns_dist.c
    mn_grid + mn_core + cache_plan, not pinned): the grid (split_grid), the pieces in the code's order (j outer, i inner) with the cuts'
    skips (grid_piece_skipped), each piece's plane (mn_shape of its own la + lb: a smaller last piece can have a smaller q, and a slot hits
    only at the same q), the slots' designation (nA = min(ka, N - 1) for A's pieces i < nA, B's piece j in slot nA + j % nB) and the lookups.
    Returns (ka, kb, pieces): pieces = [(i, j, la, lb, pts, a, b)], a / b = 'hit' | 'miss' (forwarded, cached or not)."""
    ka, kb, _ = split_grid(na, nb, cap, g)
    pa, pb = -(-na // ka), -(-nb // kb)
    N = slots; nA = min(ka, N - 1) if N > 0 else 0; nA = max(nA, 0); nB = N - nA
    slot = [None] * N                                                 # the slot's key: ('A', i, q) or ('B', j, q)
    out = []
    for j in range(kb):
        for i in range(ka):
            oa, ob = i * pa, j * pb
            la, lb = min(pa, na - oa), min(pb, nb - ob)
            if la <= 0 or lb <= 0: continue
            if (highcut is not None and oa + ob >= highcut) or (oa + ob + la + lb <= lowcut): continue
            q = mem_model.mn_shape(la + lb, g)[3]; pts = plane_pts(la + lb, g)
            kA, kB = ('A', i, q), ('B', j, q)
            ha = slot.index(kA) if kA in slot else -1; hb = slot.index(kB) if kB in slot else -1
            sa = i if i < nA else -1; sb = nA + j % nB if nB > 0 else -1
            if ha >= 0: sa = ha
            elif sa >= 0 and sa == hb: sa = -1
            if hb >= 0: sb = hb
            elif sb >= 0 and sb == ha: sb = -1
            if sa >= 0 and sa == sb and ha < 0: sb = -1
            if ha < 0 and sa >= 0: slot[sa] = kA
            if hb < 0 and sb >= 0: slot[sb] = kB
            out.append((i, j, la, lb, pts, 'hit' if ha >= 0 else 'miss', 'hit' if hb >= 0 else 'miss'))
    return ka, kb, out

def cache_pieces_t(na, nb, g, cap, lowcut=0, highcut=None, slots=2):
    """Phase 15 CX (proposal): cache_pieces with the loop along the other axis (A's piece i reused across j): the transposed grid's pieces"""
    ka, kb, _ = split_grid(na, nb, cap, g)
    return [(i, j, la, lb, pts, a, b) for (j, i, lb, la, pts, b, a) in _cache_pieces_grid(nb, na, kb, ka, g, lowcut, highcut, slots)]   # (back to A's i, B's j)

def _cache_pieces_grid(na, nb, ka, kb, g, lowcut, highcut, slots, p24=False):
    pa, pb = -(-na // ka), -(-nb // kb)
    N = slots; nA = max(min(ka, N - 1), 0) if N > 0 else 0; nB = N - nA
    slot = [None] * N; out = []
    for j in range(kb):
        for i in range(ka):
            oa, ob = i * pa, j * pb
            la, lb = min(pa, na - oa), min(pb, nb - ob)
            if la <= 0 or lb <= 0: continue
            if (highcut is not None and oa + ob >= highcut) or (oa + ob + la + lb <= lowcut): continue
            n_ = p24_pts(la) + p24_pts(lb) if p24 else la + lb        # PC: a P24 piece's plane is on its points
            q = mem_model.mn_shape(n_, g)[3]; pts = plane_pts(n_, g)
            kA, kB = ('A', i, q), ('B', j, q)
            ha = slot.index(kA) if kA in slot else -1; hb = slot.index(kB) if kB in slot else -1
            sa = i if i < nA else -1; sb = nA + j % nB if nB > 0 else -1
            if ha >= 0: sa = ha
            elif sa >= 0 and sa == hb: sa = -1
            if hb >= 0: sb = hb
            elif sb >= 0 and sb == ha: sb = -1
            if sa >= 0 and sa == sb and ha < 0: sb = -1
            if ha < 0 and sa >= 0: slot[sa] = kA
            if hb < 0 and sb >= 0: slot[sb] = kB
            out.append((i, j, la, lb, pts, 'hit' if ha >= 0 else 'miss', 'hit' if hb >= 0 else 'miss'))
    return out

def cache_hits_p24(na, nb, g, ka, kb, lowcut=0, highcut=None, slots=1, loop='code'):
    """Phase 15 PC: the hit pieces {(i, j): bool} of a P24 grid (ka x kb from p24_split) as rns_dist.c mn_grid takes them under
    RNS_DIST_CACHE_PARTIAL=1 (loop 'long': the loop along the longer axis when kb > ka, as the switch does)"""
    if loop == 'long' and kb > ka:
        out = [(i, j, la, lb, pts, a, b) for (j, i, lb, la, pts, b, a) in _cache_pieces_grid(nb, na, kb, ka, g, lowcut, highcut, slots, True)]
    else: out = _cache_pieces_grid(na, nb, ka, kb, g, lowcut, highcut, slots, True)
    return {(p[0], p[1]): p[5] == 'hit' or p[6] == 'hit' for p in out}

def hit_cost(fab, pts, g, la, lb, form='grid', p24=False, np_=None):
    """a hit piece (CX's term, shared by the 18- and 24-digit forms): the saving CACHE_HIT_F x (fwd 2 - fwd 1); a slot of k < np primes
    (cache_primes_now()) saves k / np of the transforms' part and no redistribution"""
    c2 = piece_cost(fab, pts, g, la, lb, la + lb, 2, False, form, grid=True, p24=p24); c1 = piece_cost(fab, pts, g, la, lb, la + lb, 1, False, form, grid=True, p24=p24)
    d_t, d_e = (c2.t - c1.t) * CACHE_HIT_F, (c2.t_exposed - c1.t_exposed) * CACHE_HIT_F
    kp = cache_primes_now()
    if kp:
        npc = np_ if np_ else (4 if p24 else piece_np(DZ, la + lb, la, lb)); k = min(kp, npc)
        if k < npc:
            t_r = fab.a2a(8 * lb / (4 * g), g, 1)[0]
            d_t = max(0.0, d_t - t_r) * k / npc * CACHE_PART_F; d_e = max(0.0, d_e - t_r) * k / npc * CACHE_PART_F
    c1.t = c2.t - d_t; c1.t_exposed = max(0.0, c2.t_exposed - d_e)
    return c1

class Cost:
    def __init__(self): self.t = 0.0; self.t_exposed = 0.0; self.nic = 0.0; self.glob = 0.0; self.msgs = 0; self.pieces = 0; self.xfers = 0
    def add(self, o):
        self.t += o.t; self.t_exposed += o.t_exposed; self.nic += o.nic; self.glob += o.glob; self.msgs += o.msgs; self.pieces += o.pieces; self.xfers += o.xfers
        return self

def piece_cost(fab, pts, g, na, nb, nc, fwd=2, with_x=False, form="grid", grid=False, p24=False):
    """one piece product of pts plane points over a group of g nodes (every node transforms: L's balanced map): the
    local passes on 4 g ranks, the fabric exchanges (12 layered all-to-alls per piece: 3 per prime, fewer with cache
    hits), the operand redistributions and the result exchange, the spill all-gather (form 'flat': every rank's 4 C
    limbs from every node -- g x C x 32 B per APU; 'grid': G's exact spills, O(C)) and the carry scan."""
    c = Cost(); c.pieces = 1
    q = pts / (4 * g)                                   # points per APU
    scale = q / (1 << 29)
    # the local part: the measured piece (its xGMI exchanges included, 84 % hidden), on shared APUs times the share
    dz = DZ
    npc = 4 if p24 else piece_np(dz, nc, na, nb)        # Phase 15 NP the piece's primes (ECALC_NP=auto: 4 over the three-prime bound); P24: four
    if dz is None or dz.legacy: t31 = T_PIECE_31 if fwd == 2 else T_PIECE_31_BHIT
    else: t31 = T_PIECE_31_NP[npc] * (1.0 if fwd == 2 else T_PIECE_31_BHIT / T_PIECE_31) * dz.f_mm() * NODE_SCALE   # Phase 13b D: S13's C at 2^31 (P = 3 / 4); Phase 16 C: x NODE_SCALE
    t_loc = t31 * scale * (P24_F if p24 else 1.0) + 0.005             # P24 (Phase 15 Batch 3): the regroup / CRT kernels' surcharge (ASSUMED, P24_F)
    if dz is not None and not dz.legacy and CAL13:                      # Phase 13d D2: the pipeline's pieces against the isolated ones
        t_loc *= PIECE13; t_loc += GRID_ADD.get("C", 0.0) * scale * (1 if grid else 0)   # (per 2^31 points = 2^29 per APU; x gpu_share below; GRID_NC not here)
    if dz is not None and not dz.legacy and dz.p15b:                    # Phase 15 (2026-09-27): DIST_TWREC=1 on the pack / unpack passes -- the equal path only
        gmap = not is_pow2(g) or dz.force_gen                           # (Phase 15 G5: the general map has its own kernels, GEN_TWPACK_F, DIST_TWREC_G)
        t_loc *= GEN_TWPACK_F[1 if TWREC_G else 0] if gmap else TWREC_F
    t_loc *= fab.gpu_share
    # the transforms' exchanges: EC_NP x (fwd + 1) layered all-to-alls of 8 q bytes per APU; the xGMI stage of one
    # runs under the fabric stage of the other (inflight 2 on the equal path; 1 on the general map: GEN_HIDE), so the
    # fabric's excess over the hidden xGMI stage is exposed
    t_x = T_XGMI_31 * scale
    if dz is None or dz.legacy or not fab.target:
        gen = GEN_HIDE if dz is None or dz.gen_hide is None else dz.gen_hide   # Phase 13b D: the depth check on aac6 sets it
        pow2 = is_pow2(g) and not (dz is not None and dz.force_gen)             # DIST_GEN=1
        hide = (1.0 if fab.target else HIDDEN_XGMI) * (1.0 if pow2 else gen)
    else:                                                             # Phase 13b D: X13's measured overlap -- the equal path hides
        hide = HIDE_POW2 if is_pow2(g) else GEN_HIDE_DEPTH[min(dz.depth, 2)]   # 3/4 of its xGMI time, the general map 1.1 % (two deep: modelled 3/4)
    n_tr = (EC_NP if dz is None or dz.legacy else npc) * (fwd + 1)
    gen_x = not is_pow2(g) or (dz is not None and dz.force_gen)       # T06: the transform's exchange is the layv kind (comm_layered.c v_post) exactly when the general map runs
    t_f, nic, glob, msgs = fab.a2a(8 * q, g, K_CHUNKS, v=gen_x)
    exposed_tr = max(0.0, t_f - hide * t_x)
    c.nic += n_tr * nic; c.glob += n_tr * glob; c.msgs += n_tr * msgs; c.xfers += n_tr
    # the operand redistributions (A, B, X) and the result: plain all-to-alls over the group of the operands' bytes
    t_r = 0.0
    for limbs in ([na] if fwd >= 1 else []) + ([nb] if fwd >= 2 else []) + ([nc] if with_x else []) + [nc]:
        t1, nic1, glob1, msgs1 = fab.a2a(8 * limbs / (4 * g), g, 1, other=True)
        t_r += t1; c.nic += nic1; c.glob += glob1; c.msgs += msgs1; c.xfers += 1
    # the spills: the columns' carries (4 limbs per column) of every rank, all-gathered (flat) or exchanged exactly (grid)
    C = 1 << (mem_model.mn_shape(pts, g)[2])
    if form == "flat": t_s, nic_s, glob_s, msgs_s = fab.allgather(C * 4 * 8, g)
    else: t_s, nic_s, glob_s, msgs_s = fab.a2a(2 * C * 4 * 8, g, 1, other=True)
    c.nic += nic_s; c.glob += glob_s; c.msgs += msgs_s; c.xfers += 1
    t_small = 3 * fab.coll(g)
    if dz is not None and dz.t_mb:                                    # Phase 13b D: MN_T_CHUNK_MB -- the result window in rounds of W
        W = mem_model.t_chunk_limbs(dz.t_mb)                          # limbs per node: each extra round one alltoallv + the adds
        extra = max(0, -(-(nc // g) // W) - 1) if W else 0
        t_small += extra * round_cost(fab, g)
    c.t = t_loc + n_tr * exposed_tr + t_r + t_s + t_small
    c.t_exposed = n_tr * exposed_tr + t_r + t_s + t_small
    return c

NP_AUTO_TERMS = mem_model.NP3_MAX_TERMS   # Phase 15 NP: ECALC_NP=auto's switch-over (ECALC_NP_AUTO_TERMS; the three-prime bound)
NP_STATS = dict(n3=0, n4=0)                # the pieces priced at three / four primes under auto (the memo counts a product once)
def piece_np(dz, nc, na=None, nb=None):
    """Phase 15 NP (crt.c ec_np_for): a piece's prime count -- the design's np, or under np_auto four when nc = pa + pb exceeds the
    switch-over (mn_core / dist_core decide per product; the one-node tiers keep three)."""
    if dz is None or dz.legacy: return EC_NP
    if getattr(dz, 'np_auto', False):
        k = 4 if nc > NP_AUTO_TERMS else 3; NP_STATS['n%d' % k] += 1; return k
    return dz.np

# ---- Phase 15 Batch 3 P24 (results/PRIMES_STRATEGY.md §P2415): four primes at 24 digits per transform point in the mn tier (MN_P24) ---------------------------
# A P24 product regroups its operands' 18-digit limbs into 24-digit points (4 limbs = 3 points) at the plane's load and the CRT's carry
# (rns_dist.c mn_core's P24 path): the plane holds p24_pts(n) = ceil(3 n / 4) points for n limbs, at four primes; the operands' redistribution
# and the result's exchange still move limbs (8 B per limb); the cap in points is the group's, at most 2^40 (the CRT's four-limb spill:
# min(pa, pb) (10^24 - 1)^2 10^12 < 10^72 needs min <= 10^12 points); the transform cache is not used by a P24 product.
# MN_P24=1: the products whose 18-digit grid's largest piece runs four primes (ECALC_NP=4: every mn product; auto: those over the
# three-prime bound); 2: every mn product (SC15's model).  P24_F: the local passes' surcharge, ASSUMED 1.0 until measured (SC: 1.05-1.20).
P24 = int(os.environ.get('MN_P24', '2') or 0)   # default 2 since Phase 15 Batch 3 (the user's decision, 2026-09-29)
P24_F = float(os.environ.get('MN_MODEL_P24_F', '1.0'))
P24_CAP_LOG = 40
def p24_pts(n): return -(-3 * n // 4)
def p24_split(na, nb, cap, g):
    """rns_dist.c p24_split: split_grid's rule on the pieces' points (p24_pts(pa) + p24_pts(pb) <= cap), i, j <= 32"""
    best = None
    for i in range(1, 33):
        for j in range(1, 33):
            pa, pb = -(-na // i), -(-nb // j)
            n = p24_pts(pa) + p24_pts(pb)
            if n > cap: continue
            pts = plane_pts(n, g); cost = i * j * pts
            if best is None or cost < best[0] or (cost == best[0] and i * j < best[1] * best[2]): best = (cost, i, j, pts)
    if best is None: raise ValueError("no P24 grid for %d x %d at cap %d" % (na, nb, cap))
    return best[1], best[2], best[3]
def p24_of(na, nb, g, cap):
    """rns_dist.c mn_p24_of: whether the product runs P24 (MN_P24 and, at 1, the 18-digit grid's largest piece at four primes)"""
    dz = DZ
    if not P24 or dz is None or dz.legacy: return False
    if P24 >= 2: return True
    pa, pb = na, nb
    if na + nb > cap:
        ka, kb, _ = split_grid(na, nb, cap, g); pa, pb = -(-na // ka), -(-nb // kb)
    if getattr(dz, 'np_auto', False):                                 # INT3: as mn_p24_of
        return pa + pb > NP_AUTO_TERMS
    return dz.np == 4
def _product_cost_p24(fab, na, nb, g, cap, lowcut=0, highcut=None, with_x=False, form="grid", cache=True):
    """a P24 product: the grid on the points (p24_split at min(cap, 2^40)), every piece at four primes; the cache only with CACHE_P24 (Phase 15 PC,
    RNS_DIST_CACHE_PARTIAL=1); X added afterwards (mdb_add_shifted)"""
    cap = min(cap, 1 << P24_CAP_LOG)
    c = Cost(); nc = na + nb
    if p24_pts(na) + p24_pts(nb) <= cap:
        c.add(piece_cost(fab, plane_pts(p24_pts(na) + p24_pts(nb), g), g, na, nb, nc, 2, False, form, p24=True))
    else:
        ka, kb, pts = p24_split(na, nb, cap, g)
        pa, pb = -(-na // ka), -(-nb // kb)
        nsl = cache_slots_now()                                       # Phase 15 PC: the cache over P24 products (RNS_DIST_CACHE_PARTIAL=1)
        hits = cache_hits_p24(na, nb, g, ka, kb, lowcut, highcut, nsl, CACHE_LOOP) if cache and CACHE_P24 and nsl > 0 and CACHE_MODEL != 'old' else None
        for j in range(kb):
            for i in range(ka):
                oa, ob = i * pa, j * pb
                la, lb = min(pa, na - oa), min(pb, nb - ob)
                if la <= 0 or lb <= 0: continue
                if (highcut is not None and oa + ob >= highcut) or (lowcut and oa + ob + la + lb <= lowcut): continue
                if hits is not None and hits.get((i, j)): c.add(hit_cost(fab, pts, g, la, lb, form, p24=True, np_=4))
                else: c.add(piece_cost(fab, pts, g, la, lb, la + lb, 2, False, form, grid=True, p24=True))
    if with_x:                                                        # mdb_add_shifted (as _product_cost's grid term)
        t1, nic1, glob1, msgs1 = fab.a2a(8 * nc / (4 * g), g, 1)
        if DZ is not None and DZ.t_mb:
            W = mem_model.t_chunk_limbs(DZ.t_mb); t1 += max(0, -(-(nc // g) // W) - 1) * round_cost(fab, g)
        c.t += t1 + fab.coll(g); c.t_exposed += t1 + fab.coll(g); c.nic += nic1; c.glob += glob1; c.msgs += msgs1; c.xfers += 1
    return c

_PC = {}
def product_cost(fab, na, nb, g, lowcut=0, highcut=None, with_x=False, cache=True, form="grid"):
    """memoised _product_cost (Phase 13b D: the design table evaluates the same products for many rows); the key is the fabric's
    parameters, the arguments and what of the design the product depends on"""
    dz = DZ
    dk = None if dz is None else (dz.legacy, dz.np, getattr(dz, 'np_auto', False), NP_AUTO_TERMS, dz.modmul, dz.pool_log(), dz.t_mb, dz.depth, dz.gen_hide, dz.force_gen, T_ROUND, HIDE_POW2, GEN_HIDE_DEPTH[1], GEN_HIDE_DEPTH[2], T_PIECE_31_NP[dz.np], F_MM1, PIECE13, CAL13, GRID_ADD['C'], dz.p15b, TWREC_F, TWREC_G, CACHE_MODEL, cache_slots_now(), CACHE_HIT_F, cache_primes_now(), CACHE_LOOP, P24, P24_F, CACHE_P24, CACHE_PART_F)
    k = (fab.bw, fab.lat, fab.group, fab.layers, fab.taper, fab.gpu_share, fab.fixed, fab.tcp_exp, fab.coll_fixed, fab.target, fab.v_factor, fab.k_other,
         na, nb, g, lowcut, highcut, with_x, cache, form, dk)
    c = _PC.get(k)
    if c is None:
        c = _product_cost(fab, na, nb, g, lowcut, highcut, with_x, cache, form); _PC[k] = c
    out = Cost(); out.add(c); return out

def _product_cost(fab, na, nb, g, lowcut=0, highcut=None, with_x=False, cache=True, form="grid"):
    """C = A B (+ X) over a group of g nodes: the grid of pieces at the group's cap (mn_logn_cap: 2^(31 + floor(log2 g))),
    the cuts, the transform cache over shares"""
    if g <= 1: raise ValueError("product over one node")
    cap = 1 << mem_model.mn_cap_log(g, 31 if DZ is None or DZ.legacy else DZ.pool_log())
    if p24_of(na, nb, g, cap): return _product_cost_p24(fab, na, nb, g, cap, lowcut, highcut, with_x, form, cache)   # P24 (MN_P24)
    nc = na + nb
    c = Cost()
    if nc <= cap:
        pts = plane_pts(nc, g)
        return c.add(piece_cost(fab, pts, g, na, nb, nc, 2, with_x, form))
    ka, kb, pts = split_grid(na, nb, cap, g)
    pa, pb = -(-na // ka), -(-nb // kb)
    first = True
    hits = None; nsl = cache_slots_now()
    if CACHE_MODEL != 'old' and cache and nsl > 0:                   # Phase 15 CX: the code's hits (the planes priced at the grid's pts as before)
        if CACHE_LOOP == 'long' and kb > ka:                          # (proposal: the grid's loop along the longer axis -- the pieces' roles swapped)
            hits = {(p[0], p[1]): p[5] == 'hit' or p[6] == 'hit' for p in cache_pieces_t(na, nb, g, cap, lowcut, highcut, nsl)}
        else:
            hits = {(p[0], p[1]): p[5] == 'hit' or p[6] == 'hit' for p in cache_pieces(na, nb, g, cap, lowcut, highcut, nsl)[2]}
    for j in range(kb):
        for i in range(ka):
            oa, ob = i * pa, j * pb
            la, lb = min(pa, na - oa), min(pb, nb - ob)
            if la <= 0 or lb <= 0: continue
            if (highcut is not None and oa + ob >= highcut) or (lowcut and oa + ob + la + lb <= lowcut): continue
            # the transform cache over shares (2 slots): B's piece j held across i, A's piece 0 held across j
            fwd = 2
            if CACHE_MODEL == 'old':
                if cache and CACHE_MN_SLOTS >= 2:
                    if i > 0 and not first: fwd = 1                   # B piece j is in its slot: A only
                    if i == 0 and j > 0: fwd = 1                      # A piece 0 is in its slot: B only
                    if i > 0 and j > 0: fwd = 1
            elif hits is not None and hits.get((i, j)): fwd = 1       # Phase 15 CX: the code's hit (one operand at most per piece)
            if fwd == 2 or CACHE_MODEL == 'old' or (CACHE_HIT_F == 1.0 and not cache_primes_now()):
                c.add(piece_cost(fab, pts, g, la, lb, la + lb, fwd, False, form, grid=True))
            else:                                                     # Phase 15 CX: the hit saves CACHE_HIT_F of the modelled saving (PC: hit_cost, the
                c.add(hit_cost(fab, pts, g, la, lb, form))            # same term; a slot of k < np primes saves k / np of the transforms, no redistribution)
            first = False
    if with_x:                                                        # mdb_add_shifted: rounds of 2^26 limbs per APU
        t1, nic1, glob1, msgs1 = fab.a2a(8 * nc / (4 * g), g, 1)
        if DZ is not None and DZ.t_mb:                                # Phase 13b D: mdb_add_shifted_rounds (MN_T_CHUNK_MB)
            W = mem_model.t_chunk_limbs(DZ.t_mb); t1 += max(0, -(-(nc // g) // W) - 1) * round_cost(fab, g)
        c.t += t1 + fab.coll(g); c.t_exposed += t1 + fab.coll(g); c.nic += nic1; c.glob += glob1; c.msgs += msgs1; c.xfers += 1
    return c

def shift_cost(fab, n, g):
    """mdb_shift: one all-to-all per APU thread of the share-intersection pieces (about half the limbs change node
    when the basis changes), then the truncation's max-reduction"""
    c = Cost()
    t1, nic1, glob1, msgs1 = fab.a2a(0.5 * 8 * n / (4 * g), g, 1)
    if DZ is not None and DZ.shift_mb:                                # Phase 13b D: MDB_SHIFT_CHUNK_MB -- K rounds of about m MB per APU
        part = -(-n // g) // 4; ch = int(DZ.shift_mb * 1048576.0 / 8)
        t1 += (max(0, -(-part // ch) - 1) if ch else 0) * round_cost(fab, g)
    c.t = t1 + fab.coll(g) + 0.002 * fab.gpu_share; c.t_exposed = t1 + fab.coll(g); c.nic = nic1; c.glob = glob1; c.msgs = msgs1; c.xfers = 1
    return c

def small_cost(fab, g, n=1):
    c = Cost(); c.t = c.t_exposed = n * (fab.coll(g) + 0.003 * fab.gpu_share); return c

# ------------------------------------------------------------------------------------------------------------
# the reciprocal (recip_mn) and the division (newton_mn_divmod) over the machine
# ------------------------------------------------------------------------------------------------------------
def choose_group(fab, n_pts, g, rule, form):
    """X1: the group for a product of n_pts points: the smallest power of two <= g whose cost is minimal (the
    model's rule), or the full group (rule 'full')."""
    if rule == "full" or g <= 2: return g
    best = None
    gp = 2
    while gp <= g:
        try: c = product_cost(fab, n_pts * 2 // 3, n_pts // 3, gp, form=form)
        except ValueError: gp *= 2; continue
        if best is None or c.t < best[0]: best = (c.t, gp)
        gp *= 2
    if best[1] * 2 > g and best[1] != g: return g
    return best[1]

NEWTON_RECIP_GUARD = 1                 # newton_db.c: the cut keeps one guard limb (NEWTON_RECIP_GUARD, default 1)
NEWTON_RECIP_MID_HI = 4                # newton_db.c MID_HI: NEWTON_RECIP_MID's high cut at take + 4 (Phase 15 R4; default since 2026-09-27)
def newton_recip_cut(v): return v - NEWTON_RECIP_GUARD if v > NEWTON_RECIP_GUARD else 0   # newton_db.c newton_recip_cut

def recip_cost(fab, nq, k, g, rule, form, split=1 << 16):
    """the anchored chain k, ceil(k/2), ... : the single-node part to `split` on every node, then the sharded
    steps: per step Q_t r (take x (j+1)), the shift chain, r d ((j+1) x (j+2)), two adds"""
    kp = k
    while kp > split and (kp + 1) // 2 > 2: kp = (kp + 1) // 2
    if kp > split: kp = 2
    c = Cost(); groups = []
    T = min(2 * kp + 2, nq)
    c.add(shift_cost(fab, T, g))                                       # Q's top to every node (mdb_to_host_all)
    t1, nic1, glob1, msgs1 = fab.a2a(8 * T / 4, g, 1); c.t += t1; c.t_exposed += t1; c.nic += nic1 * 1; c.glob += glob1; c.msgs += msgs1
    c.t += 0.02 * fab.gpu_share * math.log2(max(kp, 4))                # the replicated single-node chain (kernel-launch bound)
    j = kp; cur = None; mid_ok = False                                 # Phase 15 R4: the first sharded round forms the whole product
    mid = DZ is not None and DZ.p15b                                   # NEWTON_RECIP_MID=1 (default since 2026-09-27)
    while j < k:
        jn = k
        while (jn + 1) // 2 > j: jn = (jn + 1) // 2
        take = min(2 * j + 2, nq)
        gp = choose_group(fab, take + j + 1, g, rule, form)
        if gp != cur:                                                  # r re-sharded onto the step's group
            c.add(shift_cost(fab, j + 1, max(gp, cur or 1))); cur = gp; mid_ok = False   # (R4: a group change restarts the middle product)
        groups.append((j, gp))
        if take < nq: c.add(shift_cost(fab, take, g))                  # Q_t out of Q (Q is over the full group)
        cut = DZ is not None and DZ.p15 and DZ.recip_cut                # Phase 15: NEWTON_RECIP_CUT (default since Phase 14): the pieces wholly below the
        hw = take + NEWTON_RECIP_MID_HI if (mid and mid_ok and NEWTON_RECIP_MID_HI <= j <= take) else None   # R4: the middle product -- pieces at or above take + 4 skipped
        c.add(product_cost(fab, take, j + 1, gp, lowcut=newton_recip_cut(take - j) if cut and j <= take else 0, highcut=hw, form=form))   # band read are not formed -- Q_t r
        mid_ok = True                                                  # (newton_db.c: mid_ok = |d| <= j + 1 -- ASSUMED true after every round, no overshoot)
        c.add(shift_cost(fab, take + j + 1, gp))                       # u
        c.add(small_cost(fab, gp, 4))                                  # limb, nonzero_below, pow, |B^2j - u|
        c.add(product_cost(fab, j + 1, j + 2, gp, lowcut=newton_recip_cut(j) if cut else 0, form=form))          # r d
        c.add(shift_cost(fab, 2 * j + 3, gp))                          # corr
        c.add(shift_cost(fab, 2 * j + 1, gp))                          # r << j
        c.add(small_cost(fab, gp, 2))                                  # cmp, add
        if jn < 2 * j: c.add(shift_cost(fab, 2 * j + 1, gp))
        j = jn
    if cur != g: c.add(shift_cost(fab, k + 1, g))                      # mu onto the full group
    return c, groups

def exact_sizes(T):
    """Phase 13d D2: the run's lengths as the code has them (agent L's mn_plan.c pq_of): dl = ceil(d / 18) for d = T rounded up to
    a multiple of 18; Q(1, N+1) = N! has log10 N! digits, P = Q (e - 1), S = P + Q = Q e; limbs = floor(log10 / 18) + 1"""
    d = mem_model.digits_of_run(int(T)); N = _terms(T); lq = _lf(N)
    lim = lambda lg: int(math.floor(lg / LIMB_DIGITS)) + 1
    return dict(dl=(d + 17) // 18, nq=lim(lq), pn=lim(lq + math.log10(math.e - 1)), sn=lim(lq + math.log10(math.e)))

DKM = os.environ.get('MN_MODEL_DKM', '1') == '1'   # default on since Phase 15 Batch 3 (the user's decision, 2026-09-29)   # Phase 15 DKM (results/DIVISION.md §DKM15): NEWTON_DKM=1 -- the division in two quotient halves
DKM_LAST = {}                                        # the last division_cost_dkm's parts (the early writer's overlap reads 'hide')
DKM_HI = os.environ.get('MN_MODEL_DKM_HI', '1') == '1'   # the code's default (always on since 2026-10-12; MN_MODEL_DKM_HI=0 prices the removed older start at the hook).  Phase 15 EW (archive:results_merged_2026-10-24/DIVISION.md §EW15): MN_OUT_DKM_HI -- the writer on X_hi's
                                                     # digits (final there) from DKM's step 1 on, X_lo's from the release before step 2's low product: early_exposed()
OVL1_HI = (0.144, 0.369)                             # Phase 15 EW, size 1: (A, B) = the division's fractions between step 1's end and the release (step 2's
                                                     # high product and window) and after it (X_lo Q, the corrections, R's residues) -- MEASURED at 1e11 on
                                                     # int15i (DKM15 4.4's on runs: 7.38 and 18.17 + 0.74 of 51.3 s); refitted by EW's series (EW_FIT below)

def division_cost_dkm(fab, nq, dl, npn, g, rule, form, sn=None):
    """Phase 15 DKM (newton_db.c mn_divmod_dkm): the reciprocal to h = floor(k_mu/2) + 1; step 1 = today's division of A >> s by Q
    (s = min(floor(k/2), dl); A_h' (k1 + 1) x mu' (k1 + 1) cut below k1 + 1, X_hi Q mod B^w, the window, the corrections); step 2 =
    the division of R1 B^s (R1_top (s + 1) x mu'' (s + 2) cut below s + 2, X_lo Q mod B^w); the assembly X_hi B^s + X_lo (three shifts,
    one add).  DKM_LAST['hide'] = what the early writer overlaps: step 2's low product and what follows"""
    if sn is None: sn = npn
    na = sn + dl; k = na - nq + 1; w = nq + 2; kmu = npn + 1 + dl - nq + 1
    s = min(k // 2, dl); k1 = k - s; h = max(k1, s + 1); hmu = kmu // 2 + 1
    rc, groups = recip_cost(fab, nq, max(hmu, h), g, rule, form)
    c = Cost()
    c.add(shift_cost(fab, nq, g)); c.add(small_cost(fab, g, 3))        # Q into P's basis, S = P + Q, residues of P, Q
    c.add(shift_cost(fab, k1 + 1, g))                                  # mu's top k1 + 1
    nah = sn - (nq - 1 - (dl - s))                                     # A_h' = S >> (nq - 1 - dl1): k1 + 1 limbs
    c.add(shift_cost(fab, nah, g))
    c1 = product_cost(fab, nah, k1 + 1, g, lowcut=k1 + 1, form=form); c.add(c1)   # X_hi = high(A_h' mu')
    c.add(shift_cost(fab, nah + k1 + 1, g))                            # X_hi
    c.add(shift_cost(fab, na, g)); c.add(shift_cost(fab, nq, g))       # the window, Q in basis w
    c2 = product_cost(fab, k1, nq, g, highcut=w, form=form); c.add(c2)   # X_hi Q mod B^w
    c.add(small_cost(fab, g, 5))                                       # cmp, sub, corrections, X_hi +- dx
    t_step1 = c.t                                                      # (X_hi is final here: X's limbs at and above s)
    c.add(shift_cost(fab, s + 2, g))                                   # mu's top s + 2
    c.add(shift_cost(fab, s + 1, g))                                   # R1_top
    c3 = product_cost(fab, s + 1, s + 2, g, lowcut=s + 2, form=form); c.add(c3)   # X_lo = high(R1_top mu'')
    c.add(shift_cost(fab, 2 * s + 3, g))                               # X_lo
    c.add(shift_cost(fab, w, g))                                       # the window of R1 B^s
    c.add(shift_cost(fab, k, g)); c.add(shift_cost(fab, k, g)); c.add(small_cost(fab, g, 1)); c.add(shift_cost(fab, k, g))   # X_hi << s, X_lo in basis, add, X in its basis
    c4 = product_cost(fab, s, nq, g, highcut=w, form=form); c.add(c4)  # X_lo Q mod B^w
    rest = Cost(); rest.add(small_cost(fab, g, 6))                     # cmp, sub, corrections, X +- dx, R residues
    c.add(rest)
    DKM_LAST.clear(); DKM_LAST.update(h=h, hmu=hmu, s=s, k1=k1, c1=c1.t, c2=c2.t, c3=c3.t, c4=c4.t, p1=c1.pieces, p2=c2.pieces, p3=c3.pieces, p4=c4.pieces, hide=c4.t + rest.t, hide_hi=c.t - t_step1, total=c.t)
    return rc, c, groups

def early_exposed(W, dc, g):
    """Phase 15 EW (MN_OUT_DKM_HI, archive:results_merged_2026-10-24/DIVISION.md §EW15 1.3, 5): the part file's exposed seconds when the writer (W seconds of formatting / writing)
    runs as a pipeline -- X_hi's share of the digits (f_hi = k1 / k: size > 1 every rank writes its share of both layers) from step 1's end,
    then, after waiting at the release if it got there first, X_lo's: exposed = max(0, max(A, f_hi W) + (1 - f_hi) W - A - B), A = the
    division from step 1's end to the release (step 2's high product, its window), B = after it.  Size > 1: A, B from division_cost_dkm's
    parts (scaled as dc.t was); size 1: OVL1_HI x the division (MEASURED).  Without the switch (or DKM): max(0, W - ovl_div)"""
    if DKM and DKM_HI:
        if g > 1 and DKM_LAST.get('total'):
            sc = dc.t / DKM_LAST['total']; A = (DKM_LAST['hide_hi'] - DKM_LAST['hide']) * sc; B = DKM_LAST['hide'] * sc
            fh = DKM_LAST['k1'] / float(DKM_LAST['k1'] + DKM_LAST['s'])
        else:
            A, B = OVL1_HI[0] * dc.t, OVL1_HI[1] * dc.t; fh = 0.5
        return max(0.0, max(A, fh * W) + (1.0 - fh) * W - A - B)
    if DKM and g == 1: return max(0.0, W - OVL1_HI[1] * dc.t)   # MN_MODEL_DKM_HI=0 at size 1: the old hook (before step 2's low product) hides B = X_lo Q + corrections + residues (MEASURED 0.369 of the division, DKM15 4.4's on runs), not OVL1 (0.577, fitted on the pre-DKM division: its hook sat before the whole X Q product); so the switch's gain is A x the division (7.4 s at 1e11), never negative
    return max(0.0, W - ovl_div(dc, g))

def ovl_div(dc, g):
    """the part of the division the early writer runs under: OVL1 x the division (FITTED at size 1); Phase 15 DKM: the hook is before
    step 2's low product, so step 2's low product and what follows (DKM_LAST['hide'], scaled as dc.t was) -- ASSUMED fully overlapped; size > 1 only
    (size 1's division is the measured phase table: DKM is measured there, not modelled)"""
    if DKM and g > 1 and DKM_LAST.get('total'): return DKM_LAST['hide_hi' if DKM_HI else 'hide'] * dc.t / DKM_LAST['total']
    return OVL1 * dc.t

def division_cost(fab, nq, dl, npn, g, rule, form, sn=None):
    """S = P + Q, A_h = S >> (nq - 1 - dl), t = A_h mu (low cut k + 1), X = t >> (k + 1), X Q mod B^w (high cut w),
    the window, the corrections, the residues; the reciprocal first.  Phase 13d D2 (newton_db.c newton_mn_divmod, L's plan):
    the reciprocal to k_mu = P + 1 + dl - nq + 1 (S's largest possible length), the division's k = S + dl - nq + 1.
    Phase 15 DKM: MN_MODEL_DKM=1 (the module's DKM) prices NEWTON_DKM=1 instead (division_cost_dkm)"""
    if DKM: return division_cost_dkm(fab, nq, dl, npn, g, rule, form, sn)
    if sn is None: sn = npn
    na = sn + dl; k = na - nq + 1; w = nq + 2; kmu = npn + 1 + dl - nq + 1
    rc, groups = recip_cost(fab, nq, kmu, g, rule, form)
    c = Cost()
    c.add(shift_cost(fab, nq, g)); c.add(small_cost(fab, g, 3))        # Q into P's basis, S = P + Q, residues of P, Q
    c.add(shift_cost(fab, na, g))                                      # A_h
    nah = sn - (nq - 1 - dl)                                           # Phase 13d D2: A_h = S >> (nq - 1 - dl): dl + 1 limbs (newton_db.c 713;
                                                                       # the 1.42e11 log: 7888888890 x 7888888891); the model had 2 dl + 1
                                                                       # (the shift applied to S B^dl), twice the product
    c.add(product_cost(fab, nah, k + 1, g, lowcut=k + 1, form=form))   # A_h mu
    c.add(shift_cost(fab, nah + k + 1, g))                             # X
    c.add(product_cost(fab, dl + 1, nq, g, highcut=w, form=form))      # X Q mod B^w
    c.add(shift_cost(fab, na, g)); c.add(shift_cost(fab, nq, g))       # the window, Q in basis w
    c.add(small_cost(fab, g, 6))                                       # cmp, sub, corrections, X +- dx, R residues
    return rc, c, groups

# ------------------------------------------------------------------------------------------------------------
# the tree's distributed levels: L's schedule (mn_groups_parse), each level as the tree forms it
# ------------------------------------------------------------------------------------------------------------

# Phase 13d D2: the code gives every node the same number of TERMS (mn.c: node r computes [1 + N r / g, 1 + N (r+1) / g)),
# not the same number of digits: a node's Q has sum log10 k over its terms, so the top node's share is ~3.6 % above the
# average at 576 nodes (4.4e13: 1.0358; node 0's 0.772).  Every tree level waits for its slowest group -- the top one -- and
# the wall for the slowest leaf -- the top node's.  SHARES = 'terms' costs the top group and the top node's leaf (the code);
# 'even' = every node D digits (the model before 13d, which put the 576-node tree step 3.6 % too late).
SHARES = 'terms'
EXACT13 = True                         # Phase 13d D2: big_shapes (size 1) on the code's exact lengths (exact_sizes)
_L10 = math.log(10.0)
def _lf(n): return math.lgamma(n + 1.0) / _L10                          # log10 n!

@functools.lru_cache(maxsize=None)
def _terms(T): return mem_model.e_terms(mem_model.digits_of_run(int(T)))

def range_limbs(T, g, r0, r1):
    """limbs of Q (P is the same length to a few limbs) over the nodes [r0, r1) of a g-node run of T total digits"""
    N = _terms(T)
    return (_lf(N * r1 // g) - _lf(N * r0 // g)) / LIMB_DIGITS

def top_factor(T, g):
    """the top node's digits over the average (1 at g = 1 or SHARES = 'even')"""
    if g <= 1 or SHARES != 'terms': return 1.0
    N = _terms(T)
    return range_limbs(T, g, g - 1, g) / (_lf(N) / LIMB_DIGITS / g)

def level_top_group(g, groups=None, which='top'):
    """per level (S, [child limbs fractions]): the top group [start, g) of the level (which = 'bottom': node 0's, [0, S)) and
    its children (the previous level's groups inside it), as node ranges"""
    out = []; P = 1
    for S in mem_model.mn_groups(g, groups):
        if which == 'top': start = ((g - 1) // S) * S
        elif which == 'lastfull': start = max(0, (g // S - 1) * S) if g % S else ((g - 1) // S) * S   # L4: the last group of the level with its full S nodes (= 'top' when S divides g)
        else: start = 0
        end = min(start + S, g); ch = []; k = start
        while k < end: ch.append((k, min(k + P, end))); k += P
        out.append((S, end - start, ch)); P = S
    return out

MIX_PEN = float(os.environ.get('MN_MODEL_MIX_PEN', '0'))   # L4: fractional extra cost of a tree level whose group is not a power of two AND whose children are multi-node groups (the children's pow2 layout is re-sharded onto the general map); measured on aac7 5/6 nodes ([2,5], [4,5], [2,6], [3,6]); 0 = off; set by the aac7_l4 profiles only
TREE_CRIT = os.environ.get('MN_MODEL_TREE_TOPONLY', '0') != '1'   # L4 (2026-10-11): a tree level's time = its slowest group (the last full group vs the clipped top group); MN_MODEL_TREE_TOPONLY=1 = the previous behaviour
def tree_cost(fab, nq_node, g, groups=None, form="grid", T=None, which='crit'):
    """level by level (mem_model.level_children: the level's group of S nodes and its children m_1 .. m_k, the
    previous level's groups): a k-way level is the fold the tree runs (archive:results_merged_2026-10-24/GRID_GROUPS.md §L; tree_level for k = 2):
    (P, Q) <- (P Q_i + P_i, Q Q_i) for i = 2 .. k -- 2 (k - 1) products over the level's S nodes, the accumulated
    operand growing from m_1 to S - m_k leaf shares, the P product with the shifted add (X).
    Phase 13d D2: SHARES = 'terms' (T, the total digits, given): the operands are the top group's children's real lengths"""
    rows = []
    if SHARES == 'terms' and T is not None:
        if TREE_CRIT and which == 'crit':                                  # L4: every level waits for its slowest group; with an uneven last group (S does not divide g) that is
            tops = tree_cost(fab, nq_node, g, groups, form, T, 'top'); fulls = tree_cost(fab, nq_node, g, groups, form, T, 'lastfull')   # the last FULL group, not the clipped top one
            return [b if b[2].t > a[2].t else a for a, b in zip(tops, fulls)]
        for S, Sg, ch in level_top_group(g, groups, 'top' if which == 'crit' else which):
            # mn.c tree_level (2 children: A = the lower, B = the upper) and tree_level_k (Horner from the top child: the running
            # pair starts as child k-1's, then P = P_i Q + P, Q = Q_i Q for i = k-2 .. 0 -- A = child i, B = the running Q)
            c = Cost(); run_ = int(range_limbs(T, g, ch[-1][0], ch[-1][1]))
            for (r0, r1) in reversed(ch[:-1]):
                m = int(range_limbs(T, g, r0, r1))
                c.add(product_cost(fab, m, run_, Sg, with_x=True, form=form))
                c.add(product_cost(fab, m, run_, Sg, form=form))
                run_ += m
            c.add(small_cost(fab, Sg, 2))
            if MIX_PEN and not is_pow2(Sg) and len(ch) >= 2 and ch[0][1] - ch[0][0] > 1: c.t *= 1.0 + MIX_PEN; c.t_exposed *= 1.0 + MIX_PEN
            rows.append((len(ch), Sg, c))
        return rows
    for S, ch in mem_model.level_children(g, groups):
        c = Cost(); acc = ch[0]
        for m in ch[1:]:
            c.add(product_cost(fab, acc * nq_node, m * nq_node, S, with_x=True, form=form))   # P <- P Q_i + P_i
            c.add(product_cost(fab, acc * nq_node, m * nq_node, S, form=form))               # Q <- Q Q_i
            acc += m
        c.add(small_cost(fab, S, 2))
        rows.append((len(ch), S, c))
    return rows

# ------------------------------------------------------------------------------------------------------------
# the single-node phases by digits (log-log interpolation of the measured table; extrapolated beyond it)
# ------------------------------------------------------------------------------------------------------------
def phase(name, D):
    xs = sorted(PHASES)
    if D <= xs[0]: a, b = xs[0], xs[1]
    elif D >= xs[-1]: a, b = xs[-2], xs[-1]
    else:
        a = max(x for x in xs if x <= D); b = min(x for x in xs if x >= D)
        if a == b: return PHASES[a][name]
    ya, yb = PHASES[a][name], PHASES[b][name]
    if ya <= 0 or yb <= 0: return ya + (yb - ya) * (D - a) / (b - a)
    p = math.log(yb / ya) / math.log(b / a)
    return ya * (D / a) ** p

# ------------------------------------------------------------------------------------------------------------
# Phase 13b agent D: the design -- prime count, product strategy, plane cap, chunking, exchange depth, modmul
# ------------------------------------------------------------------------------------------------------------
# The legacy path (design None: the constants above, four primes, the Phase 10/11 phase table) is what --calib and the
# Phase 12 tables were computed with; everything the design table prints goes through Design (the code after step 0).
T_PIECE_31_NP = {4: 1.078, 3: 0.827}   # MEASURED (archive:results_merged_2026-10-24/PRIMES_STRATEGY.md §S13, E0 decimal, s24-26): C at 2^31 over the four APUs, P = 4 / 3
HIDE_POW2 = float(os.environ.get('MN_MODEL_HIDE_POW2', '0.75'))   # MEASURED (results/EXCHANGE.md §X13 3.2): the equal-slab path hides 74-76 % of its xGMI link time
                                       # (Phase 16 C: MN_MODEL_HIDE_POW2 / MN_MODEL_GEN_HIDE1 / MN_MODEL_GEN_HIDE2 / MN_MODEL_MAP_RATE override, as MN_MODEL_T_ROUND does)
GEN_HIDE_DEPTH = {1: float(os.environ.get('MN_MODEL_GEN_HIDE1', '0.011')), 2: float(os.environ.get('MN_MODEL_GEN_HIDE2', '0.74'))}
                                       # general map: MEASURED 1.1 % one deep (X13; 1.4 % on two real nodes, X13b); two deep MEASURED 74.3 %
                                       # on two real nodes (X13b, job 21068; 72.6-75.1 % on one node)
F_MM1 = 58.0 / 58.4                    # MEASURED (archive:results_merged_2026-10-24/KERNELS.md §K13, one pair at 4e10): NTT_MODMUL=1 phases 58.4 -> 58.0 s
MAP_RATE = float(os.environ.get('MN_MODEL_MAP_RATE', '0.065'))   # MEASURED (archive:results_merged_2026-10-24/MEMORY.md §I t_alloc 0.057-0.072 s/GB; P3: 25.8 GB fewer planes = -1.5..-2.9 s of init)
NODE_SCALE = float(os.environ.get('MN_MODEL_NODE_SCALE', '1.0'))   # Phase 16 C: the node's compute (bs, the leaf, the pieces, the size-1 dm) x this -- 1 = aac6's calibration
INIT_SCALE = float(os.environ.get('MN_MODEL_INIT_SCALE', '1.0'))   # Phase 16 C: init x this (the mapping rate of another ROCm / node class on top of MAP_RATE's device term)
if 'MN_MODEL_LOCAL_F' in os.environ:   # Phase 16 F: one factor on the local (non-fabric) terms = NODE_SCALE and INIT_SCALE together (each env above still wins); default 1.0
    if 'MN_MODEL_NODE_SCALE' not in os.environ: NODE_SCALE = float(os.environ['MN_MODEL_LOCAL_F'])
    if 'MN_MODEL_INIT_SCALE' not in os.environ: INIT_SCALE = float(os.environ['MN_MODEL_LOCAL_F'])
# Phase 16 C (results/TIME_MODEL.md §C16 2.3-2.5, 2.7): the aac7 values of the constants above, applied by apply_profile('aac7_c16') (and the base of aac7_s48) -- MEASURED on 2 nodes
# (HIDE_POW2, GEN_HIDE at depth 1 / 2 from COMM_LAYER_STATS, removed 2026-10-19 (D56); T_ROUND from the MN_T_CHUNK_MB 512 / 1024 / 2048 series) and on one node
# (MAP_RATE from t_alloc at ROCm 7.0.3; NODE_SCALE / INIT_SCALE FITTED on the one-node 1e10 / 1e11 runs against this model's aac6 calibration)
AAC7_CONSTS = dict(HIDE_POW2=0.72, GEN_HIDE1=0.011, GEN_HIDE2=0.74, T_ROUND=0.015, MAP_RATE=0.070, NODE_SCALE=1.12, INIT_SCALE=1.20)
# HIDE_POW2 0.72 MEASURED (C16 2.3: 71-72 % at depth 2, 76 % at depth 1); GEN_HIDE aac6's, ASSUMED (no 3-node run); T_ROUND 0.015 = the MEASURED upper
# bound (the whole shift round at 1e10 / 2: 15-18 ms, the fixed part <= 3 ms); MAP_RATE 0.070 MEASURED (t_alloc hipmalloc, 7.0.3 = 7.2.4);
# NODE_SCALE 1.12 and INIT_SCALE 1.20 FITTED on the 7.0.3 one-node 1e11 (205.9 s = 1.12 x the model's 183.8; init 22.2 = 1.20 x 18.4);
# ROCm 7.2.4 on the same node is 168.5 s (NODE_SCALE 0.92, INIT_SCALE 0.88): MN_MODEL_NODE_SCALE / MN_MODEL_INIT_SCALE override
T_ROUND = float(os.environ.get('MN_MODEL_T_ROUND', '0.030'))   # int15k: MN_MODEL_T_ROUND=<s> once the target measures it (TARGET_TASKS T11)   # FITTED on aac6 loopback (M13, 64 MB chunks: 1e10/4 shift +5.4 s over 70 rounds, both +12.9 s over 123, 1e10/2 both +2.2 over 235; least squares, +-100 %):
                                       # the fixed cost of one extra exchange round (launches, the node scan, the sync); ASSUMED on the target
CHUNK_MB = 1024                        # the chunk the table uses for both switches (M13's recommendation for the target)
# the POOL_LOG-30 caps (2^30, 3 2^29): agent P's runs (job 21046) take 1.3-1.4 x the per-product law's big products (cap_factor,
# fitted on those runs; at POOL_LOG 31 the law alone matches the measured 2^31 against 3 2^30 to <= 0.3 s per phase, I's runs)
BATCH_CAP = {1 << 30: 18.9 / 17.45, 3 << 29: 18.9 / 17.45, 1 << 31: 22.65 / 22.3, 3 << 30: 1.0}   # MEASURED at 4e10: the batch tier
                                       # against the 3 2^30 cap (P13b: 18.9 s at POOL_LOG 30 against 17.45; I: 2^31 at four primes 22.65 vs 22.3)
STRATEGIES = ('C', 'B', 'B4', 'auto')
CHUNKS = ('off', 'shift', 'both')

_OLD13 = dict(tight=False, early_free=False, t_chunk_mb=0, shift_chunk_mb=0, pool='max', vmm=False, round_mb=0, cap=None, seed_fill=0, out_early=False, pool1_np=3, arena_room=0.0, dkm=False)   # the Phase 13 memory forms (the calibrated designs)
_P15 = dict(_OLD13, tight=True, early_free=True, t_chunk_mb=1024, shift_chunk_mb=1024, pool='plan', vmm=True, cap=1 << 31)

class Design:
    """one row of the design space.  np: ECALC_NP; strategy: RNS_STRATEGY (C | B | B4 | auto); cap: the plane cap in points
    (None = the code's own rule: 3 2^30 below 5e10 digits of the run, else 2^31); chunk: 'off' | 'shift'
    (MDB_SHIFT_CHUNK_MB) | 'both' (+ MN_T_CHUNK_MB), at chunk_mb; depth: the uneven (alltoallv) exchange's depth 1 | 2;
    modmul: NTT_MODMUL (1 = the default since step 0); legacy: the pre-13b constants (four primes, Phase 10/11 phases)"""
    def __init__(self, np=3, strategy='C', cap=None, chunk='off', depth=1, modmul=1, chunk_mb=CHUNK_MB, legacy=False, p15=False, tight=True, early_free=True,
                 round_mb=1024, pool='plan', vmm=True, recip_cut=True, out_overlap=None, p15b=False, np_mn=None, packed=None, early=None, np_auto=False,
                 p15c=False, arena_room=None, cache_fit=None):
        self.np, self.strategy, self.cap, self.chunk, self.depth, self.modmul, self.chunk_mb, self.legacy = np, strategy, cap, chunk, depth, modmul, chunk_mb, legacy
        self.gen_hide = None; self.force_gen = False                      # the aac6 loopback depth check (design_table --calibrate)
        self.shift_mb = chunk_mb if chunk in ('shift', 'both') else 0
        self.t_mb = chunk_mb if chunk == 'both' else 0
        # Phase 15 (agent MD): p15 = the code since Phase 14 (DEFAULT15()): the memory forms (DM_TIGHT, MN_TREE_EARLY_FREE, the SHMEM pool from the
        # plan, DB_POOL_VMM's host and bs terms, COMM_SHMEM_ROUND_MB = round_mb: 1024 on the target's launch line, the user's D2), NEWTON_RECIP_CUT
        # in the fabric's reciprocal, CAL15 (the one-node compute refitted on the Phase 14 defaults, the seed wait as its own term) and the part
        # file as the code writes it (size > 1: after T1, not overlapped; out_overlap='half' = the model before: hidden under half the division).
        # p15 False: exactly the Phase 13/14 model (every memory form as before, no cut, no CAL15, the part file beyond half the division).
        self.p15, self.tight, self.early_free, self.round_mb, self.pool, self.vmm, self.recip_cut = p15, tight, early_free, round_mb, pool, vmm, recip_cut
        self.out_overlap = out_overlap if out_overlap is not None else ('none' if p15 else 'half')
        # Phase 15 (agent DOC): p15b = the user's decisions of 2026-09-27 on top of p15 -- BS_SEED_FILL=128 (FILL_BS, the regions' bytes in mem_model),
        # BI_MUL1_FAST=1 (SEED15B: no slow path above 2^33), NEWTON_RECIP_MID=1 (the middle product in recip_cost at size > 1), DIST_TWREC=1 (TWREC_F on the
        # pieces' local passes), RNS_AUTO_PIECE_COST=1 (C2: one-node grids only, 0 at the target -- archive:results_merged_2026-10-24/GRID_GROUPS.md §C215), ECALC_CORR_PATCH=2 (no rewrite, no T1 wait),
        # size 1 recip / div by the measured ratios (P15B_RECIP1, P15B_DIV1); packed = ECALC_OUT_PACKED=1 (8 bytes per 18 digits); early = MN_OUT_EARLY=1
        # (size > 1: the part file from the hook before the low product, overlapping OVL1 x the division as at size 1); EXIT_S on both walls.
        # np_mn: ECALC_NP at size > 1 (the target's launch line: 4, the user's decision 1); None = np.
        self.p15b, self.np_mn = p15b, np_mn
        # Phase 15 NP: np_mn = 'auto' (ECALC_NP=auto at size > 1): at_g gives np 3 with np_auto -- every piece of the distributed tiers priced at
        # four primes when its pa + pb exceeds the three-prime bound (piece_np), the leaf and the one-node tiers at three; pool 0 at four planes
        # where the run's largest group can form such a piece (mem_model.np_planes: at the target, as ECALC_NP=4)
        self.np_auto = np_auto
        self.packed = p15b if packed is None else packed
        self.early = p15b if early is None else early
        # Phase 15 (agent DOC2): p15c = the user's decisions of 2026-09-28 on top of p15b (main B2) -- BS_ARENA_ROOM (arena_room, default 0.16 with p15c:
        # mem_model's arena term; at size 1 the division without the fill's remaps, P15C_DIV1), DIST_TWREC_G=1 (the global TWREC_G), RNS_POOL1_4Q=1 (mem_model);
        # cache_fit = RNS_DIST_CACHE_FIT=1 on the launch line (the mn cache's slots as FIT allows them; default with p15c)
        self.p15c = p15c
        self.arena_room = (mem_model.ARENA_ROOM if p15c else 0.0) if arena_room is None else arena_room
        self.cache_fit = p15c if cache_fit is None else cache_fit
    def at_g(self, g):
        """the design as a run of g node-processes uses it: np_mn at size > 1 (ECALC_NP=4 on the target's launch line)"""
        if g > 1 and self.np_mn and self.np_mn != self.np:
            auto = self.np_mn == 'auto'
            d = Design(np=3 if auto else self.np_mn, np_auto=auto, strategy=self.strategy, cap=self.cap, chunk=self.chunk, depth=self.depth, modmul=self.modmul, chunk_mb=self.chunk_mb, legacy=self.legacy,
                       p15=self.p15, tight=self.tight, early_free=self.early_free, round_mb=self.round_mb, pool=self.pool, vmm=self.vmm, recip_cut=self.recip_cut,
                       out_overlap=self.out_overlap, p15b=self.p15b, np_mn=None, packed=self.packed, early=self.early,
                       p15c=self.p15c, arena_room=self.arena_room, cache_fit=self.cache_fit)
            d.gen_hide, d.force_gen = self.gen_hide, self.force_gen
            return d
        return self
    def f_mm(self): return F_MM1 if self.modmul == 1 else 1.0
    def cap_at(self, digits): return self.cap if self.cap is not None else mem_model.code_cap(digits)
    def pool_log(self, digits=1e12): return mem_model.cap_pool(self.cap_at(digits))[0]
    def mem_opts(self, digits):
        o = dict(_P15 if self.p15 else _OLD13)
        if self.p15: o.update(tight=self.tight, early_free=self.early_free, round_mb=self.round_mb, pool=self.pool, vmm=self.vmm,
                              seed_fill=mem_model.SEED_FILL if self.p15b else 0, out_early=bool(self.early), arena_room=self.arena_room)   # Phase 15 (2026-09-27): BS_SEED_FILL, MN_OUT_EARLY; (2026-09-28) BS_ARENA_ROOM
        if self.p15c: o['pool1_np'] = None                          # Phase 15 DOC2: RNS_POOL1_4Q=1 (PS, B2's default): pool 1 at the one-node primes (4 q under ECALC_NP=4); B1 and before: 3 q + 16 (OLD13's pool1_np 3)
        o.update(np='auto' if self.np_auto else self.np, strategy=self.strategy, cap=self.cap_at(digits), shift_chunk_mb=self.shift_mb, t_chunk_mb=self.t_mb, depth=self.depth)
        return o
    def key(self): return (self.np, self.strategy, self.cap, self.chunk, self.depth, self.modmul, self.chunk_mb, self.legacy,
                           self.p15, self.tight, self.early_free, self.round_mb, self.pool, self.vmm, self.recip_cut, self.out_overlap, self.p15b, self.np_mn, self.packed, self.early, self.np_auto,
                           self.p15c, self.arena_room, self.cache_fit)
    def name(self):
        return '%s %s %s d%d%s%s%s%s' % (self.strategy, mem_model.cap_name(self.cap) if self.cap else 'rule', self.chunk, self.depth, ' p15' if self.p15 else '', ('c' if self.p15c else 'b') if self.p15b else '',
                                       ' np-auto' if (self.np_auto or self.np_mn == 'auto') else '', ' cache-fit' if self.cache_fit else '')
    def env(self):
        """the environment that selects this row: RNS_STRATEGY (agent B), ECALC_PLANE_CAP (agent P: sets POOL_LOG,
        RNS_PLANES_3Q30 and DIST_LOGN_TEST), MDB_SHIFT_CHUNK_MB / MN_T_CHUNK_MB (Phase 13a M), COMM_ALLTOALLV_DEPTH (agent X)"""
        e = dict(ECALC_NP='auto' if (self.np_auto or self.np_mn == 'auto') else (self.np_mn or self.np), NTT_MODMUL=self.modmul, RNS_STRATEGY=self.strategy)   # (np_mn: the target's launch line, ECALC_NP=4)
        if self.cap is not None: e['ECALC_PLANE_CAP'] = mem_model.cap_name(self.cap)
        if self.shift_mb: e['MDB_SHIFT_CHUNK_MB'] = int(self.shift_mb)
        if self.t_mb: e['MN_T_CHUNK_MB'] = int(self.t_mb)
        e['COMM_ALLTOALLV_DEPTH'] = self.depth
        if self.p15 and self.round_mb: e['COMM_SHMEM_ROUND_MB'] = int(self.round_mb)   # the target's launch line (D2); the rest are the code's defaults
        if self.cache_fit: e['RNS_DIST_CACHE_FIT'] = 1                    # Phase 15 DOC2: the launch line (the user's decision 2 of 2026-09-28)
        if self.p15b and not self.p15c: e['BS_ARENA_ROOM'] = 0; e['DIST_TWREC_G'] = 0   # B1 on today's code
        return e

def DEFAULT15B(**kw):
    """Phase 15 (agent DOC): the code's defaults of 2026-09-27 (the user's decisions: BS_SEED_FILL=128, BI_MUL1_FAST, NEWTON_RECIP_MID, DIST_TWREC,
    RNS_AUTO_PIECE_COST, ECALC_CORR_PATCH=2, ECALC_OUT_PACKED, MN_OUT_EARLY, ECALC_ODIRECT=auto) on the target's launch line (COMM_SHMEM_ROUND_MB=1024,
    ECALC_NP=4 at size > 1; three primes, the code's default, at size 1)"""
    o = dict(np=3, np_mn=4, strategy='auto', cap=1 << 31, chunk='both', depth=2, modmul=1, p15=True, p15b=True); o.update(kw)   # (np_mn 4: B1's launch line; TARGET_NP is 'auto' since 2026-09-28)
    return Design(**o)
def DEFAULT15C(**kw):
    """Phase 15 (agent DOC2): the code's defaults of 2026-09-28 (main B2: + BS_ARENA_ROOM=0.16, DIST_TWREC_G=1, RNS_POOL1_4Q=1) on the target's launch line
    (ECALC_NP=auto at size > 1, RNS_DIST_CACHE_FIT=1, COMM_SHMEM_ROUND_MB=1024; three primes at size 1)"""
    o = dict(np=3, np_mn=mem_model.TARGET_NP, strategy='auto', cap=1 << 31, chunk='both', depth=2, modmul=1, p15=True, p15b=True, p15c=True); o.update(kw)
    return Design(**o)
DEFAULT = DEFAULT15C()                 # Phase 15 (2026-09-28): the estimate's design (DEFAULT15B() on 2026-09-27; DEFAULT15() before)
DZ = None                              # the design of the run in progress (run() sets it; the cost functions read it)

def round_cost(fab, g):
    """one extra exchange round (a chunked mdb_shift / window / mdb_add_shifted): a small all-to-all's latency over g
    nodes + a collective + the round's fixed cost"""
    return fab.a2a(1.0, g, 1)[0] + fab.coll(g) + T_ROUND

# ---- the per-product law: S13's E0 medians (t_strategy), P = 4 and 3 ---------------------------------------------------
E0_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'results', 'S13_e0.txt')
E0_B13B = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'results', 'D13b_strategy_e0.txt')   # agent B's t_strategy (13b): B4, the library forms
_E0 = None
def e0_table(extra=None):
    """{(P, strategy, logn): (wall, load, ntt, crt, merge)} from results/S13_e0.txt (+ `extra`, e.g. agent B's t_strategy log with B4
    lines); the last line of a size wins (as t_cap).  Strategies: A, B, C, b (B with 128-bit loads), B4 when a log has it"""
    global _E0
    if _E0 is not None and extra is None: return _E0
    import re
    t = {}; lib = set()
    NAMES = {'4': 'B4', 'LC': 'C', 'LB': 'B', 'LB4': 'B4'}         # agent B's t_strategy: strat=4 is B4, strat=L<form> the library's forms
    for fn in [E0_FILE, E0_B13B] + ([extra] if extra else []):
        if not fn or not os.path.exists(fn): continue
        for line in open(fn, errors='replace'):
            m = re.match(r'STRAT logn=(\d+) P=(\d) strat=(\w+) wall_med=([\d.]+).*\| load ([\d.]+) ntt ([\d.]+) crt ([\d.]+) merge ([\d.]+)', line)
            if not m: continue
            st = m.group(3); key = (int(m.group(2)), NAMES.get(st, st), int(m.group(1)))
            if key in lib and not st.startswith('L'): continue       # the library's own form (strat=L...) wins over the test's
            if st.startswith('L'): lib.add(key)
            t[key] = tuple(float(m.group(i)) for i in range(4, 9))
    _E0 = t
    return t

def t_prod(strategy, pts, np):
    """one product on a plane of pts points (2^k or 3 2^k) under a strategy at np primes, seconds.  Returns (t, label):
    'measured' = an E0 median at this 2^k and P; 'modelled' otherwise -- 3 2^k planes 1.5 x 1.05 x the 2^(k+1) time (t_cap's
    rule), a P = 3 size E0 lacks from the P = 4 time x the neighbours' measured P3/P4 ratio, B4 (until agent B's
    t_strategy log is given with --e0) = B with the critical APU's transforms 2.5 n instead of 3 n (rns_dist.c's B4: APUs 0-2
    transform X whole and Y's lower residue, APU 3 the upper residues), B's load and CRT unchanged"""
    E = e0_table()
    logn = 0
    while (1 << logn) < pts: logn += 1
    r3 = (1 << logn) != pts
    lb = logn - 1 if r3 else logn
    f = 1.5 * 1.05 if r3 else 1.0
    lab = 'modelled' if r3 else 'measured'
    s = 'B' if strategy == 'B4' else strategy
    def get(P, lg):
        return E.get((P, strategy if (P, strategy, lg) in E else s, lg))
    v = get(np, lb)
    if v is None:
        v4 = get(4, lb)
        if v4 is None:                                                  # beyond the table: scale the nearest size linearly in points
            ks = sorted(k for (P, st, k) in E if P == 4 and st == s)
            k0 = min(ks, key=lambda k: abs(k - lb)); v0 = get(4, k0); sc = 2.0 ** (lb - k0)
            v4 = tuple(x * sc for x in v0)
        if np == 4: v = v4
        else:
            rs = [get(np, k)[0] / get(4, k)[0] for k in (lb - 1, lb + 1, lb - 2, lb + 2) if get(np, k) and get(4, k)]
            ratio = sum(rs[:2]) / len(rs[:2]) if rs else 0.77
            v = tuple(x * ratio for x in v4)
        lab = 'modelled'
    t = v[0]
    if strategy == 'B4':
        if (np, 'B4', lb) not in E and np == 3: t = v[0] - v[2] / 6.0; lab = 'modelled'   # at P = 4, B already uses the four APUs: B4 = B
    return t * f, lab

# ---- the pipeline's big products (a port of tests/t_cap.c: the grid split_grid_cap forms under a cap, the cuts) -------
def _plane_pts_cap(nc, r3, logmax):
    n = 1 << 20
    while n < nc:
        if r3 and n >= (1 << (logmax - 2)) and n // 2 * 3 >= nc: return n // 2 * 3
        n <<= 1
    return n

@functools.lru_cache(maxsize=None)
def _split_cap(na, nb, cap, r3, logmax):
    best = None
    for i in range(1, 33):
        for j in range(1, 33):
            pa, pb = -(-na // i), -(-nb // j)
            if pa + pb > cap: continue
            pts = _plane_pts_cap(pa + pb, r3, logmax)
            cost = i * j * pts * (21 if pts & (pts - 1) else 20)
            if best is None or cost < best[0] or (cost == best[0] and i * j < best[1] * best[2]): best = (cost, i, j)
    return best[1], best[2]

LEAF13 = True                          # Phase 13d D2: the leaf's mdev levels from the code's span tree (leaf_shapes), not nq/2 x nq/2 and nq/4 x nq/4
BS_SEED_TERMS = 256
BS_MDEV_LOGL = 30                      # RNS_BATCH_LOGL_MAX: a level whose 2 max + 1 limbs exceed 2^30 runs on the mdev tier (binsplit.c)

def _lg10_f(a, b):
    """log10 of P / Q over the terms [a, b) (mn_plan.c lg10_f: f = sum_{m=a}^{b-1} 1 / (a (a+1) ... m))"""
    t = 1.0; f = 0.0
    for m in range(a, min(b, a + 64)):
        t /= m; f += t
        if t < f * 1e-30: break
    return math.log10(f)

def _pq_limbs(a, b):
    """(P limbs, Q limbs) over the terms [a, b): Q = a (a+1) ... (b-1)"""
    lq = (math.lgamma(b) - math.lgamma(a)) / _L10 if b > a + 1 else (math.log10(a) if b == a + 1 else 0.0)
    lim = lambda lg: 1 if lg < 0 else int(math.floor(lg / LIMB_DIGITS)) + 1
    return lim(lq + _lg10_f(a, b)), lim(lq)

@functools.lru_cache(maxsize=None)
def leaf_shapes(D, a0=1, b1=None):
    """binsplit.c's level loop over spans of BS_SEED_TERMS terms (agent L's mn_plan.c plan_leaf): node i of level l covers the spans
    [i 2^l, (i+1) 2^l) (the last cut), pairs (2i, 2i+1) combined as P = P_a Q_b + P_b, Q = Q_a Q_b; the levels whose largest node
    (the last two) has 2 max + 1 > 2^BS_MDEV_LOGL limbs are the mdev tier: every pair's two products, (phase 'top', ...)"""
    if b1 is None: b1 = _terms(D) + 1
    S = BS_SEED_TERMS; nspan = (b1 - a0 + S - 1) // S; n = nspan; out = []; l = 0
    def rng(i, m):
        s0 = i * m; s1 = min((i + 1) * m, nspan); return a0 + s0 * S, min(a0 + s1 * S, b1)
    while n > 1:
        npairs = n // 2; m = 1 << l
        mx = max(max(_pq_limbs(*rng(i, m))) for i in range(max(0, n - 2), n))
        if 2 * mx + 1 > (1 << BS_MDEV_LOGL):
            for i in range(npairs):
                ap, aq = _pq_limbs(*rng(2 * i, m)); bp, bq = _pq_limbs(*rng(2 * i + 1, m))
                out += [('top', 'leaf level %d pair %d P' % (l + 1, i), ap, bq, 0, 1 << 62, 1), ('top', 'leaf level %d pair %d Q' % (l + 1, i), aq, bq, 0, 1 << 62, 1)]
        n = npairs + (n & 1); l += 1
    return tuple(out)

def big_shapes(D, scope=('top', 'recip', 'div'), v13=True):
    """t_cap's shapes at D digits (nq = D / 18 limbs): (phase, name, na, nb, lowcut, w, count)"""
    nq = int(D / 18); k = nq + 1; big = 1 << 62; out = []
    ex = exact_sizes(D) if (EXACT13 and v13) else None                 # Phase 13d D2: the code's lengths (the cuts' skips turn on a few limbs); v13 off = 13b
    if ex: nq = ex['nq']; k = ex['pn'] + 1 + ex['dl'] - nq + 1          # the reciprocal's k_mu (newton_db.c)
    if 'top' in scope:
        if ex and LEAF13: out += leaf_shapes(D)                         # Phase 13d D2: the leaf's device (mdev) levels as binsplit.c forms them
        else: out += [('top', 'tree top', nq // 2, nq // 2, 0, big, 2), ('top', 'tree top-1', nq // 4, nq // 4, 0, big, 4)]
    if 'recip' in scope:
        for j in ((k + 1) // 2, (k + 3) // 4, (k + 7) // 8):
            take = min(2 * j + 2, nq)
            out += [('recip', 'Q_t r', take, j + 1, 0, big, 1), ('recip', 'r d', j + 1, j + 2, 0, big, 1)]
    if 'div' in scope:
        if ex:                                                         # Phase 13d D2: A_h = S >> (nq - 1 - dl), k = S + dl - nq + 1, X = dl + 1
            dl, sn = ex['dl'], ex['sn']; kd = sn + dl - nq + 1
            out += [('div', 'A_h mu', sn - (nq - 1 - dl), kd + 1, kd + 1, big, 1), ('div', 'X Q', dl + 1, nq, 0, nq + 2, 1)]
        elif v13: out += [('div', 'A_h mu', nq + 1, nq + 2, nq + 2, big, 1), ('div', 'X Q', nq + 1, nq, 0, nq + 2, 1)]   # A_h has dl + 1 limbs (was 2 nq + 1)
        else: out += [('div', 'A_h mu', 2 * nq + 1, nq + 2, nq + 2, big, 1), ('div', 'X Q', nq + 1, nq, 0, nq + 2, 1)]    # the Phase 13b shapes (A_h 2 nq + 1)
    return out

EDGE_INIT = 10.0                       # FITTED (agent P's five edge runs, 465-509 GB of device at init: init 34.4-47.1 s against 28-31 by the mapping
                                       # rate, +10 s mean, +-7 s): init near the node's edge is slower than the mapping rate says; ramped in from 440 to 465 GB
EXTRA_MAP = {'B': 11.47 / 154.6, 'B4': 4.50 / 90.2}   # s/GB: the B forms' extra planes mapped inside bs (agent B, jobs 21054)
AUTO_FORM = 'B'                        # rns_dist.c: RNS_STRATEGY_FORM, default B (agent B: B4 is 3-7 % slower than B and fits the same planes)
AUTO_GRID = True                       # RNS_STRATEGY_GRID=1 (the default under auto): the grid weighs B pieces x 0.70, C x 1, 3 2^k x 1.05

def _b_len(nc):
    """rns_dist.c b_len: the B form's transform length -- 2^k, or 3 2^(k-2) where it holds nc (the three-prime set has 3 2^k roots)"""
    k = 20
    while (1 << k) < nc: k += 1
    if k > 20 and (3 << (k - 2)) >= nc: return 3 << (k - 2), True
    return 1 << k, False

@functools.lru_cache(maxsize=None)
def _b_fits(nc, pl, r3, np):
    """rns_dist.c b_fits: the B form's planes at b_len(nc) fit the pools as sized at init (b_place, first fit)"""
    return not any(mem_model.b_extra_limbs(AUTO_FORM, _b_len(nc)[0], pl, r3, np))

@functools.lru_cache(maxsize=None)
def _split_auto_13b(na, nb, cap, r3, logmax, np):
    """the Phase 13b port of split_grid under auto (the B pieces at the C plane length, an absolute tie): the 13b layer (pipe off)"""
    best = None
    for i in range(1, 33):
        for j in range(1, 33):
            pa, pb = -(-na // i), -(-nb // j)
            if pa + pb > cap: continue
            pts = _plane_pts_cap(pa + pb, r3, logmax)
            w = (0.70 if not any(mem_model.b_extra_limbs(AUTO_FORM, pts, logmax, r3, np)) else 1.0) * (1.05 if pts & (pts - 1) else 1.0)
            cost = i * j * pts * w
            if best is None or cost < best[0] - 1e-6 or (abs(cost - best[0]) <= 1e-6 and i * j < best[1] * best[2]): best = (cost, i, j)
    return best[1], best[2]

@functools.lru_cache(maxsize=None)
def _split_auto(na, nb, cap, r3, logmax, np):
    """rns_dist.c split_grid under auto (RNS_STRATEGY_GRID=1): a piece that fits the B form weighs its B length (b_len: 2^k or
    3 2^(k-2)) x 1.05 if 3 2^k x 0.70, one that does not its C plane x 1.05 if 3 2^k; the least cost (within 0.1 %), then the
    fewest pieces (Phase 13d D2: the B pieces' length was the C plane's, so the 3 2^k B pieces at the 2^31 cap were missed)"""
    best = None
    for i in range(1, 33):
        for j in range(1, 33):
            pa, pb = -(-na // i), -(-nb // j)
            if pa + pb > cap: continue
            if _b_fits(pa + pb, logmax, r3, np):
                n, t3 = _b_len(pa + pb); cost = i * j * n * (1.05 if t3 else 1.0) * 0.70
            else:
                pts = _plane_pts_cap(pa + pb, r3, logmax); cost = i * j * pts * (1.05 if pts & (pts - 1) else 1.0)
            if best is None or cost < best[0] * 0.999 or (cost <= best[0] * 1.001 and i * j < best[1] * best[2]): best = (cost, i, j)
    return best[1], best[2]

@functools.lru_cache(maxsize=None)
def product_pieces(na, nb, lowcut, w, strategy, cap, np, v13=True):
    """rns_dist.c mul_grid's pieces of one single-node product: ((ka, kb), [(form, points), ...] of the formed pieces).
    v13 = False: the Phase 13b port (the model node_phases had before 13d: design_table --calibrate's gate)"""
    pl, r3 = mem_model.cap_pool(cap); logmax = pl
    nc = na + nb; one = nc <= cap; auto = strategy == 'auto' and AUTO_GRID
    if not v13:
        if one and r3 and nc > (1 << logmax):
            ka, kb = _split_cap(na, nb, cap, True, logmax); one = ka * kb == 1
        if auto and not (one and not any(mem_model.b_extra_limbs(AUTO_FORM, _plane_pts_cap(nc, r3, logmax), pl, r3, np))):
            ka, kb = _split_auto_13b(na, nb, cap, r3, logmax, np); one = ka * kb == 1
        elif one: ka = kb = 1
        else: ka, kb = _split_cap(na, nb, cap, r3, logmax)
        pa, pb = -(-na // ka), -(-nb // kb); out = []
        for jb in range(kb):
            for ia in range(ka):
                oa, ob = ia * pa, jb * pb
                if oa >= na or ob >= nb: continue
                la, lb = min(pa, na - oa), min(pb, nb - ob)
                if oa + ob >= w or oa + ob + la + lb <= lowcut: continue
                p = _plane_pts_cap(nc if one else la + lb, r3, logmax); st = strategy
                if strategy == 'auto': st = AUTO_FORM if not any(mem_model.b_extra_limbs(AUTO_FORM, p, pl, r3, np)) else 'C'
                out.append((st, p))
        return (ka, kb), tuple(out)
    if one and (nc > (1 << logmax) or (auto and not _b_fits(nc, pl, r3, np))):   # rns_dist.c mul_grid (Phase 13d D2: as the code)
        ka, kb = _split_auto(na, nb, cap, r3, logmax, np) if auto else _split_cap(na, nb, cap, r3, logmax); one = ka * kb == 1
    elif one: ka = kb = 1
    else: ka, kb = _split_auto(na, nb, cap, r3, logmax, np) if auto else _split_cap(na, nb, cap, r3, logmax)
    pa, pb = -(-na // ka), -(-nb // kb); out = []
    for jb in range(kb):
        for ia in range(ka):
            oa, ob = ia * pa, jb * pb
            if oa >= na or ob >= nb: continue
            la, lb = min(pa, na - oa), min(pb, nb - ob)
            if oa + ob >= w or oa + ob + la + lb <= lowcut: continue
            n_ = nc if one else la + lb
            p = _plane_pts_cap(n_, r3, logmax); st = strategy
            if strategy == 'auto':                                      # rns_dist.c b_choose: the form if its planes (at b_len) fit the pools
                if _b_fits(n_, pl, r3, np): st = AUTO_FORM; p = _b_len(n_)[0]
                else: st = 'C'
            out.append((st, p))
    return (ka, kb), tuple(out)

@functools.lru_cache(maxsize=None)
def big_products(D, strategy, cap, np, scope=('top', 'recip', 'div'), pipe=False):
    """the time of the big products at D digits under (strategy, cap, np), per phase: {'top': s, 'recip': s, 'div': s, 'label': ...}.
    'auto' (rns_dist.c b_choose): the B form (RNS_STRATEGY_FORM, default B) for a piece whose planes fit the pools as sized at
    init (whole planes, first fit: b_place), else C; auto never allocates.  Its grid (RNS_STRATEGY_GRID=1) prefers pieces that
    fit B (_split_auto): at the 3 2^30 cap the pieces become 2^31 (which fits the 18 + 18 GiB pools) and every product runs B"""
    pl, r3 = mem_model.cap_pool(cap)
    out = {'top': 0.0, 'recip': 0.0, 'div': 0.0}; labels = set()
    for ph, name, na, nb, lowcut, w, count in big_shapes(D, scope, v13=pipe):   # Phase 13d D2: the 13d structure with the pipeline law (run() under
        t = 0.0                                                         # CAL13), the Phase 13b one without (node_phases' default: the 13b gate)
        (ka, kb), pcs = product_pieces(na, nb, lowcut, w, strategy, cap, np, v13=pipe)
        for st, p in pcs:
            tp, lab = t_prod(st, p, np); labels.add(lab)
            if CAL13 and pipe:                                          # Phase 13d D2: the pipeline's per-product law (fit_pipe, G's logs)
                tp = tp * PIPE_ONE.get(st, 1.0) + ((GRID_ADD.get(st, 0.0) * p + GRID_NC * (na + nb)) / (1 << 31) if ka * kb > 1 else 0.0)
            t += tp
        out[ph] += t * count
    out['label'] = 'modelled' if 'modelled' in labels else 'measured'
    k = cap_factor(cap, np) if pl <= 30 else 1.0
    if (strategy, cap) in STRAT_FIT and np == 3: k *= strat_factor(strategy, cap)
    for ph in ('top', 'recip', 'div'): out[ph] *= k
    return out

# the M-run (Phase 13b, ~/mrun on aac6, results/mrun_13b.log): 4e10 at size 1, three primes, NTT_MODMUL=1, agent K's kernels on, s24-26
K_FACTOR = 62.91 / 65.38               # MEASURED: auto at 2^31 with NTT_B1R=3 NTT_PLAN=1 (n 8) against without (n 3)
STRAT_FIT = {                          # FITTED: the (strategy, cap) rows the per-product law misses by > 3 % -- measured mean wall (K on), n
    ('auto', 1 << 30): (86.17, 3),     #   auto's grid prefers B-fitting 2^29 pieces at the 2^30 cap: dm 40.3 s against C's 34.3 (the law says faster)
    ('B4', 3 << 29): (67.88, 3),       #   B4 at 3 2^29: 67.9 s against 70.9 modelled
}
_SF = {}
def strat_factor(strategy, cap):
    """the factor on the big products that makes the model's 4e10 wall at (strategy, cap) equal the M-run's (K removed by K_FACTOR)"""
    if (strategy, cap) in _SF: return _SF[(strategy, cap)]
    _SF[(strategy, cap)] = 1.0
    d = Design(strategy=strategy, cap=cap)
    p = node_phases(4e10, d); tot = sum(v for k, v in p.items() if k != 'label')
    bp = big_products(4e10, strategy, cap, 3); bps = sum(bp[ph] for ph in ('top', 'recip', 'div'))
    target = STRAT_FIT[(strategy, cap)][0] / K_FACTOR
    k = 1.0 + (target - tot) / (bps * d.f_mm()) if bps > 0 else 1.0
    _SF[(strategy, cap)] = k; big_products.cache_clear()
    return k

_CAPF = {}
def cap_factor(cap, np):
    """the POOL_LOG-30 caps: measured (top + recip + div at 4e10, agent P's runs) less the model's rest, over the per-product law's
    big products at that cap -- the factor by which the pipeline's products at 2^30 pools exceed t_strategy's isolated ones (the
    mechanism is not identified: the mdev tier's pieces at 3 2^28 with three primes, pool 1 at the batch tile, DIST_LOGN_TEST=30).
    FITTED on one run per cap; 1 where no run exists"""
    if cap in _CAPF: return _CAPF[cap]
    _CAPF[cap] = 1.0
    rs = [r for r in RUNS if r['use'] == 'cap' and r['cap'] == cap and r['D'] == 4e10]
    if not rs: return 1.0
    meas = _mean(r['dm'] + r['top'] for r in rs)                     # top + recip + div
    d = Design(np=rs[0]['np'], cap=cap, modmul=rs[0].get('mm', 0))
    p = node_phases(4e10, d)                                          # with factor 1 (set above while computing)
    bp = big_products(4e10, 'C', cap, rs[0]['np'])
    bps = sum(bp[ph] for ph in ('top', 'recip', 'div')); tot = sum(p[ph] for ph in ('top', 'recip', 'div'))
    k = 1.0 + (meas - tot) / (bps * d.f_mm()) if bps > 0 else 1.0
    _CAPF[cap] = k; big_products.cache_clear()
    return k

# ---- the measured size-1 runs of the current code (the phase table's inputs and the calibration's points) -----------------
# per run: D, np, modmul, cap (points; the code's rule where the run left it), total, init, bs, top (= the bs line's mdev part),
# recip, dm (= recip + div), device GB at init, host HWM GB, use ('table': the phase table; 'fit': the three-prime factors;
# 'check': not an input), node, source.  Seconds.  The logs are on aac6 (paths in the source strings).
R31 = 1 << 31; R3_30 = 3 << 30
RUNS = [
    # 1e9
    dict(D=1e9, np=4, cap=R3_30, total=18.21, init=13.9, bs=0.88, top=0.0, recip=2.57, dm=2.88, dev=186.4, hwm=15.9, use='table', src='P3 b2 e9_np4 (job 21005, s24-26)'),
    dict(D=1e9, np=3, cap=R3_30, total=15.09, init=11.4, bs=0.88, top=0.0, recip=2.17, dm=2.44, dev=160.6, hwm=15.7, use='check', src='P3 b2 e9_np3 (job 21005, s24-26)'),
    dict(D=1e9, np=4, cap=R31, total=14.27, init=10.3, bs=0.90, top=0.0, recip=2.43, dm=2.74, dev=126.2, hwm=16.0, use='check', src='i12 e9_def (Phase 12 I, 2^31 planes)'),
    # 1e10
    dict(D=1e10, np=4, cap=R3_30, total=36.22, init=18.5, bs=7.88, top=0.0, recip=4.87, dm=7.61, dev=234.4, hwm=16.6, use='table', src='M13 b1 e10 (job 21008, s24-30)'),
    # 4e10, four primes, the code's cap (3 2^30)
    dict(D=4e10, np=4, cap=R3_30, total=81.30, init=22.5, bs=32.63, top=10.5, recip=12.33, dm=26.11, dev=313.3, hwm=12.2, use='table', src='P3 b4 e4e10_def (job 21009, s24-26)'),
    dict(D=4e10, np=4, cap=R3_30, total=80.03, init=21.8, bs=32.39, top=10.3, recip=12.11, dm=25.83, dev=313.3, hwm=12.1, use='table', src='P3 b6 e4e10_def2 (job 21021, s24-26)'),
    dict(D=4e10, np=4, cap=R3_30, total=82.56, init=20.8, bs=34.24, top=10.9, recip=12.96, dm=27.43, dev=313.3, hwm=12.1, use='table', src='M13 b1 e4e10 (job 21008, s24-30)'),
    dict(D=4e10, np=4, cap=R3_30, total=81.60, init=22.6, bs=32.9, top=None, recip=None, dm=26.1, dev=313.3, hwm=12.1, n=5, use='table', src='RESULTS 78 closing series, defaults (job 21039, s24-26): 82.82/81.82/81.31/79.46/82.78'),
    dict(D=4e10, np=4, cap=R3_30, total=80.58, init=21.8, bs=32.65, top=10.5, recip=12.28, dm=26.06, dev=313.3, hwm=12.1, n=5, use='check', src='i12 e4_auto1-5 (Phase 12 I, 3 2^30 planes by the size rule; 80.44/78.81/82.19/81.71/79.77)'),
    # 4e10, four primes, cap 2^31 (RNS_PLANES_3Q30 off): the cap axis, measured once (Phase 12 code)
    dict(D=4e10, np=4, cap=R31, total=81.96, init=18.04, bs=35.25, top=12.6, recip=13.36, dm=28.61, dev=253.1, hwm=12.1, n=5, use='check', src='i12 e4_def/def2/def3/off/t176 (Phase 12 I, 2^31 planes: 81.48/82.26/82.07/81.99/82.02)'),
    # 4e10, three primes (the step-0 default's prime count, NTT_MODMUL=0)
    dict(D=4e10, np=3, cap=R3_30, total=68.92, init=21.9, bs=25.90, top=8.3, recip=9.90, dm=21.02, dev=287.5, hwm=12.2, use='fit', src='int13 close10 run1 (job 21041, s24-26)'),
    dict(D=4e10, np=3, cap=R3_30, total=68.63, init=21.3, bs=26.23, top=8.2, recip=9.94, dm=21.01, dev=287.5, hwm=12.1, use='fit', src='int13 close10 run2 (job 21041)'),
    dict(D=4e10, np=3, cap=R3_30, total=69.30, init=22.3, bs=25.83, top=8.3, recip=10.01, dm=21.13, dev=287.5, hwm=12.1, use='fit', src='int13 close10 run3 (job 21039)'),
    dict(D=4e10, np=3, cap=R3_30, total=69.91, init=23.0, bs=25.68, top=8.2, recip=10.08, dm=21.17, dev=287.5, hwm=12.1, use='fit', src='int13 close10 run4 (job 21039)'),
    dict(D=4e10, np=3, cap=R3_30, total=67.95, init=21.0, bs=25.75, top=8.2, recip=9.98, dm=21.13, dev=287.5, hwm=12.1, use='fit', src='int13 close10 run5 (job 21039)'),
    dict(D=4e10, np=3, cap=R3_30, total=66.40, init=19.9, bs=25.57, top=8.3, recip=9.81, dm=20.88, dev=287.5, hwm=12.1, use='fit', src='P3 b3 (job 21005, s24-26)'),
    dict(D=4e10, np=3, cap=R3_30, total=66.51, init=19.6, bs=25.91, top=8.3, recip=9.83, dm=20.95, dev=287.5, hwm=12.1, use='fit', src='P3 b4 (job 21009, s24-26)'),
    dict(D=4e10, np=3, cap=R3_30, total=65.46, init=18.6, bs=25.81, top=8.3, recip=9.99, dm=21.01, dev=287.5, hwm=12.1, use='fit', src='P3 b6 np3_2 (job 21021, s24-26)'),
    dict(D=4e10, np=3, cap=R3_30, total=71.32, init=24.5, bs=25.55, top=8.3, recip=10.10, dm=21.22, dev=287.5, hwm=12.2, use='check', src='P3 b6 np3_lmin8 (job 21021; RNS_BATCH_LOCAL_MIN=8, which did not engage)'),
    # 8e10, 1e11: four primes, 2^31 planes (the size rule)
    # 4e10, three primes, NTT_MODMUL=1 (step 0): agent P's plane-cap switch (ECALC_PLANE_CAP), the POOL_LOG-30 caps (cap_factor's inputs)
    dict(D=4e10, np=3, mm=1, cap=1 << 30, total=84.20, init=13.7, bs=33.95, top=14.2, recip=16.40, dm=36.48, dev=184.9, hwm=12.1, use='cap', src='P13b b1 e4e10_230 (job 21046, s24-30)'),
    dict(D=4e10, np=3, mm=1, cap=3 << 29, total=76.79, init=15.2, bs=31.08, top=11.0, recip=13.86, dm=30.44, dev=202.1, hwm=11.9, use='cap', src='P13b b1 e4e10_3229 (job 21046, s24-30)'),
    dict(D=1e9, np=3, mm=1, cap=1 << 30, total=9.99, init=6.3, bs=0.96, top=0.0, recip=2.16, dm=2.45, dev=66.1, hwm=16.0, use='check', src='P13b b1 e9_230 (job 21046)'),
    dict(D=1e9, np=3, mm=1, cap=3 << 29, total=11.63, init=8.0, bs=0.89, top=0.0, recip=2.06, dm=2.35, dev=83.3, hwm=15.8, use='check', src='P13b b1 e9_3229 (job 21046)'),
    dict(D=1e9, np=3, mm=1, cap=1 << 31, total=13.56, init=10.0, bs=0.89, top=0.0, recip=2.03, dm=2.30, dev=109.1, hwm=15.9, use='check', src='P13b b1 e9_231 (job 21046)'),
    dict(D=1e9, np=3, mm=1, cap=3 << 30, total=16.78, init=13.0, bs=0.85, top=0.0, recip=2.10, dm=2.50, dev=160.6, hwm=16.2, use='check', src='P13b b1 e9_3230 (job 21046)'),
    dict(D=4e10, np=3, mm=1, cap=R31, total=70.73, init=18.2, bs=28.1, top=None, recip=None, dm=24.3, dev=236.0, hwm=12.1, use='check', src='P13b 2^31 (s24-30)'),
    dict(D=4e10, np=3, mm=1, cap=R31, total=67.89, init=17.3, bs=None, top=None, recip=None, dm=None, dev=236.0, hwm=12.2, use='check', src='P13b 2^31 (s24-16; phases 50.6)'),
    dict(D=4e10, np=3, mm=1, cap=R3_30, total=65.09, init=18.7, bs=25.1, top=None, recip=None, dm=21.2, dev=287.5, hwm=12.1, use='check', src='P13b b2 3*2^30 (job 21051, s24-16)'),
    # agent P's one-node ceilings at three primes (archive:results_merged_2026-10-24/MEMORY.md §P13b; NTT_MODMUL=1): beyond the phase table (extrapolated walls), the device exact
    dict(D=1e11, np=3, mm=1, cap=R3_30, total=206.9, init=45.5, bs=76.3, top=None, recip=None, dm=85.0, dev=465.5, hwm=14.1, use='edge', src='P13b 3*2^30 1e11 (job 21051, s24-16)'),
    dict(D=1.14e11, np=3, mm=1, cap=R3_30, total=228.7, init=34.5, bs=104.2, top=None, recip=None, dm=89.9, dev=509.0, hwm=14.8, use='edge', src='P13b 3*2^30 edge (job 21059, s24-30; 1.16e11 OOM-killed at 528.7)'),
    dict(D=1.30e11, np=3, mm=1, cap=R31, total=353.1, init=38.6, bs=148.6, top=None, recip=None, dm=165.8, dev=507.1, hwm=15.7, use='edge', src='P13b 2^31 edge (job 21069, s24-30; 1.34e11 allocation refused)'),
    dict(D=1.42e11, np=3, mm=1, cap=1 << 30, total=677.2, init=34.4, bs=242.4, top=None, recip=None, dm=400.2, dev=501.4, hwm=16.4, use='edge', src='P13b 2^30 (job 21056, s24-30)'),
    dict(D=1.44e11, np=3, mm=1, cap=1 << 30, total=1085.8, init=47.1, bs=376.3, top=None, recip=None, dm=662.2, dev=507.6, hwm=16.5, use='edge', src='P13b 2^30 edge (job 21072, s24-26; 1.46e11 OOM-killed at 529.6)'),
    dict(D=8e10, np=4, cap=R31, total=191.99, init=20.9, bs=86.34, top=36.1, recip=38.61, dm=84.67, dev=369.1, hwm=13.0, use='table', src='i12 e8 (Phase 12 I, s24-26)'),
    dict(D=8e10, np=4, cap=R31, total=189.33, init=23.1, bs=83.57, top=35.6, recip=37.56, dm=82.53, dev=369.1, hwm=13.0, use='table', src='i12 e8b (Phase 12 I, s24-26)'),
    dict(D=8e10, np=4, cap=R31, total=195.5, init=None, bs=None, top=None, recip=None, dm=None, dev=369.1, hwm=12.8, use='check', src='M11 v3 (Phase 11 code, s24-16)'),
    dict(D=1e11, np=4, cap=R31, total=262.9, init=25.9, bs=117.0, top=51.4, recip=52.9, dm=119.8, dev=431.2, hwm=14.0, use='table', src='M11 v4 (Phase 11 code, s24-26; the only 1e11 of the tail layout)'),
]

def _mean(xs):
    xs = [x for x in xs if x is not None]; return sum(xs) / len(xs) if xs else None

def _run_phases(r):
    """a run's phases: batch (bs less its mdev levels), top, recip, div, other (the rest of the total)"""
    if r.get('recip') is None: return None
    top = r['top'] or 0.0; batch = r['bs'] - top; div = r['dm'] - r['recip']
    return dict(init=r['init'], batch=batch, top=top, recip=r['recip'], div=div, other=max(0.0, r['total'] - r['init'] - r['bs'] - r['dm']))

def _dev_init_ref(D, np, cap):
    return mem_model.mem_per_node(int(D), 1, dict(_OLD13, np=np, cap=cap))['dev_init'] / 1e9

@functools.lru_cache(maxsize=None)
def phase_table(exclude=None):
    """{D: dict(init, batch, top_rest, recip_rest, div_rest, other)} at four primes, NTT_MODMUL=0, strategy C, with the big products
    (t_cap's shapes at the run's cap) taken out of top / recip / div -- the 'rest' is cap-free; init normalised to the device of
    the four-prime, 2^31 configuration by MAP_RATE.  exclude: a D left out (the leave-one-out check)"""
    out = {}
    for D in sorted(set(r['D'] for r in RUNS if r['use'] == 'table')):
        if exclude is not None and D == exclude: continue
        rs = [r for r in RUNS if r['use'] == 'table' and r['D'] == D]
        ps = [p for p in (_run_phases(r) for r in rs) if p]
        cap = rs[0]['cap']
        bp = big_products(D, 'C', cap, 4)
        e = dict(batch=_mean(p['batch'] for p in ps), top=_mean(p['top'] for p in ps), recip=_mean(p['recip'] for p in ps),
                 div=_mean(p['div'] for p in ps), other=_mean(p['other'] for p in ps))
        # the walls: the mean total of every table run (weighted by n) sets init + other so the table reproduces the mean wall
        wsum = sum(r['total'] * r.get('n', 1) for r in rs); n = sum(r.get('n', 1) for r in rs)
        isum = sum(r['init'] * r.get('n', 1) for r in rs if r['init'] is not None); ni = sum(r.get('n', 1) for r in rs if r['init'] is not None)
        e['wall'] = wsum / n; e['init'] = isum / ni
        e['other'] = max(0.0, e['wall'] - e['init'] - e['batch'] - e['top'] - e['recip'] - e['div'])
        for ph in ('top', 'recip', 'div'):                            # may be negative (at 1e10 the tree's top levels run inside the
            e[ph + '_rest'] = e[ph] - bp[ph]                          # batch tier): the table point is then reproduced exactly, and a
                                                                      # design differs from it by its big products' difference
        e['init_ref'] = e['init'] - MAP_RATE * (_dev_init_ref(D, 4, cap) - _dev_init_ref(D, 4, R31))
        e['batch_ref'] = e['batch'] / BATCH_CAP.get(cap, 1.0)           # the batch tier normalised to the 3 2^30 cap
        e['cap'] = cap
        out[D] = e
    return out

def _interp(tab, key, D):
    xs = sorted(tab)
    if len(xs) == 1: return tab[xs[0]][key] * D / xs[0]
    if D <= xs[0]: a, b = xs[0], xs[1]
    elif D >= xs[-1]: a, b = xs[-2], xs[-1]
    else:
        a = max(x for x in xs if x <= D); b = min(x for x in xs if x >= D)
        if a == b: return tab[a][key]
    ya, yb = tab[a][key], tab[b][key]
    if ya <= 0 or yb <= 0: return ya + (yb - ya) * (D - a) / (b - a)
    return ya * (D / a) ** (math.log(yb / ya) / math.log(b / a))

@functools.lru_cache(maxsize=None)
def np_factors(np):
    """the three-prime factor of each phase's rest (and of the batch tier), from the 4e10 series at NP = 3 against the table's
    4e10 at NP = 4 (both at the code's 3 2^30 cap; MEASURED inputs, the factor is the model's)"""
    if np == 4: return dict(batch=1.0, top=1.0, recip=1.0, div=1.0, other=1.0)
    tab = phase_table(); e4 = tab[4e10]
    ps = [p for p in (_run_phases(r) for r in RUNS if r['use'] == 'fit' and r['np'] == np and r['D'] == 4e10) if p]
    bp = big_products(4e10, 'C', R3_30, np)
    f = dict(batch=_mean(p['batch'] for p in ps) / e4['batch_ref'])
    rest3 = sum(_mean(p[ph] for p in ps) - bp[ph] for ph in ('top', 'recip', 'div'))
    rest4 = sum(e4[ph + '_rest'] for ph in ('top', 'recip', 'div'))
    for ph in ('top', 'recip', 'div'): f[ph] = rest3 / rest4          # one factor for the rest of the three (the division's rest alone
    f['other'] = 1.0                                                  # is 0.4 s at 4e10: its own ratio would be noise)
    return f

_BIG = {}
def node_big(D, dz, g=1):
    """the design's big products (with the pipeline law) inside node_phases' top and recip + div: (top, dm) seconds"""
    if (D, dz.key(), g) not in _BIG: node_phases(D, dz, g, pipe=True)
    return _BIG[(D, dz.key(), g)]

DM_REST = 4.00                         # Phase 13d D2: dm's rest (the reciprocal's small steps, the shifts, adds, window, corrections) as
                                       # DM_REST x (D / 4e10) seconds -- FITTED (fit13) on measured dm less the pipeline law's big products;
                                       # None: CAL13_LAW['dm'] on the phase table's rest (whose extrapolation beyond 1e11 runs 2.5 x high)
def cal13_apply(D, dz, g, bs, dm):
    """Phase 13d D2: CAL13's smooth ratio on the rest of bs (the phase table's part) and dm's rest (DM_REST, linear in D), the big products
    as the pipeline law has them"""
    bt, bd = node_big(D, dz, g)
    dm1 = (bd + DM_REST * D / 4e10) if DM_REST is not None else (cal13(D, 'dm') * (dm - bd) + bd)
    return cal13(D, 'bs') * (bs - bt) + bt, dm1

def node_phases(D, dz, g=1, exclude=None, pipe=False):
    """the per-node phases of one node at D digits per node under design dz: init, batch, top, recip, div, other (seconds) and
    the label of each.  At g = 1 the whole pipeline; at g > 1 init, batch and top (the leaf) -- the distributed levels, the
    reciprocal and the division come from the fabric model.  top / recip / div = rest x the prime factor + the big products
    under (strategy, cap) (S13's per-product law); x F_MM1 with NTT_MODMUL=1; init by the mapped device (MAP_RATE)"""
    tab = phase_table(exclude); f = np_factors(dz.np); fm = dz.f_mm()
    digits = D * g; cap = dz.cap_at(digits)
    bp = big_products(D, dz.strategy, cap, dz.np, pipe=pipe)            # Phase 13d D2: pipe = the pipeline law on the design pieces (run() under CAL13); off = the Phase 13b model
    out = dict(batch=_interp(tab, 'batch_ref', D) * BATCH_CAP.get(cap, 1.0) * f['batch'] * fm, other=_interp(tab, 'other', D))
    for ph in ('top', 'recip', 'div'):
        out[ph] = max(0.0, _interp(tab, ph + '_rest', D) * f[ph] + bp[ph]) * fm
    o = dz.mem_opts(digits); oc = dict(o, strategy='C')
    dev = mem_model.mem_per_node(int(D), g, oc)['dev_init'] / 1e9          # the pools and the arena, mapped at init
    extra = mem_model.mem_per_node(int(D), g, o)['dev_init'] / 1e9 - dev   # B / B4 forced: the grow-only buffer, mapped inside bs
    out['init'] = _interp(tab, 'init_ref', D) + MAP_RATE * (dev - _dev_init_ref(D, 4, R31)) + EDGE_INIT * min(1.0, max(0.0, (dev - 440.0) / 25.0))
    out['top'] += EXTRA_MAP.get(dz.strategy, MAP_RATE) * extra        # MEASURED per form (agent B, 4e10: 11.47 s for 154.6 GB, 4.50 s for 90.2 GB)
    out['label'] = 'modelled (%s products)' % bp['label']
    if pipe: _BIG[(D, dz.key(), g)] = (bp['top'] * fm, (bp['recip'] + bp['div']) * fm)   # Phase 13d D2: the big products inside top / recip + div (CAL13's law acts on the rest)
    return out

# ---- Phase 13d D2: the per-node compute recalibrated on the Phase 13c defaults ------------------------------------------------
# node_phases is the Phase 13b model (K's kernels off, the four-prime phase table extrapolated beyond 4e10 by the rest's law);
# at 7.64e10 it ran 6.2 % optimistic (RESULTS 80) -- all of it in dm (the design table's leaf_scale, fitted at 4e10 where the
# model's bs is 12 % high, also scaled bs down by 5 %).  The recalibration: per phase group (init, bs, dm) the ratio measured /
# node_phases of the DEFAULT design at every measured one-node size of the 13c code, interpolated linearly in log D (flat
# outside the measured range) and applied to every non-legacy design in run() (CAL13 = True).  K-off runs enter through the
# measured K ratios.  At g > 1: bs's ratio at the top node's leaf; dm's ratio (the pipeline's big products against the
# isolated t_strategy pieces: ~85 % of dm at >= 7e10) on the fabric pieces' local part (ASSUMED to carry over).
CAL13 = True
# Phase 13d D2: the pipeline's per-product law.  S13's t_strategy medians are isolated products; in the pipeline (G's sweep logs, every
# 'dist_db' line with its time: fit_pipe) a one-plane product takes PIPE_ONE x the law by form, and a piece of a grid adds GRID_ADD
# seconds per 2^31 points (the piece's temporary and its shifted add into C).  FITTED on G13d's logs (see archive:results_merged_2026-10-24/TIME_MODEL.md §D213d).
PIPE_ONE = {'C': 1.221, 'B': 1.150, 'B4': 1.150}     # FITTED (fit_pipe on G13d's 11 sweep logs, 303 products / 1381 pieces >= 2^27 limbs)
GRID_ADD = {'C': 0.0, 'B': 0.075, 'B4': 0.075}       # FITTED (s per 2^31 points of the piece)
GRID_NC = 0.0793                          # s per piece of a grid per 2^31 limbs of the whole product (the size-1 grid's accumulation passes over C;
                                       # the mn tier normalises once per product, mn_grid: not applied there)
K_BS = 23.5 / 25.2                     # MEASURED (M-run 13b, auto 2^31 at 4e10): K's kernels on (n 8) / off (n 3), bs
K_DM = 22.5 / 22.9                     #   and dm; ASSUMED size-independent where a K-off run is used
CAL13_RUNS = [   # D, K on, total, init, bs, dm, n, use ('fit' | 'check'), source -- the 13c defaults (auto, 2^31, shift 1024, depth 2) or as noted
    dict(D=4e10, k=1, total=63.5, init=17.2, bs=23.7, dm=22.5, n=5, use='fit', src='RESULTS 80: five-run series C13c, s24-26 (63.5 +- 1.5 s)'),
    dict(D=7.64e10, k=1, total=137.9, init=19.5, bs=57.5, dm=60.8, n=1, use='fit', src='RESULTS 80: the share run, s24-30'),
    dict(D=1.30e11, k=0, total=353.1, init=38.6, bs=148.6, dm=165.8, n=1, use='check', strategy='C', node='s24-30',
         src='P13b 2^31 edge (job 21069, s24-30): RNS_STRATEGY=C (not auto), K off, no chunking, depth 1 -- a shape check, not fitted'),
    # G13d's sweep (a), job 21105, s24-16, main's defaults (auto, 2^31, shift 1024, depth 2, K's kernels): ~/g13d/a_*.log, every run VERIFY OK
    dict(D=6.59e10, k=1, total=109.83, init=22.0, bs=45.3, dm=42.5, n=1, use='fit', src='G13d a_S3_lo_659e8 (s24-16)'),
    dict(D=6.66e10, k=1, total=120.97, init=22.8, bs=46.0, dm=52.0, n=1, use='fit', src='G13d a_S3_hi_666e8 (s24-16)'),
    dict(D=7.20e10, k=1, total=126.43, init=21.0, bs=50.8, dm=54.5, n=1, use='fit', src='G13d a_mid_720e8 (s24-16)'),
    dict(D=7.77e10, k=1, total=150.50, init=24.4, bs=58.1, dm=67.9, n=1, use='fit', src='G13d a_S4_hi_777e8 (s24-16)'),
    dict(D=8.56e10, k=1, total=172.55, init=22.6, bs=71.6, dm=77.7, n=1, use='fit', src='G13d a_S5_lo_856e8 (s24-16)'),
    dict(D=8.61e10, k=1, total=178.92, init=22.3, bs=72.7, dm=83.8, n=1, use='fit', src='G13d a_S5_hi_861e8 (s24-16)'),
    dict(D=9.24e10, k=1, total=189.83, init=24.0, bs=77.0, dm=88.7, n=1, use='fit', src='G13d a_S6_lo_924e8 (s24-16)'),
    dict(D=9.31e10, k=1, total=191.70, init=20.7, bs=78.4, dm=92.5, n=1, use='fit', src='G13d a_S6_hi_931e8 (s24-16)'),
    dict(D=1.156e11, k=1, total=307.85, init=31.4, bs=114.6, dm=161.7, n=1, use='fit', src='G13d a_S9_lo_1156e8 (s24-16)'),
    dict(D=1.163e11, k=1, total=313.86, init=28.3, bs=114.4, dm=170.9, n=1, use='fit', src='G13d a_S9_hi_1163e8 (s24-16)'),
]
                                       # calib13 only (the model predicts an s24-16 / s24-26 node)

_C13 = {}
def _cal13_points():
    if 'pts' in _C13: return _C13['pts']
    d = DEFAULT13(); pts = []
    for r in CAL13_RUNS:
        if r['use'] != 'fit': continue
        p = node_phases(r['D'], d)
        kb, kd = (1.0, 1.0) if r['k'] else (K_BS, K_DM)
        pts.append((r['D'], dict(init=r['init'] / p['init'], bs=r['bs'] * kb / (p['batch'] + p['top']), dm=r['dm'] * kd / (p['recip'] + p['div']))))
    pts.sort(key=lambda x: x[0]); _C13['pts'] = pts
    return pts

CAL13_MODE = 'law'                     # 'law': the ratio a (D / 4e10)^b per group (CAL13_LAW, fitted by fit13 with PIPE_F); 'interp': between the runs
CAL13_LAW = {'init': (1.047, -0.105), 'bs': (0.753, -0.071), 'dm': (1.0, 0.0)}   # FITTED (fit13 on CAL13_RUNS' 'fit' rows; 'dm' unused with DM_REST)
CAL13_RANGE = (4e10, 1.3e11)           # the law is held flat outside the measured range

def cal13(D, key):
    """the ratio measured / node_phases for phase group key (init | bs | dm) at D digits per node (1 without CAL13)"""
    if not CAL13: return 1.0
    if CAL13_MODE == 'law':
        a, b = CAL13_LAW[key]; Dc = min(max(D, CAL13_RANGE[0]), CAL13_RANGE[1])
        return a * (Dc / 4e10) ** b
    pts = _cal13_points()
    if not pts: return 1.0
    if D <= pts[0][0]: return pts[0][1][key]
    if D >= pts[-1][0]: return pts[-1][1][key]
    for (a, fa), (b, fb) in zip(pts, pts[1:]):
        if a <= D <= b:
            t = math.log(D / a) / math.log(b / a); return fa[key] + t * (fb[key] - fa[key])





def cal13_reset():
    global PIECE13
    _BIG.clear(); _C13.clear(); _PC.clear(); big_products.cache_clear(); phase_table.cache_clear(); np_factors.cache_clear(); _CAPF.clear(); _SF.clear()
    PIECE13 = 1.0; PIECE13 = piece13()

def piece13():
    """the fabric piece's local-part factor: the one-node pipeline's C-form factor PIPE_ONE['C'] (ASSUMED to carry to the mn tier's
    pieces, which S13's isolated C at 2^31 prices); a piece of a grid adds GRID_ADD['C'] (piece_cost)"""
    return PIPE_ONE.get('C', 1.0) if CAL13 else 1.0
PIECE13 = 1.0

def DEFAULT13(): return Design(np=3, strategy='auto', cap=1 << 31, chunk='shift', depth=2, modmul=1)   # the Phase 13c defaults

# ---- Phase 15 (agent MD): the one-node compute refitted on the Phase 14 defaults, the seed wait, the part file -------------------------------
# CAL13 (above) is the 13c calibration; on the Phase 14 defaults (RNS_PLANES_FIRST, NEWTON_RECIP_CUT, DB_POOL_VMM + DM_TIGHT, ...) it had init
# +7 s, bs -7 s and dm +7 s at 1e11 (23.6 / 90.0 / 110.0 against the measured 16.5 / 97.2 / 102.8).  CAL15 is a ratio law per group on top of
# it, fitted like fit13 (weighted log-linear least squares in D / 4e10, held flat outside CAL15_RANGE) on CAL15_RUNS, and applies to every p15
# design: init and bs (without the seed wait) at the leaf of the slowest node, dm at size 1 only (at size > 1 the reciprocal and the division
# are the fabric model's pieces, with NEWTON_RECIP_CUT in the plan).  The seed wait (bs waiting for the level-0 spans after init; V3,
# archive:results_merged_2026-10-24/SINGLE_NODE.md §V314 1) is its own term: the seeds end at SEED15[0] + SEED15[1] x the node's span digits / 1e9 after the start, where a term above
# 2^33 costs SLOW_F x (mul1_serial's u128 path, measured x 1.29 per limb-step on the node's Zen 4 CPU by V3) -- at the 576 target every
# term of the top node is above 2^33, on one node only those of the top 18 % (1e11) to 36 % (1.3e11) of the digits.
CAL15 = True
CAL15_RANGE = (4e10, 1.3e11)
CAL15_LAW = {'init': (0.6589, 0.2432), 'bs': (1.0154, -0.206), 'recip': (0.8657, -0.1572), 'div': (1.1631, -0.0625)}   # FITTED by fit15() on CAL15_RUNS (`mn_model.py --fit15`)
SEED15 = (7.589, 0.28189)               # FITTED by fit15(): seeds end = a + b x span digits / 1e9 (s after the start)
SLOW_K = 1 << 33                       # binsplit.c mul1_serial: the Barrett step only for m < 2^33
SLOW_F = 1.29                          # MEASURED (archive:results_merged_2026-10-24/SINGLE_NODE.md §V314 1: the u128 path's cost per limb-step against the fast one, Zen 4)
OVL1 = 0.577                           # FITTED by fit15(): size 1 -- the writer runs under this fraction of the division (it starts at the hook, before the low product)
DC_FIX = 1.6                           # MEASURED (the corrected 1e11 runs): dc's residues + fetch beside the write (0.7 + 0.9 s)
DC_FMT1 = 0.0545                       # MEASURED (archive:results_merged_2026-10-24/SINGLE_NODE.md §V314 e11_def / e11_def2 without a file: dc 5.55 / 5.43 s at 1e11, the digits redone after the corrections): s per 1e9 digits
DC_FMT_MN = 0.061                      # s per 1e9 digits: size > 1, mn_out_run's formatting + digit residues + fetch, pipelined with the write -- MEASURED at size 1,
                                       # 1e11 without a file (format 1.86 + digit residue 2.7 + fetch 0.86 + residues 0.7 = 6.1 s, archive:results_merged_2026-10-24/SINGLE_NODE.md §V314), ASSUMED to carry to the target's
                                       # 7.4e10 per node; 2 nodes at 5e9 per node measured 0.104 (V114 b2: 0.23 + 0.22 + 0.07 s, the small size's fixed costs)
# ---- Phase 15 (agent DOC): the user's decisions of 2026-09-27 (p15b designs), calibrated on RESULTS 86's paired 1e11 series (jobs 21550 with the digit
# file on s24-16, 21563 without it on s24-26; B0 = the Phase 14 defaults, "cand" = every candidate on = the defaults of 2026-09-27; logs ~/fin15/ on aac6).
# Means, without the file (four runs each, s24-26): B0 init 16.60, bs 93.40 (seed wait 20.10), recip 41.41, dm 99.17 (2 corrections), total 210.15;
# cand init 18.08, bs 72.50 (seed wait 16.11), recip 37.67, dm 100.35 (0 corrections), total 191.28.
FILL_BS = 0.7693                       # MEASURED ratio of bs without the seed wait, cand / B0 = 56.39 / 73.30 s (BS_SEED_FILL=128: one more level in the batch tier,
                                       # every batch length filled; T2 modelled -8.4 s of it, results/SINGLE_NODE.md §T215); ASSUMED to carry to the target's leaf (T2: the fill holds
                                       # 128 limbs on every node, S = 235 on node 0, ~184 on the top node)
SEED15B = (7.589, 0.2681)              # FITTED (ten cand runs, both series: the seeds end at init + the wait = 34.40 s at 1e11): BI_MUL1_FAST -- one form for every
                                       # multiplier, no u128 path above 2^33 (SLOW_F no longer applies); the intercept kept from SEED15.  At the target's top node the
                                       # seeds end 7.3 s earlier (modelled; S1 had -3...-7 s, archive:results_merged_2026-10-24/KERNELS.md §S115)
P15B_RECIP1 = 0.9097                   # MEASURED ratio at size 1, cand / B0: the reciprocal 37.67 / 41.41 s (NEWTON_RECIP_MID -3.4 s (R4), C2's grids, DIST_TWREC)
P15B_DIV1 = 1.0851                     # MEASURED ratio at size 1, cand / B0: the division (dm - recip) 62.68 / 57.76 s -- the fill's pool remaps (+6..+12 s, RL showed the
                                       # HIP runtime serializes them) against C2 (no corrections) and DIST_TWREC; size 1 only (at size > 1 the division is the fabric
                                       # model's pieces; the remap penalty there is ASSUMED 0 -- the arena is the dm need's, which the fill leaves unchanged)
# Phase 15 G5 (archive:results_merged_2026-10-24/KERNELS.md §G515): the general map (g not a power of two) packs with k_twpack_g / k_unpacktw_g, which DIST_TWREC never
# changed (TWREC_F was applied to every piece before G5: -1.4 s at the target that the code does not have).  DIST_TWREC_G=1: the recurrence
# forms, MEASURED (gen_pack_bench, s24-26) x1.75-2.03 at the 576 plane shape (2^20 x 2^20 over 2304 ranks: 750-900 -> 1460-1620 GB/s) and
# x1.5-1.7 at 192's (2^19 x 2^19 over 768); the saving per point is 5.7 % (576) / 3.8 % (192) of the piece's local time at T_PIECE_31_NP[4]
# per 2^29 points (12 twiddled passes per piece: 4 primes x 2 forward packs + 1 inverse unpack) -- MODELLED 0.95 here (the average over the
# target's general-map pieces), ASSUMED on the critical path (the model adds local time and exposed fabric; the chunk pipeline may hide
# the packs of chunks 1..K-1 under the wire: then about a quarter of it).  GEN_TWPACK_F[0] = 1: the plain kernels as the measured piece.
GEN_TWPACK_F = {0: 1.0, 1: 0.95}
TWREC_G = True                         # DIST_TWREC_G: default 1 in the code since 2026-09-28 (the user's decision 4; be2eec3; was 0); --no-twrec-g
P15C_DIV1 = 53.50 / 68.15              # MEASURED ratio at size 1, B2 / B1: the division (dm - recip) at 1e11, BS_ARENA_ROOM=0.16 (+ NP auto, TWREC_G: no size-1 effect)
                                       # against B1 -- the fill's remaps gone (the arena mapped whole at init: device 393.6 GB from init, as B1's dm peak); fin15e
                                       # (job 21670, s24-16, 2026-09-28 04:59-05:44 EDT), 6 + 6 runs: 53.12-54.12 against 67.14-69.19 s; the reciprocal unchanged
                                       # (37.80 vs 38.27 s: not applied).  Size 1 only (ASSUMED at other D); at size > 1 no remap term exists to remove
B3_DIV1_FIX = os.environ.get('MN_MODEL_B3_DIV1', '0') == '1'   # 06 EVALUATION item 13 / 04 D15: NEWTON_DKM=1 (the user's Batch 3 decision) has no size-1
                                       # calibration ratio of its own -- P15B_DIV1 and P15C_DIV1 both predate it, so the size-1 model (183.8 s at 1e11
                                       # nowrite today) overcounts the division by about the gap to V6's measured 166.7 s.  Off by default (CLI
                                       # --dkm1-fix / MN_MODEL_B3_DIV1=1) so the standing estimate does not move until this is measured properly
                                       # (the ratio below is fitted to close that one gap, not an independent measurement).
B3_DIV1 = 32.84420642877413 / 49.943741516194756   # = 0.6576: solved so nowrite at 1e11, g=1 (today 183.8 s, division 49.94 s of it) lands on 166.7 s
TWREC_F = 0.98                         # the pieces' local passes x this with DIST_TWREC=1: MODELLED from the measured -1.8 s of dm at 1e11 on ~90 s of four-step products
                                       # (archive:results_merged_2026-10-24/GRID_GROUPS.md §C215 2: -2.8 +- 0.8 s of total); X13b measured the pack x1.65, the unpack x1.43 (bench); ASSUMED to carry to the mn tier
PACKED_BPD = 7.5 / 18.0                # ECALC_OUT_PACKED=2 (default): 60-bit limbs, 7.5 bytes per 18 digits (0.417 B/digit; 1.0 for ASCII)
# O8 (2026-10-11): K2 + K3 (DBIG_ADDSUB3, DBIG_MODQ_MONT; default ON since T08) as a per-node saving on the wall.  MEASURED at 1 node, 6.441e10 digits (the 576-node
# per-node size at 3.71e13): -2.03 s (aac7, CI half 1.38) / -1.22 s (aac6 PPAC, 0.84), results/KERNELS.md §S51; kernel-metric sum at 4 nodes -8.4 s (run totals not significant).
# The base takes the aac7 1-node figure (the serial db_mod_qs / add-sub calls run once per node, so the same per-node figure is ASSUMED to carry to 576).  the estimate applies it
# AFTER mn_model.run(), so the calibrations (which fit pre-K2/K3 runs) are untouched; --k23-s 0 / MN_MODEL_K23_S=0 gives the previous estimate.
K23_S = float(os.environ.get('MN_MODEL_K23_S', '2.03'))
EXIT_S = 2.4                           # MEASURED: the process's elapsed wall less `total` less dc, every run of RESULTS 86's series (2.15-2.64 s: the pools' release, exit)


def cal15(D, key):
    """the Phase 15 ratio measured / (the model with CAL13) for group key (init | bs | dm) at D digits of the node"""
    if not CAL15: return 1.0
    a, b = CAL15_LAW[key]; Dc = min(max(D, CAL15_RANGE[0]), CAL15_RANGE[1])
    return a * (Dc / 4e10) ** b

def slow_frac(T, g):
    """the fraction of the slowest node's digits whose terms are above 2^33 (weighted by log10 k: the digits a term adds)"""
    N = _terms(T); a0 = N * (g - 1) // g if g > 1 else 1
    if N <= SLOW_K: return 0.0
    return (_lf(N) - _lf(max(a0, SLOW_K))) / max(1e-9, _lf(N) - _lf(a0))

def span_digits(Dt, g, T):
    return Dt * (1.0 + (SLOW_F - 1.0) * slow_frac(T, g))

def seed_wait15(Dt, g, T, init, fast=False):
    """bs's wait for the seeds after init (s): the seed thread's end (SEED15, on the node's span digits) less init; fast = BI_MUL1_FAST (SEED15B,
    no slow path above 2^33)"""
    if not CAL15: return 0.0
    if fast: return max(0.0, SEED15B[0] + SEED15B[1] * Dt / 1e9 - init)
    return max(0.0, SEED15[0] + SEED15[1] * span_digits(Dt, g, T) / 1e9 - init)





def memory(D, g, form="grid", groups=None, transport="shmem", pool_log=31, staging="code", design=None):
    """the per-node memory model (mem_model.mem_per_node): GB of device at the dm peak, host (with the SHMEM pool), the node peak"""
    if design is not None: design = design.at_g(g)                   # Phase 15 (2026-09-27): ECALC_NP=4 at size > 1
    o = dict(_OLD13); o.update(form=form, groups=groups, transport=transport, pool_log=pool_log, staging=staging)   # Phase 15: the old forms unless the design is p15
    if design is not None and not design.legacy: o.update(design.mem_opts(D * g)); o.update(p24=P24, dkm=DKM)   # MS: MN_P24's scratch (mem_model's P24 branch); DL: dm_layout follows NEWTON_DKM (DKM)
    elif design is None: o.update(np=4, host_fit=False)                                 # the legacy path: four primes (before step 0)
    r = mem_model.mem_per_node(int(D), g, o)
    gb = lambda k: r[k] / 1e9
    return dict(device=gb("dev_max"), host=gb("host_hwm"), node=gb("node_peak"), planes=gb("planes"), arena=gb("arena"),
                dm_need=gb("dm_need"), tree_need=gb("tree_need"), top_scratch=gb("top_scratch"), exchange=gb("exchange"), regions=gb("regions_bs"),
                shmem_pool=gb("shmem_pool"), shmem_staging=gb("shmem_staging"),
                cache_slot=gb("cache_slot"), cache_fit_slots=r["cache_fit_slots"], cache_fit_room=gb("cache_fit_room"), layout_node=gb("layout_node"))   # Phase 15 DOC2

# ------------------------------------------------------------------------------------------------------------
def run(fab, D, g, rule="model", verbose=True, leaf_scale=1.0, init_override=None, dc_exposed=None, groups=None, form="grid", transport="shmem", pool_log=31, staging="code", design=None, corrections=0):
    """one run of g nodes at D digits per node.  design None: the legacy constants (four primes, the Phase 10/11 phase table;
    --calib and the Phase 12 tables); a Design: the code after Phase 13b step 0 and the row's options (Phase 13b D)"""
    global DZ, CACHE_RUN_SLOTS
    if design is not None: design = design.at_g(g)                   # Phase 15 (2026-09-27): ECALC_NP=4 at size > 1 on the target's launch line
    saved = DZ; DZ = design; saved_c = CACHE_RUN_SLOTS
    if design is not None and getattr(design, 'cache_fit', False) and g > 1:   # Phase 15 DOC2: RNS_DIST_CACHE_FIT -- the slots the code's budget rule allows
        o = dict(_OLD13); o.update(form=form, groups=groups, transport=transport, pool_log=pool_log, staging=staging); o.update(design.mem_opts(D * g)); o.update(p24=P24, dkm=DKM)
        CACHE_RUN_SLOTS = min(CACHE_MN_SLOTS, mem_model.mem_per_node(int(D), g, o)['cache_fit_slots'])
    else: CACHE_RUN_SLOTS = None
    try:
        r = _run(fab, D, g, rule, verbose, leaf_scale, init_override, dc_exposed, groups, form, transport, pool_log, staging, design, corrections)
        r['cache_slots'] = cache_slots_now() if g > 1 else 0
        m = r['mem']; m['cache'] = r['cache_slots'] * m['cache_slot'] if g > 1 else 0.0   # Phase 15 DOC2: the slots' bytes (GB per node; 0 under FIT at the target)
        m['node_cache'] = m['node'] + m['cache']; m['cache_fits'] = m['node_cache'] <= NODE_GB_MARGIN
        return r
    finally:
        DZ = saved; CACHE_RUN_SLOTS = saved_c

def _run(fab, D, g, rule, verbose, leaf_scale, init_override, dc_exposed, groups, form, transport, pool_log, staging, design, corrections=0):
    nq = int(D / LIMB_DIGITS)                          # limbs of Q per node (the leaf's share)
    ftop = 1.0
    dl = nq
    nq_tot, dl_tot, np_tot = nq * g, dl * g, nq * g
    if design is None or design.legacy:
        ph = dict(init=init_override if init_override is not None else phase("init", D),
                  batch=phase("batch", D) * leaf_scale, top=phase("top", D) * leaf_scale, other=0.0)
    else:
        ftop = top_factor(D * g, g)                    # Phase 13d D2: the slowest leaf is the top node's (SHARES = 'terms')
        npf = node_phases(D * ftop, design, g, pipe=CAL13)
        fi = cal13(D * ftop, 'init')                   # Phase 13d D2: the 13c recalibration (1 without CAL13): the law on bs's rest
        bs0 = npf["batch"] + npf["top"]; bs1 = cal13_apply(D * ftop, design, g, bs0, 0.0)[0] if CAL13 else bs0; fb = bs1 / bs0 if bs0 > 0 else 1.0
        ph = dict(init=init_override if init_override is not None else npf["init"] * fi, batch=npf["batch"] * leaf_scale * fb, top=npf["top"] * leaf_scale * fb,
                  other=npf["other"])
    levels = tree_cost(fab, nq, g, groups, form, T=None if design is None or design.legacy else D * g) if g > 1 else []
    if g > 1:
        if design is None or design.legacy: rc, dc, grp = division_cost(fab, nq_tot, dl_tot, np_tot, g, rule, form)
        else:                                          # Phase 13d D2: the code's exact lengths (the cuts' skips are decided at a few limbs)
            ex = exact_sizes(D * g); rc, dc, grp = division_cost(fab, ex['nq'], ex['dl'], ex['pn'], g, rule, form, sn=ex['sn'])
    elif design is None or design.legacy:
        rc = Cost(); rc.t = phase("recip", D); dc = Cost(); dc.t = phase("div", D); grp = []
    else:
        dm0 = npf["recip"] + npf["div"]; fd = (cal13_apply(D, design, 1, 0.0, dm0)[1] / dm0 if dm0 > 0 else 1.0) if CAL13 else 1.0   # Phase 13d D2: the law on dm's rest
        rc = Cost(); rc.t = npf["recip"] * fd; dc = Cost(); dc.t = npf["div"] * fd; grp = []
    t_levels = sum(c.t for _, _, c in levels)
    p15 = design is not None and not design.legacy and design.p15
    p15b = p15 and design.p15b
    seed_wait = 0.0
    if p15 and CAL15 and init_override is None:          # Phase 15: the one-node compute refitted on the Phase 14 defaults (CAL15), the seed wait its own term
        Dt = D * ftop
        ph["init"] = ph["init"] * cal15(Dt, 'init')
        f_bs = cal15(Dt, 'bs') * (FILL_BS if p15b else 1.0); ph["batch"] *= f_bs; ph["top"] *= f_bs
        seed_wait = seed_wait15(Dt, g, D * g, ph["init"], fast=p15b) * (leaf_scale if leaf_scale > 0 else 0.0)
        if g == 1:
            rc.t *= cal15(D, 'recip') * (P15B_RECIP1 if p15b else 1.0); dc.t *= cal15(D, 'div') * (P15B_DIV1 if p15b else 1.0)   # (the reciprocal and the division apart: the size-1 writer overlaps the division)
            if p15b and design.p15c and design.arena_room > 0: dc.t *= P15C_DIV1   # Phase 15 DOC2: BS_ARENA_ROOM -- no remaps in the division (measured at 1e11)
            if B3_DIV1_FIX and DKM: dc.t *= B3_DIV1   # 06 EVALUATION item 13 / 04 D15: the size-1 DKM term, off by default (--dkm1-fix)
    if NODE_SCALE != 1.0 or INIT_SCALE != 1.0:             # Phase 16 C: another machine's node (apply_profile / MN_MODEL_NODE_SCALE, MN_MODEL_INIT_SCALE); the pieces carry it in t31
        ph["init"] *= INIT_SCALE; ph["batch"] *= NODE_SCALE; ph["top"] *= NODE_SCALE; seed_wait *= NODE_SCALE
        if g == 1: rc.t *= NODE_SCALE; dc.t *= NODE_SCALE
    t_compute = ph["init"] + seed_wait + ph["batch"] + ph["top"] + t_levels + rc.t + dc.t + ph["other"]
    out_write = D * (PACKED_BPD if (p15 and design.packed) else 1.0) / 1e9 / fab.write_bw   # the node's part file at the write bandwidth (packed: 0.417 B/digit)
    t_lowprod = dc.t * 0.5
    t1_wait = 0.0
    if p15 and dc_exposed is None:                     # Phase 15 (D3): the two walls, the part file as the code writes it
        if g == 1:                                     # size 1: the writer starts at the hook (before the low product) and overlaps OVL1 x the division; a
            O = ovl_div(dc, 1); fmt = DC_FMT1 * D / 1e9   # correction after the hook makes T1 wait for it (inside `total`) and then redoes the digits and rewrites
            if corrections and not p15b:               # the file after `total` (V2's finding, archive:results_merged_2026-10-24/SINGLE_NODE.md §V214); ECALC_CORR_PATCH (p15b): no wait, no rewrite
                t1_wait = max(0.0, out_write - O); dc_file = out_write + DC_FIX; dc_nofile = fmt
            else:
                dc_file = early_exposed(out_write, dc, 1); dc_nofile = 0.0   # (Phase 15 EW: MN_OUT_DKM_HI's pipeline; else max(0, W - O))
            out_exposed = t1_wait + dc_file
            wall_nowrite = t_compute + dc_nofile
        else:                                          # size > 1: mn_out_run after T1 -- formatting (DC_FMT_MN) and the write in a pipeline, nothing under the division
            fmt = DC_FMT_MN * D / 1e9
            if design.early:                           # MN_OUT_EARLY=1 (2026-09-27): the part file starts at the hook (X formed, before the low product) and
                out_exposed = early_exposed(max(out_write, fmt), dc, g)   # overlaps OVL1 x the division, as the size-1 writer does (IO15: dc -> 0 on 2 nodes); Phase 15 EW: MN_OUT_DKM_HI's pipeline
            else: out_exposed = max(out_write, fmt) if design.out_overlap == 'none' else max(fmt, out_write - t_lowprod)
            wall_nowrite = t_compute + fmt                 # (without a part file the hook is not set: the digits' residues after T1)
        if p15b: out_exposed += EXIT_S; wall_nowrite += EXIT_S   # the process's exit (measured), on both walls
    else:
        out_exposed = (max(0.0, out_write - t_lowprod) if g > 1 else 0.0) if dc_exposed is None else dc_exposed   # at size 1 the file is hidden under the division (measured: the wall = init + phases)
        wall_nowrite = t_compute
    wall = t_compute + out_exposed
    total_line = t_compute + t1_wait if g == 1 else wall   # what ecalc's `total` line shows (size 1: before the writer's join)
    tot = Cost()
    for _, _, c in levels: tot.add(c)
    tot.add(rc); tot.add(dc)
    m = memory(D, g, form, groups, transport, pool_log, staging, design)
    res = dict(D=D, g=g, wall=wall, wall_write=wall, wall_nowrite=wall_nowrite, total_line=total_line, t1_wait=t1_wait, seed_wait=seed_wait, compute=t_compute,
               corrections=corrections, write_bw=fab.write_bw, other=ph["other"], init=ph["init"], batch=ph["batch"], top=ph["top"], levels=t_levels, recip=rc.t, div=dc.t,
               out=out_exposed, out_write=out_write, exposed=tot.t_exposed, nic=tot.nic, glob=tot.glob, msgs=tot.msgs,
               pieces=tot.pieces, digits=D * g, levels_rows=levels, groups=grp, rc=rc, dc=dc, mem=m, form=form, staging=staging, schedule=mem_model.mn_groups(g, groups))
    if verbose: print_run(fab, res, rule)
    return res

def fmt_b(b):
    return "%.1f TB" % (b / 1e12) if b >= 1e12 else "%.1f GB" % (b / 1e9)

def print_run(fab, r, rule):
    D, g = r["D"], r["g"]
    print("=" * 112)
    print("D = %.2e digits per node, g = %d nodes (%s; dragonfly group %d nodes, %d-layer all-to-all, X1 groups: %s; tree form %s; MN_GROUPS %s)"
          % (D, g, fab.name, fab.group, fab.layers, rule, r["form"], ",".join(str(s) for s in r["schedule"]) or "-"))
    print("  total digits %.3e; per-node wall %.1f s = %.1f min with the part file, %.1f s = %.1f min without it (D3): init %.1f, seed wait %.1f, batch %.1f, top levels %.1f, distributed levels %.1f, reciprocal %.1f, division %.1f, output exposed %.1f (the part file %.0f s at %.2f GB/s%s)"
          % (r["digits"], r["wall"], r["wall"] / 60, r["wall_nowrite"], r["wall_nowrite"] / 60, r["init"], r["seed_wait"], r["batch"], r["top"], r["levels"], r["recip"], r["div"], r["out"], r["out_write"], fab.write_bw,
             "; T1 waits %.1f s for it" % r["t1_wait"] if r["t1_wait"] else ""))
    for k, size, c in r["levels_rows"]:
        print("    level: group %4d (%d-way)  %6.2f s  exposed %5.2f  pieces %2d  NIC %s  global %s  msgs/APU %d" % (size, k, c.t, c.t_exposed, c.pieces, fmt_b(c.nic), fmt_b(c.glob), c.msgs))
    rc, dc = r["rc"], r["dc"]
    print("    reciprocal %6.2f s (exposed %.2f, %d pieces, NIC %s, global %s, msgs/APU %d); groups by step: %s" % (rc.t, rc.t_exposed, rc.pieces, fmt_b(rc.nic), fmt_b(rc.glob), rc.msgs,
          " ".join("%s:%d" % ("%.1e" % j, gp) for j, gp in r["groups"][::max(1, len(r["groups"]) // 8)])))
    print("    division   %6.2f s (exposed %.2f, %d pieces, NIC %s, global %s, msgs/APU %d)" % (dc.t, dc.t_exposed, dc.pieces, fmt_b(dc.nic), fmt_b(dc.glob), dc.msgs))
    print("  exposed communication %.1f s of the wall (%.0f %%); fabric bytes per node %s (per NIC %s over 8), on global links %s; %d messages per APU"
          % (r["exposed"], 100 * r["exposed"] / r["wall"], fmt_b(r["nic"]), fmt_b(r["nic"] / 8), fmt_b(r["glob"]), r["msgs"]))
    m = r["mem"]
    print("  memory per node (mem_model, tree form %s, SHMEM staging %s): device %.0f GB at the dm peak (planes %.0f, arena %.0f = max(bs regions %.0f, dm need %.0f, tree need %.0f; top scratch %.0f), exchange %.0f), host %.0f GB (the SHMEM pool %.0f: staging %.0f); node peak %.0f of %.0f GB%s"
          % (r["form"], r["staging"], m["device"], m["planes"], m["arena"], m["regions"], m["dm_need"], m["tree_need"], m["top_scratch"], m["exchange"], m["host"], m["shmem_pool"], m["shmem_staging"], m["node"], NODE_GB,
             "" if m["node"] <= NODE_GB else "  ** DOES NOT FIT **"))

def max_digits(g, node_gb, form="grid", groups=None, transport="shmem", staging="code", design=None):
    """the largest D per node (to 1e8) whose modelled node peak fits node_gb (design None: the legacy four-prime memory)"""
    if design is not None: design = design.at_g(g)                   # Phase 15 (2026-09-27): ECALC_NP=4 at size > 1
    o = dict(_OLD13, form=form, groups=groups, transport=transport, staging=staging)   # Phase 15: a p15 design's mem_opts override the old forms
    if design is None: o.update(np=4, host_fit=False)
    elif not design.legacy:
        o.update(design.mem_opts(1e12 * g))                     # the cap rule at the run's digits: 2^31 above 5e10 (every 576-node size)
        if design.cap is None and g == 1: o.pop('cap')          # size 1: the code's rule follows D (3 2^30 below 5e10)
    return mem_model.max_digits_per_node(node_gb * 1e9, g, o)


# ------------------------------------------------------------------------------------------------------------
# calibration: every recorded aac6 point -- g node-processes sharing one node over a loopback transport
# (archive:results_merged_2026-10-24/TIME_MODEL.md §X job 20802/20815, L.md jobs 20808/20813, M11.md job 20814, S.md job 20817; the logs' "total" lines)
# ------------------------------------------------------------------------------------------------------------


def plan(g, T, design=None, groups=None, fab=None):
    """Phase 13d D2: the model's piece counts in agent L's categories (mn_plan.c's summary): tree (node 0's groups), tree_max
    (each level's top group), recip (the sharded steps), div (A_h mu, X Q); the top node's leaf pieces (its big products)"""
    design = (design or DEFAULT15B()).at_g(g); fab = fab or TARGET   # Phase 15: the code's plan has NEWTON_RECIP_CUT and (2026-09-27) NEWTON_RECIP_MID (DEFAULT15B)
    global DZ
    saved = DZ; DZ = design
    try:
        D = T / g; nq = int(D / LIMB_DIGITS)
        t0 = tree_cost(fab, nq, g, groups, "grid", T=T, which='bottom'); tm = tree_cost(fab, nq, g, groups, "grid", T=T, which='top')
        ex = exact_sizes(T); rc, dc, grp = division_cost(fab, ex['nq'], ex['dl'], ex['pn'], g, "model", "grid", sn=ex['sn'])
        return dict(tree=sum(c.pieces for _, _, c in t0), tree_max=sum(c.pieces for _, _, c in tm), recip=rc.pieces, div=dc.pieces,
                    levels0=[c.pieces for _, _, c in t0], levels_max=[c.pieces for _, _, c in tm], groups=sorted(set(gp for _, gp in grp)))
    finally:
        DZ = saved



# ============================================================================================================
# Phase 15 CX (results/CACHE.md §CX15): the mn transform cache at the target -- what the slots are worth (the code's hits x CACHE_HIT_F) and
# the ways to hold one inside 480 GB, each with its memory and its modelled gain.  The rooms per node (modelled, mem_model / the
# standing estimate): the tree levels 480 - the bs-phase node (device 411.0 + host 44.4) + the arena's slack at the tree (arena - tree
# need: a slot drawn from the block pool), the division 480 - (device 417.0 + host 28.8); a slot = k primes x 2^29 limbs x 8 B x 4 APUs.
# ============================================================================================================
def _run_cache(T, g, design, slots=None, phase=None, primes=None, loop='code', model='code', primes_phase=None, fab=None, groups=None):
    """one modelled run with the cache configured (restored after); returns the run's dict"""
    fab = fab if fab is not None else TARGET
    global CACHE_MN_SLOTS, CACHE_SLOTS_PHASE, CACHE_PHASE_NOW, CACHE_PRIMES, CACHE_LOOP, CACHE_MODEL, tree_cost, division_cost, CACHE_PRIMES_PHASE
    saved = (CACHE_MN_SLOTS, CACHE_SLOTS_PHASE, CACHE_PRIMES, CACHE_LOOP, CACHE_MODEL, tree_cost, division_cost, CACHE_PRIMES_PHASE)
    tc0, dc0 = tree_cost, division_cost
    def tc(*a, **k):
        global CACHE_PHASE_NOW
        CACHE_PHASE_NOW = 'tree'
        try: return tc0(*a, **k)
        finally: CACHE_PHASE_NOW = None
    def dc(*a, **k):
        global CACHE_PHASE_NOW
        CACHE_PHASE_NOW = 'dm'
        try: return dc0(*a, **k)
        finally: CACHE_PHASE_NOW = None
    try:
        CACHE_MN_SLOTS = slots if slots is not None else CACHE_MN_SLOTS; CACHE_SLOTS_PHASE = phase; CACHE_PRIMES = primes; CACHE_LOOP = loop; CACHE_MODEL = model
        CACHE_PRIMES_PHASE = primes_phase
        tree_cost, division_cost = tc, dc; _PC.clear()
        return run(fab, T / g, g, verbose=False, design=design, groups=groups)
    finally:
        (CACHE_MN_SLOTS, CACHE_SLOTS_PHASE, CACHE_PRIMES, CACHE_LOOP, CACHE_MODEL, tree_cost, division_cost, CACHE_PRIMES_PHASE) = saved; _PC.clear()



# ============================================================================================================
# Phase 15 Batch 3 PC (archive:results_merged_2026-10-24/CACHE.md §PC15): RNS_DIST_CACHE_PARTIAL=1 -- the slots' planes drawn per grid product from the block pool (the arena),
# as many primes as its free bytes allow after the product's own need and a margin (0.5 GB per APU), the grid's
# loop along its longer axis, P24 products cached too.  The pool's free bytes at a phase's peak product (modelled, mem_model): the tree
# arena - tree_need (the arena's slack); the division arena - dm_need, the arena laid for the default division (binsplit.c dm_layout does not
# follow NEWTON_DKM) and the need DKM's (mem_model.dm_layout(dkm=True)) when MN_MODEL_DKM=1 -- DKM's unused hole.  keep_room: whether the
# BS_ARENA_ROOM bytes inside the need are left alone (they are free at the products: the code's rule takes them; True = the conservative figure)
# ============================================================================================================
PARTIAL_MARGIN = 0.5e9                   # the code's margin per APU (rns_dist.c)
def cache_partial_rooms(T=None, g=None, design=None, keep_room=False):
    """(tree, dm) pool bytes per node free at the phase's peak product (modelled), and the details"""
    T = T or TARGET_DIGITS; g = g or TARGET_NODES; design = (design or DEFAULT15C(cache_fit=False)).at_g(g)
    o = design.mem_opts(T); r0 = mem_model.mem_per_node(T / g, g, o)
    o1 = dict(o); o1['dkm'] = bool(DKM); r1 = mem_model.mem_per_node(T / g, g, o1)
    L = mem_model.dm_layout(int(r0['N']), g, 31, True, True, room=o.get('arena_room', 0.0) or 0.0, dkm=bool(DKM))
    room_b = 4 * L['room'] if not keep_room else 0
    tree = r0['arena'] - r0['tree_need']; dm = r0['arena'] - (r1['dm_need'] - room_b)
    return tree, dm, dict(arena=r0['arena'], tree_need=r0['tree_need'], dm_need=r1['dm_need'], dm_need0=r0['dm_need'], room=4 * L['room'])
def cache_partial_primes(free_node, q=1 << 29, np_=4):
    """the planes of q limbs one APU's pool share holds after the margin (the code's rule, cache_pc_take), at most np_"""
    return max(0, min(np_, int((free_node / 4 - PARTIAL_MARGIN) // (q * 8))))
def cache_partial(T=None, g=None, keep_room=False, loops=('code', 'long'), arena_room=None, wbs=None, fab=None, design=None, groups=None):
    """the estimate at the target with RNS_DIST_CACHE_PARTIAL=1 (modelled): the primes per phase from the pool's rooms, 1 slot, the loop;
    arena_room: BS_ARENA_ROOM (None: the design's 0.16); wbs: the write rates priced (default TARGET_WRITE_BW, 0.6); fab: the fabric
    priced (default TARGET -- callers passing --bw/--lat/--group/--layers/--taper build their own Fabric and pass it here).
    design: a Design to price this row at (06 EVALUATION B10 / 04 D3: the estimate's launch-line row honouring --chunk-mb/--groups/
    --np-mn/--room/--strategy/--cap/--depth/--ascii) -- default None keeps the old DEFAULT15C(cache_fit=False) (+ arena_room); the
    caller is expected to have forced cache_fit=False on it (partial caching is priced separately from RNS_DIST_CACHE_FIT's slots).
    groups: MN_GROUPS for the priced runs (default None: the code's own schedule)"""
    global CACHE_P24
    T = T or TARGET_DIGITS; g = g or TARGET_NODES; GB = 1e9
    fab = fab if fab is not None else TARGET
    d = design if design is not None else (DEFAULT15C(cache_fit=False) if arena_room is None else DEFAULT15C(cache_fit=False, arena_room=arena_room))
    wbs = wbs or (TARGET_WRITE_BW, 0.6)
    def runw(**kw):                                                   # the run at each write rate: (wall_nowrite, [wall at wbs])
        out = []
        for w in wbs:
            w0 = fab.write_bw; fab.write_bw = w
            try: r = _run_cache(T, g, d, fab=fab, groups=groups, **kw)
            finally: fab.write_bw = w0
            out.append(r)
        return out[0]['wall_nowrite'], [r['wall'] for r in out]
    tree, dm, x = cache_partial_rooms(T, g, d, keep_room)
    kt, kd = cache_partial_primes(tree), cache_partial_primes(dm)
    print('== PC: %.3g digits on %d nodes, MN_P24=%d, MN_MODEL_DKM=%d, ECALC_NP=auto (modelled; the fabric assumed as in TARGET.md)' % (T, g, P24, int(DKM)))
    print('   the block pool per node: arena %.1f GB; tree need %.1f -> free %.1f GB; division need %.1f (%s; the default division\'s %.1f) %s -> free %.1f GB; margin %.1f GB per APU'
          % (x['arena'] / GB, x['tree_need'] / GB, tree / GB, x['dm_need'] / GB, 'DKM' if DKM else 'no DKM', x['dm_need0'] / GB,
             'with BS_ARENA_ROOM\'s %.1f GB kept' % (x['room'] / GB) if keep_room else 'less BS_ARENA_ROOM\'s %.1f GB (free at the products)' % (x['room'] / GB), dm / GB, PARTIAL_MARGIN / GB))
    print('   primes per slot (q = 2^29, %.2f GB per prime per APU): tree %d, division %d; BS_ARENA_ROOM %.2f; the write at %s GB/s'
          % ((1 << 29) * 8 / GB, kt, kd, d.arena_room, ' / '.join('%.1f' % w for w in wbs)))
    saved = CACHE_P24
    try:
        b0, bw = runw(slots=0)
        print('   0 slots: %.1f s without the write, %s with it' % (b0, ' / '.join('%.1f s' % x for x in bw)))
        rows = []
        for p24c in ((False, True) if P24 else (False,)):
            CACHE_P24 = p24c
            for lp in loops:
                done = set()
                for (a, b) in ((kt, kd), (1, 0), (1, 1), (2, 1), (2, 2)):
                    if (a, b) in done or (p24c is False and P24 >= 2 and (a, b) != (kt, kd)): continue
                    done.add((a, b))
                    ph = {'tree': 1 if a else 0, 'dm': 1 if b else 0}
                    r0, rw = runw(phase=ph, loop=lp, primes_phase={'tree': max(a, 1), 'dm': max(b, 1)})
                    tag = 'the pool rule' if (a, b) == (kt, kd) else 'forced'
                    rows.append((p24c, lp, a, b, tag, r0, rw))
                    print('   P24 cached %-3s loop %-4s tree %d / division %d primes (%-13s) | %6.1f s (%.2f min) | with the write %s | gain %+6.1f / %s s'
                          % ('yes' if p24c else 'no', lp, a, b, tag, r0, r0 / 60, ' / '.join('%6.1f s' % x for x in rw), r0 - b0, ' / '.join('%+6.1f' % (x - y) for x, y in zip(rw, bw))))
        return (b0, bw), rows
    finally: CACHE_P24 = saved

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "estimate": return estimate_main(sys.argv[2:])   # the standing estimate (was estimate.py)
    sys.exit("usage: mn_model.py estimate [options]   (estimate --help)")


# ============================================================================================================
# Phase 14 R1 (archive:PLAN §34 row R1, results/DIVISION.md §R114): E7 the reciprocal's products cut to the band read, E9 Karatsuba over
# the device grid, E10 the 576-node tree-level spill.  Model terms only: the calibrated path (run / node_phases) is
# untouched; every function here is evaluated on demand (--e7 / --e9 / --e10).  Labels: the pipeline law's constants
# are FITTED (D2), the Karatsuba add rate and the spill's hidden writes are ASSUMED (said where used).
# ============================================================================================================






# ---- E9: a 2 x 2 Karatsuba layer on top of mul_grid ---------------------------------------------------------------------------




# ---- E10: the 576-node tree-level spill (apumult M56) against MN_T_CHUNK_MB=1024 -------------------------------------------------




cal13_reset()                          # Phase 13d D2: PIECE13 from the recalibration


# ---- `mn_model.py estimate ...` (was ecalc/estimate.py, merged 2026-10-23, user decision Q89 a) ----
EST_DOC = """mn_model.py estimate - the standing estimate for a run of ecalc over g nodes at D digits per node (Phase 12 agent Q; archive:PLAN §26's
standing rule and 27 row Q).  Imports mn_model.py (the fabric and phase model, X2 + Q) and mem_model.py (the per-node
memory model, M + Q):

    estimate(g, D, tree='grid', groups=None, fabric=None) -> dict      the numbers
    mn_model.py estimate                                                      the table for g in {1, 4, 64, 576}, D in {4e10, 7.7e10, 1e11}
    mn_model.py estimate --g 576 --D 6e10 --tree grid --groups 2,4,8,16,32,64,192,576 --bw 100 --lat 2e-6 --write-bw 2 --group 64
    mn_model.py estimate --max                                                the largest D per node that fits 502 / 480 GB at each g

Every number is labelled: measured (a recorded aac6 run), modelled (this arithmetic on measured inputs), assumed (a
target parameter no aac6 measurement can give: the fabric's bandwidth and per-message cost, the part-file bandwidth,
the general map's exchange overlap).  internal target notes says what to measure first on the target and how to feed it in
(--bw, --lat, --write-bw; the constants at the top of mn_model.py).

Phase 13b (agent D): the estimate is of the code after step 0 (three primes, NTT_MODMUL=1) and of a design -- --np, --strategy
(C | B | B4 | auto), --cap (2^30 | 3*2^29 | 2^31 | 3*2^30; default the code's rule), --chunk (off | shift | both), --depth (1 | 2),
--modmul.  archive:scripts_2026-10-23/design_table.py prints every design.

Phase 15 (agent MD): the default is the code's defaults since Phase 14 on the target's launch line (mn_model.DEFAULT15: auto, 2^31, both chunks
at 1024 MB, depth 2, DM_TIGHT, MN_TREE_EARLY_FREE, NEWTON_RECIP_CUT, the SHMEM pool from the plan, COMM_SHMEM_ROUND_MB=1024 = the user's D2;
--round-mb 0 without it).  Every estimate prints two walls (the user's D3): without the disk write (the digits
computed and verified) and with it, the part file at --write-bw GB/s per node (default 0.6: the target's /ssd0 is Lustre, 0.58-0.64 GB/s
single-stream, measured there; --target prints 2.0 (the old assumption), 0.8 and 0.6).  mn_model.py estimate --target is the standing estimate.

Phase 15 (agent DOC, the user's decisions of 2026-09-27): the default is mn_model.DEFAULT15B -- the code's defaults of 2026-09-27 (BS_SEED_FILL=128,
BI_MUL1_FAST, NEWTON_RECIP_MID, DIST_TWREC, RNS_AUTO_PIECE_COST, ECALC_CORR_PATCH=2, ECALC_OUT_PACKED, MN_OUT_EARLY, ECALC_ODIRECT=auto) on the target's
launch line (ECALC_NP=4 at size > 1: --np-mn; COMM_SHMEM_ROUND_MB=1024): the part file packed (0.444 B/digit; --ascii for 1 B/digit) and, at size > 1,
started at the division's hook (MN_OUT_EARLY: it overlaps the low product as the size-1 writer does).

Phase 15 (agent DOC2, the user's decisions of 2026-09-28): the default is mn_model.DEFAULT15C -- main B2 (B1 + BS_ARENA_ROOM=0.16, DIST_TWREC_G=1,
RNS_POOL1_4Q=1) on the target's launch line with ECALC_NP=auto (--np-mn auto) and RNS_DIST_CACHE_FIT=1: the mn transform cache priced at the slots FIT
allows (0 at 5.1e13 and 4.74e13: the code's rule, mem_model.cache_fit_slots); --cache-slots n prices n slots whatever FIT allows (their bytes added to the
node: "does not fit" over 480 GB); --room F (BS_ARENA_ROOM), --no-twrec-g.  --target prints the standing figures, the cache at 1 / 2 slots and ECALC_NP=4 beside them.
"""

EST_LABELS = {
    "digits": "modelled (D x g; D is the request per node, the run computes to the next multiple of 18)",
    "walls": "modelled, two (D3): without the disk write (the digits computed and verified: + the formatting and the digit residues, 0.104 s per 1e9 digits, measured on 2 nodes) and with it (the part file after T1, as the code writes it at size > 1)",
    "p15": "modelled (Phase 15, mn_model.CAL15): the one-node compute refitted on the Phase 14 defaults (the five-run 1e11 series, V2's and V3's runs at 4e10-1.3e11: `total` within -1.3..+4.2 %, `mn_model.py --calib15`); the seed wait as its own term (the top node's terms all above 2^33: x 1.29 per limb-step, measured by V3); NEWTON_RECIP_CUT in the fabric's reciprocal (67 pieces = the C plan)",
    "wall": "modelled: the per-node compute recalibrated on the Phase 13c defaults (Phase 13d D2, mn_model.CAL13: G13d's ten one-node runs 6.6e10-1.16e11 + the 4e10 series + the 7.64e10 share run, `mn_model.py --calib13`; the pipeline's per-product law fitted on G's logs); the top node's leaf (the code's equal-term layout: 1.036 x the average digits at 576) and each tree level's largest group; the distributed levels, reciprocal and division built from S13's 2^31 piece x the pipeline factor (1.22, assumed to carry) and the fabric",
    "steps": "modelled = the C code's own plan (agent L's MN_PLAN_ONLY; mn_model.plan equals it at all 401 sizes 2.0-6.0e13): at the 2^31 cap the tree levels' largest groups step 88 -> 124 pieces over 4.30-4.39e13",
    "exposed": "modelled: the fabric time not hidden under the local passes -- assumes 100 GB/s per APU, 2 us per message (--bw, --lat), the general map's overlap (GEN_HIDE)",
    "fabric": "modelled: bytes through the node's eight NICs and over the dragonfly's global links (MN_TOPO_GROUP = 64, two-layer all-to-all)",
    "device": "modelled: mem_model (the code's own sizing formulas; measured to 0.05 % at size 1: 4e10 313.3 GB at four primes / 287.5 at three, 8e10 369.1, 1e11 431.2; at 10^10/4 19.9 GB arena)",
    "host": "modelled: 7 GB runtime + 4 GiB staging + the seed buffers at init + 6 GB per process of transport + the SHMEM pool ('pool': the larger of COMM_SHMEM_POOL_MB (8192) and the pool the run needs -- Phase 14 P2's law (--staging code, the default): 4 APU threads x the largest staged exchange's send + receive (the division's A_h mu result exchange: my rows of the piece + a quarter of my share of C inside it; MN_T_CHUNK_MB bounds it) + the control blocks; measured to 0.01 % at 1e8-1e10 on 2 nodes and 1e9-1e10 on 4 processes, results/SHMEM_ITEMS.md §P214)",
    "fits": "modelled against 502 GB per node (the MI300A's usable HBM; 480 GB = the safe budget with a 5 % margin)",
    "output": "the part file at --write-bw GB/s per node: 0.6 = the target's Lustre single-stream rate, MEASURED there (the apumult catalog: 0.58-0.64 write, 0.78-0.86 read), ASSUMED to hold with 576 nodes writing at once; 2.0 = the node-local NVMe assumed before.  Phase 15 (2026-09-27): packed, 8 bytes per 18 digits (ECALC_OUT_PACKED=1: 32.8 GB per node at the target, exact), started at the division's hook (MN_OUT_EARLY=1) and hidden under 0.577 of the division (OVL1, FITTED at size 1 and ASSUMED at size > 1); the process's exit 2.4 s (MEASURED) on both walls.  --b0: the ASCII file after T1, exposed whole",
    "p15b": "modelled (Phase 15, the defaults of 2026-09-27, mn_model DEFAULT15B, calibrated on RESULTS 86's paired 1e11 series within -3.1..+2.0 %: `mn_model.py --calib15b`): bs without the seed wait x 0.769 (BS_SEED_FILL, MEASURED at 1e11, ASSUMED at the target's leaf); the seeds' end on SEED15B (BI_MUL1_FAST, FITTED on ten runs; no slow path above 2^33); NEWTON_RECIP_MID in the fabric's reciprocal (66 pieces = the C plan); DIST_TWREC x 0.98 on the pieces' local passes (MODELLED from C2's -2.8 s at 1e11); ECALC_NP=4 at size > 1 (the plan check refuses three primes at 4.25e13 on 576: results/PRIMES_STRATEGY.md §P15)",
    "p15c": "modelled (Phase 15 DOC2, main B2 of 2026-09-28): BS_ARENA_ROOM=0.16 -- the node's arena in whole VMM chunks + 0.16 x the hole (mem_model, exact against BS_LAYOUT_ONLY: +16.5 GB of arena at 5.1e13), at size 1 the division x 0.785 (P15C_DIV1, MEASURED at 1e11: no remaps); DIST_TWREC_G=1 (the general map's packs x 0.95, G5, MODELLED); ECALC_NP=auto (four primes only for pieces over the three-prime bound; pool 0 at four planes at 576, pool 1 at three); RNS_DIST_CACHE_FIT=1: the mn cache at the slots the code's budget rule allows (0 at the target), each slot's hit x CACHE_HIT_F 0.96 (FITTED by CX on aac6); `mn_model.py --calib15c` against the fin15e 1e11 pairs (-2.1..-3.9 % on B2)",
    "host15": "modelled (Phase 15): + DB_POOL_VMM's two 8 GiB pinned seed buffers at init (measured 21.5 GB pinned on one node), the bs phase's +6 GB device (measured 2.1-5.8), the SHMEM pool = the plan's need (256 MiB steps) = the C plan exactly (43008 MiB; 9472 with COMM_SHMEM_ROUND_MB=1024)",
}

def estimate(g, D, tree="grid", groups=None, fabric=None, rule="model", transport="shmem", staging="code", verbose=False, design=DEFAULT, corrections=0):
    """the estimate for g nodes at D digits per node: a dict of the numbers (seconds, GB, bytes) and their labels.
    design: an mn_model.Design (default: DEFAULT15, the code's defaults + the target's launch line); None = the legacy (Phase 12) model.
    Phase 15 (D3): wall_s = the wall with the part file written (at the fabric's write_bw), nowrite_s without it."""
    fab = fabric or TARGET
    r = run(fab, D, g, rule, verbose=verbose, groups=groups, form=tree, transport=transport, staging=staging, design=design, corrections=corrections)
    m = r["mem"]
    k23 = K23_S                                                      # O8: K2 + K3 per-node saving (mn_model.K23_S, measured at 1 node, assumed to carry)
    r = dict(r, wall=r["wall"] - k23, wall_nowrite=r["wall_nowrite"] - k23)
    out = dict(g=g, D=D, digits=r["digits"], wall_s=r["wall"], minutes=r["wall"] / 60, nowrite_s=r["wall_nowrite"], nowrite_min=r["wall_nowrite"] / 60,
               t1_wait=r["t1_wait"], seed_wait=r["seed_wait"], total_line=r["total_line"], write_bw=fab.write_bw, out_write=r["out_write"],
               other=r["other"], init=r["init"], batch=r["batch"], top=r["top"], levels=r["levels"], recip=r["recip"], div=r["div"], out=r["out"],
               exposed_s=r["exposed"], exposed_pct=100 * r["exposed"] / r["wall"],
               nic_bytes=r["nic"], nic_per_nic=r["nic"] / 8, global_bytes=r["glob"], msgs_apu=r["msgs"], pieces=r["pieces"],
               device_gb=m["device"], host_gb=m["host"], node_gb=m["node"], shmem_pool_gb=m["shmem_pool"], fits=m["node"] <= NODE_GB, fits_margin=m["node"] <= NODE_GB_MARGIN,
               tree=tree, staging=staging, schedule=r["schedule"], mem=m, cache_slots=r.get("cache_slots", 0), cache_gb=m.get("cache", 0.0), node_cache_gb=m.get("node_cache", m["node"]))
    return out

def est_table(gs, Ds, tree, groups, fab, rule, staging="code", design=DEFAULT, corrections=0):
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

def estimate_main(argv=None):
    ap = argparse.ArgumentParser(prog="mn_model.py estimate", description=EST_DOC, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--k23-s", type=float, default=None, help="O8: the K2 + K3 (DBIG_ADDSUB3 / DBIG_MODQ_MONT, default ON) per-node saving in seconds (default mn_model.K23_S = 2.03, S51 aac7 1 node, measured; 0 = the estimate before K2/K3)")
    ap.add_argument("--g", type=int, nargs="*", default=[1, 4, 64, 576])
    ap.add_argument("--D", type=float, nargs="*", default=[4e10, 7.7e10, 1e11])
    ap.add_argument("--tree", default="grid", choices=("grid", "flat"), help="the top product's memory form: grid (Phase 12 G) or flat (the code at 7aded87)")
    ap.add_argument("--groups", default=None, help="MN_GROUPS (e.g. 2,4,8,16,32,64,192,576); default = the code's schedule")
    ap.add_argument("--staging", default="code", choices=("code", "sym", "resident", "per_exchange", "cached"), help="Phase 14 P2: code (the default: the measured law, mem_model.shmem_pool -- the staged exchanges' largest send + receive x 4 APU threads + the control blocks; results/SHMEM_ITEMS.md §P214), sym (DIST_MN_SYM_SLABS=1: + 3 q per APU of resident slabs); the hypotheses before: the SHMEM transport's staging in its pool: resident (Phase 12 S: the callers' slabs in the pool, no staging), per_exchange (the staging freed after each wait), cached (the code at 7aded87: kept per live communicator -- 345 GB per node at 576)")
    ap.add_argument("--as-is", action="store_true", help="the code at main 7aded87: --tree flat --staging cached")
    ap.add_argument("--rule", default="model", choices=("model", "full"))
    ap.add_argument("--fabric", default=os.environ.get("MN_MODEL_PROFILE", "target_l4"), choices=("target", "aac7", "aac7_c16", "target-m", "aac7_s48", "aac7_l4", "aac7_l4a", "target_l4"), help="Phase 16 C: the fabric profile -- target_l4 (the DEFAULT since 2026-10-11, user-adopted: the target fabric with the L4 refit layv factor, results/TIME_MODEL.md §L4); target (the previous default: the constants as before, layv 0.655) or aac7 (B01, 2026-10-11: the S48 refit, an alias of aac7_s48, aac7_fit.json (s48); the previous aac7, mn_model.AAC7, is aac7_c16: the measured Slingshot-11 / Cray OpenSHMEMX rates and AAC7_CONSTS; --bw / --lat / --write-bw given explicitly still win); TGTBENCH2: target-m = the target fabric (still ASSUMED) with the constants measured on the target (mn_model.TARGET_M_CONSTS: MAP_RATE 0.010) and a line comparing the modelled device with the measured 373.44 GB/node edge")
    ap.add_argument("--bw", type=float, default=None, help="GB/s per APU injection (default 100: assumed, PLAN 25's two 400 Gb/s NICs; the profile's value under --fabric aac7)")
    ap.add_argument("--lat", type=float, default=None, help="seconds per message (default 2e-6 assumed; the profile's under --fabric aac7)")
    ap.add_argument("--group", type=int, default=64, help="nodes per dragonfly group (MN_TOPO_GROUP)")
    ap.add_argument("--layers", type=int, default=2, choices=(2, 3))
    ap.add_argument("--taper", type=float, default=1.0)
    ap.add_argument("--write-bw", type=float, default=None, help="GB/s per node for the part file (Phase 15: 0.6 = the target's Lustre /ssd0 single-stream, measured there; 2.0 was assumed before)")
    ap.add_argument("--round-mb", type=float, default=1024, help="COMM_SHMEM_ROUND_MB on the launch line (D2: 1024; 0 = the code's default, off)")
    ap.add_argument("--out-overlap", default="none", choices=("none", "half"), help="size > 1: the part file after T1 (none: the code) or under half the division (half: the model before Phase 15)")
    ap.add_argument("--corrections", type=int, default=0, help="size 1: the division's corrections (data-dependent; 2 at 1e11 on the defaults)")
    ap.add_argument("--max", action="store_true", help="the largest D per node that fits 502 and 480 GB at each g, with its wall")
    ap.add_argument("--target", action="store_true", help="the standing estimate at 576 nodes -- the target (mn_model.TARGET_DIGITS: 3.82e13 since the user's decision of 2026-10-12, results/TARGET.md §TGT), its step, and history: 3.71e13 (2026-10-06 to 2026-10-12, TGTBENCH2/s18-target), 4.08e13 (TGT17, a few hours on 2026-10-06), 5.276e13 (B3 2026-09-29 to 2026-10-06) and its former second test size 5.167e13 (2026-10-03), 5.1e13 (2026-09-27 23:50 EDT), 4.25e13 (Phase 13d), 4.4e13 (Phase 13c)")
    ap.add_argument("--verbose", action="store_true", help="the per-phase, per-level breakdown of every run")
    ap.add_argument("--np", type=int, default=3, choices=(3, 4), help="ECALC_NP at size 1 (Phase 13b step 0: 3)")
    ap.add_argument("--np-mn", type=lambda v: v if v == 'auto' else int(v), default='auto', choices=(3, 4, 'auto'), help="ECALC_NP at size > 1 (Phase 15, the user's decision 1 of 2026-09-28: auto on the target's launch line -- Phase 15 NP's per-product count, four only over the three-prime bound; 4 was the launch line of 2026-09-27 (+17.2 GB per node: 489 GB at 5.1e13 with the room, over 480); 3 is refused by the plan check at 4.25e13 and 5.1e13 on 576)")
    ap.add_argument("--room", type=float, default=None, help="BS_ARENA_ROOM (default 0.16: the code's default since 2026-09-28; 0 = off)")
    ap.add_argument("--no-twrec-g", action="store_true", help="DIST_TWREC_G=0 (the default is 1 since 2026-09-28)")
    ap.add_argument("--cache-slots", type=int, default=None, choices=(0, 1, 2), help="price n mn transform-cache slots whatever RNS_DIST_CACHE_FIT allows (default: what FIT allows -- 0 at the target; the slots' bytes are added to the node)")
    ap.add_argument("--no-cache-fit", action="store_true", help="without RNS_DIST_CACHE_FIT on the launch line: the code's default 2 slots priced (137.4 GB per node at the target that no budget holds: TC15)")
    ap.add_argument("--ascii", action="store_true", help="ECALC_OUT_PACKED=0: the ASCII part file (1 B/digit) instead of the packed default (0.444 B/digit)")
    ap.add_argument("--strategy", default="auto", choices=STRATEGIES, help="RNS_STRATEGY (agent B, Phase 13b)")
    ap.add_argument("--cap", default="2^31", choices=list(mem_model.CAPS) + ["rule"], help="the plane cap (default 2^31 since Phase 13c; rule = the pre-13c size rule)")
    ap.add_argument("--chunk", default="both", choices=CHUNKS, help="off | shift (MDB_SHIFT_CHUNK_MB) | both (+ MN_T_CHUNK_MB, the default since Phase 14), at --chunk-mb")
    ap.add_argument("--chunk-mb", type=float, default=CHUNK_MB)
    ap.add_argument("--depth", type=int, default=2, choices=(1, 2), help="the uneven exchange's depth (agent X, Phase 13b)")
    ap.add_argument("--modmul", type=int, default=1, choices=(0, 1), help="NTT_MODMUL (step 0: 1)")
    ap.add_argument("--hide-pow2", type=float, default=None, help="Phase 16 F: HIDE_POW2, the equal-slab path's hidden xGMI fraction (default 0.75 measured on aac6; 0.72 measured on aac7 at depth 2, C16); = MN_MODEL_HIDE_POW2")
    ap.add_argument("--t-round", type=float, default=None, help="Phase 16 F: T_ROUND, the fixed cost of one chunk round in s (default 0.030 fitted on aac6 loopback; <= 0.015 measured on aac7, C16); = MN_MODEL_T_ROUND")
    ap.add_argument("--local-factor", type=float, default=None, help="Phase 16 F: one factor on the local (non-fabric) terms -- the node's compute and init (NODE_SCALE and INIT_SCALE together; default 1.0 = aac6's calibration at ROCm 7.2.4; the ROCm 7.0.3 toolchain measured +22 %% on bs / dm at 1e11 on aac7, C16 2.7; the profile's fit 1.12 / 1.20); = MN_MODEL_LOCAL_F; MN_MODEL_NODE_SCALE / MN_MODEL_INIT_SCALE set them apart")
    ap.add_argument("--staging-copy", type=float, default=None, help="06 EVALUATION item 13 / 04 open item (model terms, behind a CLI flag): a D2H + H2D copy of every inter-node byte, at this HBM copy rate in GB/s (mn_model.Fabric.staging_bw); None (default) = off -- no transport on the target stages exchanges through host memory today, so the standing estimate does not move; priced on every a2a/allgather exchange (2 x the node's NIC bytes / this rate)")
    ap.add_argument("--layv-factor", type=float, default=None, metavar="F", help="T06 (results/TIME_MODEL.md §T06): the layv (variable-layout, non-power-of-2 group) transform exchange's rate as a fraction of the equal-slab layeq rate (mn_model.Fabric.v_factor); default = the profile's factor: 0.655 for target / target-m (adopted 2026-10-11), the aac7_s48 fit for aac7_s48, 1.0 for aac7 and the others; 1.0 = off, raw xstats ratio 0.41")
    ap.add_argument("--fall-off", type=float, default=0.0, metavar="A", help="06 EVALUATION item 13 / 04 WISHLIST 1.4 / B2 open item 9 (model terms, behind a CLI flag): a per-group-size fabric fall-off -- the per-APU injection rate x (g/2)^(-A) (mn_model.Fabric.fall_off), fed by an aac7 t_comm all-to-all measurement at 2/4/8/12 nodes when one exists; 0 (default) = off, the flat rate assumed elsewhere in this file")
    ap.add_argument("--dkm1-fix", action="store_true", help="06 EVALUATION item 13 / 04 D15 (model terms, behind a CLI flag): a size-1 NEWTON_DKM calibration ratio (mn_model.B3_DIV1) so the one-node model matches V6's measured 166.7 s at 1e11 (183.8 s without it) -- P15B_DIV1 / P15C_DIV1 both predate DKM becoming the default (Batch 3) and carry no ratio of their own for it; off by default (MN_MODEL_B3_DIV1=1) since the ratio is fitted to close this one gap, not an independent measurement")
    a = ap.parse_args(argv)
    global K23_S, HIDE_POW2, T_ROUND, NODE_SCALE, INIT_SCALE, TWREC_G, CACHE_FORCE, B3_DIV1_FIX
    if a.k23_s is not None: K23_S = a.k23_s
    prof = apply_profile(a.fabric)                                      # Phase 16 C: the profile's fabric and constants (target = as before)
    a.k_other = prof.k_other                                              # O8: target() and the helpers below read it from a (prof is local to main)
    if a.layv_factor is None: a.layv_factor = prof.v_factor               # T06: the profile's layv factor unless --layv-factor is given
    if a.hide_pow2 is not None: HIDE_POW2 = a.hide_pow2                 # Phase 16 F: the CLI's constants win over the profile and the environment
    if a.t_round is not None: T_ROUND = a.t_round
    if a.local_factor is not None:
        NODE_SCALE = a.local_factor
        if 'MN_MODEL_INIT_SCALE' not in os.environ: INIT_SCALE = a.local_factor   # (the env splits init off: MN_MODEL_INIT_SCALE=1.20 --local-factor 1.12 = the AAC7 fit)
    if a.bw is None: a.bw = prof.bw
    if a.lat is None: a.lat = prof.lat
    if a.write_bw is None: a.write_bw = prof.write_bw
    if a.as_is: a.tree, a.staging = "flat", "cached"
    TWREC_G = not a.no_twrec_g                                          # DIST_TWREC_G (a global of the model)
    CACHE_FORCE = a.cache_slots
    if a.dkm1_fix: B3_DIV1_FIX = True                                   # 06 EVALUATION item 13 / 04 D15: off by default
    design = Design(np=a.np, strategy=a.strategy, cap=mem_model.CAPS[a.cap] if a.cap and a.cap != "rule" else None, chunk=a.chunk, depth=a.depth, modmul=a.modmul, chunk_mb=a.chunk_mb,
                    p15=True, round_mb=a.round_mb, out_overlap=a.out_overlap, p15b=True, np_mn=a.np_mn, packed=not a.ascii,
                    p15c=True, arena_room=a.room, cache_fit=not a.no_cache_fit)
    fab = Fabric(TARGET.name, a.bw, a.lat, group=a.group, layers=a.layers, taper=a.taper, write_bw=a.write_bw, staging_bw=a.staging_copy, fall_off=a.fall_off, v_factor=a.layv_factor, k_other=prof.k_other)
    print("ecalc estimate -- %s; tree form %s, SHMEM staging %s, MN_GROUPS %s, fabric %.0f GB/s per APU, %.1f us per message, dragonfly group %d, %d layers, taper %.2f, part files %.2f GB/s per node%s"
          % ("legacy (Phase 12: four primes)" if design is None else "design %s, ECALC_NP=%d%s, NTT_MODMUL=%d%s" % (design.name(), design.np,
             " (%s at size > 1)" % design.np_mn if design.np_mn and design.np_mn != design.np else "", design.modmul,
             (", COMM_SHMEM_ROUND_MB=%g, the part file %s" % (design.round_mb, "%s, from the division's hook (MN_OUT_EARLY)" % ("packed" if design.packed else "ASCII") if design.p15b else
              ("after T1" if design.out_overlap == 'none' else "under half the division"))) if design.p15 else " (the Phase 13/14 model)"),
             a.tree, a.staging, a.groups or "(default)", a.bw, a.lat * 1e6, a.group, a.layers, a.taper, a.write_bw,
             "" if design is None or not design.p15b else "; BS_ARENA_ROOM %.2f, DIST_TWREC_G=%d, the mn cache %s" % (design.arena_room, TWREC_G,
                 ("%d slot(s) forced (--cache-slots)" % a.cache_slots) if a.cache_slots is not None else ("as RNS_DIST_CACHE_FIT=1 allows" if design.cache_fit else "the code's default 2 slots (no FIT)"))))
    if a.target:
        est_target(a, design); return
    if a.max:
        print("%-4s | %14s %10s %8s %8s | %14s %10s %8s %8s   (min: without / with the part file at %.2f GB/s)" % ("g", "max D @502 GB", "digits", "no-write", "write", "max D @480 GB", "digits", "no-write", "write", a.write_bw))
        for g in a.g:
            cells = []
            for budget in (NODE_GB, NODE_GB_MARGIN):
                D = max_digits(g, budget, a.tree, a.groups, staging=a.staging, design=design)
                e = estimate(g, D, a.tree, a.groups, fab, a.rule, staging=a.staging, design=design, corrections=a.corrections) if D else None
                cells.append("%14.2e %10.3e %8.1f %8.1f" % (D, e["digits"], e["nowrite_min"], e["minutes"]) if e else "%14s %10s %8s %8s" % ("-", "-", "-", "-"))
            print("%-4d | %s | %s" % (g, cells[0], cells[1]))
        return
    if a.verbose:
        for g in a.g:
            for D in a.D: estimate(g, D, a.tree, a.groups, fab, a.rule, staging=a.staging, verbose=True, design=design, corrections=a.corrections)
    est_table(a.g, a.D, a.tree, a.groups, fab, a.rule, a.staging, design, a.corrections)
    if a.fabric == "target-m": est_edge_lines(a, fab, design)
    print()
    print("labels:")
    for k, v in EST_LABELS.items(): print("  %-8s %s" % (k, v))

def est_edge_lines(a, fab, design):
    """TGTBENCH2 (--fabric target-m): the modelled device peak against the device edge MEASURED on the target (373.44 GB/node, A6) and the init
    with the target's measured MAP_RATE.  The modelled device is mem_model's (v-slots included since B7ACCT); the C layout's own figure
    (BS_LAYOUT_ONLY's vslot: device_with) is the one to compare with a bar -- this line is a cross-check, not the sizing."""
    for g in a.g:
        for D in a.D:
            e = estimate(g, D, a.tree, a.groups, fab, a.rule, staging=a.staging, design=design, corrections=a.corrections)
            print("  target-m: g %d, %.3e digits: modelled device %.1f GB vs the measured edge %.2f GB/node (m, target): %+.1f GB; init %.1f s + seed wait %.1f s at MAP_RATE %.3f s/GB"
                  % (g, e["digits"], e["device_gb"], TARGET_DEVICE_EDGE_GB, TARGET_DEVICE_EDGE_GB - e["device_gb"], e["init"], e["seed_wait"], MAP_RATE))

def est_target(a, design):
    """Phase 15: the standing estimate at 576 nodes -- two walls (D3), the part file at 2.0 / 0.8 / 0.6 GB/s, the node memory, the grid step, the
    memory ceiling at 480 / 502 GB per node"""
    groups = a.groups or '2,4,8,16,32,64,192,576'
    fabs = [(bw, Fabric(TARGET.name, a.bw, a.lat, group=a.group, layers=a.layers, taper=a.taper, write_bw=bw, staging_bw=a.staging_copy, fall_off=a.fall_off, v_factor=a.layv_factor, k_other=a.k_other)) for bw in TARGET_WRITE_BWS]
    print("the standing estimate, 576 nodes, MN_GROUPS %s (modelled; the fabric assumed: %.0f GB/s per APU, %.1f us per message; the part file at %s GB/s per node --"
          " 2.0 the old assumption, 0.6 / 0.8 the target's Lustre prior: 0.58-0.64 GB/s single-stream write measured there, 0.78-0.86 read):" % (groups, a.bw, a.lat * 1e6, ' / '.join('%g' % b for b, f in fabs)))
    print("  %-10s %-44s | %9s | %s | %s | %s" % ("digits", "", "no write", " | ".join("write @%.1f" % b for b, f in fabs), "pieces tree_max + recip + div", "node GB (device + host; pool) [mn cache slots]"))
    for T, what in ((TARGET_DIGITS, "the target (user decision 2026-10-12, results/TARGET.md §TGT; device {device_gb} GB vs the 373.44 GB measured edge on the aac7-class layout; the device figure is computed here)"),
                    (3.71e13, "history: the target 2026-10-06 to 2026-10-12 (TGTBENCH2/s18-target, chosen with margin against B7ACCT's v-slots)"),
                    (4.0816e13, "history: the grid step just above TGT17's 4.08e13 target (arena 214.7 -> 223.3 GB)"),
                    (4.08e13, "history: the target for a few hours on 2026-10-06 (TGT17) -- superseded once B7ACCT's v-slots left only 0.88 GB of margin to the device edge"),
                    (5.276e13, "history: the target from 2026-09-29 (Batch 3) to 2026-10-06 (superseded, over the device edge, CAP17)"),
                    (5.167e13, "history: the second test size (int15k, 2026-10-03)"),
                    (5.28e13, "history: the step above the 2026-09-29 target (recip 51 -> 53, div 20 -> 28)"), (5.39e13, "history: the next step (tree 86 -> 88)"),
                    (5.74e13, "history: the next (tree 88 -> 108)"),
                    (TARGET_BELOW, "a smaller reference below the target (3.6e13, one step below the former 3.71e13; same 206.158 GB arena tier, pieces 101/107 vs 103/107; s18-target Part 1)"),
                    (4.74e13, "history: one step below the 2026-09-29 target (test after that headline)"), (4.75e13, "history: its step (4.74 -> 4.75e13)"),
                    (4.25e13, "history: the target until 2026-09-27 23:50 EDT")):
        es = [estimate(576, T / 576, a.tree, groups, f, a.rule, staging=a.staging, design=design) for b, f in fabs]
        p = plan(576, T, design); e = es[0]
        print("  %.3e %-44s | %5.1f s %s | %s | %3d + %2d + %2d = %3d | %5.1f (%5.1f + %4.1f; %.1f)%s" % (
            T, what.replace("{device_gb}", "%.1f" % e["device_gb"]), e["nowrite_s"], "(%.2f min)" % e["nowrite_min"], " | ".join("%5.1f s (%.2f min)" % (x["wall_s"], x["minutes"]) for x in es), p["tree_max"], p["recip"], p["div"],
            p["tree_max"] + p["recip"] + p["div"], e["node_gb"], e["device_gb"], e["host_gb"], e["shmem_pool_gb"], "" if e["fits_margin"] else "  (over 480 GB)")
            + (" [%d slot%s%s]" % (e["cache_slots"], "" if e["cache_slots"] == 1 else "s", ", + %.1f GB -> node %.1f GB%s" % (e["cache_gb"], e["node_cache_gb"], "" if e["node_cache_gb"] <= NODE_GB_MARGIN else ": does not fit 480") if e["cache_gb"] else "") if design is not None and design.p15b else ""))
    for TT in (TARGET_DIGITS, TARGET_BELOW):
        est_by_phase(a, design, groups, fabs, TT)
    if design is not None and design.p15c and a.cache_slots is None:
        est_cache_rows(a, design, groups, fabs)
        est_partial_row(a, design, groups)
    print("  the ceiling by memory (the largest D per node whose node peak fits; the walls at that size, which is past the grid steps):")
    for budget in (NODE_GB_MARGIN, NODE_GB):
        D = max_digits(576, budget, a.tree, groups, staging=a.staging, design=design)
        es = [estimate(576, D, a.tree, groups, f, a.rule, staging=a.staging, design=design) for b, f in fabs]
        p = plan(576, D * 576, design)
        print("    %.0f GB: D %.2e per node -> %.3e digits: %.1f s without the write; %s with it; pieces %d + %d + %d; node %.1f GB" % (
            budget, D, D * 576, es[0]["nowrite_s"], " / ".join("%.1f s @%.1f" % (x["wall_s"], b) for x, (b, f) in zip(es, fabs)), p["tree_max"], p["recip"], p["div"], es[0]["node_gb"]))

def est_partial_row(a, design=None, groups=None):
    """Phase 15 int15j (2026-09-29, the user's decisions 2 and 3): the launch line carries RNS_DIST_CACHE_PARTIAL=1 (PC: the slots' primes per
    grid product from the block pool's free bytes, P24 cached, the loop along the longer axis) and MN_OUT_DKM_HI=1 (EW: the writer on
    X_hi after step 1; the model's DKM_HI term is on by default, as in the code; MN_MODEL_DKM_HI=0 switches it off).  The row is mn_model.cache_partial's 'P24 cached yes, loop long, the pool rule' line at the target: the standing estimate,
    priced at the CLI's fabric (--bw/--lat/--group/--layers/--taper), same as target()'s other rows.
    06 EVALUATION B10 / 04 D3: this row used to ignore the CLI design (--chunk-mb/--groups/--np-mn/--room/--strategy/--cap/--depth/
    --ascii) -- cache_partial() always priced DEFAULT15C(cache_fit=False) and the default schedule.  Now it is priced at main()'s own
    `design` (copied, with cache_fit forced False: partial caching is priced on its own terms, not RNS_DIST_CACHE_FIT's slot count)
    and `groups`, so the deltas the CLI asks for (e.g. --chunk-mb 2048) show up here too."""
    import io, contextlib, copy
    fab = Fabric(TARGET.name, a.bw, a.lat, group=a.group, layers=a.layers, taper=a.taper, write_bw=TARGET_WRITE_BW, staging_bw=a.staging_copy, fall_off=a.fall_off, v_factor=a.layv_factor, k_other=a.k_other)
    pd = None
    if design is not None:
        pd = copy.copy(design); pd.cache_fit = False
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf): (b0, bw), rows = cache_partial(wbs=(2.0, a.write_bw, 0.6), fab=fab, design=pd, groups=groups)
    r = [x for x in rows if x[0] and x[1] == 'long' and x[4] == 'the pool rule']
    if not r: return
    p24c, lp, kt, kd, tag, r0, rw = r[0]
    with contextlib.redirect_stdout(buf): (b02, bw2), rows2 = cache_partial(T=5.167e13, wbs=(2.0, a.write_bw, 0.6), fab=fab, design=pd, groups=groups)   # Phase 16 F: history -- the former second test size's row (RESULTS 94); superseded by TGT17, 2026-10-06
    r2 = [x for x in rows2 if x[0] and x[1] == 'long' and x[4] == 'the pool rule']
    print("  the launch line's RNS_DIST_CACHE_PARTIAL=1 (int15j; the pool rule: tree %d / division %d primes per slot, P24 cached, the loop along the longer axis)%s:"
          % (kt, kd, " with MN_OUT_DKM_HI=1" if DKM_HI else " (MN_MODEL_DKM_HI=0: the MN_OUT_DKM_HI pipeline is not in this row)"))
    print("    %.3e the target                               | %5.1f s (%.2f min) | %s | gain %+.1f s without the write, %s with it"
          % (TARGET_DIGITS, r0, r0 / 60, " | ".join("%5.1f s (%.2f min)" % (x, x / 60) for x in rw), r0 - b0, " / ".join("%+.1f" % (x - y) for x, y in zip(rw, bw))))
    if r2:
        p24c, lp, kt2, kd2, tag, r0, rw = r2[0]
        print("    %.3e history: the former second test size      | %5.1f s (%.2f min) | %s | gain %+.1f s without the write, %s with it"
              % (5.167e13, r0, r0 / 60, " | ".join("%5.1f s (%.2f min)" % (x, x / 60) for x in rw), r0 - b02, " / ".join("%+.1f" % (x - y) for x, y in zip(rw, bw2))))

def est_cache_rows(a, design, groups, fabs):
    """Phase 15 DOC2: at the target and one step below -- the mn transform cache at 0 (what RNS_DIST_CACHE_FIT allows), 1 and 2 slots (forced: their bytes on
    the node), the launch line without FIT (the code's default 2 slots, in no budget: TC15), and ECALC_NP=4 in place of auto"""
    print("  the mn transform cache and the prime count (modelled; a slot = 4 APUs x 4 primes x 2^29 limbs x 8 B = 68.7 GB per node; FIT's room = 480 - the layout node - 32 GB):")
    print("    %-10s %-58s | %9s | %s | %s" % ("digits", "", "no write", " | ".join("write @%.1f" % b for b, f in fabs), "node GB (with the slots)"))
    global CACHE_FORCE
    import copy
    np4 = copy.copy(design); np4.np_mn = 4
    for T in (TARGET_DIGITS, TARGET_BELOW):
        rows = []
        for name, force, dz in (("0 slots: what RNS_DIST_CACHE_FIT=1 allows (the launch line)", None, design),
                                ("1 slot (forced; does not fit)", 1, design), ("2 slots (forced; does not fit)", 2, design),
                                ("ECALC_NP=4 instead of auto (0 slots)", None, np4)):
            CACHE_FORCE = force; _PC.clear()
            try: es = [estimate(576, T / 576, a.tree, groups, f, a.rule, staging=a.staging, design=dz) for b, f in fabs]
            finally: CACHE_FORCE = None; _PC.clear()
            e = es[0]; nc = e["node_cache_gb"]
            if T == TARGET_DIGITS and force is None and dz is design:
                print("    (FIT at %.3ge13: room %.2f GB = 480 - the layout node %.2f - 32 -> %d slots)" % (T / 1e13, e["mem"]["cache_fit_room"], e["mem"]["layout_node"], e["mem"]["cache_fit_slots"]))
            print("    %.3e %-58s | %5.1f s (%.2f min) | %s | %5.1f%s" % (T, name, e["nowrite_s"], e["nowrite_min"], " | ".join("%5.1f s" % x["wall_s"] for x in es), nc,
                  "" if nc <= NODE_GB_MARGIN else "  (over 480 GB)"))

def est_by_phase(a, design, groups, fabs, T):
    """the by-phase line at T total digits on 576 nodes (Phase 15 TGT: the target and the size one step below)"""
    e = estimate(576, T / 576, a.tree, groups, fabs[-1][1], a.rule, staging=a.staging, design=design)
    fm = DC_FMT_MN * T / 576 / 1e9
    b15 = design is not None and design.p15b; bpd = PACKED_BPD if (b15 and design.packed) else 1.0; gbn = T / 576 * bpd / 1e9
    print("  %.3ge13 by phase (modelled): init %.1f + seed wait %.1f + batch %.1f + top %.1f + distributed levels %.1f + reciprocal %.1f + division %.1f + other %.1f + the digits' formatting"
          " and residues %.1f%s = %.1f s without the write; the part file %.1f GB per node (%s), %s: %s s of writing" % (T / 1e13, e["init"], e["seed_wait"], e["batch"],
          e["top"], e["levels"], e["recip"], e["div"], e["other"], fm, " + the exit %.1f" % EXIT_S if b15 else "", e["nowrite_s"], gbn, "packed, 0.444 B/digit" if bpd < 1 else "ASCII",
          "started at the division's hook, %.1f s of it hidden under the division (MN_OUT_EARLY)" % (OVL1 * e["div"]) if b15 and design.early else "written after T1 with the formatting in a pipeline",
          " / ".join("%.1f @%.1f" % (gbn / b, b) for b, f in fabs)))

if __name__ == "__main__":
    main()

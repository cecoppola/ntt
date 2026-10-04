#!/usr/bin/env python3
"""mn_model.py - the fabric model of the multi-node run (Phase 11 agent X, PLAN.md 25/26: X2; Phase 12 agent Q: the
real level schedule, both memory forms, the calibration on every aac6 point, estimate()).

Per node, for D digits per node over g nodes: the wall by phase, the bytes on the fabric (per NIC and on the
dragonfly's global links), the exposed (un-hidden) communication, the memory, and the total digits.  The
single-node phases come from the measured table below; the distributed levels, the sharded reciprocal and the
sharded division are built product by product (the same grids, chains and cuts the code forms) from the measured
2^31-point piece product and the target fabric (PLAN 25: 100 GB/s per APU, two-tier dragonfly, ~2 us per message).

    ./mn_model.py                  the tables: D in {4e10, 8e10, 1e11} x g in {4, 64, 576} (+ the headline)
    ./mn_model.py --calib          the aac6 calibration (sizes 2, 3, 4, 6, 9 at 10^8-10^10 on one node, TCP and OSHMEM loopback)
    ./mn_model.py --schedules      the 576-node level schedules compared (MN_GROUPS: binary-clipped, 9-way, 3.3)
    ./mn_model.py --tree grid --groups 2,4,8,16,32,64,192,576 --group 64 --layers 3 --taper 0.5 --write-bw 2   (see ARGS)
    ./mn_model.py --calib15        Phase 15: the model on the defaults against the Phase 14 one-node runs (both walls)
    ./mn_model.py --fit15          Phase 15: refit CAL15 (init, bs, the seed wait, recip, division, the size-1 writer) and print it

Everything printed as "modelled" is this script's arithmetic; "measured" cites the RESULTS section.

Phase 15 (agent MD): the default design is DEFAULT15() -- the code's defaults since Phase 14 on the target's launch line (COMM_SHMEM_ROUND_MB=1024,
the user's D2); --model p13 gives the Phase 13/14 model, --model legacy the Phase 12 one.  Two walls everywhere (the user's D3): without the disk
write (the digits computed and verified) and with it; the part file at --write-bw GB/s per node (default 0.6: the target's /ssd0 is Lustre,
0.58-0.64 GB/s single-stream, MEASURED there -- apucode/apumult-ntt-reverse-port-catalog.md; 2.0 was the assumption before).

Phase 15 (agent DOC, the user's decisions of 2026-09-27): the default design is DEFAULT15B() -- the code's defaults of 2026-09-27 (BS_SEED_FILL=128,
BI_MUL1_FAST, NEWTON_RECIP_MID, DIST_TWREC, RNS_AUTO_PIECE_COST, ECALC_CORR_PATCH=2, ECALC_OUT_PACKED, MN_OUT_EARLY, ECALC_ODIRECT=auto) on the
target's launch line (ECALC_NP=4 at size > 1, COMM_SHMEM_ROUND_MB=1024); --model p15 is B0 (DEFAULT15, the Phase 14 defaults).  The terms and their
labels are in the block above CAL15_RUNS (FILL_BS, SEED15B, P15B_RECIP1, P15B_DIV1, TWREC_F, PACKED_BPD, EXIT_S); --calib15b compares the model with
RESULTS 86's paired 1e11 series.

Phase 15 (agent DOC2, the user's decisions of 2026-09-28): the default design is DEFAULT15C() -- main B2 = B1 + BS_ARENA_ROOM=0.16 (the arena room: the
node's arena in whole VMM chunks + 0.16 x the hole; at size 1 the division without remaps, P15C_DIV1) + DIST_TWREC_G=1 (TWREC_G True) + RNS_POOL1_4Q=1, on
the target's launch line with ECALC_NP=auto (np_mn 'auto') and RNS_DIST_CACHE_FIT=1 (cache_fit: the mn transform cache's slots are what FIT allows --
mem_model's cache_fit_slots, the code's rule: 0 at 5.1e13 and 4.74e13; CACHE_FORCE / --cache-slots n prices n slots whether or not they fit, with their
bytes added to the node).  --model p15b is B1 (DEFAULT15B: ECALC_NP=4, no room, the code's default 2 cache slots as the time assumed before TC / CX).
--calib15c compares the model with the paired 1e11 series B1 / B2 (fin15e).
"""
import argparse, math, sys, os, functools
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mem_model

# ------------------------------------------------------------------------------------------------------------
# MEASURED INPUTS (one MI300A node, four APUs; the code of main @ 7aded87 unless noted)
# ------------------------------------------------------------------------------------------------------------
# single-node phases by digits (seconds): init, batch tier, top levels (device tier), reciprocal, division, wall.
#   1e9, 1e10: X's calibration batch (job 20802, s24-26; results/X.md)
#   4e10: results/P.md gate (81.5 +- 1.4 s, phases 66.0);  7e10: results/P.md (153.5 s, ECALC_DM_POOL on)
#   8e10: results/M11.md v3/v4 (195.5 s, s24-16; zero hipMalloc)  1e11: results/M11.md v4 (262.9 s, s24-26)
PHASES = {          #  init   batch   top    recip   div    wall
    1e9:    dict(init=9.3,  batch=1.5,  top=0.1,  recip=2.76, div=0.38, wall=14.4),
    1e10:   dict(init=10.8, batch=7.1,  top=1.1,  recip=4.78, div=2.98, wall=29.0),
    4e10:   dict(init=15.5, batch=22.5, top=13.1, recip=15.2, div=15.1, wall=81.5),
    7e10:   dict(init=18.4, batch=37.7, top=24.9, recip=32.3, div=35.4, wall=153.5),
    8e10:   dict(init=20.7, batch=44.0, top=38.8, recip=37.9, div=45.2, wall=195.5),
    1e11:   dict(init=25.9, batch=65.5, top=51.4, recip=52.9, div=66.9, wall=262.9),   # M11 v4 log: bs 117.0 (seeds 10.9 + batch 54.6 + mdev 51.4), dm 119.8
}
# the distributed piece product on one node (results/G.md, RESULTS 75): a 2^31-point piece (4 primes, 2^29 points
# per APU): load 0.33 + ntt 0.67 + crt/out 0.11 = 1.11 s; with the B operand's transform cached 0.72 s.
T_PIECE_31 = 1.11
T_PIECE_31_BHIT = 0.72
# the four-step transform at 2^31 over the four APUs (results/C.md, DIST_STATS, per THREE transforms of one prime):
# rows 0.0624 cols 0.0429 packs 0.0385 exchange (xGMI push) 0.0571, exposed 0.0089 (84 % hidden).
T_XGMI_31 = 0.0571 / 3          # the xGMI stage per transform per prime, 2^29 points per APU
HIDDEN_XGMI = 0.84
XGMI_LINK_GBS = 91.0            # RESULTS 11: one xGMI link; three per APU
NODE_GB = 502.0
NODE_GB_MARGIN = 480.0          # the safe budget (DECISIONS2 7: 480 GB)
LIMB_DIGITS = 18                # decimal limbs (RESULTS 67)
EC_NP = 4
K_CHUNKS = 4                    # the slab pipeline (M7; DIST_CHUNKS)
CACHE_MN_SLOTS = int(os.environ.get('MN_MODEL_CACHE_SLOTS', '2'))   # RNS_DIST_CACHE_MN default over shares (results/G.md); Phase 15 CX: MN_MODEL_CACHE_SLOTS=n prices n
                                # slots (0: the cache off -- what the 480 GB budget allows at the target, results/TC15.md, CX15.md)
CACHE_MODEL = os.environ.get('MN_MODEL_CACHE', 'code')   # Phase 15 CX (results/CX15.md): 'code' = the hits as the code takes them (cache_pieces: the slots'
                                # designation, the cuts' skips, the planes' q) x CACHE_HIT_F; 'old' = the term before CX (at >= 2 slots every piece after
                                # the first with one operand cached; nothing at 1 slot)
CACHE_HIT_F = 0.96              # Phase 15 CX: a hit's saving as a fraction of its piece, measured over modelled (piece_cost fwd=2 - fwd=1 over fwd=2 on the aac6
                                # fabric, the same pieces) -- FITTED on aac6, results/CX15.md: tests/cx_grid (the target's 9 grid shapes at cap 2^27, cache
                                # 0/1/2 interleaved; 489 hit pieces) 1.043 at 2 node-processes, 0.976 at 4; ecalc 1e10 end to end (the hit pieces against
                                # themselves in the cache-off run) 0.965 at 2 (4 runs), 0.840 at 4 (4 runs); mean 0.956.  The model's structure (1 of the 3
                                # transforms per prime, one redistribution) holds within -16..+4 %; ASSUMED to carry to the target's fabric
# Phase 15 CX proposals (results/CX15.md section 3; off unless set): CACHE_PRIMES = k -> a slot holds k of the piece's primes (k / np of a hit's transform
# saving, no redistribution saved); CACHE_LOOP = 'long' -> mn_grid's loop along the grid's longer axis (the slots follow); CACHE_SLOTS_PHASE =
# {'tree': n, 'dm': n} -> the slots per phase (the tree's arena slack, the division's room); cache_slots_now() is what _product_cost prices
CACHE_PRIMES = None
CACHE_LOOP = 'code'
CACHE_SLOTS_PHASE = None
CACHE_PHASE_NOW = None
# Phase 15 DOC2 (the user's decision 2 of 2026-09-28: RNS_DIST_CACHE_FIT=1 on the launch line): a design with cache_fit prices the slots FIT allows
# (run() sets CACHE_RUN_SLOTS = min(CACHE_MN_SLOTS, mem_model's cache_fit_slots) for the run: 0 at the target); CACHE_FORCE = n prices n slots
# whatever FIT allows (estimate.py --cache-slots n: what a slot would be worth; its bytes are added to the node and it is labelled "does not fit")
CACHE_RUN_SLOTS = None
CACHE_FORCE = None
# Phase 15 Batch 3 PC (results/PC15.md; RNS_DIST_CACHE_PARTIAL=1): CACHE_P24 -> the P24 products (MN_P24) use the cache too (PC builds it: the slot's
# key carries the 24-digit form); CACHE_PRIMES_PHASE = {'tree': k, 'dm': k} -> the primes a slot holds per phase (PC: the slot's planes drawn per grid
# product from the block pool's free bytes -- the arena's slack at the tree, DKM's unused hole in the division; cache_partial() derives k per phase)
CACHE_P24 = os.environ.get('MN_MODEL_CACHE_P24', '0') == '1'
CACHE_PART_F = float(os.environ.get('MN_MODEL_CACHE_PART_F', '1.08'))   # PC: a partial hit's measured saving over hit_cost's k / np term -- FITTED on aac6
                                # (results/PC15.md 2.1: cx_grid at 2 procs, k = 1 / 2, 18- and 24-digit: 1.069, 1.093, 1.075, 1.087; the full hits 1.017 / 1.039)
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
    coll_fixed: a fixed cost per small collective (s); write_bw: the part file (GB/s per node)."""
    def __init__(self, name, bw_apu, lat, group=64, layers=2, taper=1.0, gpu_share=1, fixed=0.0, write_bw=2.0, tcp_exp=0.0, coll_fixed=None, target=True):
        self.name, self.bw, self.lat, self.group, self.layers, self.taper = name, bw_apu, lat, group, layers, taper
        self.gpu_share, self.fixed, self.write_bw, self.tcp_exp, self.target = gpu_share, fixed, write_bw, tcp_exp, target
        self.coll_fixed = fixed if coll_fixed is None else coll_fixed

    def rate(self, bytes_apu):
        """GB/s per APU: constant on the target; the loopback transports' single stream per mesh is slower on small
        exchanges (fitted: rate = bw x (bytes / 1 GiB per APU)^tcp_exp, capped at bw)"""
        if not self.tcp_exp: return self.bw
        return self.bw * min(1.0, (max(bytes_apu, 1.0) / (1 << 30)) ** self.tcp_exp)

    def a2a(self, bytes_apu, g, chunks=1):
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
        t_nic = nic / (self.rate(bytes_apu) * 1e9)
        t_glob = cross / (self.rate(bytes_apu) * 1e9 * self.taper) if cross else 0.0   # the group's aggregate to each peer group = G x 4 x bw x taper / (ng - 1), shared by its G nodes evenly
        t = max(t_nic, t_glob) + msgs * self.lat + self.fixed * chunks
        return t, nic * 4, cross * 4, msgs

    def allgather(self, bytes_apu, g):
        """an all-gather over g nodes: bytes_apu received from every peer (the spills)"""
        if g <= 1: return 0.0, 0.0, 0.0, 0
        recv = bytes_apu * (g - 1)
        cross = bytes_apu * max(0, g - self.group) if g > self.group else 0.0
        t = recv / (self.rate(bytes_apu) * 1e9) + (g - 1) * self.lat + self.fixed
        return t, recv * 4, cross * 4, g - 1

    def coll(self, g):
        """a small collective (all-gather of a few words, a max-reduction) over g nodes"""
        return 0.0 if g <= 1 else (g - 1) * self.lat + self.coll_fixed + 20e-6

TARGET_WRITE_BW = 1.0                  # 2026-09-29: the user's Lustre test on the target: ~1 GB/s write per node (ASSUMED to hold with 576 nodes writing at once); before: 0.6 (the apumult catalog's 0.58-0.64 single-stream)
                                       # catalog); ASSUMED to hold with 576 writers at once (the aggregate, ~350 GB/s, is not measured); was 2.0 (node-local NVMe, assumed)
TARGET_WRITE_BWS = (2.0, 1.0, 0.6)     # the rates every target estimate prints: the old assumption, the catalog's read rate as an upper write prior, the write prior
TARGET = Fabric("Slingshot-2 dragonfly (PLAN 25)", bw_apu=100.0, lat=2e-6, group=64, layers=2, taper=1.0, write_bw=TARGET_WRITE_BW)
TARGET_DIGITS = mem_model.TARGET_DIGITS   # Phase 15 TGT (the user's decision of 2026-09-27 23:50 EDT): 5.1e13 on 576 nodes (was 4.25e13); mem_model.py holds it
TARGET_BELOW = mem_model.TARGET_BELOW     # the runtime one step below: 4.74e13
TARGET_NODES = mem_model.TARGET_NODES
TARGET_W2 = Fabric("Slingshot-2 dragonfly (PLAN 25)", bw_apu=100.0, lat=2e-6, group=64, layers=2, taper=1.0, write_bw=2.0)   # the historical reports (e10) at the old 2 GB/s
# Phase 16 C (results/C16.md): aac7 -- HPE Cray EX, 4 x MI300A, 4 x Slingshot-11 (one 200 Gb/s Cassini per socket), Cray OpenSHMEMX 11.8.0, ROCm
# 7.0.3, one process per node (one NIC per PE: C16 2.1).  MEASURED on 2 nodes: 3.6 GB/s per APU thread through the put path at the exchange
# sizes ecalc sends (13.5-14.8 GB/s per node aggregate), 8.5 us per put + signal + wait, the part file on NFS at 0.116 GB/s (A16 4).
# `estimate.py --fabric aac7` (or MN_MODEL_PROFILE=aac7) selects it and applies AAC7_CONSTS to the constants above; the TARGET profile and
# every default stay as they are.
AAC7 = Fabric("aac7 Slingshot-11 / Cray OpenSHMEMX (C16)", bw_apu=3.6, lat=8.5e-6, group=64, layers=2, taper=1.0, write_bw=0.116)
PROFILES = {'target': TARGET, 'aac7': AAC7}
def apply_profile(name):
    """Phase 16 C: select the fabric profile and set the measured constants of that machine (an env override, MN_MODEL_*, still wins).
    Returns the Fabric."""
    global HIDE_POW2, T_ROUND, MAP_RATE, NODE_SCALE, INIT_SCALE
    name = (name or 'target').lower()
    if name not in PROFILES: raise SystemExit('mn_model: unknown profile %s (target, aac7)' % name)
    if name == 'aac7':
        c = AAC7_CONSTS; e = os.environ
        if 'MN_MODEL_HIDE_POW2' not in e: HIDE_POW2 = c['HIDE_POW2']
        if 'MN_MODEL_GEN_HIDE1' not in e: GEN_HIDE_DEPTH[1] = c['GEN_HIDE1']
        if 'MN_MODEL_GEN_HIDE2' not in e: GEN_HIDE_DEPTH[2] = c['GEN_HIDE2']
        if 'MN_MODEL_T_ROUND' not in e: T_ROUND = c['T_ROUND']
        if 'MN_MODEL_MAP_RATE' not in e: MAP_RATE = c['MAP_RATE']
        if 'MN_MODEL_NODE_SCALE' not in e: NODE_SCALE = c['NODE_SCALE']
        if 'MN_MODEL_INIT_SCALE' not in e: INIT_SCALE = c['INIT_SCALE']
    return PROFILES[name]

# aac6: g node-processes on ONE node over a loopback transport (correctness transports; every product's local passes
# shared g-way over the four APUs).  Fitted on the recorded walls (results/X.md, L.md, M11.md, S.md; --calib):
#   TCP (comm_tcp.c): one stream per mesh pair; per process ~3.3-3.7 GB/s aggregate at 1 GiB exchanges, slower below
#   (exponent), plus a fixed cost per chunk exchange that grows with the process count (the threads of g x 4 meshes x
#   (g - 1) peers contend on one node: 1 ms at 2, 8 ms at 9 processes) and a per-collective cost of the same kind.
#   OSHMEM (comm_shmem.c on OpenMPI 4.1.6, the serial lock, staging through the host-registered pool): the puts are
#   memcpys through shared memory, slower per byte (the D2H/H2D staging on both sides), with a larger fixed cost.
def aac6_fabric(transport, g):
    """the (bw GB/s per process, fixed s per chunk exchange, exponent) per (transport, process count), fitted on the
    recorded walls at that count (10^8, 10^9 and, at 2 and 4, 10^10): a substitute for a fabric, not a prediction --
    every g has its own constants and the only genuine check is the consistency across D at one g (--calib)."""
    if transport == "shmem":
        bw, fx, ex = {2: (1.5, 0.5e-3, 0.2), 3: (1.5, 1.0e-3, 0.2), 4: (1.5, 0.25e-3, 0.2)}.get(g, (1.5, 1.0e-3, 0.2))
        return Fabric("aac6 loopback OSHMEM, %d node-processes" % g, bw_apu=bw / 4, lat=0.0, group=10**6, fixed=fx, write_bw=1.0, tcp_exp=ex, gpu_share=g, target=False)
    bw, fx, ex = {2: (3.0, 0.5e-3, 0.3), 3: (3.0, 2.0e-3, 0.1), 4: (3.0, 1.5e-3, 0.0), 6: (3.0, 8.0e-3, 0.4), 9: (3.0, 32e-3, 0.4)}.get(g, (3.0, 32e-3, 0.4))
    return Fabric("aac6 loopback TCP, %d node-processes" % g, bw_apu=bw / 4, lat=0.0, group=10**6, fixed=fx, write_bw=1.0, tcp_exp=ex, gpu_share=g, target=False)

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
    """Phase 15 CX (results/CX15.md): mn_grid's pieces and the transform cache's state per operand, as the code decides them (rns_dist.c
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
    npc = 4 if p24 else piece_np(dz, nc, na, nb)        # Phase 15 NP (MPB: min(na, nb) under NP_AUTO_MIN): the piece's primes (ECALC_NP=auto: 4 over the three-prime bound); P24: four
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
    t_f, nic, glob, msgs = fab.a2a(8 * q, g, K_CHUNKS)
    exposed_tr = max(0.0, t_f - hide * t_x)
    c.nic += n_tr * nic; c.glob += n_tr * glob; c.msgs += n_tr * msgs; c.xfers += n_tr
    # the operand redistributions (A, B, X) and the result: plain all-to-alls over the group of the operands' bytes
    t_r = 0.0
    for limbs in ([na] if fwd >= 1 else []) + ([nb] if fwd >= 2 else []) + ([nc] if with_x else []) + [nc]:
        t1, nic1, glob1, msgs1 = fab.a2a(8 * limbs / (4 * g), g, 1)
        t_r += t1; c.nic += nic1; c.glob += glob1; c.msgs += msgs1; c.xfers += 1
    # the spills: the columns' carries (4 limbs per column) of every rank, all-gathered (flat) or exchanged exactly (grid)
    C = 1 << (mem_model.mn_shape(pts, g)[2])
    if form == "flat": t_s, nic_s, glob_s, msgs_s = fab.allgather(C * 4 * 8, g)
    else: t_s, nic_s, glob_s, msgs_s = fab.a2a(2 * C * 4 * 8, g, 1)
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
NP_AUTO_MIN = os.environ.get('ECALC_NP_AUTO_MIN', '0').strip() not in ('', '0')   # Phase 15 MPB: the C switch (crt.c ec_np_terms): the switch-over
                                                                                    # on min(pa, pb), the coefficients' real term count, instead of pa + pb
NP_STATS = dict(n3=0, n4=0)                # the pieces priced at three / four primes under auto (the memo counts a product once)
def piece_np(dz, nc, na=None, nb=None):
    """Phase 15 NP (crt.c ec_np_for): a piece's prime count -- the design's np, or under np_auto four when nc = pa + pb exceeds the
    switch-over (mn_core / dist_core decide per product; the one-node tiers keep three).  Phase 15 MPB: with NP_AUTO_MIN
    (ECALC_NP_AUTO_MIN=1) the term count is min(na, nb) (crt.c ec_np_terms)"""
    if dz is None or dz.legacy: return EC_NP
    if getattr(dz, 'np_auto', False):
        t = min(na, nb) if NP_AUTO_MIN and na is not None and nb is not None else nc
        k = 4 if t > NP_AUTO_TERMS else 3; NP_STATS['n%d' % k] += 1; return k
    return dz.np

# ---- Phase 15 Batch 3 P24 (results/P2415.md): four primes at 24 digits per transform point in the mn tier (MN_P24) ---------------------------
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
    if getattr(dz, 'np_auto', False):                                 # INT3: MPB's rule (min(pa, pb) under ECALC_NP_AUTO_MIN=1), as mn_p24_of
        return (min(pa, pb) if NP_AUTO_MIN else pa + pb) > NP_AUTO_TERMS
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
    dk = None if dz is None else (dz.legacy, dz.np, getattr(dz, 'np_auto', False), NP_AUTO_TERMS, NP_AUTO_MIN, dz.modmul, dz.pool_log(), dz.t_mb, dz.depth, dz.gen_hide, dz.force_gen, T_ROUND, HIDE_POW2, GEN_HIDE_DEPTH[1], GEN_HIDE_DEPTH[2], T_PIECE_31_NP[dz.np], F_MM1, PIECE13, CAL13, GRID_ADD['C'], dz.p15b, TWREC_F, TWREC_G, CACHE_MODEL, cache_slots_now(), CACHE_HIT_F, cache_primes_now(), CACHE_LOOP, P24, P24_F, CACHE_P24, CACHE_PART_F)
    k = (fab.bw, fab.lat, fab.group, fab.layers, fab.taper, fab.gpu_share, fab.fixed, fab.tcp_exp, fab.coll_fixed, fab.target,
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

DKM = os.environ.get('MN_MODEL_DKM', os.environ.get('NEWTON_DKM', '1')) == '1'   # default on since Phase 15 Batch 3 (the user's decision, 2026-09-29)   # Phase 15 DKM (results/DKM15.md): NEWTON_DKM=1 -- the division in two quotient halves
DKM_LAST = {}                                        # the last division_cost_dkm's parts (the early writer's overlap reads 'hide')
DKM_HI = os.environ.get('MN_MODEL_DKM_HI', os.environ.get('MN_OUT_DKM_HI', '0')) == '1'   # Phase 15 EW (results/EW15.md): MN_OUT_DKM_HI=1 -- the writer on X_hi's
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
    """Phase 15 EW (MN_OUT_DKM_HI, results/EW15.md 1.3, 5): the part file's exposed seconds when the writer (W seconds of formatting / writing)
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
SCHEDULES = {                       # the three schedules for 576 (MN_GROUPS values); None = the code's default
    "binary": None,                                    # 2, 4, ..., 512, 576: the top level joins a 512-group and a 64-group
    "9way": "2,4,8,16,32,64,576",                      # six doublings inside a dragonfly group of 64, then one 9-way level
    "3x3": "2,4,8,16,32,64,192,576",                   # ... then two 3-way levels
}

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
        start = ((g - 1) // S) * S if which == 'top' else 0; end = min(start + S, g); ch = []; k = start
        while k < end: ch.append((k, min(k + P, end))); k += P
        out.append((S, end - start, ch)); P = S
    return out

def tree_cost(fab, nq_node, g, groups=None, form="grid", T=None, which='top'):
    """level by level (mem_model.level_children: the level's group of S nodes and its children m_1 .. m_k, the
    previous level's groups): a k-way level is the fold the tree runs (results/L.md; tree_level for k = 2):
    (P, Q) <- (P Q_i + P_i, Q Q_i) for i = 2 .. k -- 2 (k - 1) products over the level's S nodes, the accumulated
    operand growing from m_1 to S - m_k leaf shares, the P product with the shifted add (X).
    Phase 13d D2: SHARES = 'terms' (T, the total digits, given): the operands are the top group's children's real lengths"""
    rows = []
    if SHARES == 'terms' and T is not None:
        for S, Sg, ch in level_top_group(g, groups, which):
            # mn.c tree_level (2 children: A = the lower, B = the upper) and tree_level_k (Horner from the top child: the running
            # pair starts as child k-1's, then P = P_i Q + P, Q = Q_i Q for i = k-2 .. 0 -- A = child i, B = the running Q)
            c = Cost(); run_ = int(range_limbs(T, g, ch[-1][0], ch[-1][1]))
            for (r0, r1) in reversed(ch[:-1]):
                m = int(range_limbs(T, g, r0, r1))
                c.add(product_cost(fab, m, run_, Sg, with_x=True, form=form))
                c.add(product_cost(fab, m, run_, Sg, form=form))
                run_ += m
            c.add(small_cost(fab, Sg, 2))
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
T_PIECE_31_NP = {4: 1.078, 3: 0.827}   # MEASURED (results/S13.md, E0 decimal, s24-26): C at 2^31 over the four APUs, P = 4 / 3
HIDE_POW2 = float(os.environ.get('MN_MODEL_HIDE_POW2', '0.75'))   # MEASURED (results/X13.md 3.2): the equal-slab path hides 74-76 % of its xGMI link time
                                       # (Phase 16 C: MN_MODEL_HIDE_POW2 / MN_MODEL_GEN_HIDE1 / MN_MODEL_GEN_HIDE2 / MN_MODEL_MAP_RATE override, as MN_MODEL_T_ROUND does)
GEN_HIDE_DEPTH = {1: float(os.environ.get('MN_MODEL_GEN_HIDE1', '0.011')), 2: float(os.environ.get('MN_MODEL_GEN_HIDE2', '0.74'))}
                                       # general map: MEASURED 1.1 % one deep (X13; 1.4 % on two real nodes, X13b); two deep MEASURED 74.3 %
                                       # on two real nodes (X13b, job 21068; 72.6-75.1 % on one node)
F_MM1 = 58.0 / 58.4                    # MEASURED (results/K13.md, one pair at 4e10): NTT_MODMUL=1 phases 58.4 -> 58.0 s
MAP_RATE = float(os.environ.get('MN_MODEL_MAP_RATE', '0.065'))   # MEASURED (results/I.md t_alloc 0.057-0.072 s/GB; P3: 25.8 GB fewer planes = -1.5..-2.9 s of init)
NODE_SCALE = float(os.environ.get('MN_MODEL_NODE_SCALE', '1.0'))   # Phase 16 C: the node's compute (bs, the leaf, the pieces, the size-1 dm) x this -- 1 = aac6's calibration
INIT_SCALE = float(os.environ.get('MN_MODEL_INIT_SCALE', '1.0'))   # Phase 16 C: init x this (the mapping rate of another ROCm / node class on top of MAP_RATE's device term)
# Phase 16 C (results/C16.md 2.3-2.5, 2.7): the aac7 values of the constants above, applied by apply_profile('aac7') -- MEASURED on 2 nodes
# (HIDE_POW2, GEN_HIDE at depth 1 / 2 from COMM_LAYER_STATS; T_ROUND from the MN_T_CHUNK_MB 512 / 1024 / 2048 series) and on one node
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
        # pieces' local passes), RNS_AUTO_PIECE_COST=1 (C2: one-node grids only, 0 at the target -- results/C215.md), ECALC_CORR_PATCH=2 (no rewrite, no T1 wait),
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
        o = dict(mem_model.DEFAULTS15 if self.p15 else mem_model.OLD13)
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
        if e['ECALC_NP'] == 'auto' and NP_AUTO_MIN: e['ECALC_NP_AUTO_MIN'] = 1              # Phase 15 MPB
        if self.cap is not None: e['ECALC_PLANE_CAP'] = mem_model.cap_name(self.cap)
        if self.shift_mb: e['MDB_SHIFT_CHUNK_MB'] = int(self.shift_mb)
        if self.t_mb: e['MN_T_CHUNK_MB'] = int(self.t_mb)
        e['COMM_ALLTOALLV_DEPTH'] = self.depth
        if self.p15 and self.round_mb: e['COMM_SHMEM_ROUND_MB'] = int(self.round_mb)   # the target's launch line (D2); the rest are the code's defaults
        if self.cache_fit: e['RNS_DIST_CACHE_FIT'] = 1                    # Phase 15 DOC2: the launch line (the user's decision 2 of 2026-09-28)
        if self.p15b and not self.p15c: e['BS_ARENA_ROOM'] = 0; e['DIST_TWREC_G'] = 0   # B1 on today's code
        return e

def DEFAULT15(**kw):
    """Phase 15 (agent MD): the code's defaults since Phase 14 on the target's launch line -- three primes, auto, 2^31, both chunks at 1024 MB,
    depth 2, NTT_MODMUL=1, DM_TIGHT, MN_TREE_EARLY_FREE, NEWTON_RECIP_CUT, the SHMEM pool from the plan, COMM_SHMEM_ROUND_MB=1024 (D2)"""
    o = dict(np=3, strategy='auto', cap=1 << 31, chunk='both', depth=2, modmul=1, p15=True); o.update(kw)
    return Design(**o)
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

def big_by_form(D, strategy='auto', cap=1 << 31, np=3):
    """Phase 13d D2: the big products' plain-law seconds by (phase group 'bs' | 'dm', form), x NTT_MODMUL=1's factor (the fit's input)"""
    out = {}
    for ph, name, na, nb, lowcut, w, count in big_shapes(D):
        g = 'bs' if ph == 'top' else 'dm'
        for st, p in product_pieces(na, nb, lowcut, w, strategy, cap, np)[1]:
            out[(g, st)] = out.get((g, st), 0.0) + count * t_prod(st, p, np)[0] * F_MM1
    return out

def pieces1(D, strategy='auto', cap=1 << 31, np=3):
    """Phase 13d D2: size 1's piece counts in L's MN_PLAN_ONLY=<D>:1 categories for the big shapes: leaf (the mdev levels), div, and
    the reciprocal's top three doublings"""
    c = {'top': 0, 'recip': 0, 'div': 0}
    for ph, name, na, nb, lowcut, w, count in big_shapes(D):
        c[ph] += count * len(product_pieces(na, nb, lowcut, w, strategy, cap, np)[1])
    return c

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
    # agent P's one-node ceilings at three primes (results/P13b.md; NTT_MODMUL=1): beyond the phase table (extrapolated walls), the device exact
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
    return mem_model.mem_per_node(int(D), 1, dict(mem_model.OLD13, np=np, cap=cap))['dev_init'] / 1e9

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
# seconds per 2^31 points (the piece's temporary and its shifted add into C).  FITTED on G13d's logs (see results/D213d.md).
PIPE_ONE = {'C': 1.221, 'B': 1.150, 'B4': 1.150}     # FITTED (fit_pipe on G13d's 11 sweep logs, 303 products / 1381 pieces >= 2^27 limbs)
GRID_ADD = {'C': 0.0, 'B': 0.075, 'B4': 0.075}       # FITTED (s per 2^31 points of the piece)
GRID_NC = 0.0793                          # s per piece of a grid per 2^31 limbs of the whole product (the size-1 grid's accumulation passes over C;
                                       # the mn tier normalises once per product, mn_grid: not applied there)
K_BS = 23.5 / 25.2                     # MEASURED (M-run 13b, auto 2^31 at 4e10): K's kernels on (n 8) / off (n 3), bs
K_DM = 22.5 / 22.9                     #   and dm; ASSUMED size-independent where a K-off run is used
K_INIT = 1.0
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
NODE_F = {'s24-30': 70.73 / 67.89}      # MEASURED (P13b, the same 2^31 run at 4e10 on s24-30 and s24-16): s24-30 runs 4.2 % slower; used by
                                       # calib13 only (the model predicts an s24-16 / s24-26 node)

def calib13(verbose=True):
    """Phase 13d D2: the recalibrated model against every measured one-node run of CAL13_RUNS (the gate: 3 % of the wall)"""
    worst = 0.0
    if verbose: print('%-10s | %7s %7s %6s | %6s %6s %6s | %s' % ('digits', 'wall', 'model', 'err', 'phases', 'model', 'err', 'source'))
    for r in sorted(CAL13_RUNS, key=lambda r: r['D']):
        d = Design(np=3, strategy=r.get('strategy', 'auto'), cap=1 << 31, chunk='shift' if r.get('strategy', 'auto') == 'auto' else 'off', depth=2 if r.get('strategy', 'auto') == 'auto' else 1)
        x = run(TARGET, r['D'], 1, verbose=False, design=d)
        nf = NODE_F.get(r.get('node') or next((k for k in NODE_F if k in r['src']), ''), 1.0)
        kb, kd = (1.0, 1.0) if r['k'] else (1 / K_BS, 1 / K_DM)
        ph = ((x['batch'] + x['top']) * kb + (x['recip'] + x['div']) * kd) * nf; w = x['init'] + ph + x['other']
        e = w / r['total'] - 1; ep = ph / (r['bs'] + r['dm']) - 1
        if r['use'] == 'fit': worst = max(worst, abs(e))
        if verbose: print('%.4e | %7.1f %7.1f %+5.1f%% | %6.1f %6.1f %+5.1f%% | %s%s%s' % (r['D'], r['total'], w, 100 * e, r['bs'] + r['dm'], ph, 100 * ep, r['src'][:70],
                          ' [node x %.3f]' % nf if nf != 1.0 else '', ' (check)' if r['use'] != 'fit' else ''))
    if verbose: print('worst fitted-run wall error %.1f %%' % (100 * worst))
    return worst
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

def _lsq_line(xs, ys, ws):
    """weighted least squares y = c0 + c1 x"""
    W = sum(ws); mx = sum(w * x for w, x in zip(ws, xs)) / W; my = sum(w * y for w, y in zip(ws, ys)) / W
    sxx = sum(w * (x - mx) ** 2 for w, x in zip(ws, xs)); sxy = sum(w * (x - mx) * (y - my) for w, x, y in zip(ws, xs, ys))
    c1 = sxy / sxx if sxx > 0 else 0.0
    return my - c1 * mx, c1

def _solve(A, b):
    """least squares by the normal equations (small, dense)"""
    n = len(A[0]); N = [[sum(r[i] * r[j] for r in A) for j in range(n)] for i in range(n)]; y = [sum(r[i] * bb for r, bb in zip(A, b)) for i in range(n)]
    for i in range(n):
        piv = max(range(i, n), key=lambda r: abs(N[r][i])); N[i], N[piv] = N[piv], N[i]; y[i], y[piv] = y[piv], y[i]
        for r in range(n):
            if r != i and N[i][i]:
                f = N[r][i] / N[i][i]; N[r] = [a_ - f * c for a_, c in zip(N[r], N[i])]; y[r] -= f * y[i]
    return [y[i] / N[i][i] if N[i][i] else 0.0 for i in range(n)]

def fit_pipe(logs, verbose=True):
    """Phase 13d D2: the pipeline's per-product law from ecalc logs of the default design (every 'dist_db' line with its time: a size-1
    product): per form F, a one-plane product = PIPE_ONE[F] x t_prod; a piece of a grid adds GRID_ADD[F] x its points / 2^31 + GRID_NC x the
    whole product's limbs / 2^31.  Least squares per form over the products of >= 2^27 limbs (weights 1 / sqrt(t)); GRID_NC is the
    pieces-weighted mean of the two forms' values (0.078 B, 0.076 C on G13d's first seven logs).  Sets them; returns the rows."""
    import re
    global GRID_NC
    cap = 1 << 31; pl, r3 = mem_model.cap_pool(cap); rows = []
    for fn in logs:
        for line in open(fn, errors='replace'):
            m = re.search(r'dist_db (\d+) x (\d+) limbs: (\d+) x (\d+) pieces of (\d+) \+ (\d+), (\d+) formed, (\d+) skipped.*?: ([\d.]+) s', line)
            if m: nc = int(m.group(1)) + int(m.group(2)); n = int(m.group(5)) + int(m.group(6)); k = int(m.group(7)); t = float(m.group(9)); grid = True
            else:
                m = re.search(r'dist_db (\d+) limbs: ([\d.]+) s', line)
                if not m or int(m.group(1)) < (1 << 27): continue
                n = nc = int(m.group(1)); k = 1; t = float(m.group(2)); grid = False
            if _b_fits(n, pl, r3, 3): st, pts = 'B', _b_len(n)[0]
            else: st, pts = 'C', _plane_pts_cap(n, r3, pl)
            rows.append((fn, st, pts, k, t, grid, nc, t_prod(st, pts, 3)[0] * F_MM1))
    nc_fit = []
    for F in ('B', 'C'):
        rs = [r for r in rows if r[1] == F]
        if not rs: continue
        A = []; bb = []
        for r in rs:
            w = 1.0 / max(r[4], 0.05) ** 0.5
            A.append([w * r[3] * r[7], w * (r[3] * r[2] / 2 ** 31 if r[5] else 0.0), w * (r[3] * r[6] / 2 ** 31 if r[5] else 0.0)]); bb.append(w * r[4])
        x = _solve(A, bb)
        PIPE_ONE[F] = x[0]; GRID_ADD[F] = max(0.0, x[1]); nc_fit.append((x[2], sum(r[3] for r in rs if r[5])))
        if F == 'B': PIPE_ONE['B4'] = x[0]; GRID_ADD['B4'] = max(0.0, x[1])
        if verbose: print('fit_pipe %s: %d products (%d pieces): one-plane %.3f x the law; a grid piece + %.3f s per 2^31 points + %.4f s per 2^31 limbs of the product'
                          % (F, len(rs), sum(r[3] for r in rs), x[0], max(0.0, x[1]), x[2]))
    if nc_fit: GRID_NC = sum(v * w for v, w in nc_fit) / max(1, sum(w for v, w in nc_fit))
    cal13_reset()
    return rows

def fit13(runs=None, verbose=True, exclude=None):
    """Phase 13d D2: the per-node compute refitted on the 13c defaults: with the pipeline law (PIPE_ONE, GRID_ADD) in node_phases, per group
    (init, bs, dm) the ratio measured / modelled as a (D/4e10)^b by weighted log-linear least squares (weights n; K-off runs through the
    measured K ratios).  exclude: a run index left out (the leave-one-out check).  Sets CAL13_LAW; returns (law, rows)."""
    global CAL13_MODE
    runs = [r for r in (runs or CAL13_RUNS) if r['use'] == 'fit']
    cal13_reset(); d = DEFAULT13(); pts = []
    for i, r in enumerate(runs):
        p = node_phases(r["D"], d, pipe=True); kb, kd = (1.0, 1.0) if r['k'] else (K_BS, K_DM)
        nf = NODE_F.get(r.get('node') or next((k for k in NODE_F if k in r['src']), ''), 1.0); kb /= nf; kd /= nf   # the phases on an s24-16 / s24-26 node
        bt, bd = node_big(r['D'], d)
        pts.append(dict(i=i, D=r['D'], n=r.get('n', 1), init=r['init'], bs=r['bs'] * kb, dm=r['dm'] * kd,
                        other=max(0.0, r['total'] - r['init'] - r['bs'] - r['dm']), m=dict(init=p['init'], bs=p['batch'] + p['top'] - bt, dm=p['recip'] + p['div'] - bd),
                        big=dict(init=0.0, bs=bt, dm=bd)))
    global DM_REST
    use = [q for q in pts if q['i'] != exclude]; law = {}
    xs = [q['D'] / 4e10 for q in use]; DM_REST = sum(q['n'] * (q['dm'] - q['big']['dm']) * x for q, x in zip(use, xs)) / sum(q['n'] * x * x for q, x in zip(use, xs))
    for g in ('init', 'bs', 'dm'):
        xs = [math.log(q['D'] / 4e10) for q in use]; ys = [math.log(max(1e-3, q[g] - q['big'][g]) / q['m'][g]) for q in use]; ws = [q['n'] for q in use]
        c0, c1 = _lsq_line(xs, ys, ws); law[g] = (math.exp(c0), c1)
    rows = []; err = 0.0
    for q in pts:
        x = min(max(q['D'], CAL13_RANGE[0]), CAL13_RANGE[1]) / 4e10
        w = sum(law[g][0] * x ** law[g][1] * q['m'][g] + q['big'][g] for g in ('init', 'bs')) + q['big']['dm'] + DM_REST * q['D'] / 4e10 + q['other']
        meas = q['init'] + q['bs'] + q['dm'] + q['other']; e = (w - meas) / meas; rows.append((q, w, meas, e))
        if q['i'] != exclude: err += q['n'] * e * e
    CAL13_LAW.update(law); CAL13_MODE = 'law'; cal13_reset()
    if verbose:
        print('fit13: law init %.3f (D/4e10)^%+.3f, bs(rest) %.3f ^%+.3f; dm = the big products + %.2f s x D/4e10; rms %.2f %%' % (law['init'][0], law['init'][1], law['bs'][0], law['bs'][1],
              DM_REST, 100 * math.sqrt(err / max(1, sum(q['n'] for q in use)))))
        for q, w, meas, e in rows:
            print('   %.4e  measured %6.1f  model %6.1f  %+5.1f %%%s   %s' % (q['D'], meas, w, 100 * e, ' (left out)' if q['i'] == exclude else '', runs[q['i']]['src']))
    return law, rows

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
# results/V314.md 1) is its own term: the seeds end at SEED15[0] + SEED15[1] x the node's span digits / 1e9 after the start, where a term above
# 2^33 costs SLOW_F x (mul1_serial's u128 path, measured x 1.29 per limb-step on the node's Zen 4 CPU by V3) -- at the 576 target every
# term of the top node is above 2^33, on one node only those of the top 18 % (1e11) to 36 % (1.3e11) of the digits.
CAL15 = True
CAL15_RANGE = (4e10, 1.3e11)
CAL15_LAW = {'init': (0.6589, 0.2432), 'bs': (1.0154, -0.206), 'recip': (0.8657, -0.1572), 'div': (1.1631, -0.0625)}   # FITTED by fit15() on CAL15_RUNS (`mn_model.py --fit15`)
SEED15 = (7.589, 0.28189)               # FITTED by fit15(): seeds end = a + b x span digits / 1e9 (s after the start)
SLOW_K = 1 << 33                       # binsplit.c mul1_serial: the Barrett step only for m < 2^33
SLOW_F = 1.29                          # MEASURED (V314 1: the u128 path's cost per limb-step against the fast one, Zen 4)
OVL1 = 0.577                           # FITTED by fit15(): size 1 -- the writer runs under this fraction of the division (it starts at the hook, before the low product)
WRITE_BW_AAC6 = 1.723                  # FITTED by fit15() (GB/s): aac6's local /tmp with ECALC_ODIRECT=1 -- the corrected runs' rewrite (dc less DC_FIX)
DC_FIX = 1.6                           # MEASURED (the corrected 1e11 runs): dc's residues + fetch beside the write (0.7 + 0.9 s)
DC_FMT1 = 0.0545                       # MEASURED (V314 e11_def / e11_def2 without a file: dc 5.55 / 5.43 s at 1e11, the digits redone after the corrections): s per 1e9 digits
DC_FMT_MN = 0.061                      # s per 1e9 digits: size > 1, mn_out_run's formatting + digit residues + fetch, pipelined with the write -- MEASURED at size 1,
                                       # 1e11 without a file (format 1.86 + digit residue 2.7 + fetch 0.86 + residues 0.7 = 6.1 s, V314), ASSUMED to carry to the target's
                                       # 7.4e10 per node; 2 nodes at 5e9 per node measured 0.104 (V114 b2: 0.23 + 0.22 + 0.07 s, the small size's fixed costs)
# ---- Phase 15 (agent DOC): the user's decisions of 2026-09-27 (p15b designs), calibrated on RESULTS 86's paired 1e11 series (jobs 21550 with the digit
# file on s24-16, 21563 without it on s24-26; B0 = the Phase 14 defaults, "cand" = every candidate on = the defaults of 2026-09-27; logs ~/fin15/ on aac6).
# Means, without the file (four runs each, s24-26): B0 init 16.60, bs 93.40 (seed wait 20.10), recip 41.41, dm 99.17 (2 corrections), total 210.15;
# cand init 18.08, bs 72.50 (seed wait 16.11), recip 37.67, dm 100.35 (0 corrections), total 191.28.
FILL_BS = 0.7693                       # MEASURED ratio of bs without the seed wait, cand / B0 = 56.39 / 73.30 s (BS_SEED_FILL=128: one more level in the batch tier,
                                       # every batch length filled; T2 modelled -8.4 s of it, results/T215.md); ASSUMED to carry to the target's leaf (T2: the fill holds
                                       # 128 limbs on every node, S = 235 on node 0, ~184 on the top node)
SEED15B = (7.589, 0.2681)              # FITTED (ten cand runs, both series: the seeds end at init + the wait = 34.40 s at 1e11): BI_MUL1_FAST -- one form for every
                                       # multiplier, no u128 path above 2^33 (SLOW_F no longer applies); the intercept kept from SEED15.  At the target's top node the
                                       # seeds end 7.3 s earlier (modelled; S1 had -3...-7 s, results/S115.md)
P15B_RECIP1 = 0.9097                   # MEASURED ratio at size 1, cand / B0: the reciprocal 37.67 / 41.41 s (NEWTON_RECIP_MID -3.4 s (R4), C2's grids, DIST_TWREC)
P15B_DIV1 = 1.0851                     # MEASURED ratio at size 1, cand / B0: the division (dm - recip) 62.68 / 57.76 s -- the fill's pool remaps (+6..+12 s, RL showed the
                                       # HIP runtime serializes them) against C2 (no corrections) and DIST_TWREC; size 1 only (at size > 1 the division is the fabric
                                       # model's pieces; the remap penalty there is ASSUMED 0 -- the arena is the dm need's, which the fill leaves unchanged)
# Phase 15 G5 (results/G515.md): the general map (g not a power of two) packs with k_twpack_g / k_unpacktw_g, which DIST_TWREC never
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
CAL15C_RUNS = [   # fin15e (job 21670, s24-16): B1 (main f184d51's defaults) against B2's switches (ECALC_NP=auto BS_ARENA_ROOM=0.16 DIST_TWREC_G=1); `total` and elapsed
    dict(arm='B1', file=False, total=(210.36, 198.65, 196.95, 197.45), wall=(230.28, 201.22, 199.49, 199.90)),
    dict(arm='B2', file=False, total=(183.69, 190.08, 184.56, 182.22), wall=(186.31, 192.99, 187.10, 184.70)),
    dict(arm='B1', file=True, total=(202.89, 202.19), wall=(205.54, 204.96)),
    dict(arm='B2', file=True, total=(190.47, 186.79), wall=(193.05, 189.47)),
]
TWREC_F = 0.98                         # the pieces' local passes x this with DIST_TWREC=1: MODELLED from the measured -1.8 s of dm at 1e11 on ~90 s of four-step products
                                       # (results/C215.md 2: -2.8 +- 0.8 s of total); X13b measured the pack x1.65, the unpack x1.43 (bench); ASSUMED to carry to the mn tier
PACKED_BPD = 8.0 / 18.0                # ECALC_OUT_PACKED=1: 8 bytes per 18 digits (0.444 B/digit; 1.0 for ASCII) -- exact (results/IO15.md W2)
EXIT_S = 2.4                           # MEASURED: the process's elapsed wall less `total` less dc, every run of RESULTS 86's series (2.15-2.64 s: the pools' release, exit)
CAL15B_RUNS = [   # RESULTS 86's paired 1e11 series: arm, file, runs (total, elapsed), node, the design's switches -- the D3 walls are the elapsed times
    dict(arm='B0', file=True, total=(239.08, 233.96, 241.05), wall=(297.71, 292.71, 300.24), node='s24-16', corr=2),
    dict(arm='B0', file=False, total=(211.11, 210.16, 210.46, 208.88), wall=(218.54, 217.62, 218.22, 215.93), node='s24-26', corr=2),
    dict(arm='def', file=True, total=(217.13, 212.00, 214.69), wall=(242.21, 237.05, 238.81), node='s24-16', corr=0),
    dict(arm='def', file=False, total=(204.30, 202.21, 203.00, 205.63), wall=(206.45, 204.36, 205.07, 207.79), node='s24-26', corr=0),
    dict(arm='cand', file=True, total=(205.37, 193.92, 203.02), wall=(228.86, 218.17, 227.01), node='s24-16', corr=0),
    dict(arm='cand', file=False, total=(193.82, 191.56, 191.49, 188.23), wall=(196.20, 193.80, 193.99, 190.44), node='s24-26', corr=0),
    dict(arm='candpk', file=True, total=(204.26, 201.06, 198.09), wall=(206.86, 203.65, 200.67), node='s24-16', corr=0),
]

CAL15_RUNS = [   # the Phase 14 defaults, one node: D, init, bs, seeds (the bs line's wait), dm, recip, T1, total, corrections, file (a digit file written), dc, n, source
    dict(D=4e10, init=11.1, bs=32.03, seeds=8.2, dm=23.47, recip=9.9, T1=0.0, total=66.66, corr=0, file=False, dc=0.0, n=1, src='V314 e4_def (job 21436, s24-30), no file'),
    dict(D=1e11, init=17.2, bs=96.29, seeds=19.7, dm=101.61, recip=43.0, T1=0.67, total=215.93, corr=2, file=False, dc=5.55, n=1, src='V314 e11_def (job 21436, s24-30), no file'),
    dict(D=1e11, init=18.5, bs=96.62, seeds=20.5, dm=101.20, recip=42.7, T1=0.67, total=217.10, corr=2, file=False, dc=5.43, n=1, src='V314 e11_def2 (job 21436, s24-30), no file'),
    dict(D=1e11, init=16.48, bs=97.23, seeds=20.36, dm=102.81, recip=42.88, T1=23.46, total=240.33, corr=2, file=True, dc=60.18, n=5,
         src='the five-run 1e11 series (closing.sh, s24-30, 2026-09-26 20:09-21:32 EDT): 239.58-241.04, digits to /tmp'),
    dict(D=1e11, init=15.9, bs=95.07, seeds=20.0, dm=100.85, recip=42.1, T1=21.6, total=233.84, corr=2, file=True, dc=None, e2e=293.0, n=1,
         src='mnaccept full 1e11 (job 21444, s24-16): 293 s elapsed with the write'),
    dict(D=1e11, init=16.8, bs=96.66, seeds=20.5, dm=99.88, recip=41.7, T1=20.91, total=234.55, corr=2, file=True, dc=57.01, n=1, src='V214 A3 c2_1e11_def (job 21443, s24-16)'),
    dict(D=1.16e11, init=21.2, bs=116.75, seeds=24.2, dm=149.32, recip=60.2, T1=0.0, total=287.74, corr=0, file=True, dc=19.48, n=1, src='V214 B c2_116_def (job 21447, s24-26)'),
    dict(D=1.3e11, init=19.5, bs=141.59, seeds=28.2, dm=163.69, recip=59.6, T1=0.0, total=325.17, corr=0, file=True, dc=22.27, n=1, src='V214 B c2_130_def (job 21447, s24-26)'),
]

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

def fit15(verbose=True):
    """Phase 15: refit CAL15_LAW, SEED15, OVL1 and WRITE_BW_AAC6 on CAL15_RUNS (the model with CAL13, CAL15 off, the default design).  Sets them."""
    global CAL15, SEED15, OVL1, WRITE_BW_AAC6
    saved = CAL15; CAL15 = False; d = DEFAULT15(); pts = []
    for r in CAL15_RUNS:
        x = run(TARGET, r['D'], 1, verbose=False, design=d)
        pts.append((r, x))
    CAL15 = saved
    law = {}
    for key, meas, mod in (('init', lambda r: r['init'], lambda x: x['init']), ('bs', lambda r: r['bs'] - r['seeds'], lambda x: x['batch'] + x['top']),
                           ('recip', lambda r: r['recip'], lambda x: x['recip']), ('div', lambda r: r['dm'] - r['recip'], lambda x: x['div'])):
        xs = [math.log(r['D'] / 4e10) for r, x in pts]; ys = [math.log(meas(r) / mod(x)) for r, x in pts]; ws = [r['n'] for r, x in pts]
        c0, c1 = _lsq_line(xs, ys, ws); law[key] = (round(math.exp(c0), 4), round(c1, 4))
    xs = [span_digits(r['D'], 1, r['D']) / 1e9 for r, x in pts]; ys = [r['init'] + r['seeds'] for r, x in pts]; ws = [r['n'] for r, x in pts]
    c0, c1 = _lsq_line(xs, ys, ws); SEED15 = (round(c0, 3), round(c1, 5))
    bws = [(r['D'] / 1e9 / (r['dc'] - DC_FIX), r['n']) for r, x in pts if r['file'] and r['corr'] and r['dc']]
    WRITE_BW_AAC6 = round(sum(b * n for b, n in bws) / sum(n for b, n in bws), 3)
    ov = []
    for r, x in pts:
        if not r['file']: continue
        W = r['D'] / 1e9 / WRITE_BW_AAC6; div = r['dm'] - r['recip']
        O = W - r['T1'] if r['corr'] else (W - r['dc'] if r['dc'] is not None else None)
        if O is not None: ov.append((O / div, r['n']))
    OVL1 = round(sum(o * n for o, n in ov) / sum(n for o, n in ov), 3)
    CAL15_LAW.update(law)
    if verbose:
        print('fit15: init %.4f (D/4e10)^%+.4f, bs (without the seed wait) %.4f ^%+.4f, recip %.4f ^%+.4f, division %.4f ^%+.4f; seeds end %.3f + %.5f s per 1e9 span digits; '
              'aac6 /tmp write %.3f GB/s; the size-1 writer under %.3f of the division (per run: %s)' % (law['init'][0], law['init'][1], law['bs'][0], law['bs'][1],
              law['recip'][0], law['recip'][1], law['div'][0], law['div'][1],
              SEED15[0], SEED15[1], WRITE_BW_AAC6, OVL1, ', '.join('%.2f' % o for o, n in ov)))
    return law

def calib15(verbose=True):
    """Phase 15: the model (DEFAULT15, one node, the aac6 disk) against every run of CAL15_RUNS: init, bs (seed wait), dm, T1, `total`, and the
    end-to-end walls with and without the digit file (D3); returns the worst error of `total`"""
    fab = Fabric(TARGET.name, TARGET.bw, TARGET.lat, write_bw=WRITE_BW_AAC6); d = DEFAULT15(); worst = 0.0
    if verbose:
        print('%-8s %-4s %-4s | %5s %5s | %6s %6s | %5s %5s | %6s %6s | %5s %5s | %7s %7s %6s | %6s %6s %6s | %s' % ('D', 'corr', 'file', 'init', 'model', 'bs', 'model', 'seeds', 'model',
              'dm', 'model', 'T1', 'model', 'total', 'model', 'err', 'e2e', 'model', 'err', 'source'))
    for r in CAL15_RUNS:
        x = run(fab, r['D'], 1, verbose=False, design=d, corrections=r['corr'])
        tot = x['total_line'] if r['file'] else x['compute']
        e2e_m = x['wall_write'] if r['file'] else x['wall_nowrite']
        e2e = (r['total'] + r['dc']) if r['dc'] is not None else r.get('e2e')
        err = tot / r['total'] - 1; worst = max(worst, abs(err))
        if verbose:
            print('%-8.3g %-4d %-4s | %5.1f %5.1f | %6.1f %6.1f | %5.1f %5.1f | %6.1f %6.1f | %5.1f %5.1f | %7.2f %7.2f %+5.1f%% | %6s %6.1f %6s | %s%s' % (
                r['D'], r['corr'], 'yes' if r['file'] else 'no', r['init'], x['init'], r['bs'], x['batch'] + x['top'] + x['seed_wait'], r['seeds'], x['seed_wait'],
                r['dm'], x['recip'] + x['div'], r['T1'], x['t1_wait'] if r['file'] else 0.0, r['total'], tot, 100 * err, '%.1f' % e2e if e2e else '-', e2e_m,
                '%+.1f%%' % (100 * (e2e_m / e2e - 1)) if e2e else '-', r['src'][:64], ' (n=%d)' % r['n'] if r['n'] > 1 else ''))
    if verbose: print('worst `total` error %.1f %% (the model: DEFAULT15 at size 1; e2e = `total` + dc, the end-to-end wall with (file yes) or without (no) the digit file; the part file at %.2f GB/s, the size-1 writer under %.3f of the division)' % (100 * worst, WRITE_BW_AAC6, OVL1))
    return worst

def calib15b(verbose=True):
    """Phase 15 (agent DOC): the model at 1e11 on one node against RESULTS 86's paired series -- B0 (DEFAULT15, two corrections), the defaults of 2026-09-27
    with ASCII output (DEFAULT15B(packed=False)) and packed (DEFAULT15B()); `total` and the two walls (D3: the process's elapsed time with and without the digit
    file; B0's model walls + EXIT_S, which the p15b designs carry).  'def' (C2 alone) is listed measured only (the model has no C2 term at size 1).
    Returns the worst error of the walls."""
    fab = Fabric(TARGET.name, TARGET.bw, TARGET.lat, write_bw=WRITE_BW_AAC6); worst = 0.0
    designs = {'B0': DEFAULT15(), 'cand': DEFAULT15B(packed=False), 'candpk': DEFAULT15B()}
    if verbose: print('%-7s %-5s %-7s %2s | %8s %8s %6s | %8s %8s %6s' % ('arm', 'file', 'node', 'n', 'total', 'model', 'err', 'wall', 'model', 'err'))
    for r in CAL15B_RUNS:
        mt = sum(r['total']) / len(r['total']); mw = sum(r['wall']) / len(r['wall'])
        d = designs.get(r['arm'])
        if d is None:
            if verbose: print('%-7s %-5s %-7s %2d | %8.1f %8s %6s | %8.1f %8s %6s   (measured only: C2 alone)' % (r['arm'], 'yes' if r['file'] else 'no', r['node'], len(r['total']), mt, '-', '-', mw, '-', '-'))
            continue
        x = run(fab, 1e11, 1, verbose=False, design=d, corrections=r['corr'])
        ex = 0.0 if d.p15b else EXIT_S
        tot = x['total_line'] if r['file'] else x['compute']
        if d.p15b and r['file']: tot = x['total_line']
        w = (x['wall_write'] if r['file'] else x['wall_nowrite']) + ex
        et, ew = tot / mt - 1, w / mw - 1; worst = max(worst, abs(ew))
        if verbose: print('%-7s %-5s %-7s %2d | %8.1f %8.1f %+5.1f%% | %8.1f %8.1f %+5.1f%%' % (r['arm'], 'yes' if r['file'] else 'no', r['node'], len(r['total']), mt, tot, 100 * et, mw, w, 100 * ew))
    if verbose: print('worst wall error %.1f %% (modelled against measured means; the part file at %.2f GB/s, the aac6 /tmp rate fitted by fit15)' % (100 * worst, WRITE_BW_AAC6))
    return worst

def calib15c(verbose=True):
    """Phase 15 (agent DOC2): the model at 1e11 on one node against the fin15e paired series (job 21670, s24-16): B1 (DEFAULT15B) and B2 (DEFAULT15C:
    + BS_ARENA_ROOM=0.16, DIST_TWREC_G=1, ECALC_NP=auto -- at size 1 only the room acts: P15C_DIV1).  Returns the worst wall error"""
    fab = Fabric(TARGET.name, TARGET.bw, TARGET.lat, write_bw=WRITE_BW_AAC6); worst = 0.0
    designs = {'B1': DEFAULT15B(), 'B2': DEFAULT15C()}
    if verbose: print('%-4s %-5s %2s | %8s %8s %6s | %8s %8s %6s | %s' % ('arm', 'file', 'n', 'total', 'model', 'err', 'wall', 'model', 'err', 'dm model (measured: B1 106.4, B2 91.3)'))
    for r in CAL15C_RUNS:
        mt = sum(r['total']) / len(r['total']); mw = sum(r['wall']) / len(r['wall'])
        x = run(fab, 1e11, 1, verbose=False, design=designs[r['arm']])
        tot = x['total_line'] if r['file'] else x['compute']
        w = x['wall_write'] if r['file'] else x['wall_nowrite']
        et, ew = tot / mt - 1, w / mw - 1; worst = max(worst, abs(ew))
        if verbose: print('%-4s %-5s %2d | %8.1f %8.1f %+5.1f%% | %8.1f %8.1f %+5.1f%% | %.1f' % (r['arm'], 'yes' if r['file'] else 'no', len(r['total']), mt, tot, 100 * et, mw, w, 100 * ew, x['recip'] + x['div']))
    if verbose: print('worst wall error %.1f %% (modelled against measured means, s24-16; B1\'s first no-file run 230.3 s elapsed is in the mean)' % (100 * worst))
    return worst

def memory(D, g, form="grid", groups=None, transport="shmem", pool_log=31, staging="code", design=None):
    """the per-node memory model (mem_model.mem_per_node): GB of device at the dm peak, host (with the SHMEM pool), the node peak"""
    if design is not None: design = design.at_g(g)                   # Phase 15 (2026-09-27): ECALC_NP=4 at size > 1
    o = dict(mem_model.OLD13); o.update(form=form, groups=groups, transport=transport, pool_log=pool_log, staging=staging)   # Phase 15: the old forms unless the design is p15
    if design is not None and not design.legacy: o.update(design.mem_opts(D * g)); o.update(p24=P24, np_auto_min=NP_AUTO_MIN, dkm=DKM)   # MS: MN_P24's scratch (mem_model's P24 branch); DL: dm_layout follows NEWTON_DKM (DKM)
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
        o = dict(mem_model.OLD13); o.update(form=form, groups=groups, transport=transport, pool_log=pool_log, staging=staging); o.update(design.mem_opts(D * g)); o.update(p24=P24, np_auto_min=NP_AUTO_MIN, dkm=DKM)
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
    if NODE_SCALE != 1.0 or INIT_SCALE != 1.0:             # Phase 16 C: another machine's node (apply_profile / MN_MODEL_NODE_SCALE, MN_MODEL_INIT_SCALE); the pieces carry it in t31
        ph["init"] *= INIT_SCALE; ph["batch"] *= NODE_SCALE; ph["top"] *= NODE_SCALE; seed_wait *= NODE_SCALE
        if g == 1: rc.t *= NODE_SCALE; dc.t *= NODE_SCALE
    t_compute = ph["init"] + seed_wait + ph["batch"] + ph["top"] + t_levels + rc.t + dc.t + ph["other"]
    out_write = D * (PACKED_BPD if (p15 and design.packed) else 1.0) / 1e9 / fab.write_bw   # the node's part file at the write bandwidth (packed: 0.444 B/digit)
    t_lowprod = dc.t * 0.5
    t1_wait = 0.0
    if p15 and dc_exposed is None:                     # Phase 15 (D3): the two walls, the part file as the code writes it
        if g == 1:                                     # size 1: the writer starts at the hook (before the low product) and overlaps OVL1 x the division; a
            O = ovl_div(dc, 1); fmt = DC_FMT1 * D / 1e9   # correction after the hook makes T1 wait for it (inside `total`) and then redoes the digits and rewrites
            if corrections and not p15b:               # the file after `total` (V2's finding, results/V214.md); ECALC_CORR_PATCH (p15b): no wait, no rewrite
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
    o = dict(mem_model.OLD13, form=form, groups=groups, transport=transport, staging=staging)   # Phase 15: a p15 design's mem_opts override the old forms
    if design is None: o.update(np=4, host_fit=False)
    elif not design.legacy:
        o.update(design.mem_opts(1e12 * g))                     # the cap rule at the run's digits: 2^31 above 5e10 (every 576-node size)
        if design.cap is None and g == 1: o.pop('cap')          # size 1: the code's rule follows D (3 2^30 below 5e10)
    return mem_model.max_digits_per_node(node_gb * 1e9, g, o)

def headline(fab, g, rule, form="grid", groups=None, staging="code", design=None):
    """the largest D per node that fits the node (502 GB) and the safe budget (480 GB), their walls"""
    out = []
    for budget in (NODE_GB, NODE_GB_MARGIN):
        D = max_digits(g, budget, form, groups, staging=staging, design=design)
        r = run(fab, D, g, rule, verbose=False, groups=groups, form=form, staging=staging, design=design) if D else None
        out.append((budget, D, r))
    return out

# ------------------------------------------------------------------------------------------------------------
# calibration: every recorded aac6 point -- g node-processes sharing one node over a loopback transport
# (results/X.md job 20802/20815, L.md jobs 20808/20813, M11.md job 20814, S.md job 20817; the logs' "total" lines)
# ------------------------------------------------------------------------------------------------------------
CALIB = [   # (D_total, g, transport, wall, init, tree levels, reciprocal, division, dc exposed, source)  -- seconds, node 0's lines
    (1e8,  2, "tcp",   8.5, 2.8,  0.6,  3.7,  1.0, 0.0, "X.md b2 (8.37-8.61 over five runs: X, L, M11)"),
    (1e8,  3, "tcp",   9.3, 2.9,  1.1,  4.0,  0.9, 0.1, "X.md b2 9.13 / L.md 9.41"),
    (1e8,  4, "tcp",   8.4, 3.0,  0.9,  3.6,  0.7, 0.1, "L.md 8.28 / X.md 8.09 / M11 8.98"),
    (1e8,  6, "tcp",  25.8, 3.3,  4.4, 14.5,  3.0, 0.1, "L.md batch 1 (the general map)"),
    (1e8,  9, "tcp",  62.2, 3.8, 14.6, 37.9,  4.9, 0.2, "L.md batch 1 (the general map)"),
    (1e9,  2, "tcp",  27.4, 5.3,  3.0, 12.2,  5.8, 0.4, "X.md 27.66 / 27.41, L.md 26.14 (B7)"),
    (1e9,  3, "tcp",  24.5, 5.5,  5.1,  9.4,  3.5, 0.4, "L.md batch 1 (26.5 with the grids forced)"),
    (1e9,  4, "tcp",  19.7, 5.3,  3.2,  8.0,  2.7, 0.1, "X.md 19.69 / 20.38, L.md 18.92"),
    (1e9,  6, "tcp",  55.8, 5.0,  9.3, 36.4,  4.2, 0.2, "L.md batch 2 (the general map)"),
    (1e10, 2, "tcp", 146.4, 9.3, 22.3, 64.9, 40.3, 5.0, "X.md job 20802 (POOL_LOG 30)"),
    (1e10, 4, "tcp", 108.0, 6.3, 28.0, 36.0, 30.0, 2.4, "X.md 110.1 / 105.7 / 100.6, L.md 99.3, M11 116.5: the node's spread"),
    (1e8,  2, "shmem", 22.4, 15.4, 0.6, 4.3, 1.7, 0.0, "S.md job 20817 (OSHMEM serial; init = shmem_init + the 8 GiB pool's registration)"),
    (1e8,  3, "shmem", 19.3, 10.9, 1.4, 4.9, 1.7, 0.0, "S.md job 20817"),
    (1e8,  4, "shmem", 18.5, 11.3, 1.2, 4.1, 1.5, 0.0, "S.md job 20817"),
    (1e9,  2, "shmem", 43.9, 10.9, 5.1, 18.6, 8.1, 0.4, "S.md job 20817"),
    (1e9,  4, "shmem", 43.8, 11.3, 7.3, 16.8, 7.8, 0.1, "S.md job 20817"),
]

def calibrate(rule, gate=0.10, verbose=True):
    print("calibration on aac6: g node-processes share one node (each drives the four APUs) over a loopback transport; the")
    print("leaves run concurrently (0.5 x the single-node bs of the total digits, measured), init is the process's own (measured);")
    print("the tree form is 'flat' (the code as run); the transport constants per (transport, g) are fitted (aac6_fabric).")
    print("%-6s %-2s %-5s | %8s %8s %6s | %6s %6s | %6s %6s | %6s %6s | %s" % ("digits", "g", "trans", "wall", "model", "err", "tree", "model", "recip", "model", "div", "model", "source"))
    ok = True; worst = 0.0
    for D, g, tr, wall, init, tree, recip, div, dcx, src in CALIB:
        fab = aac6_fabric(tr, g)
        Dn = D / g
        leaf = 0.5 * (phase("batch", D) + phase("top", D))
        r = run(fab, Dn, g, rule, verbose=False, leaf_scale=0.0, init_override=init, dc_exposed=dcx, form="flat", transport=tr, pool_log=27 if D <= 1e8 else 29)
        model = r["wall"] + leaf
        err = (model - wall) / wall
        ok &= abs(err) <= gate; worst = max(worst, abs(err))
        print("%-6.0e %-2d %-5s | %8.1f %8.1f %+5.0f%% | %6.1f %6.1f | %6.1f %6.1f | %6.1f %6.1f | %s" % (D, g, tr, wall, model, 100 * err, tree, r["levels"], recip, r["recip"], div, r["div"], src))
    print("calibration %s (gate: every run within %.0f %%; worst %.1f %%)" % ("OK" if ok else "FAILED", 100 * gate, 100 * worst))
    return ok

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

def plan_sweep(g, lo, hi, step, design=None, out=sys.stdout):
    """L's plan_sweep.sh columns, from the model"""
    print("# mn_model.plan sweep: g = %d, total digits %.4e .. %.4e step %.4e; SHARES %s" % (g, lo, hi, step, SHARES), file=out)
    print("# columns: digits | tree (node 0's groups) | tree (each level's largest group) | recip | div | total (node 0) | total (largest groups) | levels node0/largest", file=out)
    n = int(round((hi - lo) / step)); prev = None; ch = []
    for i in range(n + 1):
        T = lo + i * step; p = plan(g, T, design); _PC.clear(); split_grid.cache_clear(); plane_pts.cache_clear()   # (the memo grows by GBs over a sweep)
        print("%.4e  tree %4d  tree_max %4d  recip %3d  div %3d  total %4d  total_max %4d  levels %s" % (T, p['tree'], p['tree_max'], p['recip'], p['div'],
              p['tree'] + p['recip'] + p['div'], p['tree_max'] + p['recip'] + p['div'], ','.join('%d/%d' % ab for ab in zip(p['levels0'], p['levels_max']))), file=out)
        k = (p['tree'], p['tree_max'], p['recip'], p['div'])
        if prev and k != prev[1]: ch.append("%.4e -> %.4e: tree %d -> %d, tree_max %d -> %d, recip %d -> %d, div %d -> %d" % (prev[0], T, prev[1][0], k[0], prev[1][1], k[1], prev[1][2], k[2], prev[1][3], k[3]))
        prev = (T, k)
        out.flush()
    print("# the steps (a count changes between two neighbouring sizes):", file=out)
    for c in ch: print("#   " + c, file=out)

def schedules(fab, rule, D_list=(4e10, 6e10, 7.7e10), form="grid", design=None):
    print("the level schedule at 576 (MN_GROUPS), per-node wall of the distributed levels (s) by D per node; every level")
    print("costed as the tree forms it (a k-way level = 2 (k - 1) products over the level's group); 'global' = TB per node over")
    print("the dragonfly's global links in the levels (MN_TOPO_GROUP = %d), 'msgs' = messages per APU in the levels (k):" % fab.group)
    print("%-8s %-28s | " % ("name", "MN_GROUPS") + " | ".join("%8.1e: levels expo pcs global  msgs" % D for D in D_list))
    best = None
    for name, spec in SCHEDULES.items():
        cells = []; tot = 0.0
        for D in D_list:
            r = run(fab, D, 576, rule, verbose=False, groups=spec, form=form, design=design)
            L = r["levels_rows"]
            cells.append("%15.1f %4.1f %3d %6.1f %5dk" % (r["levels"], sum(c.t_exposed for _, _, c in L), sum(c.pieces for _, _, c in L), sum(c.glob for _, _, c in L) / 1e12, sum(c.msgs for _, _, c in L) / 1000))
            tot += r["levels"]
        print("%-8s %-28s | " % (name, spec or "(default: 2,4,...,512,576)") + " | ".join(cells))
        if best is None or tot < best[0]: best = (tot, name, spec)
    print("cheapest by the model (the sum over the three sizes): %s (MN_GROUPS=%s); the three are within the model's own error of each other" % (best[1], best[2] or "unset"))
    return best

# ============================================================================================================
# Phase 15 CX (results/CX15.md): the mn transform cache at the target -- what the slots are worth (the code's hits x CACHE_HIT_F) and
# the ways to hold one inside 480 GB, each with its memory and its modelled gain.  The rooms per node (modelled, mem_model / the
# standing estimate): the tree levels 480 - the bs-phase node (device 411.0 + host 44.4) + the arena's slack at the tree (arena - tree
# need: a slot drawn from the block pool), the division 480 - (device 417.0 + host 28.8); a slot = k primes x 2^29 limbs x 8 B x 4 APUs.
# ============================================================================================================
def _run_cache(T, g, design, slots=None, phase=None, primes=None, loop='code', model='code', primes_phase=None, fab=None):
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
        return run(fab, T / g, g, verbose=False, design=design)
    finally:
        (CACHE_MN_SLOTS, CACHE_SLOTS_PHASE, CACHE_PRIMES, CACHE_LOOP, CACHE_MODEL, tree_cost, division_cost, CACHE_PRIMES_PHASE) = saved; _PC.clear()

def cache_rooms(T=None, g=None, design=None):
    """(tree room, division room) per node in bytes (modelled): 480 GB less the phase's node figure, + the arena's slack in the tree"""
    T = T or TARGET_DIGITS; g = g or TARGET_NODES; design = (design or DEFAULT15C(cache_fit=False)).at_g(g)   # (Phase 15 DOC2: today's memory -- the arena room)
    r = mem_model.mem_per_node(T / g, g, design.mem_opts(T)); host_x = _run_cache(T, g, DEFAULT15C(cache_fit=False), slots=0)['mem']['host'] * 1e9 - r['host_hwm']   # (the run's mem is in GB: + the SHMEM pool)
    GB = 1e9; bud = NODE_GB_MARGIN * GB
    tree = bud - (r['dev_bs'] + r['host_hwm'] + host_x) + (r['arena'] - r['tree_need'])
    dm = bud - (r['dev_dm'] + r['host_dm'] + host_x)
    return tree, dm, dict(dev_bs=r['dev_bs'], dev_dm=r['dev_dm'], host=r['host_hwm'] + host_x, host_dm=r['host_dm'] + host_x, hole=r['arena'] - r['tree_need'])

def cache_proposals(T=None, g=None):
    T = T or TARGET_DIGITS; g = g or TARGET_NODES
    GB = 1e9; prime_slot = (1 << 29) * 8 * 4                         # one prime's plane per APU at the cap (q = 2^29 at every target grid), x 4 APUs
    for npm in (4, 'auto'):
        d = DEFAULT15C(np_mn=npm, cache_fit=False)                     # (Phase 15 DOC2: B2's defaults; the slots forced per row)
        tree_room, dm_room, x = cache_rooms(T, g, d)
        base = _run_cache(T, g, d, slots=0)
        print('== %.3g digits on %d nodes, ECALC_NP=%s (modelled; the fabric assumed as in TARGET.md): 0 slots %.1f s without the write, %.1f s with it (@%.1f GB/s)'
              % (T, g, npm, base['wall_nowrite'], base['wall'], TARGET.write_bw))
        print('   rooms per node: the tree levels %.1f GB (480 - bs %.1f - host %.1f + the arena\'s slack %.1f), the division %.1f GB (480 - dm %.1f - host %.1f); one prime of a slot %.1f GB, a four-prime slot %.1f GB'
              % (tree_room / GB, x['dev_bs'] / GB, x['host'] / GB, x['hole'] / GB, dm_room / GB, x['dev_dm'] / GB, x['host_dm'] / GB, prime_slot / GB, 4 * prime_slot / GB))
        rows = [('the old term, 2 slots (the standing 5.1e13 time)', dict(slots=2, model='old'), (2 * 4, 2 * 4)),
                ('2 full slots (the code\'s default RNS_DIST_CACHE_MN=2)', dict(slots=2), (2 * 4, 2 * 4)),
                ('1 full slot (B\'s piece j across i)', dict(slots=1), (4, 4)),
                ('1 slot, loop along the longer axis', dict(slots=1, loop='long'), (4, 4)),
                ('1 slot of 1 prime', dict(slots=1, primes=1), (1, 1)),
                ('1 slot of 1 prime, the longer axis', dict(slots=1, primes=1, loop='long'), (1, 1)),
                ('tree 1 slot of 2 primes, division 1 of 1', dict(phase={'tree': 1, 'dm': 1}, primes=None, split=(2, 1)), (2, 1)),
                ('tree 1 slot of 2 primes, division 1 of 1, longer axis', dict(phase={'tree': 1, 'dm': 1}, primes=None, split=(2, 1), loop='long'), (2, 1)),
                ('tree 2 slots of 1 prime, division 1 of 1', dict(phase={'tree': 2, 'dm': 1}, primes=1), (2, 1)),
                ('1 slot of 2 primes (both phases)', dict(slots=1, primes=2), (2, 2)),
                ('1 slot of 3 primes (both phases)', dict(slots=1, primes=3), (3, 3))]
        print('   %-52s | slot GB per node tree / division | fits 480 | no write        | with the write  | gain' % 'configuration')
        for name, kw, (pt, pd) in rows:
            split = kw.pop('split', None)
            if split:                                                     # tree k = split[0], division k = split[1]: two runs combined by phase
                lp = kw.get('loop', 'code')
                ra = _run_cache(T, g, d, slots=1, primes=split[0], loop=lp); rb = _run_cache(T, g, d, slots=1, primes=split[1], loop=lp)
                r = dict(ra); dl = (ra['recip'] + ra['div']) - (rb['recip'] + rb['div'])
                r['wall_nowrite'] = ra['wall_nowrite'] - dl; r['wall'] = ra['wall'] - dl
            else: r = _run_cache(T, g, d, **kw)
            mt, md = pt * prime_slot, pd * prime_slot
            fits = mt <= tree_room and md <= dm_room
            print('   %-52s | %5.1f / %5.1f                    | %-8s | %6.1f s (%.2f min) | %6.1f s | %+6.1f s'
                  % (name, mt / GB, md / GB, 'yes' if fits else 'no', r['wall_nowrite'], r['wall_nowrite'] / 60, r['wall'], r['wall_nowrite'] - base['wall_nowrite']))

# ============================================================================================================
# Phase 15 Batch 3 PC (results/PC15.md): RNS_DIST_CACHE_PARTIAL=1 -- the slots' planes drawn per grid product from the block pool (the arena),
# as many primes as its free bytes allow after the product's own need and a margin (RNS_DIST_CACHE_PARTIAL_MARGIN_GB per APU, 0.5), the grid's
# loop along its longer axis, P24 products cached too.  The pool's free bytes at a phase's peak product (modelled, mem_model): the tree
# arena - tree_need (the arena's slack); the division arena - dm_need, the arena laid for the default division (binsplit.c dm_layout does not
# follow NEWTON_DKM) and the need DKM's (mem_model.dm_layout(dkm=True)) when MN_MODEL_DKM=1 -- DKM's unused hole.  keep_room: whether the
# BS_ARENA_ROOM bytes inside the need are left alone (they are free at the products: the code's rule takes them; True = the conservative figure)
# ============================================================================================================
PARTIAL_MARGIN = float(os.environ.get('RNS_DIST_CACHE_PARTIAL_MARGIN_GB', '0.5')) * 1e9
def cache_partial_rooms(T=None, g=None, design=None, keep_room=False):
    """(tree, dm) pool bytes per node free at the phase's peak product (modelled), and the details"""
    T = T or TARGET_DIGITS; g = g or TARGET_NODES; design = (design or DEFAULT15C(cache_fit=False)).at_g(g)
    o = design.mem_opts(T); r0 = mem_model.mem_per_node(T / g, g, o)
    o1 = dict(o); o1['dkm'] = bool(DKM); r1 = mem_model.mem_per_node(T / g, g, o1)
    L = mem_model.dm_layout(int(r0['N']), g, 31, True, True, 0, room=o.get('arena_room', 0.0) or 0.0, dkm=bool(DKM))
    room_b = 4 * L['room'] if not keep_room else 0
    tree = r0['arena'] - r0['tree_need']; dm = r0['arena'] - (r1['dm_need'] - room_b)
    return tree, dm, dict(arena=r0['arena'], tree_need=r0['tree_need'], dm_need=r1['dm_need'], dm_need0=r0['dm_need'], room=4 * L['room'])
def cache_partial_primes(free_node, q=1 << 29, np_=4):
    """the planes of q limbs one APU's pool share holds after the margin (the code's rule, cache_pc_take), at most np_"""
    return max(0, min(np_, int((free_node / 4 - PARTIAL_MARGIN) // (q * 8))))
def cache_partial(T=None, g=None, keep_room=False, loops=('code', 'long'), arena_room=None, wbs=None, fab=None):
    """the estimate at the target with RNS_DIST_CACHE_PARTIAL=1 (modelled): the primes per phase from the pool's rooms, 1 slot, the loop;
    arena_room: BS_ARENA_ROOM (None: the design's 0.16); wbs: the write rates priced (default TARGET_WRITE_BW, 0.6); fab: the fabric
    priced (default TARGET -- callers passing --bw/--lat/--group/--layers/--taper build their own Fabric and pass it here)"""
    global CACHE_P24
    T = T or TARGET_DIGITS; g = g or TARGET_NODES; GB = 1e9
    fab = fab if fab is not None else TARGET
    d = DEFAULT15C(cache_fit=False) if arena_room is None else DEFAULT15C(cache_fit=False, arena_room=arena_room)
    wbs = wbs or (TARGET_WRITE_BW, 0.6)
    def runw(**kw):                                                   # the run at each write rate: (wall_nowrite, [wall at wbs])
        out = []
        for w in wbs:
            w0 = fab.write_bw; fab.write_bw = w
            try: r = _run_cache(T, g, d, fab=fab, **kw)
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
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--calib", action="store_true", help="the aac6 calibration against the recorded multi-process walls")
    ap.add_argument("--schedules", action="store_true", help="compare the 576-node level schedules")
    ap.add_argument("--group", type=int, default=64, help="nodes per dragonfly group (MN_TOPO_GROUP)")
    ap.add_argument("--layers", type=int, default=2, choices=(2, 3), help="the layered all-to-all: 2 (APU x node) or 3 (APU x node-in-group x group)")
    ap.add_argument("--taper", type=float, default=1.0, help="global-link bandwidth as a fraction of the group's injection")
    ap.add_argument("--bw", type=float, default=100.0, help="GB/s per APU injection")
    ap.add_argument("--lat", type=float, default=2e-6, help="seconds per message")
    ap.add_argument("--write-bw", type=float, default=TARGET_WRITE_BW, help="GB/s per node for the part file (Phase 15: 0.6, the target's Lustre /ssd0 single-stream, measured there; 2.0 was assumed before)")
    ap.add_argument("--model", default="p15c", choices=("p15c", "p15b", "p15", "p13", "legacy"), help="Phase 15 (2026-09-28): p15c = DEFAULT15C() (main B2 on the launch line: ECALC_NP=auto, RNS_DIST_CACHE_FIT=1, BS_ARENA_ROOM=0.16, DIST_TWREC_G=1); p15b = DEFAULT15B() (B1) (the code's defaults of 2026-09-27 + the target's launch line: ECALC_NP=4 at size > 1, COMM_SHMEM_ROUND_MB=1024); p15 = DEFAULT15() (the Phase 14 defaults, B0; three primes); p13 = the Phase 13/14 model (auto 2^31 both d2 without the Phase 15 terms); legacy = Phase 12")
    ap.add_argument("--ascii", action="store_true", help="p15b: ECALC_OUT_PACKED=0 (the ASCII part file, 1 B/digit)")
    ap.add_argument("--twrec-g", action="store_true", help="Phase 15 G5: DIST_TWREC_G=1 (the general map's recurrence packs, GEN_TWPACK_F) -- the default since 2026-09-28")
    ap.add_argument("--no-twrec-g", action="store_true", help="Phase 15 DOC2: DIST_TWREC_G=0 (B1)")
    ap.add_argument("--cache-slots", type=int, default=None, help="Phase 15 DOC2: price n mn cache slots whatever RNS_DIST_CACHE_FIT allows (default: what FIT allows under p15c -- 0 at the target; the code's 2 otherwise)")
    ap.add_argument("--calib15c", action="store_true", help="Phase 15 DOC2: the model at 1e11 against the fin15e paired series (B1 / B2)")
    ap.add_argument("--cache-partial", action="store_true", help="Phase 15 PC: RNS_DIST_CACHE_PARTIAL=1 at the target -- the primes per phase from the block pool's rooms, the loop, P24 cached or not (results/PC15.md)")
    ap.add_argument("--cache-partial-arena-room", type=float, default=None, help="Phase 15 PC: --cache-partial at this BS_ARENA_ROOM (default the design's 0.16)")
    ap.add_argument("--cache-partial-digits", type=float, default=None, help="Phase 15 PC: --cache-partial at this many total digits (default the target)")
    ap.add_argument("--cache-partial-keep-room", action="store_true", help="Phase 15 PC: --cache-partial with BS_ARENA_ROOM's bytes left out of the division's free pool (conservative)")
    ap.add_argument("--cache-proposals", action="store_true", help="Phase 15 CX: the mn transform cache at the target -- 0 / 1 / 2 slots, the old term, and the ways to hold a slot inside 480 GB (results/CX15.md section 3)")
    ap.add_argument("--calib15b", action="store_true", help="Phase 15 (2026-09-27): the model at 1e11 against RESULTS 86's paired series (B0, the new defaults ASCII and packed)")
    ap.add_argument("--round-mb", type=float, default=1024, help="COMM_SHMEM_ROUND_MB on the target's launch line (D2: 1024; 0 = off, the code's default)")
    ap.add_argument("--out-overlap", default="none", choices=("none", "half"), help="size > 1: none = the part file after T1 (the code); half = hidden under half the division (the model before Phase 15)")
    ap.add_argument("--corrections", type=int, default=0, help="size 1: the division's corrections (data-dependent: 2 at 1e11 on the defaults) -- T1 waits for the writer and the file is rewritten")
    ap.add_argument("--calib15", action="store_true", help="Phase 15: the model against the Phase 14 one-node runs")
    ap.add_argument("--fit15", action="store_true", help="Phase 15: refit CAL15 on the Phase 14 one-node runs and print the check")
    ap.add_argument("--rule", default="model", choices=("model", "full"), help="X1's group choice in the reciprocal (model) or every product on the full group (full)")
    ap.add_argument("--tree", default="grid", choices=("grid", "flat"), help="the top product's form: grid (Phase 12 G: O(share) spills) or flat (the code at 7aded87)")
    ap.add_argument("--groups", default=None, help="MN_GROUPS (e.g. 2,4,8,16,32,64,576); default = the code's schedule")
    ap.add_argument("--staging", default="code", choices=("code", "sym", "resident", "per_exchange", "cached"), help="Phase 14 P2: code (the default: the measured law, mem_model.shmem_pool -- the staged exchanges' largest send + receive x 4 APU threads + the control blocks; results/P214.md), sym (DIST_MN_SYM_SLABS=1: + 3 q per APU of resident slabs); the hypotheses before: the SHMEM transport's staging: resident (Phase 12 S: the slabs in the pool), per_exchange (freed after each wait), cached (the code at 7aded87: kept per communicator)")
    ap.add_argument("--calib13", action="store_true", help="Phase 13d D2: the recalibrated model against the 13c one-node runs")
    ap.add_argument("--plan", type=float, nargs=4, metavar=("G", "FROM", "TO", "STEP"), help="Phase 13d D2: the piece counts in agent L's plan_sweep columns (total digits)")
    ap.add_argument("--e7", action="store_true", help="Phase 14 R1: the reciprocal's cut (NEWTON_RECIP_CUT) per doubling under the pipeline law")
    ap.add_argument("--e9", action="store_true", help="Phase 14 R1: Karatsuba over the device grid and the cut-aware grid, the dm phase's big products")
    ap.add_argument("--e10", action="store_true", help="Phase 14 R1: the 576-node tree-level spill against MN_T_CHUNK_MB=1024")
    ap.add_argument("--D", type=float, nargs="*", default=[4e10, 8e10, 1e11])
    ap.add_argument("--g", type=int, nargs="*", default=[4, 64, 576])
    a = ap.parse_args()
    global TWREC_G, CACHE_FORCE; TWREC_G = not a.no_twrec_g              # Phase 15 G5; DOC2: on by default (the code's default since 2026-09-28)
    CACHE_FORCE = a.cache_slots
    if a.calib15c:
        calib15c(); return
    if a.calib:
        sys.exit(0 if calibrate(a.rule) else 1)
    if a.calib13:
        calib13(); return
    if a.e7: e7_report(); return
    if a.e9: e9_report(share576=7.64e10); return
    if a.e10: e10_report(); return
    if a.fit15:
        fit15(); calib15(); return
    if a.calib15:
        calib15(); return
    if a.calib15b:
        calib15b(); return
    if a.cache_proposals:
        cache_proposals(); return
    if a.cache_partial or a.cache_partial_keep_room:
        cache_partial(T=a.cache_partial_digits, keep_room=a.cache_partial_keep_room, arena_room=a.cache_partial_arena_room); return
    design = {'p15c': lambda: DEFAULT15C(round_mb=a.round_mb, packed=not a.ascii), 'p15b': lambda: DEFAULT15B(round_mb=a.round_mb, packed=not a.ascii), 'p15': lambda: DEFAULT15(round_mb=a.round_mb, out_overlap=a.out_overlap), 'p13': lambda: Design(np=3, strategy='auto', cap=1 << 31, chunk='both', depth=2, modmul=1),
              'legacy': lambda: None}[a.model]()
    if a.plan:
        plan_sweep(int(a.plan[0]), a.plan[1], a.plan[2], a.plan[3], design=design if design is not None else None); return
    fab = Fabric(TARGET.name, a.bw, a.lat, group=a.group, layers=a.layers, taper=a.taper, write_bw=a.write_bw)
    if a.schedules:
        schedules(fab, a.rule, form=a.tree, design=design); return
    print("the target (PLAN 25): %d nodes = 2 304 APUs; %.0f GB/s per APU, %.1f us per message, dragonfly groups of %d nodes, %d-layer all-to-all, global taper %.2f; part files at %.2f GB/s per node; tree form %s; model %s%s"
          % (576, a.bw, a.lat * 1e6, a.group, a.layers, a.taper, a.write_bw, a.tree, a.model, (" (" + design.name() + ", COMM_SHMEM_ROUND_MB=%g)" % a.round_mb) if design is not None else ""))
    print("single node (measured, size 1, the Phase 14 defaults): 1e11 `total` 240.3 s (five runs, the digit file to /tmp; T1 waits 23.5 s for the writer after the division's 2 corrections,"
          " then dc 60.2 s after `total`: ~300 s end to end); without the file 215.9 / 217.1 s (+ dc 5.5 s); 4e10 66.7 s (no file)")
    print("single node (measured, RESULTS 86, the defaults of 2026-09-27): 1e11 wall 203.7 s with the packed file (3 runs, s24-16), 224.7 s with ASCII, 193.6 s without a file (4 runs, s24-26);"
          " B0 on the same nodes 296.9 / 217.6 s (`mn_model.py --calib15b`)")
    for D in a.D:
        for g in a.g:
            run(fab, D, g, a.rule, groups=a.groups, form=a.tree, staging=a.staging, design=design, corrections=a.corrections)
    print("=" * 112)
    print("HEADLINE (modelled; model %s; tree form %s, SHMEM staging %s; two walls: without / with the part file at %.2f GB/s):" % (a.model, a.tree, a.staging, a.write_bw))
    for budget, D, r in headline(fab, 576, a.rule, a.tree, a.groups, a.staging, design):
        if r is None: print("  576 nodes: nothing fits %.0f GB" % budget); continue
        print("  576 nodes, %.0f GB per node: the largest D per node %.2e (node peak %.0f GB) -> %.3e digits: %.1f min without the disk write, %.1f min with it (the part file's exposed %.0f s)"
              % (budget, D, r["mem"]["node"], r["digits"], r["wall_nowrite"] / 60, r["wall"] / 60, r["out"]))
    for Dn in (4e10, 7.7e10):
        r = run(fab, Dn, 576, a.rule, verbose=False, groups=a.groups, form=a.tree, staging=a.staging, design=design)
        print("  576 nodes x %.1e = %.3e digits: %.1f / %.1f min per-node wall without / with the write (exposed communication %.1f s, %s on the NICs per node, node peak %.0f GB%s)" % (Dn, r["digits"], r["wall_nowrite"] / 60, r["wall"] / 60, r["exposed"], fmt_b(r["nic"]), r["mem"]["node"], "" if r["mem"]["node"] <= NODE_GB else " -- does not fit"))
    for g in a.g:
        if g < 8: continue
        rm = run(fab, 4e10, g, "model", verbose=False, form=a.tree, design=design); rf = run(fab, 4e10, g, "full", verbose=False, form=a.tree, design=design)
        print("  X1 at 4e10 x %d: the reciprocal on the model's groups %.1f s vs every product on the full group %.1f s (exposed %.1f vs %.1f, messages per APU %d vs %d)"
              % (g, rm["recip"], rf["recip"], rm["rc"].t_exposed, rf["rc"].t_exposed, rm["rc"].msgs, rf["rc"].msgs))
    if a.layers == 2:
        fab3 = Fabric(TARGET.name, a.bw, a.lat, group=a.group, layers=3, taper=a.taper, write_bw=a.write_bw)
        r2 = run(fab, 4e10, 576, a.rule, verbose=False, form=a.tree, design=design); r3 = run(fab3, 4e10, 576, a.rule, verbose=False, form=a.tree, design=design)
        print("  the third layer at 4e10 x 576 (group %d): wall %.1f -> %.1f s, exposed %.1f -> %.1f s, NIC bytes %s -> %s, messages per APU %d -> %d"
              % (a.group, r2["wall"], r3["wall"], r2["exposed"], r3["exposed"], fmt_b(r2["nic"]), fmt_b(r3["nic"]), r2["msgs"], r3["msgs"]))
    schedules(fab, a.rule, form=a.tree, design=design)


# ============================================================================================================
# Phase 14 R1 (PLAN 34 row R1, results/R114.md): E7 the reciprocal's products cut to the band read, E9 Karatsuba over
# the device grid, E10 the 576-node tree-level spill.  Model terms only: the calibrated path (run / node_phases) is
# untouched; every function here is evaluated on demand (--e7 / --e9 / --e10).  Labels: the pipeline law's constants
# are FITTED (D2), the Karatsuba add rate and the spill's hidden writes are ASSUMED (said where used).
# ============================================================================================================
KARA_ADD_S = GRID_NC                   # ASSUMED: a dbig add/sub pass costs what the grid's accumulation pass over C costs, per 2^31 limbs (the same HBM-bound kernel family)
E10_PEAK_GB = (NODE_GB_MARGIN, NODE_GB)

def _law_piece(st, p, nc, grid, np=3):
    """one piece's seconds under the pipeline law (D2): t_prod x PIPE_ONE (+ GRID_ADD per 2^31 points of the piece + GRID_NC per 2^31 limbs
    of the whole product, when the product is a grid), x NTT_MODMUL=1's factor"""
    tp = t_prod(st, p, np)[0] * PIPE_ONE.get(st, 1.0)
    if grid: tp += (GRID_ADD.get(st, 0.0) * p + GRID_NC * nc) / (1 << 31)
    return tp * F_MM1

def law_product(na, nb, lowcut=0, w=1 << 62, strategy='auto', cap=1 << 31, np=3):
    """(seconds, formed pieces, (ka, kb)) of one single-node product C = A B with the grid's cuts, under the pipeline law"""
    if na <= 0 or nb <= 0 or w <= 0 or lowcut >= na + nb: return 0.0, 0, (0, 0)
    (ka, kb), pcs = product_pieces(na, nb, lowcut, w, strategy, cap, np)
    return sum(_law_piece(st, p, na + nb, ka * kb > 1, np) for st, p in pcs), len(pcs), (ka, kb)

def recip_chain(D):
    """the anchored chain at D digits (the code's exact sizes): [(j, jn, take)], k_mu, nq"""
    ex = exact_sizes(D); nq = ex['nq']; k = ex['pn'] + 1 + ex['dl'] - nq + 1
    j = 2; out = []
    while j < k:
        jn = k
        while (jn + 1) // 2 > j: jn = (jn + 1) // 2
        out.append((j, jn, min(2 * j + 2, nq))); j = jn
    return out, k, nq

def dm_phase(D, design=None):
    """the calibrated model's reciprocal + division seconds at D digits on one node (run() under CAL13)"""
    r = run(TARGET, D, 1, verbose=False, design=design or DEFAULT13())
    return r['recip'], r['div']

def e7_report(Ds=(4e10, 7.64e10, 1e11, 1.3e11), guard=1, verbose=True):
    """E7: the reciprocal's two products per doubling with and without the low cut newton_db.c takes under NEWTON_RECIP_CUT (band - guard),
    under the pipeline law; the saving against the calibrated dm phase.  Returns {D: (saved_s, recip_s, div_s)}"""
    out = {}
    for D in Ds:
        chain, k, nq = recip_chain(D); t0 = t1 = 0.0; rows = []
        for j, jn, take in chain:
            for name, na, nb, v in (('Q_t r', take, j + 1, take - j), ('r d', j + 1, j + 2, j)):
                c = v - guard if v > guard else 0
                a, fa, g0 = law_product(na, nb); b, fb, g1 = law_product(na, nb, c)
                t0 += a; t1 += b
                if g0[0] * g0[1] > 1: rows.append((j, jn, name, na, nb, c, g0, fa, fb, a, b))
        rs, ds = dm_phase(D); out[D] = (t0 - t1, rs, ds)
        if verbose:
            print("E7 at %.3g digits: k_mu %d, nq %d; the grid doublings (pipeline law, modelled):" % (D, k, nq))
            for j, jn, name, na, nb, c, g, fa, fb, a, b in rows:
                print("   j %d -> %d %-6s %d x %d limbs, grid %d x %d, cut %d: %d -> %d pieces, %.2f -> %.2f s" % (j, jn, name, na, nb, g[0], g[1], c, fa, fb, a, b))
            print("   reciprocal's grid products %.2f -> %.2f s: saving %.2f s = %.1f %% of the modelled reciprocal (%.1f s), %.1f %% of dm (%.1f s)"
                  % (t0, t1, t0 - t1, 100 * (t0 - t1) / rs if rs else 0, rs, 100 * (t0 - t1) / (rs + ds) if rs + ds else 0, rs + ds))
    return out

# ---- E9: a 2 x 2 Karatsuba layer on top of mul_grid ---------------------------------------------------------------------------
def _split_cut_aware(na, nb, lowcut, w, cap, r3, logmax, np):
    """the grid (ka, kb) that minimises the cost of the FORMED pieces under the cuts (rns_dist.c split_grid weighs every piece,
    cut or not): the same weights as _split_auto (B length x 0.70, 3 2^k x 1.05), the fewest pieces on a 0.1 % tie"""
    best = None
    for i in range(1, 33):
        for j in range(1, 33):
            pa, pb = -(-na // i), -(-nb // j)
            if pa + pb > cap: continue
            if _b_fits(pa + pb, logmax, r3, np): n, t3 = _b_len(pa + pb); wgt = n * (1.05 if t3 else 1.0) * 0.70
            else: pts = _plane_pts_cap(pa + pb, r3, logmax); wgt = pts * (1.05 if pts & (pts - 1) else 1.0)
            formed = 0
            for jb in range(j):
                for ia in range(i):
                    oa, ob = ia * pa, jb * pb
                    if oa >= na or ob >= nb: continue
                    la, lb = min(pa, na - oa), min(pb, nb - ob)
                    if oa + ob >= w or oa + ob + la + lb <= lowcut: continue
                    formed += 1
            cost = formed * wgt
            if best is None or cost < best[0] * 0.999 or (cost <= best[0] * 1.001 and i * j < best[1] * best[2]): best = (cost, i, j)
    return best[1], best[2]

def law_product_cutaware(na, nb, lowcut=0, w=1 << 62, strategy='auto', cap=1 << 31, np=3):
    """law_product with the grid chosen knowing the cuts (a candidate code change in split_grid: E9's cheap alternative)"""
    if na <= 0 or nb <= 0 or w <= 0 or lowcut >= na + nb: return 0.0, 0, (0, 0)
    pl, r3 = mem_model.cap_pool(cap); logmax = pl
    if na + nb <= cap and (na + nb <= (1 << logmax) and (strategy != 'auto' or _b_fits(na + nb, pl, r3, np))): return law_product(na, nb, lowcut, w, strategy, cap, np)
    ka, kb = _split_cut_aware(na, nb, lowcut, w, cap, r3, logmax, np)
    pa, pb = -(-na // ka), -(-nb // kb); t = 0.0; n = 0
    for jb in range(kb):
        for ia in range(ka):
            oa, ob = ia * pa, jb * pb
            if oa >= na or ob >= nb: continue
            la, lb = min(pa, na - oa), min(pb, nb - ob)
            if oa + ob >= w or oa + ob + la + lb <= lowcut: continue
            n_ = la + lb
            if strategy == 'auto' and _b_fits(n_, pl, r3, np): st = AUTO_FORM; p = _b_len(n_)[0]
            else: st = 'C' if strategy == 'auto' else strategy; p = _plane_pts_cap(n_, r3, logmax)
            t += _law_piece(st, p, na + nb, ka * kb > 1, np); n += 1
    return t, n, (ka, kb)

def kara_product(na, nb, lowcut=0, w=1 << 62, depth=1, min_pieces=4, strategy='auto', cap=1 << 31, np=3, cutaware=False):
    """C = A B with the grid's cuts, with up to `depth` 2 x 2 Karatsuba layers above mul_grid: (seconds, pieces, add_limbs, extra_limbs).
    A layer splits both operands at h = ceil(max(na, nb) / 2) (z0 = A0 B0, z1 = (A0 + A1)(B0 + B1), z2 = A1 B1; unequal operands:
    the longer one in chunks of the shorter's length, each chunk x the shorter as its own product); each half product carries the
    band the cuts need of it (z0 on [lowcut - h, w), z1 on [lowcut - h, w - h), z2 on [lowcut - 2h, w - h): the correction z1 - z0 - z2
    needs the low parts even where the plain grid skips them -- for a half-cut product a layer saves nothing, see R114.md 3);
    the layer's adds (two half-sums, two subtractions, two shifted adds) at KARA_ADD_S per 2^31 limbs (ASSUMED); the extra memory =
    z1's buffer + the two half-sums (the plain grid needs C and one piece temporary).  A product that fits one plane, or whose plain grid
    forms fewer than min_pieces pieces, is the plain grid."""
    lp = (law_product_cutaware if cutaware else law_product)(na, nb, lowcut, w, strategy, cap, np)
    if depth <= 0 or lp[1] < min_pieces or na + nb <= cap: return lp[0], lp[1], 0, 0
    if na > 1.5 * nb or nb > 1.5 * na:                                 # unequal: chunks of the shorter's length
        if nb > na: na, nb = nb, na
        t = 0.0; n = 0; adds = 0; extra = 0; off = 0
        while off < na:
            la = min(nb, na - off)
            lo2 = max(0, lowcut - off); w2 = w - off
            if w2 > 0 and lo2 < la + nb:
                a, b, c, d = kara_product(la, nb, lo2, w2, depth, min_pieces, strategy, cap, np, cutaware)
                t += a; n += b; adds += c + (la + nb if off else 0); extra = max(extra, d)
            off += nb
        return t, n, adds, extra
    h = -(-max(na, nb) // 2)
    lo1 = max(0, lowcut - h); lo2 = max(0, lowcut - 2 * h); w1 = w - h
    z0 = kara_product(h, h, lo1, w, depth - 1, min_pieces, strategy, cap, np, cutaware)
    z1 = kara_product(h + 1, h + 1, lo1, w1, depth - 1, min_pieces, strategy, cap, np, cutaware)
    z2 = kara_product(na - h, nb - h, lo2, w1, depth - 1, min_pieces, strategy, cap, np, cutaware)
    layer_adds = 2 * (h + 1) + 2 * (2 * h + 2) + 2 * (2 * h + 2)
    adds = layer_adds + z0[2] + z1[2] + z2[2]
    extra = (2 * h + 2) + 2 * (h + 1) + max(z0[3], z1[3], z2[3])
    t = z0[0] + z1[0] + z2[0] + KARA_ADD_S * F_MM1 * layer_adds / (1 << 31)
    return t, z0[1] + z1[1] + z2[1], adds, extra

def e9_report(Ds=(4e10, 7.64e10, 1e11, 1.3e11), verbose=True, share576=None):
    """E9: the dm phase's big products (the reciprocal's top doublings, A_h mu, X Q) plain / cut-aware grid / Karatsuba 1 and 2 layers;
    the saving against the calibrated dm phase and the extra memory.  share576: also the 576-node share (its size-1 shapes: the mn tier
    is not modelled here -- its pieces are C-form over 576 nodes and its grids follow mn_grid, see R114.md 3)"""
    res = {}
    for D in list(Ds) + ([share576] if share576 else []):
        chain, k, nq = recip_chain(D); ex = exact_sizes(D); dl, sn = ex['dl'], ex['sn']; kd = sn + dl - nq + 1
        prods = []
        for j, jn, take in chain[-3:]:
            prods.append(('recip Q_t r j %d' % j, take, j + 1, 0, 1 << 62))
            prods.append(('recip r d   j %d' % j, j + 1, j + 2, 0, 1 << 62))
        prods.append(('div A_h mu (low cut)', sn - (nq - 1 - dl), kd + 1, kd + 1, 1 << 62))
        prods.append(('div X Q mod B^w', dl + 1, nq, 0, nq + 2))
        tot = dict(plain=0.0, cutaware=0.0, kara1=0.0, kara2=0.0, kara1c=0.0); extra = 0; rows = []
        for name, na, nb, lo, w in prods:
            p = law_product(na, nb, lo, w); c = law_product_cutaware(na, nb, lo, w)
            k1 = kara_product(na, nb, lo, w, 1); k2 = kara_product(na, nb, lo, w, 2); k1c = kara_product(na, nb, lo, w, 1, cutaware=True)
            tot['plain'] += p[0]; tot['cutaware'] += c[0]; tot['kara1'] += k1[0]; tot['kara2'] += k2[0]; tot['kara1c'] += k1c[0]
            extra = max(extra, k1[3]); rows.append((name, na, nb, p, c, k1, k2, k1c))
        rs, ds = dm_phase(D); res[D] = dict(tot=tot, extra_gb=extra * 8 / 1e9, recip=rs, div=ds)
        if verbose:
            print("E9 at %.4g digits (nq %d): dm = reciprocal %.1f + division %.1f s (calibrated model); the big products under the pipeline law:" % (D, nq, rs, ds))
            print("   %-22s %11s %11s | %7s %-9s | %7s %-9s | %7s %-5s | %7s %-5s | %7s" % ('product', 'na', 'nb', 'plain', 'grid pcs', 'cut-aw', 'grid pcs', 'kara1', 'pcs', 'kara2', 'pcs', 'k1+cut'))
            for name, na, nb, p, c, k1, k2, k1c in rows:
                print("   %-22s %11d %11d | %6.1fs %2dx%-2d %3d | %6.1fs %2dx%-2d %3d | %6.1fs %4d  | %6.1fs %4d  | %6.1fs" % (name, na, nb, p[0], p[2][0], p[2][1], p[1], c[0], c[2][0], c[2][1], c[1], k1[0], k1[1], k2[0], k2[1], k1c[0]))
            t = tot; dm = rs + ds
            print("   totals: plain %.1f | cut-aware grid %.1f (%+.1f %% of dm) | Karatsuba 1 layer %.1f (%+.1f %%), 2 layers %.1f (%+.1f %%), 1 layer + cut-aware %.1f (%+.1f %%); extra memory of a layer %.1f GB (z1 + the half-sums)"
                  % (t['plain'], t['cutaware'], 100 * (t['cutaware'] - t['plain']) / dm, t['kara1'], 100 * (t['kara1'] - t['plain']) / dm, t['kara2'], 100 * (t['kara2'] - t['plain']) / dm, t['kara1c'], 100 * (t['kara1c'] - t['plain']) / dm, extra * 8 / 1e9))
    return res

# ---- E10: the 576-node tree-level spill (apumult M56) against MN_T_CHUNK_MB=1024 -------------------------------------------------
def tree_need_e10(nq_leaf, g, opts, variant='code', where='top'):
    """mem_model.tree_need_dev's level loop with the top level's live set under a variant (per device, bytes; the 1/16 as there):
    'code'  : both pairs of the child, the running pair (k-way), the new pair, all live across the level's two products (mn.c tree_level_k);
    'free'  : E10a (no disk) -- P_i and P_run are dead after MUL1 (P_n = P_i Q_run + P_run) and freed before MUL2 (Q_n = Q_i Q_run);
    'spill' : E10b (M56) -- 'free' + Q_i spilled to disk under MUL1 (read back before MUL2) and P_n spilled under MUL2 (read back after);
    the peak is max(MUL1, MUL2) of the shares live + the product's scratch.  where: 'top' (the top level only) or 'all'.
    Returns (need_bytes, level_bytes_by_size, cold_read_bytes at the top level)"""
    qb = mem_model.quarter_bytes; best = 0; levels = {}; cold = 0; lv = []
    for S, ch in mem_model.level_children(g, opts.get('groups')):
        gg = S; nch = len(ch); P = ch[0]
        if nch < 2: continue
        nqc = nq_leaf * P + 8; na = nqc; nb = nqc * (nch - 1); nc = na + nb
        share_child = nq_leaf + 8; share_run = -(-nb // gg); share_new = -(-nc // gg)
        scratch, pieces = mem_model.mn_scratch(na, nb, 1, gg, share_child, share_run, share_new, opts.get('pool_log', 31), opts.get('logr_delta', 0), opts.get('t_chunk_mb', 0))
        c, r, n = qb(share_child + share_child // 8), qb(share_run + share_run // 8), qb(share_new + share_new // 8)
        lv.append((S, nch, c, r, n, scratch))
    top_code = max(2 * c + (2 * r if nch > 2 else 0) + 2 * n + scratch for S, nch, c, r, n, scratch in lv)
    for S, nch, c, r, n, scratch in lv:
        code = 2 * c + (2 * r if nch > 2 else 0) + 2 * n
        v = variant if (S == g or (where == 'all' and code + scratch >= 0.9 * top_code)) else 'code'   # 'all': every level as heavy as the top (the 3-way ones at 576)
        if v == 'code': live = code
        else:
            rr = r if nch > 2 else 0
            mul1 = 2 * c + 2 * rr + n if v == 'free' else c + 2 * rr + n      # P_i (Q_i spilled), P_run, Q_run, P_n
            mul2 = c + rr + 2 * n if v == 'free' else c + rr + n              # Q_i, Q_run, Q_n (P_n spilled)
            live = max(mul1, mul2)
            if v == 'spill': cold += c + n                                     # read back per spilled level: Q_i before MUL2, P_n after
        levels[S] = (live + scratch) * 17 // 16
        best = max(best, live + scratch)
    return best + best // 16, levels, cold

def node_peak_e10(D, g, opts, variant='code', dm_factor=1.0, where='top'):
    """mem_model.mem_per_node with the tree need replaced by tree_need_e10's and the dm need scaled by dm_factor (V2 / V3 of F1 6:
    ASSUMED as the ratio 270 / 309, 243 / 309 at the target share): the node peak (bytes), the parts, the cold bytes read back"""
    r = mem_model.mem_per_node(int(D), g, opts)
    nq_leaf = (r['nq'] + g - 1) // g
    tree1, levels, cold = tree_need_e10(nq_leaf, g, opts, variant, where)
    dm = r['dm_need'] * dm_factor
    arena0 = max(r['dm_need'], r['tree_need']); arena1 = max(dm, mem_model.NR * tree1)
    peak = r['node_peak'] - (arena0 - arena1)
    return dict(peak=peak, arena=r['arena'] - (arena0 - arena1), dm_need=dm, tree_need=mem_model.NR * tree1, tree0=r['tree_need'], cold=mem_model.NR * cold, levels=levels, planes=r['planes'], exchange=r['exchange'], host=r['host_hwm'])

def ceiling_e10(g, budget_bytes, opts, variant='code', dm_factor=1.0, lo=2e10, hi=2e11, where='top'):
    """the largest D per node (to 10^8) whose node_peak_e10 fits the budget"""
    lo, hi = int(lo), int(hi)
    if node_peak_e10(hi, g, opts, variant, dm_factor, where)['peak'] <= budget_bytes: return hi
    if node_peak_e10(lo, g, opts, variant, dm_factor, where)['peak'] > budget_bytes: return 0
    while hi - lo > 1e8:
        m = (lo + hi) // 2
        if node_peak_e10(m, g, opts, variant, dm_factor, where)['peak'] <= budget_bytes: lo = m
        else: hi = m
    return lo // 10**8 * 10**8

def e10_report(g=576, bws=(5.0, 10.0), verbose=True):
    """E10 at 576: per variant (the code / E10a free / E10b spill) x (dm as built V0 / V2 / V3 of F1) x (MN_T_CHUNK_MB 0 / 1024): the
    per-node ceiling at 480 and 502 GB, the total digits (the top node's share = 1.036 x the average: top_factor), the wall at the
    ceiling (mn_model.run + the spill's exposed read-back at bw GB/s; the writes hidden under the level's products if bw >= bytes /
    product time, else the excess exposed -- ASSUMED), and the elasticity against the built default (% time per % digits)"""
    dz0 = DEFAULT13(); dz1 = Design(np=3, strategy='auto', cap=1 << 31, chunk='both', depth=2, modmul=1)
    base = None; rows = []
    for chunk_name, dz in (('T off', dz0), ('T 1024', dz1)):
        for dmn, dmf in (('V0', 1.0), ('V2', 270.0 / 309.0), ('V3', 243.0 / 309.0)):
            for variant, where in (('code', 'top'), ('free', 'all'), ('spill', 'top'), ('spill', 'all')):
                opts = dict(dz.mem_opts(4.25e13), transport='shmem', staging='resident')
                for budget in E10_PEAK_GB:
                    Dn = ceiling_e10(g, budget * 1e9, opts, variant, dmf, where=where)
                    if not Dn: continue
                    m = node_peak_e10(Dn, g, opts, variant, dmf, where)
                    tf = top_factor(Dn * g, g); Dtot = Dn * g / tf
                    r = run(TARGET_W2, Dn / tf, g, verbose=False, design=dz)
                    top = [c for k_, s_, c in r['levels_rows'] if s_ == g]; t_top = top[0].t if top else 0.0
                    io = {}
                    for bw in bws:
                        rd = m['cold'] / 1e9 / bw
                        wr = max(0.0, m['cold'] / 1e9 / bw - t_top / 2)
                        io[bw] = rd + wr
                    row = dict(chunk=chunk_name, dm=dmn, variant=variant + ('' if where == 'top' or variant == 'code' else '*'), budget=budget, Dn=Dn, Dtot=Dtot, peak=m['peak'] / 1e9, arena=m['arena'] / 1e9, dm_need=m['dm_need'] / 1e9,
                               tree=m['tree_need'] / 1e9, tree0=m['tree0'] / 1e9, cold=m['cold'] / 1e9, wall=r['wall'], io=io, t_top=t_top)
                    rows.append(row)
                    if base is None and chunk_name == 'T off' and dmn == 'V0' and variant == 'code' and budget == NODE_GB_MARGIN: base = row
    if verbose:
        print("E10 at %d nodes (modelled: mem_model's terms with the tree's top level under the variant; the wall from mn_model.run at the ceiling; the spill's I/O per node at %s GB/s):" % (g, '/'.join('%g' % b for b in bws)))
        print("   base = the built default at 480 GB: D/node %.3e, %.3e digits, wall %.1f s" % (base['Dn'], base['Dtot'], base['wall']))
        print("   (tree: code = mn.c as built; free = E10a, P_i and P_run freed after the level's first product, every heavy level; spill = E10b (M56), + Q_i and P_n spilled, the top level only, spill* = every heavy level)")
        print("   %-7s %-3s %-6s %4s | %9s %9s | %6s %6s %6s %6s %5s | %7s %s | %s" % ('chunk', 'dm', 'tree', 'GB', 'D/node', 'digits', 'peak', 'arena', 'dm', 'tree', 'cold', 'wall', ' '.join('+io@%g' % b for b in bws), 'elasticity (% time / % digits) at each bw'))
        for row in rows:
            el = []
            for bw in bws:
                w1 = row['wall'] + row['io'][bw]; dd = row['Dtot'] / base['Dtot'] - 1
                el.append('%.2f' % ((w1 / base['wall'] - 1) / dd) if abs(dd) > 1e-3 else '  - ')
            print("   %-7s %-3s %-6s %4.0f | %9.3e %9.3e | %6.0f %6.0f %6.0f %6.0f %5.0f | %7.1f %s | %s" % (row['chunk'], row['dm'], row['variant'], row['budget'], row['Dn'], row['Dtot'], row['peak'], row['arena'], row['dm_need'], row['tree'], row['cold'], row['wall'],
                  ' '.join('%6.1f' % row['io'][b] for b in bws), ' '.join(el)))
    return rows

cal13_reset()                          # Phase 13d D2: PIECE13 from the recalibration

if __name__ == "__main__":
    main()

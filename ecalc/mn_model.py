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

Everything printed as "modelled" is this script's arithmetic; "measured" cites the RESULTS section.
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
CACHE_MN_SLOTS = 2              # RNS_DIST_CACHE_MN default over shares (results/G.md)
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

TARGET = Fabric("Slingshot-2 dragonfly (PLAN 25)", bw_apu=100.0, lat=2e-6, group=64, layers=2, taper=1.0)

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

class Cost:
    def __init__(self): self.t = 0.0; self.t_exposed = 0.0; self.nic = 0.0; self.glob = 0.0; self.msgs = 0; self.pieces = 0; self.xfers = 0
    def add(self, o):
        self.t += o.t; self.t_exposed += o.t_exposed; self.nic += o.nic; self.glob += o.glob; self.msgs += o.msgs; self.pieces += o.pieces; self.xfers += o.xfers
        return self

def piece_cost(fab, pts, g, na, nb, nc, fwd=2, with_x=False, form="grid"):
    """one piece product of pts plane points over a group of g nodes (every node transforms: L's balanced map): the
    local passes on 4 g ranks, the fabric exchanges (12 layered all-to-alls per piece: 3 per prime, fewer with cache
    hits), the operand redistributions and the result exchange, the spill all-gather (form 'flat': every rank's 4 C
    limbs from every node -- g x C x 32 B per APU; 'grid': G's exact spills, O(C)) and the carry scan."""
    c = Cost(); c.pieces = 1
    q = pts / (4 * g)                                   # points per APU
    scale = q / (1 << 29)
    # the local part: the measured piece (its xGMI exchanges included, 84 % hidden), on shared APUs times the share
    dz = DZ
    if dz is None or dz.legacy: t31 = T_PIECE_31 if fwd == 2 else T_PIECE_31_BHIT
    else: t31 = T_PIECE_31_NP[dz.np] * (1.0 if fwd == 2 else T_PIECE_31_BHIT / T_PIECE_31) * dz.f_mm()   # Phase 13b D: S13's C at 2^31 (P = 3 / 4)
    t_loc = t31 * scale + 0.005
    t_loc *= fab.gpu_share
    # the transforms' exchanges: EC_NP x (fwd + 1) layered all-to-alls of 8 q bytes per APU; the xGMI stage of one
    # runs under the fabric stage of the other (inflight 2 on the equal path; 1 on the general map: GEN_HIDE), so the
    # fabric's excess over the hidden xGMI stage is exposed
    t_x = T_XGMI_31 * scale
    if dz is None or dz.legacy or not fab.target:
        hide = (1.0 if fab.target else HIDDEN_XGMI) * (1.0 if is_pow2(g) else GEN_HIDE)
    else:                                                             # Phase 13b D: X13's measured overlap -- the equal path hides
        hide = HIDE_POW2 if is_pow2(g) else GEN_HIDE_DEPTH[min(dz.depth, 2)]   # 3/4 of its xGMI time, the general map 1.1 % (two deep: modelled 3/4)
    n_tr = (EC_NP if dz is None or dz.legacy else dz.np) * (fwd + 1)
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

_PC = {}
def product_cost(fab, na, nb, g, lowcut=0, highcut=None, with_x=False, cache=True, form="grid"):
    """memoised _product_cost (Phase 13b D: the design table evaluates the same products for many rows); the key is the fabric's
    parameters, the arguments and what of the design the product depends on"""
    dz = DZ
    dk = None if dz is None else (dz.legacy, dz.np, dz.modmul, dz.pool_log(), dz.t_mb, dz.depth, T_ROUND, HIDE_POW2, GEN_HIDE_DEPTH[1], GEN_HIDE_DEPTH[2], T_PIECE_31_NP[dz.np], F_MM1)
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
    nc = na + nb
    c = Cost()
    if nc <= cap:
        pts = plane_pts(nc, g)
        return c.add(piece_cost(fab, pts, g, na, nb, nc, 2, with_x, form))
    ka, kb, pts = split_grid(na, nb, cap, g)
    pa, pb = -(-na // ka), -(-nb // kb)
    first = True
    for j in range(kb):
        for i in range(ka):
            oa, ob = i * pa, j * pb
            la, lb = min(pa, na - oa), min(pb, nb - ob)
            if la <= 0 or lb <= 0: continue
            if (highcut is not None and oa + ob >= highcut) or (lowcut and oa + ob + la + lb <= lowcut): continue
            # the transform cache over shares (2 slots): B's piece j held across i, A's piece 0 held across j
            fwd = 2
            if cache and CACHE_MN_SLOTS >= 2:
                if i > 0 and not first: fwd = 1                       # B piece j is in its slot: A only
                if i == 0 and j > 0: fwd = 1                          # A piece 0 is in its slot: B only
                if i > 0 and j > 0: fwd = 1
            c.add(piece_cost(fab, pts, g, la, lb, la + lb, fwd, False, form))
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
    j = kp; cur = None
    while j < k:
        jn = k
        while (jn + 1) // 2 > j: jn = (jn + 1) // 2
        take = min(2 * j + 2, nq)
        gp = choose_group(fab, take + j + 1, g, rule, form)
        if gp != cur:                                                  # r re-sharded onto the step's group
            c.add(shift_cost(fab, j + 1, max(gp, cur or 1))); cur = gp
        groups.append((j, gp))
        if take < nq: c.add(shift_cost(fab, take, g))                  # Q_t out of Q (Q is over the full group)
        c.add(product_cost(fab, take, j + 1, gp, form=form))           # Q_t r
        c.add(shift_cost(fab, take + j + 1, gp))                       # u
        c.add(small_cost(fab, gp, 4))                                  # limb, nonzero_below, pow, |B^2j - u|
        c.add(product_cost(fab, j + 1, j + 2, gp, form=form))          # r d
        c.add(shift_cost(fab, 2 * j + 3, gp))                          # corr
        c.add(shift_cost(fab, 2 * j + 1, gp))                          # r << j
        c.add(small_cost(fab, gp, 2))                                  # cmp, add
        if jn < 2 * j: c.add(shift_cost(fab, 2 * j + 1, gp))
        j = jn
    if cur != g: c.add(shift_cost(fab, k + 1, g))                      # mu onto the full group
    return c, groups

def division_cost(fab, nq, dl, npn, g, rule, form):
    """S = P + Q, A_h = S >> (nq - 1 - dl), t = A_h mu (low cut k + 1), X = t >> (k + 1), X Q mod B^w (high cut w),
    the window, the corrections, the residues; the reciprocal first"""
    na = npn + dl; k = na - nq + 1; w = nq + 2
    rc, groups = recip_cost(fab, nq, k, g, rule, form)
    c = Cost()
    c.add(shift_cost(fab, nq, g)); c.add(small_cost(fab, g, 3))        # Q into P's basis, S = P + Q, residues of P, Q
    c.add(shift_cost(fab, na, g))                                      # A_h
    nah = 2 * dl + 1
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

def tree_cost(fab, nq_node, g, groups=None, form="grid"):
    """level by level (mem_model.level_children: the level's group of S nodes and its children m_1 .. m_k, the
    previous level's groups): a k-way level is the fold the tree runs (results/L.md; tree_level for k = 2):
    (P, Q) <- (P Q_i + P_i, Q Q_i) for i = 2 .. k -- 2 (k - 1) products over the level's S nodes, the accumulated
    operand growing from m_1 to S - m_k leaf shares, the P product with the shifted add (X)."""
    rows = []
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
HIDE_POW2 = 0.75                       # MEASURED (results/X13.md 3.2): the equal-slab path hides 74-76 % of its xGMI link time
GEN_HIDE_DEPTH = {1: 0.011, 2: 0.75}   # general map: MEASURED 1.1 % one deep (X13); two deep MODELLED = the equal path's 3/4
F_MM1 = 58.0 / 58.4                    # MEASURED (results/K13.md, one pair at 4e10): NTT_MODMUL=1 phases 58.4 -> 58.0 s
MAP_RATE = 0.065                       # MEASURED (results/I.md t_alloc 0.057-0.072 s/GB; P3: 25.8 GB fewer planes = -1.5..-2.9 s of init)
T_ROUND = 0.030                        # FITTED on aac6 loopback (M13, 64 MB chunks: 1e10/4 shift +5.4 s over 70 rounds, both +12.9 s over 123, 1e10/2 both +2.2 over 235; least squares, +-100 %):
                                       # the fixed cost of one extra exchange round (launches, the node scan, the sync); ASSUMED on the target
CHUNK_MB = 1024                        # the chunk the table uses for both switches (M13's recommendation for the target)
STRATEGIES = ('C', 'B', 'B4', 'auto')
CHUNKS = ('off', 'shift', 'both')

class Design:
    """one row of the design space.  np: ECALC_NP; strategy: RNS_STRATEGY (C | B | B4 | auto); cap: the plane cap in points
    (None = the code's own rule: 3 2^30 below 5e10 digits of the run, else 2^31); chunk: 'off' | 'shift'
    (MDB_SHIFT_CHUNK_MB) | 'both' (+ MN_T_CHUNK_MB), at chunk_mb; depth: the uneven (alltoallv) exchange's depth 1 | 2;
    modmul: NTT_MODMUL (1 = the default since step 0); legacy: the pre-13b constants (four primes, Phase 10/11 phases)"""
    def __init__(self, np=3, strategy='C', cap=None, chunk='off', depth=1, modmul=1, chunk_mb=CHUNK_MB, legacy=False):
        self.np, self.strategy, self.cap, self.chunk, self.depth, self.modmul, self.chunk_mb, self.legacy = np, strategy, cap, chunk, depth, modmul, chunk_mb, legacy
        self.shift_mb = chunk_mb if chunk in ('shift', 'both') else 0
        self.t_mb = chunk_mb if chunk == 'both' else 0
    def f_mm(self): return F_MM1 if self.modmul == 1 else 1.0
    def cap_at(self, digits): return self.cap if self.cap is not None else mem_model.code_cap(digits)
    def pool_log(self, digits=1e12): return mem_model.cap_pool(self.cap_at(digits))[0]
    def mem_opts(self, digits):
        return dict(np=self.np, strategy=self.strategy, cap=self.cap_at(digits), shift_chunk_mb=self.shift_mb, t_chunk_mb=self.t_mb, depth=self.depth)
    def key(self): return (self.np, self.strategy, self.cap, self.chunk, self.depth, self.modmul, self.chunk_mb, self.legacy)
    def name(self):
        return '%s %s %s d%d' % (self.strategy, mem_model.cap_name(self.cap) if self.cap else 'rule', self.chunk, self.depth)
    def env(self):
        """the environment that selects this row: RNS_STRATEGY (agent B), ECALC_PLANE_CAP (agent P: sets POOL_LOG,
        RNS_PLANES_3Q30 and DIST_LOGN_TEST), MDB_SHIFT_CHUNK_MB / MN_T_CHUNK_MB (Phase 13a M), COMM_ALLTOALLV_DEPTH (agent X)"""
        e = dict(ECALC_NP=self.np, NTT_MODMUL=self.modmul, RNS_STRATEGY=self.strategy)
        if self.cap is not None: e['ECALC_PLANE_CAP'] = mem_model.cap_name(self.cap)
        if self.shift_mb: e['MDB_SHIFT_CHUNK_MB'] = int(self.shift_mb)
        if self.t_mb: e['MN_T_CHUNK_MB'] = int(self.t_mb)
        e['COMM_ALLTOALLV_DEPTH'] = self.depth
        return e

DEFAULT = Design()                     # the code after step 0: three primes, C, the code's cap rule, no chunking, depth 1, NTT_MODMUL=1
DZ = None                              # the design of the run in progress (run() sets it; the cost functions read it)

def round_cost(fab, g):
    """one extra exchange round (a chunked mdb_shift / window / mdb_add_shifted): a small all-to-all's latency over g
    nodes + a collective + the round's fixed cost"""
    return fab.a2a(1.0, g, 1)[0] + fab.coll(g) + T_ROUND

# ---- the per-product law: S13's E0 medians (t_strategy), P = 4 and 3 ---------------------------------------------------
E0_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'results', 'S13_e0.txt')
_E0 = None
def e0_table(extra=None):
    """{(P, strategy, logn): (wall, load, ntt, crt, merge)} from results/S13_e0.txt (+ `extra`, e.g. agent B's t_strategy log with B4
    lines); the last line of a size wins (as t_cap).  Strategies: A, B, C, b (B with 128-bit loads), B4 when a log has it"""
    global _E0
    if _E0 is not None and extra is None: return _E0
    import re
    t = {}; lib = set()
    NAMES = {'4': 'B4', 'LC': 'C', 'LB': 'B', 'LB4': 'B4'}         # agent B's t_strategy: strat=4 is B4, strat=L<form> the library's forms
    for fn in [E0_FILE] + ([extra] if extra else []):
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

def big_shapes(D, scope=('top', 'recip', 'div')):
    """t_cap's shapes at D digits (nq = D / 18 limbs): (phase, name, na, nb, lowcut, w, count)"""
    nq = int(D / 18); k = nq + 1; big = 1 << 62; out = []
    if 'top' in scope:
        out += [('top', 'tree top', nq // 2, nq // 2, 0, big, 2), ('top', 'tree top-1', nq // 4, nq // 4, 0, big, 4)]
    if 'recip' in scope:
        for j in ((k + 1) // 2, (k + 3) // 4, (k + 7) // 8):
            take = min(2 * j + 2, nq)
            out += [('recip', 'Q_t r', take, j + 1, 0, big, 1), ('recip', 'r d', j + 1, j + 2, 0, big, 1)]
    if 'div' in scope:
        out += [('div', 'A_h mu', 2 * nq + 1, nq + 2, nq + 2, big, 1), ('div', 'X Q', nq + 1, nq, 0, nq + 2, 1)]
    return out

@functools.lru_cache(maxsize=None)
def big_products(D, strategy, cap, np, scope=('top', 'recip', 'div')):
    """the time of the big products at D digits under (strategy, cap, np), per phase: {'top': s, 'recip': s, 'div': s, 'label': ...}.
    'auto' (rns_dist.c): the B form -- RNS_STRATEGY_FORM, default B4 at P = 3, B at P = 4 -- for a piece whose planes fit the
    pools as sized at init (the C pools of the cap: (np + 3) cap 2 bytes per APU), else C; auto never allocates"""
    pl, r3 = mem_model.cap_pool(cap); logmax = pl
    budget = mem_model.plane_bytes_apu('C', cap, np)
    out = {'top': 0.0, 'recip': 0.0, 'div': 0.0}; labels = set()
    for ph, name, na, nb, lowcut, w, count in big_shapes(D, scope):
        nc = na + nb; one = nc <= cap
        if one and r3 and nc > (1 << logmax):
            ka, kb = _split_cap(na, nb, cap, True, logmax); one = ka * kb == 1
        if one: ka = kb = 1
        else: ka, kb = _split_cap(na, nb, cap, r3, logmax)
        pa, pb = -(-na // ka), -(-nb // kb); t = 0.0
        for jb in range(kb):
            for ia in range(ka):
                oa, ob = ia * pa, jb * pb
                if oa >= na or ob >= nb: continue
                la, lb = min(pa, na - oa), min(pb, nb - ob)
                if oa + ob >= w or oa + ob + la + lb <= lowcut: continue
                p = _plane_pts_cap(nc if one else la + lb, r3, logmax)
                st = strategy
                if strategy == 'auto':
                    form = 'B4' if np == 3 else 'B'
                    st = form if mem_model.plane_bytes_apu(form, p, np) <= budget else 'C'
                tp, lab = t_prod(st, p, np); t += tp; labels.add(lab)
        out[ph] += t * count
    out['label'] = 'modelled' if 'modelled' in labels else 'measured'
    return out

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
    return mem_model.mem_per_node(int(D), 1, dict(np=np, cap=cap))['dev_init'] / 1e9

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
    if ya <= 0 or yb <= 0: return max(0.0, ya + (yb - ya) * (D - a) / (b - a))
    return ya * (D / a) ** (math.log(yb / ya) / math.log(b / a))

@functools.lru_cache(maxsize=None)
def np_factors(np):
    """the three-prime factor of each phase's rest (and of the batch tier), from the 4e10 series at NP = 3 against the table's
    4e10 at NP = 4 (both at the code's 3 2^30 cap; MEASURED inputs, the factor is the model's)"""
    if np == 4: return dict(batch=1.0, top=1.0, recip=1.0, div=1.0, other=1.0)
    tab = phase_table(); e4 = tab[4e10]
    ps = [p for p in (_run_phases(r) for r in RUNS if r['use'] == 'fit' and r['np'] == np and r['D'] == 4e10) if p]
    bp = big_products(4e10, 'C', R3_30, np)
    f = dict(batch=_mean(p['batch'] for p in ps) / e4['batch'])
    rest3 = sum(_mean(p[ph] for p in ps) - bp[ph] for ph in ('top', 'recip', 'div'))
    rest4 = sum(e4[ph + '_rest'] for ph in ('top', 'recip', 'div'))
    for ph in ('top', 'recip', 'div'): f[ph] = rest3 / rest4          # one factor for the rest of the three (the division's rest alone
    f['other'] = 1.0                                                  # is 0.4 s at 4e10: its own ratio would be noise)
    return f

def node_phases(D, dz, g=1, exclude=None):
    """the per-node phases of one node at D digits per node under design dz: init, batch, top, recip, div, other (seconds) and
    the label of each.  At g = 1 the whole pipeline; at g > 1 init, batch and top (the leaf) -- the distributed levels, the
    reciprocal and the division come from the fabric model.  top / recip / div = rest x the prime factor + the big products
    under (strategy, cap) (S13's per-product law); x F_MM1 with NTT_MODMUL=1; init by the mapped device (MAP_RATE)"""
    tab = phase_table(exclude); f = np_factors(dz.np); fm = dz.f_mm()
    digits = D * g; cap = dz.cap_at(digits)
    bp = big_products(D, dz.strategy, cap, dz.np)
    out = dict(batch=_interp(tab, 'batch', D) * f['batch'] * fm, other=_interp(tab, 'other', D))
    for ph in ('top', 'recip', 'div'):
        out[ph] = max(0.0, _interp(tab, ph + '_rest', D) * f[ph] + bp[ph]) * fm
    dev = mem_model.mem_per_node(int(D), g, dz.mem_opts(digits))['dev_init'] / 1e9
    out['init'] = _interp(tab, 'init_ref', D) + MAP_RATE * (dev - _dev_init_ref(D, 4, R31))
    out['label'] = 'modelled (%s products)' % bp['label']
    return out

def memory(D, g, form="grid", groups=None, transport="shmem", pool_log=31, staging="resident", design=None):
    """the per-node memory model (mem_model.mem_per_node): GB of device at the dm peak, host (with the SHMEM pool), the node peak"""
    o = dict(form=form, groups=groups, transport=transport, pool_log=pool_log, staging=staging)
    if design is not None and not design.legacy: o.update(design.mem_opts(D * g))
    elif design is None: o.update(np=4, host_fit=False)                                 # the legacy path: four primes (before step 0)
    r = mem_model.mem_per_node(int(D), g, o)
    gb = lambda k: r[k] / 1e9
    return dict(device=gb("dev_dm"), host=gb("host_hwm"), node=gb("node_peak"), planes=gb("planes"), arena=gb("arena"),
                dm_need=gb("dm_need"), tree_need=gb("tree_need"), top_scratch=gb("top_scratch"), exchange=gb("exchange"), regions=gb("regions_bs"),
                shmem_pool=gb("shmem_pool"), shmem_staging=gb("shmem_staging"))

# ------------------------------------------------------------------------------------------------------------
def run(fab, D, g, rule="model", verbose=True, leaf_scale=1.0, init_override=None, dc_exposed=None, groups=None, form="grid", transport="shmem", pool_log=31, staging="resident", design=None):
    """one run of g nodes at D digits per node.  design None: the legacy constants (four primes, the Phase 10/11 phase table;
    --calib and the Phase 12 tables); a Design: the code after Phase 13b step 0 and the row's options (Phase 13b D)"""
    global DZ
    saved = DZ; DZ = design
    try:
        return _run(fab, D, g, rule, verbose, leaf_scale, init_override, dc_exposed, groups, form, transport, pool_log, staging, design)
    finally:
        DZ = saved

def _run(fab, D, g, rule, verbose, leaf_scale, init_override, dc_exposed, groups, form, transport, pool_log, staging, design):
    nq = int(D / LIMB_DIGITS)                          # limbs of Q per node (the leaf's share)
    dl = nq
    nq_tot, dl_tot, np_tot = nq * g, dl * g, nq * g
    if design is None or design.legacy:
        ph = dict(init=init_override if init_override is not None else phase("init", D),
                  batch=phase("batch", D) * leaf_scale, top=phase("top", D) * leaf_scale, other=0.0)
    else:
        npf = node_phases(D, design, g)
        ph = dict(init=init_override if init_override is not None else npf["init"], batch=npf["batch"] * leaf_scale, top=npf["top"] * leaf_scale,
                  other=npf["other"])
    levels = tree_cost(fab, nq, g, groups, form) if g > 1 else []
    if g > 1:
        rc, dc, grp = division_cost(fab, nq_tot, dl_tot, np_tot, g, rule, form)
    elif design is None or design.legacy:
        rc = Cost(); rc.t = phase("recip", D); dc = Cost(); dc.t = phase("div", D); grp = []
    else:
        rc = Cost(); rc.t = npf["recip"]; dc = Cost(); dc.t = npf["div"]; grp = []
    t_levels = sum(c.t for _, _, c in levels)
    out_write = D / 1e9 / fab.write_bw                 # the node's part file at the write bandwidth
    t_lowprod = dc.t * 0.5
    out_exposed = (max(0.0, out_write - t_lowprod) if g > 1 else 0.0) if dc_exposed is None else dc_exposed   # at size 1 the file is hidden under the division (measured: the wall = init + phases)
    wall = ph["init"] + ph["batch"] + ph["top"] + t_levels + rc.t + dc.t + out_exposed + ph["other"]
    tot = Cost()
    for _, _, c in levels: tot.add(c)
    tot.add(rc); tot.add(dc)
    m = memory(D, g, form, groups, transport, pool_log, staging, design)
    res = dict(D=D, g=g, wall=wall, other=ph["other"], init=ph["init"], batch=ph["batch"], top=ph["top"], levels=t_levels, recip=rc.t, div=dc.t,
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
    print("  total digits %.3e; per-node wall %.1f s = %.1f min: init %.1f, batch %.1f, top levels %.1f, distributed levels %.1f, reciprocal %.1f, division %.1f, output exposed %.1f (the part file %.0f s at %.1f GB/s)"
          % (r["digits"], r["wall"], r["wall"] / 60, r["init"], r["batch"], r["top"], r["levels"], r["recip"], r["div"], r["out"], r["out_write"], fab.write_bw))
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

def max_digits(g, node_gb, form="grid", groups=None, transport="shmem", staging="resident", design=None):
    """the largest D per node (to 1e8) whose modelled node peak fits node_gb (design None: the legacy four-prime memory)"""
    o = dict(form=form, groups=groups, transport=transport, staging=staging)
    if design is None: o.update(np=4, host_fit=False)
    elif not design.legacy:
        o.update(design.mem_opts(1e12 * g))                     # the cap rule at the run's digits: 2^31 above 5e10 (every 576-node size)
        if design.cap is None and g == 1: o.pop('cap')          # size 1: the code's rule follows D (3 2^30 below 5e10)
    return mem_model.max_digits_per_node(node_gb * 1e9, g, o)

def headline(fab, g, rule, form="grid", groups=None, staging="resident"):
    """the largest D per node that fits the node (502 GB) and the safe budget (480 GB), their walls"""
    out = []
    for budget in (NODE_GB, NODE_GB_MARGIN):
        D = max_digits(g, budget, form, groups, staging=staging)
        r = run(fab, D, g, rule, verbose=False, groups=groups, form=form, staging=staging) if D else None
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

def schedules(fab, rule, D_list=(4e10, 6e10, 7.7e10), form="grid"):
    print("the level schedule at 576 (MN_GROUPS), per-node wall of the distributed levels (s) by D per node; every level")
    print("costed as the tree forms it (a k-way level = 2 (k - 1) products over the level's group); 'global' = TB per node over")
    print("the dragonfly's global links in the levels (MN_TOPO_GROUP = %d), 'msgs' = messages per APU in the levels (k):" % fab.group)
    print("%-8s %-28s | " % ("name", "MN_GROUPS") + " | ".join("%8.1e: levels expo pcs global  msgs" % D for D in D_list))
    best = None
    for name, spec in SCHEDULES.items():
        cells = []; tot = 0.0
        for D in D_list:
            r = run(fab, D, 576, rule, verbose=False, groups=spec, form=form)
            L = r["levels_rows"]
            cells.append("%15.1f %4.1f %3d %6.1f %5dk" % (r["levels"], sum(c.t_exposed for _, _, c in L), sum(c.pieces for _, _, c in L), sum(c.glob for _, _, c in L) / 1e12, sum(c.msgs for _, _, c in L) / 1000))
            tot += r["levels"]
        print("%-8s %-28s | " % (name, spec or "(default: 2,4,...,512,576)") + " | ".join(cells))
        if best is None or tot < best[0]: best = (tot, name, spec)
    print("cheapest by the model (the sum over the three sizes): %s (MN_GROUPS=%s); the three are within the model's own error of each other" % (best[1], best[2] or "unset"))
    return best

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--calib", action="store_true", help="the aac6 calibration against the recorded multi-process walls")
    ap.add_argument("--schedules", action="store_true", help="compare the 576-node level schedules")
    ap.add_argument("--group", type=int, default=64, help="nodes per dragonfly group (MN_TOPO_GROUP)")
    ap.add_argument("--layers", type=int, default=2, choices=(2, 3), help="the layered all-to-all: 2 (APU x node) or 3 (APU x node-in-group x group)")
    ap.add_argument("--taper", type=float, default=1.0, help="global-link bandwidth as a fraction of the group's injection")
    ap.add_argument("--bw", type=float, default=100.0, help="GB/s per APU injection")
    ap.add_argument("--lat", type=float, default=2e-6, help="seconds per message")
    ap.add_argument("--write-bw", type=float, default=2.0, help="GB/s per node for the part file")
    ap.add_argument("--rule", default="model", choices=("model", "full"), help="X1's group choice in the reciprocal (model) or every product on the full group (full)")
    ap.add_argument("--tree", default="grid", choices=("grid", "flat"), help="the top product's form: grid (Phase 12 G: O(share) spills) or flat (the code at 7aded87)")
    ap.add_argument("--groups", default=None, help="MN_GROUPS (e.g. 2,4,8,16,32,64,576); default = the code's schedule")
    ap.add_argument("--staging", default="resident", choices=("resident", "per_exchange", "cached"), help="the SHMEM transport's staging: resident (Phase 12 S: the slabs in the pool), per_exchange (freed after each wait), cached (the code at 7aded87: kept per communicator)")
    ap.add_argument("--D", type=float, nargs="*", default=[4e10, 8e10, 1e11])
    ap.add_argument("--g", type=int, nargs="*", default=[4, 64, 576])
    a = ap.parse_args()
    if a.calib:
        sys.exit(0 if calibrate(a.rule) else 1)
    fab = Fabric(TARGET.name, a.bw, a.lat, group=a.group, layers=a.layers, taper=a.taper, write_bw=a.write_bw)
    if a.schedules:
        schedules(fab, a.rule, form=a.tree); return
    print("the target (PLAN 25): %d nodes = 2 304 APUs; %.0f GB/s per APU, %.1f us per message, dragonfly groups of %d nodes, %d-layer all-to-all, global taper %.2f; part files at %.1f GB/s per node; tree form %s"
          % (576, a.bw, a.lat * 1e6, a.group, a.layers, a.taper, a.write_bw, a.tree))
    print("single node (measured, size 1): 4e10 in 81.5 s, 8e10 in 195.5 s, 1e11 in 262.9 s")
    for D in a.D:
        for g in a.g:
            run(fab, D, g, a.rule, groups=a.groups, form=a.tree, staging=a.staging)
    print("=" * 112)
    print("HEADLINE (modelled from the measured per-node profile; the memory model with the tail layout and alltoallv shifts; tree form %s, SHMEM staging %s):" % (a.tree, a.staging))
    for budget, D, r in headline(fab, 576, a.rule, a.tree, a.groups, a.staging):
        if r is None: print("  576 nodes: nothing fits %.0f GB" % budget); continue
        print("  576 nodes, %.0f GB per node: the largest D per node %.2e (node peak %.0f GB) -> %.3e digits in %.1f min per-node wall (%.1f min without the part file's exposed %.0f s)"
              % (budget, D, r["mem"]["node"], r["digits"], r["wall"] / 60, (r["wall"] - r["out"]) / 60, r["out"]))
    for Dn in (4e10, 7.7e10):
        r = run(fab, Dn, 576, a.rule, verbose=False, groups=a.groups, form=a.tree, staging=a.staging)
        print("  576 nodes x %.1e = %.3e digits: %.1f min per-node wall (exposed communication %.1f s, %s on the NICs per node, node peak %.0f GB%s)" % (Dn, r["digits"], r["wall"] / 60, r["exposed"], fmt_b(r["nic"]), r["mem"]["node"], "" if r["mem"]["node"] <= NODE_GB else " -- does not fit"))
    for g in a.g:
        if g < 8: continue
        rm = run(fab, 4e10, g, "model", verbose=False, form=a.tree); rf = run(fab, 4e10, g, "full", verbose=False, form=a.tree)
        print("  X1 at 4e10 x %d: the reciprocal on the model's groups %.1f s vs every product on the full group %.1f s (exposed %.1f vs %.1f, messages per APU %d vs %d)"
              % (g, rm["recip"], rf["recip"], rm["rc"].t_exposed, rf["rc"].t_exposed, rm["rc"].msgs, rf["rc"].msgs))
    if a.layers == 2:
        fab3 = Fabric(TARGET.name, a.bw, a.lat, group=a.group, layers=3, taper=a.taper, write_bw=a.write_bw)
        r2 = run(fab, 4e10, 576, a.rule, verbose=False, form=a.tree); r3 = run(fab3, 4e10, 576, a.rule, verbose=False, form=a.tree)
        print("  the third layer at 4e10 x 576 (group %d): wall %.1f -> %.1f s, exposed %.1f -> %.1f s, NIC bytes %s -> %s, messages per APU %d -> %d"
              % (a.group, r2["wall"], r3["wall"], r2["exposed"], r3["exposed"], fmt_b(r2["nic"]), fmt_b(r3["nic"]), r2["msgs"], r3["msgs"]))
    schedules(fab, a.rule, form=a.tree)

if __name__ == "__main__":
    main()

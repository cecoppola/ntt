#!/usr/bin/env python3
"""mn_model.py - the fabric model of the multi-node run (Phase 11 agent X, PLAN.md 25/26: X2).

Per node, for D digits per node over g nodes: the wall by phase, the bytes on the fabric (per NIC and on the
dragonfly's global links), the exposed (un-hidden) communication, the memory, and the total digits.  The
single-node phases come from the measured table below; the distributed levels, the sharded reciprocal and the
sharded division are built product by product (the same grids, chains and cuts the code forms) from the measured
2^31-point piece product and the target fabric (PLAN 25: 100 GB/s per APU, two-tier dragonfly, ~2 us per message).

    ./mn_model.py                  the 25 tables: D in {4e10, 8e10, 1e11} x g in {4, 64, 576} (+ the headline)
    ./mn_model.py --calib          the aac6 calibration (g = 2, 4 on one node over loopback TCP) against the measured walls
    ./mn_model.py --group 64 --layers 3 --taper 0.5 --write-bw 2   parameters (see ARGS)

Everything printed as "modelled" is this script's arithmetic; "measured" cites the RESULTS section.
"""
import argparse, math, sys

# ------------------------------------------------------------------------------------------------------------
# MEASURED INPUTS (one MI300A node, four APUs; the code of main @ 72aa2e9 unless noted)
# ------------------------------------------------------------------------------------------------------------
# single-node phases by digits (seconds): init, batch tier, top levels (device tier), reciprocal, division, wall.
#   1e9, 1e10: this session's calibration batch (job 20802, s24-26; results/X.md)
#   4e10: RESULTS 75 closing series (83.0 +- 1.2 s);  7e10: RESULTS 74 / results/M.md (159.7 s, default pool)
#   8e10: results/M.md (210.4 s, ECALC_DM_POOL=1)
PHASES = {          #  init   batch   top    recip   div    wall
    1e9:    dict(init=9.3,  batch=1.5,  top=0.1,  recip=2.76, div=0.38, wall=14.4),
    1e10:   dict(init=10.8, batch=7.1,  top=1.1,  recip=4.78, div=2.98, wall=29.0),
    4e10:   dict(init=16.4, batch=23.0, top=13.1, recip=15.2, div=14.9, wall=83.0),
    7e10:   dict(init=22.2, batch=39.5, top=25.7, recip=35.9, div=36.6, wall=159.7),
    8e10:   dict(init=25.1, batch=46.6, top=38.8, recip=49.6, div=49.5, wall=210.4),
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
# memory per node by digits (GB; results/M.md tables at 4/7/8e10, size 1): planes fixed at the 2^31 layout;
# regions and the dm pool's peak live scale with n_Q; the host is H's profile (RESULTS 75: 11.7 GB at 4e10).
MEM_PLANES = 120.3
MEM_REGIONS_PER_1E9 = 2.3       # 95.7 / 169.6 / 176.8 GB at 4 / 7 / 8e10
MEM_DMPOOL_PER_1E9 = 2.9        # peak live 121.5 / 202.2 / 229.3 GB (6.5 n_Q limbs of 8 B)
MEM_HOST_FIXED, MEM_HOST_PER_1E9 = 3.1, 0.215   # 2.9 other + 8.6 pinned staging at 4e10 (H's B2)
NODE_GB = 502.0
LIMB_DIGITS = 18                # decimal limbs (RESULTS 67)
EC_NP = 4
DIST_LOGN_MAX = 31
K_CHUNKS = 4                    # the slab pipeline (M7)
CACHE_MN_SLOTS = 2              # RNS_DIST_CACHE_MN default over shares (results/G.md)

# ------------------------------------------------------------------------------------------------------------
# the fabric
# ------------------------------------------------------------------------------------------------------------
class Fabric:
    """bw_apu: injection per APU (GB/s); lat: per-message latency (s); group: nodes per dragonfly group (the
    MN_TOPO_GROUP parameter); layers: 2 = the layered all-to-all (APU x node), 3 = APU x node-in-group x group;
    taper: the global tier's bandwidth as a fraction of the group's aggregate injection to each peer group;
    gpu_share: node-processes sharing one node's APUs (aac6 calibration; 1 on the target);
    fixed: a fixed cost per exchange (s) beyond the latency model (the TCP transport's staging and syncs)."""
    def __init__(self, name, bw_apu, lat, group=64, layers=2, taper=1.0, gpu_share=1, fixed=0.0, write_bw=2.0, tcp_exp=0.0):
        self.name, self.bw, self.lat, self.group, self.layers, self.taper = name, bw_apu, lat, group, layers, taper
        self.gpu_share, self.fixed, self.write_bw, self.tcp_exp = gpu_share, fixed, write_bw, tcp_exp

    def rate(self, bytes_apu):
        """GB/s per APU: constant on the target; the TCP loopback's single stream per mesh is slower on small
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
            ng = math.ceil(g / G)
            cross = bytes_apu * max(0, g - G) / g if g > G else 0.0
            nic = out
            msgs = chunks * (g - 1)
        t_nic = nic / (self.rate(bytes_apu) * 1e9)
        t_glob = cross / (self.rate(bytes_apu) * 1e9 * self.taper) if cross else 0.0   # the group's aggregate to each peer group = G x 4 x bw x taper / (ng - 1), shared by its G nodes evenly
        t = max(t_nic, t_glob) + msgs * self.lat + self.fixed * chunks
        return t, nic * 4, cross * 4, msgs

    def coll(self, g):
        """a small collective (all-gather of a few words, a max-reduction) over g nodes"""
        return 0.0 if g <= 1 else (g - 1) * self.lat + self.fixed + 20e-6

TARGET = Fabric("Slingshot-2 dragonfly (PLAN 25)", bw_apu=100.0, lat=2e-6, group=64, layers=2, taper=1.0)
# aac6 loopback: node-processes on one node over the TCP transport (correctness transport): fitted below on the
# measured products of job 20802 (per node-process 3.3 GB/s aggregate over its four meshes, ~1 ms per chunk exchange)
# per node-process: 3.3 GB/s (g = 2, one stream per mesh: slower below 1 GiB per APU, exponent 0.3) / 3.7 GB/s
# (g = 4, exponent 0.1) aggregate over its four meshes at the 2^31 products, 1 ms fixed per chunk exchange
AAC6 = {2: Fabric("aac6 loopback TCP, 2 node-processes", bw_apu=3.3 / 4, lat=0.0, group=10**6, fixed=1.0e-3, write_bw=1.0, tcp_exp=0.3),
        4: Fabric("aac6 loopback TCP, 4 node-processes", bw_apu=3.7 / 4, lat=0.0, group=10**6, fixed=1.0e-3, write_bw=1.0, tcp_exp=0.1)}

# ------------------------------------------------------------------------------------------------------------
# the product tier over a group (rns_dist.c: mn_core / mn_grid), as cost
# ------------------------------------------------------------------------------------------------------------
def plane_pts(nc, lgt):
    n = 1 << 20
    while n < nc: n <<= 1
    logmin = max(20, 2 * (7 + lgt))                    # rows = R / nr >= 32, R, C >= 2^10 (mn_core)
    return max(n, 1 << logmin)

def split_grid(na, nb, cap, lgt):
    """(ka, kb, pieces_pts): the fewest plane points in total, then the fewest pieces (split_grid_cap)"""
    best = None
    for i in range(1, 33):
        for j in range(1, 33):
            pa, pb = -(-na // i), -(-nb // j)
            if pa + pb > cap: continue
            pts = plane_pts(pa + pb, lgt)
            cost = i * j * pts
            if best is None or cost < best[0] or (cost == best[0] and i * j < best[1] * best[2]): best = (cost, i, j, pts)
    if best is None: raise ValueError("no grid for %d x %d at cap %d" % (na, nb, cap))
    return best[1], best[2], best[3]

class Cost:
    def __init__(self): self.t = 0.0; self.t_exposed = 0.0; self.nic = 0.0; self.glob = 0.0; self.msgs = 0; self.pieces = 0; self.xfers = 0
    def add(self, o):
        self.t += o.t; self.t_exposed += o.t_exposed; self.nic += o.nic; self.glob += o.glob; self.msgs += o.msgs; self.pieces += o.pieces; self.xfers += o.xfers
        return self

def piece_cost(fab, pts, g, gt, na, nb, nc, fwd=2, with_x=False):
    """one piece product of pts plane points over a group of g nodes (gt transform nodes): the local passes on
    gt x 4 ranks, the fabric exchanges (12 layered all-to-alls per piece: 3 per prime, fewer with cache hits), the
    operand redistributions and the result exchange, the spill all-gather and carry scan."""
    c = Cost(); c.pieces = 1
    q = pts / (4 * gt)                                  # points per APU
    scale = q / (1 << 29)
    # the local part: the measured piece (its xGMI exchanges included, 84 % hidden), on shared APUs times the share
    t_loc = (T_PIECE_31 if fwd == 2 else T_PIECE_31_BHIT) * scale + 0.005
    t_loc *= fab.gpu_share
    # the transforms' exchanges: EC_NP x (fwd + 1) layered all-to-alls of 8 q bytes per APU; the xGMI stage of one
    # runs under the fabric stage of the other (inflight 2), so the fabric's excess over the xGMI stage is exposed
    t_x = T_XGMI_31 * scale
    n_tr = EC_NP * (fwd + 1)
    t_f, nic, glob, msgs = fab.a2a(8 * q, g, K_CHUNKS)
    exposed_tr = max(0.0, t_f - t_x) if fab.name.startswith("Slingshot") else max(0.0, t_f - HIDDEN_XGMI * t_x)
    c.nic += n_tr * nic; c.glob += n_tr * glob; c.msgs += n_tr * msgs; c.xfers += n_tr
    # the operand redistributions (A, B, X) and the result: plain all-to-alls over the group of the operands' bytes
    n_red = fwd + (1 if with_x else 0)
    t_r = 0.0
    for limbs in ([na] if fwd >= 1 else []) + ([nb] if fwd >= 2 else []) + ([nc] if with_x else []) + [nc]:
        t1, nic1, glob1, msgs1 = fab.a2a(8 * limbs / (4 * g), g, 1)
        t_r += t1; c.nic += nic1; c.glob += glob1; c.msgs += msgs1; c.xfers += 1
    t_small = 3 * fab.coll(g)
    c.t = t_loc + n_tr * exposed_tr + t_r + t_small
    c.t_exposed = n_tr * exposed_tr + t_r + t_small
    return c

def product_cost(fab, na, nb, g, gt, lowcut=0, highcut=None, with_x=False, cache=True):
    """C = A B (+ X) over a group: the grid of pieces at the group's cap (2^(31 + log2 gt)), the cuts, the cache"""
    lgt = max(0, math.ceil(math.log2(gt)))
    cap = 1 << (DIST_LOGN_MAX + lgt)
    nc = na + nb
    c = Cost()
    if nc <= cap:
        pts = plane_pts(nc, lgt)
        return c.add(piece_cost(fab, pts, g, gt, na, nb, nc, 2, with_x))
    ka, kb, pts = split_grid(na, nb, cap, lgt)
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
            c.add(piece_cost(fab, pts, g, gt, la, lb, la + lb, fwd, False))
            first = False
    if with_x:                                                        # mdb_add_shifted: rounds of 2^26 limbs per APU
        t1, nic1, glob1, msgs1 = fab.a2a(8 * nc / (4 * g), g, 1)
        c.t += t1 + fab.coll(g); c.t_exposed += t1 + fab.coll(g); c.nic += nic1; c.glob += glob1; c.msgs += msgs1; c.xfers += 1
    return c

def shift_cost(fab, n, g):
    """mdb_shift: one all-to-all per APU thread of the share-intersection pieces (about half the limbs change node
    when the basis changes), then the truncation's max-reduction"""
    c = Cost()
    t1, nic1, glob1, msgs1 = fab.a2a(0.5 * 8 * n / (4 * g), g, 1)
    c.t = t1 + fab.coll(g) + 0.002 * fab.gpu_share; c.t_exposed = t1 + fab.coll(g); c.nic = nic1; c.glob = glob1; c.msgs = msgs1; c.xfers = 1
    return c

def small_cost(fab, g, n=1):
    c = Cost(); c.t = c.t_exposed = n * (fab.coll(g) + 0.003 * fab.gpu_share); return c

# ------------------------------------------------------------------------------------------------------------
# the reciprocal (recip_mn) and the division (newton_mn_divmod) over the machine
# ------------------------------------------------------------------------------------------------------------
def choose_group(fab, n_pts, g, rule):
    """X1: the group for a product of n_pts points: the smallest power of two <= g whose cost is minimal (the
    model's rule), or the full group (rule 'full')."""
    if rule == "full" or g <= 2: return g
    best = None
    gp = 2
    while gp <= g:
        gt = gp
        try: c = product_cost(fab, n_pts * 2 // 3, n_pts // 3, gp, gt)
        except ValueError: gp *= 2; continue
        if best is None or c.t < best[0]: best = (c.t, gp)
        gp *= 2
    if best[1] * 2 > g and best[1] != g: return g
    return best[1]

def recip_cost(fab, nq, k, g, rule, split=1 << 16):
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
        gp = choose_group(fab, take + j + 1, g, rule)
        if gp != cur:                                                  # r re-sharded onto the step's group
            c.add(shift_cost(fab, j + 1, max(gp, cur or 1))); cur = gp
        groups.append((j, gp))
        gt = gp
        if take < nq: c.add(shift_cost(fab, take, g))                  # Q_t out of Q (Q is over the full group)
        c.add(product_cost(fab, take, j + 1, gp, gt))                  # Q_t r
        c.add(shift_cost(fab, take + j + 1, gp))                       # u
        c.add(small_cost(fab, gp, 4))                                  # limb, nonzero_below, pow, |B^2j - u|
        c.add(product_cost(fab, j + 1, j + 2, gp, gt))                 # r d
        c.add(shift_cost(fab, 2 * j + 3, gp))                          # corr
        c.add(shift_cost(fab, 2 * j + 1, gp))                          # r << j
        c.add(small_cost(fab, gp, 2))                                  # cmp, add
        if jn < 2 * j: c.add(shift_cost(fab, 2 * j + 1, gp))
        j = jn
    if cur != g: c.add(shift_cost(fab, k + 1, g))                      # mu onto the full group
    return c, groups

def division_cost(fab, nq, dl, npn, g, rule):
    """S = P + Q, A_h = S >> (nq - 1 - dl), t = A_h mu (low cut k + 1), X = t >> (k + 1), X Q mod B^w (high cut w),
    the window, the corrections, the residues; the reciprocal first"""
    na = npn + dl; k = na - nq + 1; w = nq + 2
    rc, groups = recip_cost(fab, nq, k, g, rule)
    c = Cost()
    c.add(shift_cost(fab, nq, g)); c.add(small_cost(fab, g, 3))        # Q into P's basis, S = P + Q, residues of P, Q
    c.add(shift_cost(fab, na, g))                                      # A_h
    nah = 2 * dl + 1
    c.add(product_cost(fab, nah, k + 1, g, g, lowcut=k + 1))           # A_h mu
    c.add(shift_cost(fab, nah + k + 1, g))                             # X
    c.add(product_cost(fab, dl + 1, nq, g, g, highcut=w))              # X Q mod B^w
    c.add(shift_cost(fab, na, g)); c.add(shift_cost(fab, nq, g))       # the window, Q in basis w
    c.add(small_cost(fab, g, 6))                                       # cmp, sub, corrections, X +- dx, R residues
    return rc, c, groups

# ------------------------------------------------------------------------------------------------------------
# the tree's distributed levels
# ------------------------------------------------------------------------------------------------------------
def level_schedule(g):
    """group sizes per level: powers of two, then 3-way steps to a 9 x 2^k count (L's C3-576 schedule)"""
    s = []; x = 1
    while x * 2 <= g and g % (x * 2) == 0: x *= 2; s.append(("2", x))
    while x < g:
        if g % (x * 3) == 0: x *= 3; s.append(("3", x))
        else: x = g; s.append(("2", x))                                # (a padded binary step)
    return s

def tree_cost(fab, nq_node, g):
    """level by level: a binary level pairs groups of m nodes (operands m n_Q limbs): Q_A Q_B and P_A Q_B + P_B;
    a 3-way level joins three groups: Q2 Q3, Q1 (Q2 Q3), P2 Q3 + P3, P1 (Q2 Q3) + (P2 Q3 + P3)"""
    rows = []; m = 1
    for kind, size in level_schedule(g):
        c = Cost(); gt = 1 << int(math.floor(math.log2(size)))
        if kind == "2":
            n = m * nq_node
            c.add(product_cost(fab, n, size * nq_node - n, size, gt, with_x=True))
            c.add(product_cost(fab, n, size * nq_node - n, size, gt))
        else:
            n = m * nq_node
            c.add(product_cost(fab, n, n, size, gt))                       # Q2 Q3
            c.add(product_cost(fab, n, 2 * n, size, gt))                   # Q1 (Q2 Q3)
            c.add(product_cost(fab, n, n, size, gt, with_x=True))          # P2 Q3 + P3
            c.add(product_cost(fab, n, 2 * n, size, gt, with_x=True))      # P1 (Q2 Q3) + ...
        c.add(small_cost(fab, size, 2))
        rows.append((kind, size, c)); m = size
    return rows

# ------------------------------------------------------------------------------------------------------------
# the single-node phases by digits (log-log interpolation of the measured table; extrapolated above 8e10)
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

def memory(D, g, gp_max, b7=True, cache_slots=0):
    """device GB per node at the dm peak, host GB (M.md's rows scaled; the dist scratch = 4 q per APU at the cap
    plane, M3; the shift buffers 2 x share/4 per APU with alltoallv (B7), g x that without)"""
    d9 = D / 1e9
    regions = MEM_REGIONS_PER_1E9 * d9; dmpool = MEM_DMPOOL_PER_1E9 * d9
    dist = 4 * (1 << 29) * 8 * 4 / 1e9 if g > 1 else 0.0
    share_gb = D / LIMB_DIGITS * 8 / 1e9
    shift = 2 * share_gb * (1 if b7 else gp_max) if g > 1 else 0.0
    cache = cache_slots * EC_NP * (1 << 29) * 8 * 4 / 1e9
    dev = MEM_PLANES + max(regions, dmpool) + dist + shift + cache
    host = MEM_HOST_FIXED + MEM_HOST_PER_1E9 * d9
    return dict(planes=MEM_PLANES, regions=regions, dmpool=dmpool, dist=dist, shift=shift, cache=cache, device=dev, host=host, node=dev + host)

# ------------------------------------------------------------------------------------------------------------
def run(fab, D, g, rule="model", verbose=True, leaf_scale=1.0, init_override=None, dc_exposed=None):
    nq = int(D / LIMB_DIGITS)                          # limbs of Q per node (the leaf's share)
    dl = nq
    nq_tot, dl_tot, np_tot = nq * g, dl * g, nq * g
    ph = dict(init=init_override if init_override is not None else phase("init", D),
              batch=phase("batch", D) * leaf_scale, top=phase("top", D) * leaf_scale)
    levels = tree_cost(fab, nq, g) if g > 1 else []
    if g > 1:
        rc, dc, groups = division_cost(fab, nq_tot, dl_tot, np_tot, g, rule)
    else:
        rc = Cost(); rc.t = phase("recip", D); dc = Cost(); dc.t = phase("div", D); groups = []
    t_levels = sum(c.t for _, _, c in levels)
    out_write = D / 1e9 / fab.write_bw                 # the node's part file at the write bandwidth
    t_lowprod = dc.t * 0.5
    out_exposed = max(0.0, out_write - t_lowprod) if dc_exposed is None else dc_exposed
    wall = ph["init"] + ph["batch"] + ph["top"] + t_levels + rc.t + dc.t + out_exposed
    tot = Cost()
    for _, _, c in levels: tot.add(c)
    tot.add(rc); tot.add(dc)
    res = dict(D=D, g=g, wall=wall, init=ph["init"], batch=ph["batch"], top=ph["top"], levels=t_levels, recip=rc.t, div=dc.t,
               out=out_exposed, out_write=out_write, exposed=tot.t_exposed, nic=tot.nic, glob=tot.glob, msgs=tot.msgs,
               pieces=tot.pieces, digits=D * g, levels_rows=levels, groups=groups, rc=rc, dc=dc)
    if verbose: print_run(fab, res, rule)
    return res

def fmt_b(b):
    return "%.1f TB" % (b / 1e12) if b >= 1e12 else "%.1f GB" % (b / 1e9)

def print_run(fab, r, rule):
    D, g = r["D"], r["g"]
    print("=" * 112)
    print("D = %.0e digits per node, g = %d nodes (%s; dragonfly group %d nodes, %d-layer all-to-all, X1 groups: %s)" % (D, g, fab.name, fab.group, fab.layers, rule))
    print("  total digits %.3e; per-node wall %.1f s = %.1f min: init %.1f, batch %.1f, top levels %.1f, distributed levels %.1f, reciprocal %.1f, division %.1f, output exposed %.1f (the part file %.0f s at %.1f GB/s)"
          % (r["digits"], r["wall"], r["wall"] / 60, r["init"], r["batch"], r["top"], r["levels"], r["recip"], r["div"], r["out"], r["out_write"], fab.write_bw))
    for kind, size, c in r["levels_rows"]:
        print("    level: group %4d (%s-way)  %6.2f s  exposed %5.2f  pieces %2d  NIC %s  global %s  msgs/APU %d" % (size, kind, c.t, c.t_exposed, c.pieces, fmt_b(c.nic), fmt_b(c.glob), c.msgs))
    rc, dc = r["rc"], r["dc"]
    print("    reciprocal %6.2f s (exposed %.2f, %d pieces, NIC %s, global %s, msgs/APU %d); groups by step: %s" % (rc.t, rc.t_exposed, rc.pieces, fmt_b(rc.nic), fmt_b(rc.glob), rc.msgs,
          " ".join("%s:%d" % ("%.1e" % j, gp) for j, gp in r["groups"][::max(1, len(r["groups"]) // 8)])))
    print("    division   %6.2f s (exposed %.2f, %d pieces, NIC %s, global %s, msgs/APU %d)" % (dc.t, dc.t_exposed, dc.pieces, fmt_b(dc.nic), fmt_b(dc.glob), dc.msgs))
    print("  exposed communication %.1f s of the wall (%.0f %%); fabric bytes per node %s (per NIC %s over 8), on global links %s; %d messages per APU"
          % (r["exposed"], 100 * r["exposed"] / r["wall"], fmt_b(r["nic"]), fmt_b(r["nic"] / 8), fmt_b(r["glob"]), r["msgs"]))
    gp_max = g
    m = memory(D, g, gp_max)
    m2 = memory(D, g, gp_max, b7=False)
    print("  memory per node at the dm peak: device %.0f GB (planes %.0f, regions/dm pool %.0f, dist scratch %.0f, shift buffers %.0f with alltoallv [%.0f padded]), host %.0f GB; node %.0f of %.0f GB%s"
          % (m["device"], m["planes"], max(m["regions"], m["dmpool"]), m["dist"], m["shift"], m2["shift"], m["host"], m["node"], NODE_GB, "" if m["node"] <= NODE_GB else "  ** DOES NOT FIT **"))

def headline(fab, g, rule):
    """the largest D per node that fits the memory model with 5 % headroom, its wall"""
    lo, hi = 1e10, 2e11
    for _ in range(40):
        mid = (lo + hi) / 2
        if memory(mid, g, g)["node"] <= 0.95 * NODE_GB: lo = mid
        else: hi = mid
    D = math.floor(lo / 1e9) * 1e9
    r = run(fab, D, g, rule, verbose=False)
    return D, r

# ------------------------------------------------------------------------------------------------------------
# calibration: aac6, node-processes sharing one node over loopback TCP (job 20802)
# ------------------------------------------------------------------------------------------------------------
CALIB = [   # (D_total, g, measured wall, init, bs incl. tree, dm, dc exposed)  -- results/X.md
    (1e9, 2, 27.66, 5.5, 3.8, 18.0, 0.4),
    (1e9, 4, 19.69, 5.1, 3.8, 10.6, 0.1),
    (1e10, 4, 110.14, 6.1, 32.6, 68.9, 2.4),
    (1e10, 2, 146.42, 9.3, 26.8, 105.2, 5.0),
]

def calibrate(rule):
    print("calibration on aac6: g node-processes share one node (each drives the four APUs), loopback TCP; the leaves run")
    print("concurrently (measured 0.5 x the single-node bs of the total digits), init is the process's own (measured);")
    for g, f in sorted(AAC6.items()): print("the model at g = %d: %.2f GB/s per APU thread at 1 GiB per APU (exponent %.1f below), %.1f ms fixed per exchange, GPUs shared g-way" % (g, f.bw, f.tcp_exp, f.fixed * 1e3))
    print("%-8s %-3s %9s %9s %7s | %8s %8s %8s %8s | %s" % ("digits", "g", "measured", "modelled", "error", "leaf+tr", "recip", "div", "dm", "measured bs / dm"))
    ok = True
    for D, g, wall, init, bs, dm, dcx in CALIB:
        f0 = AAC6[g]; fab = Fabric(f0.name, f0.bw, f0.lat, group=f0.group, layers=2, gpu_share=g, fixed=f0.fixed, write_bw=f0.write_bw, tcp_exp=f0.tcp_exp)
        Dn = D / g
        # the leaves: g concurrent leaves of D/g digits on the shared node cost 0.5 x bs(D) (measured, see X.md)
        leaf = 0.5 * (phase("batch", D) + phase("top", D))
        r = run(fab, Dn, g, rule, verbose=False, leaf_scale=0.0, init_override=init, dc_exposed=dcx)
        model = r["wall"] + leaf
        err = (model - wall) / wall
        ok &= abs(err) <= 0.2
        print("%-8.0e %-3d %9.1f %9.1f %+6.0f%% | %8.1f %8.1f %8.1f %8.1f | %.1f / %.1f" % (D, g, wall, model, 100 * err, leaf + r["levels"], r["recip"], r["div"], r["recip"] + r["div"], bs, dm))
    print("calibration %s (gate: every run within 20 %%)" % ("OK" if ok else "FAILED"))
    return ok

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--calib", action="store_true", help="the aac6 calibration against the measured multi-process walls")
    ap.add_argument("--group", type=int, default=64, help="nodes per dragonfly group (MN_TOPO_GROUP)")
    ap.add_argument("--layers", type=int, default=2, choices=(2, 3), help="the layered all-to-all: 2 (APU x node) or 3 (APU x node-in-group x group)")
    ap.add_argument("--taper", type=float, default=1.0, help="global-link bandwidth as a fraction of the group's injection")
    ap.add_argument("--bw", type=float, default=100.0, help="GB/s per APU injection")
    ap.add_argument("--lat", type=float, default=2e-6, help="seconds per message")
    ap.add_argument("--write-bw", type=float, default=2.0, help="GB/s per node for the part file")
    ap.add_argument("--rule", default="model", choices=("model", "full"), help="X1's group choice in the reciprocal (model) or every product on the full group (full)")
    ap.add_argument("--D", type=float, nargs="*", default=[4e10, 8e10, 1e11])
    ap.add_argument("--g", type=int, nargs="*", default=[4, 64, 576])
    a = ap.parse_args()
    if a.calib:
        sys.exit(0 if calibrate(a.rule) else 1)
    fab = Fabric(TARGET.name, a.bw, a.lat, group=a.group, layers=a.layers, taper=a.taper, write_bw=a.write_bw)
    print("the target (PLAN 25): %d nodes = 2 304 APUs; %.0f GB/s per APU, %.1f us per message, dragonfly groups of %d nodes, %d-layer all-to-all, global taper %.2f; part files at %.1f GB/s per node"
          % (576, a.bw, a.lat * 1e6, a.group, a.layers, a.taper, a.write_bw))
    print("single node (measured, size 1): 4e10 in 83.0 s, 8e10 in 210.4 s; the modelled size-1 dm at 4e10: recip %.1f / div %.1f (measured 15.2 / 14.9)" %
          (run(TARGET, 4e10, 1, verbose=False)["recip"], run(TARGET, 4e10, 1, verbose=False)["div"]))
    for D in a.D:
        for g in a.g:
            run(fab, D, g, a.rule)
    print("=" * 112)
    print("HEADLINE (modelled from the measured per-node profile; memory with 5 %% headroom, M's tail layout assumed, alltoallv shifts (L's B7) assumed):")
    for g in (576,):
        D, r = headline(fab, g, a.rule)
        m = memory(D, g, g)
        print("  %d nodes: the largest digit count per node that fits %.0f GB is %.0e (node peak %.0f GB) -> %.3e digits in %.1f min per-node wall (%.1f min without the part file's exposed %.0f s)"
              % (g, NODE_GB, D, m["node"], r["digits"], r["wall"] / 60, (r["wall"] - r["out"]) / 60, r["out"]))
        for Dn in (4e10, 8e10):
            r = run(fab, Dn, g, a.rule, verbose=False)
            print("  %d nodes x %.0e = %.3e digits: %.1f min per-node wall (exposed communication %.1f s, %s on the NICs per node)" % (g, Dn, r["digits"], r["wall"] / 60, r["exposed"], fmt_b(r["nic"])))
    for g in a.g:
        if g < 8: continue
        rm = run(fab, 4e10, g, "model", verbose=False); rf = run(fab, 4e10, g, "full", verbose=False)
        print("  X1 at 4e10 x %d: the reciprocal on the model's groups %.1f s vs every product on the full group %.1f s (exposed %.1f vs %.1f, messages per APU %d vs %d)"
              % (g, rm["recip"], rf["recip"], rm["rc"].t_exposed, rf["rc"].t_exposed, rm["rc"].msgs, rf["rc"].msgs))
    if a.layers == 2:
        fab3 = Fabric(TARGET.name, a.bw, a.lat, group=a.group, layers=3, taper=a.taper, write_bw=a.write_bw)
        r2 = run(fab, 4e10, 576, a.rule, verbose=False); r3 = run(fab3, 4e10, 576, a.rule, verbose=False)
        print("  the third layer at 4e10 x 576 (group %d): wall %.1f -> %.1f s, exposed %.1f -> %.1f s, NIC bytes %s -> %s, messages per APU %d -> %d"
              % (a.group, r2["wall"], r3["wall"], r2["exposed"], r3["exposed"], fmt_b(r2["nic"]), fmt_b(r3["nic"]), r2["msgs"], r3["msgs"]))

if __name__ == "__main__":
    main()

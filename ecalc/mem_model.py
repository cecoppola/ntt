#!/usr/bin/env python3
"""mem_model.py - the per-node memory model of ecalc (Phase 11, agent M; PLAN.md 26 row M, item 3).

Device and host bytes of one node-process as functions of the digits per node D and of the group size g
(the number of node-processes the run is spread over), built from the same formulas the code sizes its
allocations with (binsplit.c: e_terms, seed_limbs, region_need, dm_layout, tree_need_dev; rns_mul.c: the
plane pools; rns_dist.c: the sharded exchange's scratch), calibrated against the mem_report tables of
results/M.md, results/M11.md (4, 7, 8 x 10^10 at size 1; 10^10 at size 4).

    mem_per_node(D, g, opts) -> dict      (bytes; opts: pool_log, tail, alltoallv, margin, form, groups, transport ...)
    python3 mem_model.py                  prints the calibration table, the ceilings per node and the 576-node digits

Agent X's mn_model.py and Q's estimate.py import mem_per_node for their memory rows.  Every number is "modelled"
unless the calibration table says "measured"; the tables' sources are named in results/M11.md.  Phase 12 (agent Q):
the tree's g-terms in both forms -- 'flat' (the code at 7aded87) and 'grid' (agent G's gridded top product, PLAN 27) --
and L's level schedule (mn_groups, MN_GROUPS); see results/Q.md.
"""
import math, sys

LN10 = math.log(10.0)
NR = 4                                   # regions = APUs per node
GB = 1e9

# ---------------------------------------------------------------- the code's sizing formulas (binsplit.c)
def e_terms(d):
    """N = min{m : lgamma(m+1)/ln10 >= d + 50} (binsplit.c e_terms)"""
    t = d + 50.0; lo, hi = 1, 2
    while math.lgamma(hi + 1) / LN10 < t: hi *= 2
    while hi - lo > 1:
        m = (lo + hi) // 2
        if math.lgamma(m + 1) / LN10 < t: lo = m
        else: hi = m
    return lo if math.lgamma(lo + 1) / LN10 >= t else hi

def digits_of_run(d_out):
    """decimal: d rounded up to a multiple of 18"""
    return (d_out + 17) // 18 * 18

def seed_limbs(N, nterms, S=256, decimal=True):
    """per (limbs per span, the bound), nspan for a node computing nterms of the N-term series"""
    nspan = (nterms + S - 1) // S
    per = (S * math.ceil(math.log2(N + 2.0)) + 128) // (59 if decimal else 64) + 2
    return per, nspan

def region_of(i, n): r = i * NR // n; return r if r < NR else NR - 1
def place_node(i, n, balance_n=16): return region_of(i, n) if n > balance_n else i % NR

def region_need(N, nterms, mdev_logl=30, decimal=True):
    """binsplit.c region_need: the limbs each region must hold over the batch levels (the simulated layout).
    Closed forms per region instead of the C loop over the nodes: every node but the last covers full spans."""
    per, nspan = seed_limbs(N, nterms, decimal=decimal)
    r0 = [min(nspan, -(-r * nspan // NR)) for r in range(NR + 1)]           # the first span of region r
    need = [2 * per * (r0[r + 1] - r0[r]) + 2 for r in range(NR)]
    l = 0
    while True:
        n_in = (nspan + (1 << l) - 1) >> l
        if n_in <= 1: break
        m_full = 1 << l; max_nl = m_full * per + l
        if 2 * int(0.9 * max_nl) + 1 > (1 << mdev_logl): break          # the mdev (device-number) levels
        npairs, odd = n_in // 2, n_in & 1; n = npairs + odd
        full = 4 * m_full * per + l + 1                                   # pa + qb + 1 + qa + qb of a full node
        i = n - 1                                                         # the last node: partial spans (and the odd one)
        ma = min(nspan - 2 * i * m_full, m_full) if 2 * i * m_full < nspan else 0
        mb = min(nspan - (2 * i + 1) * m_full, m_full) if (2 * i + 1) * m_full < nspan else 0
        last = (ma * per + l) + ma * per if (odd and i == npairs) else (ma * per + l) + mb * per + 1 + ma * per + mb * per
        offr = [0] * NR
        for r in range(NR):
            if n > 16: a, b = -(-r * n // NR), -(-(r + 1) * n // NR); cnt = max(0, b - a)   # region_of: i in [a, b)
            else: cnt = len(range(r, n, NR))
            if place_node(i, n) == r: offr[r] += (cnt - 1) * full + last
            else: offr[r] += cnt * full
        for r in range(NR): need[r] = max(need[r], offr[r] + 2)
        l += 1
    return need, per, nspan

def arena_bs_bytes(N, nterms, decimal=True):
    """the two parities' halves per region (pool_get's +1/8, arena_get's 2 MiB rounding), bytes per region"""
    need, per, nspan = region_need(N, nterms, decimal=decimal)
    out = []
    for r in range(NR):
        cap = need[r] + need[r] // 8 + 4096
        cap = (cap * 8 + (2 << 20) - 1) // (2 << 20) * (2 << 20) // 8
        out.append(2 * cap * 8)
    return out

def quarter_bytes(limbs): return ((limbs + 3) // 4 + 4095) // 4096 * 4096 * 8

def dm_layout(N, g, pool_log=31, decimal=True):
    """binsplit.c dm_layout: n_Q, k_mu, t1, the hole (t1's quarter), the dm need per device (bytes)"""
    lg = math.lgamma(N + 1.0) / LN10
    dl10 = 18.0 if decimal else 64.0 / math.log2(10.0)
    nq = math.ceil(lg / dl10) + 2; dl = math.ceil((lg - 50.0) / dl10) + 1
    k = nq + 1 + dl - nq + 2 + 1; tcap = max(nq + k, 2 * k) + 8
    hole1 = quarter_bytes(tcap); hole1 += hole1 // 64
    nq_s, k_s, tcap_s = (nq + g - 1) // g, (k + g - 1) // g, (tcap + g - 1) // g
    hole = quarter_bytes(tcap_s); hole += hole // 64
    if g == 1: hole = hole1
    piece = min((1 << pool_log) + 8, nq_s + k_s + 16)
    need = 2 * quarter_bytes(nq_s + nq_s // 10 + 8) + 2 * quarter_bytes(k_s + 4) + hole + quarter_bytes(piece)
    need += min(need // 8, 1 << 30)
    top = 4 * quarter_bytes(nq_s // 2 + nq_s // 20 + 8) + 2 * quarter_bytes(nq_s + nq_s // 10 + 8); top += top // 8 + hole   # v3: the top bs levels beside the tail
    need_v2 = need; need = max(need, top)
    return dict(nq=nq, k=k, tcap=tcap, hole=hole, thresh=hole - hole * 3 // 8, need_dev=need, need_v2=need_v2, t1_quarter=quarter_bytes(tcap))

def mn_groups(g, spec=None):
    """rns_dist.c mn_groups_parse: the group size per tree level from MN_GROUPS (a list, or the string), default the
    powers of two up to g then g itself (576: 2, 4, ..., 512, 576 -- the last level joins a 512-group and a 64-group)"""
    if g <= 1: return []
    if spec is None or spec == '':                                       # Phase 12 G: the default = the powers of two dividing g, then the odd part's prime factors ascending (576 -> 2 .. 64, 192, 576)
        out = []; v = 1
        while v * 2 <= g and g % (v * 2) == 0: v *= 2; out.append(v)
        rest = g // v; f = 3
        while rest > 1:
            while rest % f == 0: v *= f; out.append(v); rest //= f
            f += 2
        if not out or out[-1] != g: out.append(g)
        return out
    else:
        out = []
        vals = [int(x) for x in spec.split(',')] if isinstance(spec, str) else list(spec)
        for v in vals:
            if v < 2 or (out and v <= out[-1]): raise ValueError('MN_GROUPS: sizes must be > 1 and increasing')
            if v >= g: out.append(g); break
            if out and v % out[-1]: raise ValueError('MN_GROUPS: %d is not a multiple of %d' % (v, out[-1]))
            out.append(v)
        if not out or out[-1] != g: out.append(g)
        return out
    while v <= g: out.append(v); v *= 2
    if not out or out[-1] != g: out.append(g)
    return out

def level_children(g, spec=None):
    """per level: (size, [child sizes]) -- the children are the previous level's groups [k P, min((k+1) P, S)) inside
    the level's group [0, S) (node 0's group; every group of the level has the same shape except a clipped top)"""
    out = []; P = 1
    for S in mn_groups(g, spec):
        ch = []; k = 0
        while k * P < S: ch.append(min(P, S - k * P)); k += 1
        out.append((S, ch)); P = S
    return out

def mn_cap_log(g, pool_log=31):
    """rns_dist.c mn_logn_cap: 2^(min(31, pool_log) + floor(log2 g)) points per piece over g nodes (one less where the
    rounding of R / nr would not fit the pools: never for g <= 2304)"""
    lgt = 0
    while (2 << lgt) <= g: lgt += 1
    c = min(31, pool_log); logn = c + lgt
    if g & (g - 1):
        qm = mn_shape(1 << logn, g)[3]
        if qm > (1 << (c - 2)): logn -= 1
    return logn

def mn_shape(nc, g, logr_delta=0):
    """rns_dist.c mn_shape: logn, logR, logC and the largest per-rank plane (limbs) for nc limbs over g nodes"""
    nr = 4 * g; lg = 0
    while (1 << lg) < nr: lg += 1
    logn = 0
    while (1 << logn) < nc: logn += 1
    logn = max(logn, max(20, 2 * (5 + lg)))
    logR = logn // 2 + logr_delta; logR = max(logR, max(10, 5 + lg)); logR = min(logR, logn - 10); logC = logn - logR
    R = 1 << logR; C = 1 << logC
    qs = -(-R // nr) * C; qr = -(-C // nr) * R
    return logn, logR, logC, max(qs, qr)

def tree_need_dev_flat(nq_leaf, g, scratch_out=None, logr_delta=0):
    """binsplit.c tree_need_dev as the code sizes the arena at main 7aded87 (the 'flat' form): binary levels clipped to
    g, ONE transform of the level's whole product (logn from 2 N_A, no cap: q = n / (4 g) grows with the operands),
    two spill buffers of g x C x 4 limbs per APU.  This is the arena the code REQUESTS at init (binsplit_pregrow), so it
    is what the node maps whether or not rns_dist.c's grid (mn_logn_cap) would have formed smaller pieces -- at 576
    nodes it exceeds the node by itself above ~2e10 digits per node (results/M11.md)."""
    best = 0; L = 0; top_scratch = 0; top_q = 0
    while (1 << L) < g: L += 1
    for l in range(1, L + 1):
        gg = min(1 << l, g); half = 1 << (l - 1); gt = gg; nr = 4 * gt; lgt = 0
        while (1 << lgt) < gt: lgt += 1
        NA = nq_leaf * half + 8; nc = 2 * NA; logn = 0
        while (1 << logn) < nc: logn += 1
        logn = max(logn, max(20, 2 * (7 + lgt)))
        logR = logn // 2 + logr_delta; logR = max(logR, max(10, 7 + lgt)); logR = min(logR, logn - 10)
        n = 1 << logn; R = 1 << logR; C = n // R; rows = R // nr; q = n // nr
        share = (NA + half - 1) // half; share_c = (nc + gg - 1) // gg; win = share_c + share_c // 8 + 2 * R
        Sin = min(q, ((share - 1) // R + 2) * rows); Sc = min(q, ((win - 1) // R + 2) * rows)
        Sin = (Sin + 15) // 16 * 16; Sc = (Sc + 15) // 16 * 16; Smax = max(Sc, Sin)
        scratch = 2 * gg * Smax * 8 + gg * Sin * 8 + 2 * q * 8 + 2 * gg * C * 4 * 8 + quarter_bytes(win)
        live = 2 * quarter_bytes(share + share // 8) + 2 * quarter_bytes(share_c + share_c // 8)
        best = max(best, live + scratch); top_scratch = scratch; top_q = q
    if scratch_out is not None: scratch_out.append(top_scratch); scratch_out.append(top_q)
    return best + best // 16

def split_grid_cap(na, nb, cap, minpts):
    """rns_dist.c split_grid_cap (the mn tier: 2^k planes): the grid (ka, kb) with the fewest plane points, then the fewest pieces"""
    best = None
    for i in range(1, 33):
        for j in range(1, 33):
            pa = -(-na // i); pb = -(-nb // j)
            if pa + pb > cap: continue
            pts = 1 << 20
            while pts < pa + pb: pts <<= 1
            pts = max(pts, minpts); cost = i * j * pts
            if best is None or cost < best[0] or (cost == best[0] and i * j < best[1] * best[2]): best = (cost, i, j)
    return best[1], best[2]

def mn_scratch(na, nb, has_x, g, share_a, share_b, share_c, pool_log=31, logr_delta=0):
    """rns_dist.c rns_mul_dist_mn_scratch (Phase 12 agent G -- the code's own formula, which binsplit.c's arena layout calls):
    the block-pool bytes per device at the peak of C = A B (+ X) over g nodes, for shares of share_a, share_b, share_c
    limbs.  The grid's largest piece at the group's cap (mn_logn_cap; the plane pools stay at their init size); mn_core's
    buffers: the received sequences rbA, rbB (and rbX on one plane: a grid adds X afterwards by mdb_add_shifted) of at most
    qs = my rows x C limbs, the packed part sb (my share's limbs on APU d's ranks: about a quarter of my part of the piece),
    cx (X, one plane only) and tmp (q; the equal-part path only); then the result exchange's rbO (my window's part), the
    exact spills (~ 4 C limbs per APU + 4 per source rank, whatever g) and the window temporary T (a quarter of the
    window, at most share_c).  Returns (bytes, pieces = ka x kb)."""
    if not na or not nb or g < 2: return 0, 0
    cap = 1 << mn_cap_log(g, pool_log); ka = kb = 1
    nr = 4 * g; lg = 0
    while (1 << lg) < nr: lg += 1
    if na + nb > cap: ka, kb = split_grid_cap(na, nb, cap, 1 << max(20, 2 * (5 + lg)))
    pa = -(-na // ka); pb = -(-nb // kb); nc = pa + pb
    logn, logR, logC, q = mn_shape(nc, g, logr_delta)
    R = 1 << logR; C = 1 << logC; rows = -(-R // nr); qs = rows * C
    def RT(ln): return min((-(-ln // R) + 1) * rows, qs)
    va = min(share_a, pa); vb = min(share_b, pb); sb = max(va, vb) // 4 + 2 * g * rows
    xin = has_x and ka * kb == 1
    win = min(share_c, nc); xq = q if xin else 0; tmp = q if (g & (g - 1)) == 0 else 0
    peak1 = RT(pa) + RT(pb) + (RT(nc) if xin else 0) + sb + xq + tmp + 16 * g
    peak2 = win // 4 + 2 * g * rows + 4 * C + 4 * g + xq + tmp
    return max(peak1, peak2) * 8 + 32 * g * 8 + quarter_bytes(win), ka * kb

def tree_need_dev(nq_leaf, g, scratch_out=None, logr_delta=0, form='grid', pool_log=31, groups=None):
    """the largest tree level's live shares + rns_mul_dist_mn's scratch, per device (bytes); scratch_out[0] = the top
    level's scratch alone (the sharded division's products carry the same), [1] = its per-rank plane q.
    form 'flat': the arena formula of the code before Phase 12 (tree_need_dev_flat above; kept for the before/after tables).
    form 'grid' (Phase 12 agent G: binsplit.c tree_need_dev as merged): the levels follow mn_groups (MN_GROUPS); a level
    of nch children of P nodes is combined by Horner from the top child (mn.c tree_level_k: (P, Q) <- (P_i Q + P, Q_i Q)
    for i = nch - 2 .. 0), its largest product P_0 x Q_run = P leaf shares x (nch - 1) P; the product's scratch is
    mn_scratch (the code's rns_mul_dist_mn_scratch: the pieces at the cap, the exact spills -- O(share) + O(q), no g-term);
    the live shares are the child's pair, the running pair (nch > 2) and the new pair, a quarter each + 1/8."""
    if form == 'flat': return tree_need_dev_flat(nq_leaf, g, scratch_out, logr_delta)
    best = 0; top_scratch = 0; top_q = 0
    for S, ch in level_children(g, groups):
        gg = S; nch = len(ch); P = ch[0]
        if nch < 2: continue
        nqc = nq_leaf * P + 8; na = nqc; nb = nqc * (nch - 1); nc = na + nb
        share_child = nq_leaf + 8; share_run = -(-nb // gg); share_new = -(-nc // gg)
        scratch, pieces = mn_scratch(na, nb, 1, gg, share_child, share_run, share_new, pool_log, logr_delta)
        live = 2 * quarter_bytes(share_child + share_child // 8) + (2 * quarter_bytes(share_run + share_run // 8) if nch > 2 else 0) + 2 * quarter_bytes(share_new + share_new // 8)
        best = max(best, live + scratch); top_scratch = scratch; top_q = mn_shape(min(nc, 1 << mn_cap_log(gg, pool_log)), gg, logr_delta)[3]
    if scratch_out is not None: scratch_out.append(top_scratch); scratch_out.append(top_q)
    return best + best // 16

# ---------------------------------------------------------------- the other pools (measured constants where the code has them)
def planes_bytes(pool_log=31):
    """rns_mul.c: pool 0 = EC_NP (4) x q limbs with q = 2^pool_log / 4, pool 1 = 3 q + 16 limbs (C4), per APU; + the contexts"""
    q = (1 << pool_log) // 4
    per_apu = 4 * q * 8 + ((3 * q + 16) * 8 if pool_log > 30 else 4 * q * 8)   # pool 1 is the full pool at POOL_LOG <= 30 (results/A-mem.md, open issue 1)
    return NR * per_apu + int((0.61 if pool_log > 30 else 2.16) * GB)   # + the transform contexts: 0.61 GB at 2^31, 2.16 at 2^29 (measured)

HOST_RUNTIME = 7.0 * GB                                  # ROCm runtime + program ("other" 6.9 GB at 4e10, the same at 1e6)
HOST_STAGING = 4 * (1 << 30)                             # the checkpoints' chunk buffer, 1 GiB per APU (H's B2)
HOST_SEEDBUF = 2 * (2 << 30)                             # the two 2 GiB seed buffers, alive during init only
HOST_WRITER = int(0.65 * GB)                             # the writer's two 256 MB chunks + a 128 MB limb buffer
HOST_COMM_PER_PROC = 6.0 * GB                            # measured at 10^9 sizes 2/4: VmHWM 19.3 / 18.9 GB per process (TCP buffers, the comm's pinned slabs)

def exchange_scratch(nq_total, g, alltoallv):
    """the sharded division's exchange scratch per APU (rns_dist.c mdb_shift / mdb_add_shifted; PLAN 23-4):
    before B7 the padded g x share / 4 limbs per APU (mdb_shift) + g x 512 MB (mdb_add_shifted); after: the counts' own sizes"""
    if g == 1: return 0
    share = (nq_total + g - 1) // g
    if alltoallv: return 2 * (share // 4) * 8 + (64 << 20)
    return g * (share // 4) * 8 + g * (512 << 20)

def shmem_staging(nq_total, g, groups=None, pool_log=31, staging='cached', chunks=4):
    """the SHMEM transport's staging in its symmetric pool (comm_shmem.c: every exchange is copied through a send
    staging and a receive staging in the pool, sized to the exchange and KEPT per communicator -- `staging()` grows
    sst/rst and never frees them; the level communicators live to the end).  Bytes per node-process:
      'cached'       (the code at 7aded87): per tree level one mesh per APU thread, its largest exchange's send + receive
                     -- the result exchange of a piece at the level's cap (q limbs x 8 B each way; the transform chunks
                     are q / K, the redistributions <= q / 2), the top level's the larger of that and mdb_shift's share/4;
                     summed over the levels (they all stay alive), x 4 threads;
      'per_exchange' the staging freed after every wait (a small change in comm_shmem.c): the largest single exchange;
      'resident'     Phase 12 agent S's form: the callers' slabs allocated in the pool (comm_sym_alloc), no staging.
    The control blocks and mailbox are small (< 100 MB at 576 PEs)."""
    if g <= 1 or staging == 'resident': return 0
    nq_leaf = (nq_total + g - 1) // g
    per_level = []
    for S, ch in level_children(g, groups):
        cap = mn_cap_log(S, pool_log); m_last = ch[-1]; acc = S - m_last
        nc = nq_leaf * (acc + m_last) + 16
        q = mn_shape(min(nc, 1 << cap), S)[3]
        per_level.append(2 * q * 8)
    top = max(per_level[-1], 2 * (nq_leaf // 4) * 8)                  # the top level's mesh also carries the sharded division's shifts
    per_level[-1] = top
    if staging == 'per_exchange': return NR * max(per_level)
    return NR * sum(per_level)

# ---------------------------------------------------------------- the model
def mem_per_node(D, g=1, opts=None):
    """bytes per node-process (one per node, four APUs) for D digits per node in a run of g node-processes.
    opts: pool_log (31), tail (True: decision 5's layout, item 1), alltoallv (True: L's B7, merged in Phase 11),
          decimal (True), margin (0.0: a fraction added to the device total), logr_delta (DIST_LOGR_DELTA),
          form ('flat': today's g-sized spill buffers; 'grid': agent G's O(share) spills -- Phase 12),
          groups (MN_GROUPS: a list or string; None = the code's default schedule),
          transport ('tcp' | 'shmem': the SHMEM transport's symmetric pool -- the larger of COMM_SHMEM_POOL_MB (pool_mb, 8192)
          and the staging the transport needs (shmem_staging: staging = 'cached' (the code) | 'per_exchange' | 'resident'),
          in the node's HBM whether host-registered or a device heap).  Returns a dict with the parts and the peaks."""
    o = dict(pool_log=31, tail=True, alltoallv=True, decimal=True, margin=0.0, logr_delta=0, form='grid', groups=None, transport='tcp', pool_mb=8192, staging='cached'); o.update(opts or {})   # form: 'grid' is the code after Phase 12 G (the arena request follows rns_mul_dist_mn_scratch); 'flat' = before
    D_total = D * g; d = digits_of_run(D_total); N = e_terms(d); nterms = (N + g - 1) // g
    bs = arena_bs_bytes(N, nterms, decimal=o['decimal']); bs_total = sum(bs)
    L = dm_layout(N, g, o['pool_log'], o['decimal'])
    sc = []; tree = tree_need_dev((L['nq'] + g - 1) // g, g, sc, o['logr_delta'], o['form'], o['pool_log'], o['groups']) if g > 1 else 0
    if g > 1: L['need_dev'] += sc[0]                                       # the sharded division's products: the same slabs and spills
    want = max(L['need_dev'], tree)
    if o['tail']:
        arena = [max(b + (L['hole'] if o['tail'] == 'v1' else 0), want) for b in bs]   # binsplit_pregrow (v2): the bs halves or the dm / tree need per device; the tail is a policy over the last bytes (tail='v1': the hole added to the halves, the batch-1/2 runs of M11.md)
        pool_in_phase = 0
        pool_total = sum(arena)
    else:
        arena = bs
        # Phase 10's behaviour: the pool maps t1's quarter per APU inside the reciprocal, plus the byte deficit
        # (measured 141.0 / 231.8 / 265.7 GB of pool at 4 / 7 / 8e10 = 1.07-1.09 x the dm need; the tree's excess at size > 1)
        pool_in_phase = max(0, int(1.08 * NR * L['need_v2']) - bs_total) if g == 1 else max(0, NR * tree - bs_total) + NR * L['hole']
        pool_total = bs_total + pool_in_phase
    xchg = NR * exchange_scratch(L['nq'], g, o['alltoallv'])
    planes = planes_bytes(o['pool_log'])
    if g > 1 and sc[1] > (1 << o['pool_log']) // 4:                       # (never with the mn tier's cap: kept for a lowered cap)
        planes = NR * (4 * sc[1] * 8 + (3 * sc[1] + 16) * 8) + int(0.61 * GB)
    dev_init = planes + bs_total
    # the exchange scratch comes from the block pool (db_pool_alloc): inside the arena while the dm shares + it fit, hipMalloc beyond
    live_dm = NR * L['need_dev'] + xchg
    if live_dm > pool_total: pool_in_phase += live_dm - pool_total; pool_total = live_dm
    dev_dm = planes + pool_total
    stg = shmem_staging(L['nq'], g, o['groups'], o['pool_log'], o['staging']) if (g > 1 and o['transport'] == 'shmem') else 0
    pool = max(o['pool_mb'] << 20, stg + (100 << 20)) if (g > 1 and o['transport'] == 'shmem') else 0
    comm = (HOST_COMM_PER_PROC + pool) if g > 1 else 0
    host_init = HOST_RUNTIME + HOST_STAGING + HOST_SEEDBUF + comm
    host_dm = HOST_RUNTIME + HOST_STAGING + HOST_WRITER + comm
    peak = max(dev_init + host_init, dev_dm + host_dm) * (1 + o['margin'])
    return dict(D=D, g=g, N=N, digits=d, nq=L['nq'], t1_quarter=L['t1_quarter'], hole=L['hole'],
                planes=planes, regions_bs=bs_total, arena=sum(arena), dm_need=NR * L['need_dev'], tree_need=NR * tree, top_scratch=NR * sc[0] if g > 1 else 0,
                pool_in_phase=pool_in_phase, pool_total=pool_total, exchange=xchg, shmem_staging=stg, shmem_pool=pool,
                dev_init=dev_init, dev_dm=dev_dm, host_init=host_init, host_dm=host_dm, host_hwm=max(host_init, host_dm),
                node_peak=peak)

def max_digits_per_node(node_bytes, g=1, opts=None, lo=1e9, hi=4e11):
    """the largest D (to 10^8) whose node_peak fits node_bytes"""
    lo, hi = int(lo), int(hi)
    if mem_per_node(hi, g, opts)['node_peak'] <= node_bytes: return hi
    if mem_per_node(lo, g, opts)['node_peak'] > node_bytes: return 0
    while hi - lo > 1e8:
        m = (lo + hi) // 2
        if mem_per_node(m, g, opts)['node_peak'] <= node_bytes: lo = m
        else: hi = m
    return lo // 10**8 * 10**8

# ---------------------------------------------------------------- calibration and the report
MEASURED = [  # (D, g, phase peaks GB: planes, regions at init, pool total at the dm peak, device at the dm peak, host HWM) from results/M.md, H.md, M11.md
    (4e10, 1, dict(planes=120.3, regions=95.7, pool=141.0, dev_dm=261.9, host=11.7, tail=False, src='M.md/H.md (Phase 10)')),
    (7e10, 1, dict(planes=120.3, regions=169.6, pool=231.8, dev_dm=352.7, host=76.4, tail=False, src='M.md (seed-sized staging)')),
    (8e10, 1, dict(planes=120.3, regions=176.8, pool=265.7, dev_dm=386.6, host=90.0, tail=False, src='M.md (seed-sized staging)')),
    (4e10, 1, dict(planes=120.3, regions=136.0, pool=136.0, dev_dm=256.9, host=11.7, tail='v1', src='M11.md batch 1 (tail v1)')),
    (8e10, 1, dict(planes=120.3, regions=249.0, pool=249.0, dev_dm=369.9, host=12.8, tail='v1', src='M11.md batch 2 (tail v1)')),
    (1e11, 1, dict(planes=120.3, regions=333.1, pool=333.1, dev_dm=454.0, host=13.9, tail='v1', src='M11.md batch 2 (tail v1)')),
    (4e10, 1, dict(planes=120.3, regions=132.3, pool=132.3, dev_dm=253.1, host=11.7, tail=True, src='M11.md batch 3 (tail v2)')),
    (2.5e9, 4, dict(planes=34.4, regions=20.3, pool=20.3, dev_dm=56.8, host=29.3, tail=True, src='M11.md batch 3 (1e10 at size 4, POOL_LOG=29, per process)')),
]

def fmt(b): return '%7.1f' % (b / GB)

def main():
    print('== calibration (GB; model vs measured; "pool" = regions + the pool\'s hipMalloc at the dm peak)')
    print('%-8s %2s | %-22s | %8s %8s %8s %8s | %s' % ('D', 'g', 'item', 'planes', 'regions', 'pool', 'dev_dm', 'source'))
    for D, g, m in MEASURED:
        r = mem_per_node(int(D), g, dict(tail=m['tail'], pool_log=29 if g > 1 else 31))
        print('%-8.0e %2d | %-22s | %s %s %s %s | %s' % (D, g, 'measured', fmt(m['planes'] * GB), fmt(m['regions'] * GB), fmt(m['pool'] * GB), fmt(m['dev_dm'] * GB), m['src']))
        print('%-8s %2s | %-22s | %s %s %s %s | %s' % ('', '', 'model (tail %s)' % m['tail'], fmt(r['planes']), fmt(r['regions_bs'] if not m['tail'] else r['arena']), fmt(r['pool_total']), fmt(r['dev_dm']),
              'dev_dm %+.1f %%' % (100.0 * (r['dev_dm'] / GB / m['dev_dm'] - 1))))
        if m['tail']: continue
        r2 = mem_per_node(int(D), g, dict(tail=True))
        print('%-8s %2s | %-22s | %s %s %s %s | %s' % ('', '', 'model (tail on)', fmt(r2['planes']), fmt(r2['regions_bs']), fmt(r2['pool_total']), fmt(r2['dev_dm']), 'node peak %.1f' % (r2['node_peak'] / GB)))
    print()
    print('== the per-node profile (GB) at size 1, tail layout on (item 1): device at init / at the dm peak, host HWM, node peak')
    for D in [4e10, 7e10, 8e10, 9e10, 1e11, 1.1e11, 1.2e11]:
        r = mem_per_node(int(D), 1); r0 = mem_per_node(int(D), 1, dict(tail=False))
        print('  D %.1e: n_Q %.2e limbs, t1 quarter %.1f; regions(bs) %.1f, dm need %.1f -> arena %.1f; device init %.1f, dm %.1f; host %.1f; node peak %.1f  (tail off: pool %.1f, dm %.1f, peak %.1f)' % (
            D, r['nq'], r['t1_quarter'] / GB, r['regions_bs'] / GB, r['dm_need'] / GB, r['arena'] / GB, r['dev_init'] / GB, r['dev_dm'] / GB, r['host_hwm'] / GB, r['node_peak'] / GB,
            r0['pool_total'] / GB, r0['dev_dm'] / GB, r0['node_peak'] / GB))
    print()
    print('== ceilings per node (Phase 12 Q): the largest D whose node peak fits, by tree form (flat = the code at 7aded87: the arena')
    print('   it requests for one uncapped transform + g x C x 4-limb spill buffers; grid = agent G: the pieces at mn_logn_cap, O(C)')
    print('   spills), tail layout and alltoallv on, the SHMEM transport\'s host pool; 502 GB = the node, 480 GB = the safe budget')
    for form in ['flat', 'grid']:
        for g in [1, 4, 64, 576]:
            cells = []
            for node in [502 * GB, 480 * GB]:
                Dm = max_digits_per_node(node, g, dict(form=form, transport='shmem'))
                r = mem_per_node(Dm, g, dict(form=form, transport='shmem'))
                cells.append('%.1e (peak %5.1f: device %5.1f = planes %5.1f + arena %5.1f [top scratch %5.1f] + exchange %4.1f; host %4.1f) -> %.2e digits' % (
                    Dm, r['node_peak'] / GB, r['dev_dm'] / GB, r['planes'] / GB, r['arena'] / GB, r['top_scratch'] / GB, r['exchange'] / GB, r['host_dm'] / GB, Dm * g))
            print('  %-4s g %4d: 502 GB: %s\n              480 GB: %s' % (form, g, cells[0], cells[1]))
    print()
    print('== 576 nodes (PLAN 25): the maximum digits = 576 x the per-node ceiling at g = 576')
    for form, groups in [('flat', None), ('grid', None), ('grid', '2,4,8,16,32,64,576'), ('grid', '2,4,8,16,32,64,192,576')]:
        Dm = max_digits_per_node(502 * GB, 576, dict(form=form, groups=groups, transport='shmem'))
        print('  form %-4s MN_GROUPS %-28s: D per node %.1e -> %.2e digits over 576 nodes' % (form, groups or '(default)', Dm, 576 * Dm))

if __name__ == '__main__':
    main()

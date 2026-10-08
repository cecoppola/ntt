#!/usr/bin/env python3
"""mem_model.py - the per-node memory model of ecalc (Phase 11, agent M; PLAN.md 26 row M, item 3).

Device and host bytes of one node-process as functions of the digits per node D and of the group size g
(the number of node-processes the run is spread over), built from the same formulas the code sizes its
allocations with (binsplit.c: e_terms, seed_limbs, region_need, dm_layout, tree_need_dev; rns_mul.c: the
plane pools; rns_dist.c: the sharded exchange's scratch), calibrated against the mem_report tables of
results/M.md, results/M11.md (4, 7, 8 x 10^10 at size 1; 10^10 at size 4).

    mem_per_node(D, g, opts) -> dict      (bytes; opts: pool_log, tail, alltoallv, margin, form, groups, transport ...)
    python3 mem_model.py                  prints the calibration table, the ceilings per node and the 576-node digits
    python3 mem_model.py --p15            Phase 15: the defaults against the measured runs, the target, the ceilings
    python3 mem_model.py --check-c FILE [POOL_LOG [MN_T_CHUNK_MB [BS_SEED_FILL [BS_ARENA_ROOM [ECALC_NP [RNS_POOL1_4Q]]]]]]   the C layout (BS_LAYOUT_ONLY) against this port (MN_T_CHUNK_MB 1024, BS_SEED_FILL 128, BS_ARENA_ROOM 0.16 (the default since 2026-09-28),
                                          ECALC_NP 3 / 4 / auto as the run's (auto read from the file's plan lines) (the `planes:` lines: the plane pools at every cap, Phase 15 PS), RNS_POOL1_4Q 1)

Phase 15 (agent MD): mem_per_node's defaults are the code's since Phase 14 (DEFAULTS15: DM_TIGHT, MN_TREE_EARLY_FREE, MN_T_CHUNK_MB and
MDB_SHIFT_CHUNK_MB 1024, the SHMEM pool from the plan, DB_POOL_VMM's host and bs terms, ECALC_PLANE_CAP 2^31); OLD13 gives the forms before,
TARGET_LAUNCH the target's launch line (COMM_SHMEM_ROUND_MB=1024, the user's D2).
Phase 15 (agent DOC, 2026-09-27): the base is DEFAULTS15B -- + BS_SEED_FILL=128 (the seed span per run, seed_terms_for: the bs regions hold one
more batch level; --check-c exact against BS_LAYOUT_ONLY at 4e10, 1e11 and the target's share) and MN_OUT_EARLY (HOST_EARLY at size > 1);
DEFAULTS15 is B0.  TARGET_NP = 4: the target's launch line (ECALC_NP=4).
B7ACCT (2026-10-06): vslot_resident -- the general map's v-exchange slots (comm_layered.c, hipMalloc'd beside the arena) on dev_bs / dev_dm;
--check-c compares the C `vslot:` line (per APU, the tree top, device / node with them); ECALC_VSLOT_BUDGET as the C.
Phase 15 (agent DOC2, the user's decisions of 2026-09-28): the base is DEFAULTS15C = DEFAULTS15B + BS_ARENA_ROOM=0.16 (ARENA_ROOM, the code's
default since be2eec3); DEFAULTS15B / DEFAULTS15 / OLD13 carry arena_room 0 (B1, B0 and before).  TARGET_NP = 'auto' (the launch line's
ECALC_NP=auto, decision 1).  The mn transform cache: cache_fit_slots follows the code's RNS_DIST_CACHE_FIT rule (rns_dist.c
rns_dist_cache_plan: ECALC_NODE_GB less the layout's node -- binsplit_node_bytes: planes + arena + the init host -- less the 32 GB reserve,
over one slot per node): 0 at the target, as the `plan cache` line prints.  --check-c defaults to BS_ARENA_ROOM 0.16 and reads ECALC_NP=auto
from the file's `plan` lines.

Agent X's mn_model.py and Q's estimate.py import mem_per_node for their memory rows.  Every number is "modelled"
unless the calibration table says "measured"; the tables' sources are named in results/M11.md.  Phase 12 (agent Q):
the tree's g-terms in both forms -- 'flat' (the code at 7aded87) and 'grid' (agent G's gridded top product, PLAN 27) --
and L's level schedule (mn_groups, MN_GROUPS); see results/Q.md.
"""
import math, sys, functools, os

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

SEED_FILL = 128                          # Phase 15 (the user's decision of 2026-09-27): BS_SEED_FILL=128 is the code's default (0 = the fixed span of 256)

def seed_terms_for(bend, fill=SEED_FILL, S0=256, decimal=True):
    """binsplit.c bs_seed_terms_for (Phase 15 T2): the largest S whose last span [bend - S, bend) has at most `fill` limbs;
    fill 0 = the fixed span S0.  The C code works in long double; the floor below agrees wherever --check-c has compared it"""
    if fill <= 0 or bend < 3: return S0
    dpl = 18.0 if decimal else 64.0 * math.log10(2.0); lb = math.lgamma(bend)
    lim = lambda S: int(math.floor((lb - math.lgamma(bend - S)) / LN10 / dpl)) + 1
    S = int(fill * dpl / math.log10(bend)); S = max(1, min(S, bend - 2))
    while S > 1 and lim(S) > fill: S -= 1
    while S + 2 < bend and lim(S + 1) <= fill: S += 1
    return S

def seed_span(N, g, fill=SEED_FILL):
    """the span S the layout uses for a run of N terms over g node-processes: node 0's terms [1, 1 + N / g) at g > 1 (binsplit_layout_only,
    mn_plan.c), [1, N + 1) at size 1"""
    return seed_terms_for(N + 1 if g <= 1 else 1 + N // g, fill)

def seed_limbs(N, nterms, S=256, decimal=True):
    """per (limbs per span, the bound), nspan for a node computing nterms of the N-term series"""
    nspan = (nterms + S - 1) // S
    per = (S * math.ceil(math.log2(N + 2.0)) + 128) // (59 if decimal else 64) + 2
    return per, nspan

def region_of(i, n): r = i * NR // n; return r if r < NR else NR - 1
def place_node(i, n, balance_n=16): return region_of(i, n) if n > balance_n else i % NR

POOL_RULE = int(os.environ.get('BS_POOL_RULE', '1'))     # Phase 16 R: 1 = the tier prediction from the lower bound on the level's largest Q (binsplit.c level_q_lower); 0 = 0.9 x the bound

def level_q_lower(a0, nterms, S, m_full, nspan, decimal=True):
    """binsplit.c level_q_lower: a lower bound (limbs) on the largest Q of a level whose nodes cover m_full spans of S terms --
    the last full node's product over its own terms [t0, t1), lgamma(t1) - lgamma(t0) in limbs (bs_seed_terms_for's measure)"""
    nfull = nspan // m_full
    if not nfull: return 0
    t0 = a0 + (nfull - 1) * m_full * S; t1 = min(t0 + m_full * S, a0 + nterms)
    dpl = 18.0 if decimal else 64.0 * math.log10(2.0)
    return int(math.floor((math.lgamma(t1) - math.lgamma(t0)) / math.log(10.0) / dpl))

def region_need(N, nterms, mdev_logl=30, decimal=True, S=256, a0=1, rule=None):
    """binsplit.c region_need: the limbs each region must hold over the batch levels (the simulated layout).
    Closed forms per region instead of the C loop over the nodes: every node but the last covers full spans.
    a0: the node's first term (rank 0: 1); rule: BS_POOL_RULE (Phase 16 R; None = the environment's, default 1)"""
    if rule is None: rule = POOL_RULE
    per, nspan = seed_limbs(N, nterms, S, decimal=decimal)
    r0 = [min(nspan, -(-r * nspan // NR)) for r in range(NR + 1)]           # the first span of region r
    need = [2 * per * (r0[r + 1] - r0[r]) + 2 for r in range(NR)]
    l = 0
    while True:
        n_in = (nspan + (1 << l) - 1) >> l
        if n_in <= 1: break
        m_full = 1 << l; max_nl = m_full * per + l
        est = level_q_lower(a0, nterms, S, m_full, nspan, decimal) if rule else int(0.9 * max_nl)   # Phase 16 R: the lower bound (rule 1) or 0.9 x the bound (rule 0)
        if 2 * est + 1 > (1 << mdev_logl): break                          # the mdev (device-number) levels
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

def arena_bs_bytes(N, nterms, decimal=True, S=256):
    """the two parities' halves per region (pool_get's +1/8, arena_get's 2 MiB rounding), bytes per region; S: the seed span (seed_span)"""
    need, per, nspan = region_need(N, nterms, decimal=decimal, S=S)
    out = []
    for r in range(NR):
        cap = need[r] + need[r] // 8 + 4096
        cap = (cap * 8 + (2 << 20) - 1) // (2 << 20) * (2 << 20) // 8
        out.append(2 * cap * 8)
    return out

def quarter_bytes(limbs): return ((limbs + 3) // 4 + 4095) // 4096 * 4096 * 8
VMM_CHUNK = 2 << 30                      # dbig.c db_pool_vmm_chunk: DB_POOL_VMM_CHUNK_GB (2 GiB)
AS_HOST = 7000000000 + (8 << 30)         # binsplit.c binsplit_node_bytes: BS_HOST_INIT_BYTES, size 1 only (size > 1: room_host, B5 fix -- see layout_node)
def room_host(N, g, pool, seedbuf, out_early=True):
    """Phase 15 DL: binsplit.c as_room_fits's host (bytes) -- this model's host HWM: size 1 host_size1_vmm with the digits as log10 N!; size > 1
    max(host_init, host_dm) = the runtime + the staging + the transport + the SHMEM pool + max(the two VMM seed buffers, the writer + MN_OUT_EARLY)"""
    if g == 1:
        D = math.lgamma(N + 1.0) / LN10 / 1e9 - 40.0
        return int((25.85 + 0.0208 * (D if D > 0 else 0.0)) * 1e9)
    base = int(HOST_RUNTIME) + HOST_STAGING + int(HOST_COMM_PER_PROC) + pool
    return max(base + seedbuf, base + HOST_WRITER + (HOST_EARLY if out_early else 0))
def as_room_fits(planes, arena_with_room, host, node_gb=480.0):
    """binsplit.c as_room_fits (Phase 15 AS; DL: the node as this model counts it): BS_ARENA_ROOM's room only if the node with it -- the planes
    + the arena with the room + the VMM pool's bs growth + room_host -- fits the budget (before DL: + 7 GB + 8 GiB (+ 6 GB) + a 10 GB margin)"""
    return planes + arena_with_room + int(VMM_BS_GROW) + host <= node_gb * 1e9
def arena_of(base, want, chunk=0):
    """binsplit.c arena_get: the two parities (base) + the extra up to the dm / tree need, rounded up to 2 MiB.
    Phase 15 AS: chunk > 0 (BS_ARENA_ROOM with the VMM pool, binsplit.c as_arena) -- the arena in whole chunks"""
    ex = max(0, want - base); a = base + (ex + (2 << 20) - 1) // (2 << 20) * (2 << 20)
    return (a + chunk - 1) // chunk * chunk if chunk else a

def dm_layout(N, g, pool_log=31, decimal=True, tight=False, tail_dead=0, anchor=True, room=0.0, dkm=False, mdev_logl=30, lean=False):
    """binsplit.c dm_layout: n_Q, k_mu, t1, the hole (t1's quarter), the dm need per device (bytes).
    Phase 14 L1 (APUMULT_STUDY E2 / E5): tight = DM_TIGHT (the reciprocal's r2 at 2 jl + 4 and t1 at Q_t r's size at the last doubling
    jl = ceil(k/2), the top level's pairs freed as consumed), tail_dead = DM_TAIL_DEAD (1: v3 without the hole; 2: v2 without P too, the
    E5 layout that needs the spill); the division's own set (S, Q, mu + t or X + xq, a piece) is a term of v2 in every variant (it never
    binds in the default one).  Returns v2 / v3 / div per device beside the need.
    Phase 15 DL (built: binsplit.c dm_layout follows NEWTON_DKM): dkm = NEWTON_DKM=1 -- the reciprocal to h, DKM's division set, the hole =
    DKM's largest block (size 1: step 1's xq = X_hi Q, k1 + n_Q + 8 limbs reserved by mul_grid; size > 1: t, k + 8); taken at size > 1 when
    k >= 5, at size 1 only in the device flow (n_Q >= 2^mdev_logl x 9/8, BS_MDEV_LOGL); 'dkm' in the result says whether it was taken."""
    lg = math.lgamma(N + 1.0) / LN10
    dl10 = 18.0 if decimal else 64.0 / math.log2(10.0)
    nq = math.ceil(lg / dl10) + 2; dl = math.ceil((lg - 50.0) / dl10) + 1
    k = nq + 1 + dl - nq + 2 + 1; tcap = max(nq + k, 2 * k) + 8
    dkm = bool(dkm) and k >= 5 and (g > 1 or nq >= (1 << mdev_logl) + (1 << mdev_logl) // 8)   # DL: where the code's division takes DKM
    lean = bool(lean) and dkm and g > 1                                        # int15k: DM_MN_LEAN counts at size > 1 only (binsplit.c dm_layout)
    kr = k // 2 + 1 if dkm else k                                              # Phase 15 DKM (results/DKM15.md 1.4): the reciprocal to h = floor(k/2) + 1
    k1 = k - k // 2                                                            # DL: step 1's quotient limbs
    jl = (kr + 1) // 2 if anchor else kr - 1
    take = min(2 * jl + 2, nq); t1a = take + (jl + 2) + 8                      # r has j + 2 limbs (measured, job 21131)
    nq_s, k_s, jl_s = (nq + g - 1) // g, (k + g - 1) // g, (jl + g - 1) // g
    if tight:                                                                  # third form: the hole = t's block (2k + 8), holding r + t1 in the reciprocal
        tcap = 2 * k + 8 if not dkm else (nq + k1 + 8 if g == 1 else k + 8)   # (DKM: size 1 step 1's xq = X_hi Q, reserved whole; size > 1 t = (k1 + 1) + (k1 + 1) ~ k limbs)
        hole = max(quarter_bytes(-(-tcap // g)), quarter_bytes(-(-t1a // g)) + quarter_bytes(jl_s + 4))
    else:
        hole = quarter_bytes(-(-tcap // g)) if g > 1 else quarter_bytes(tcap)
    hole += hole // 64
    piece = min((1 << pool_log) + 8, nq_s + k_s + 16)
    qp = quarter_bytes(nq_s + nq_s // 10 + 8)
    if not tight: v2 = 2 * qp + 2 * quarter_bytes(k_s + 4) + hole + quarter_bytes(piece)
    else: v2 = 2 * qp + quarter_bytes(2 * jl_s + 4) + hole + quarter_bytes(piece)
    if tail_dead >= 2: v2 -= qp
    hi = 2 * qp + quarter_bytes(k_s + 1) + quarter_bytes(2 * k_s + 8)
    lo = (qp + quarter_bytes(k_s) + quarter_bytes(nq_s + 2) + quarter_bytes(nq_s + k_s + 8)) if tight else (2 * qp + quarter_bytes(k_s) + quarter_bytes(nq_s + k_s + 8))
    if dkm:                                                                    # DKM: step 1's high product Q + S + mu (h) + t (k); step 2's Q + X_hi (k/2)
        h_s = (k // 2 + 1 + g - 1) // g                                        # + R1 (n_Q) + mu + t; step 2's low product Q + X + Aw + xq (n_Q + k/2) + X_lo
        hi = max(2 * qp + quarter_bytes(h_s + 1) + quarter_bytes(k_s + 8), qp + quarter_bytes(h_s) + quarter_bytes(nq_s + 2) + quarter_bytes(h_s + 1) + quarter_bytes(k_s + 8))
        lo = qp + quarter_bytes(k_s) + quarter_bytes(nq_s + 2) + quarter_bytes(nq_s + h_s + 8) + quarter_bytes(h_s)   # X, Aw, X_lo Q (n_Q + k/2), X_lo
        if lean: lo = 3 * quarter_bytes(nq_s + 2) + quarter_bytes(k_s + 1) + quarter_bytes(h_s + 1)   # int15k (DM_MN_LEAN, size > 1): Qw + Aw + xql (w each) + X (k; the X0 form) + X_lo (h)
    div = quarter_bytes(piece) + max(hi, lo)
    v2 = max(v2, div)
    v2 += min(v2 // 8, 1 << 30)
    inn = quarter_bytes(nq_s // 2 + nq_s // 20 + 8); out = qp
    top = 4 * inn + 2 * out if not tight else max(4 * inn + out, 2 * inn + 2 * out)
    top += top // 8 + (0 if tail_dead else hole)                                      # v3: the top bs levels beside the tail
    need = max(v2, top)
    room_b = int(room * float(hole)) if room > 0 else 0                  # Phase 15 AS: BS_ARENA_ROOM=<room> (with the VMM pool): room x the hole in the dm need
    need += room_b
    return dict(nq=nq, k=k, tcap=tcap, hole=hole, thresh=hole - hole * 3 // 8, need_dev=need, need_v2=v2, v2=v2, v3=top, div=div, jl=jl, t1_quarter=quarter_bytes(tcap), room=room_b, dkm=dkm, lean=lean)

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

@functools.lru_cache(maxsize=None)
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

# MS (Phase 15): P24's branch of rns_mul_dist_mn_scratch (MN_P24, results/P2415.md; rns_dist.c mn_p24_of / p24_grid_shape / p24_split).
# p24 = (mode, np, g_run, auto_min): mode 2 = every mn product when pool 0 holds four planes (rns_pool0_np: np_planes(np, g_run) >= 4);
# 1 = the products whose 18-digit grid's largest piece runs four primes (ECALC_NP=4: all; auto: pa + pb, or min(pa, pb) under
# ECALC_NP_AUTO_MIN=1, over the three-prime bound).  +34 MB of dm need per device at 5.1e13 on 576, measured by the layout.
P24_CAP_LOG = 40
def p24_pts(n): return -(-3 * n // 4)
def mn_logmin(g):
    nr = 4 * g; lg = 0
    while (1 << lg) < nr: lg += 1
    return max(20, 2 * (5 + lg))
def mn_p24_of(na, nb, g, p24, pool_log=31):
    """rns_dist.c mn_p24_of"""
    if not p24 or not p24[0]: return False
    mode, np, g_run, auto_min = p24
    if mode >= 2: return np_planes(np, g_run, pool_log) >= 4
    pa, pb, cap = na, nb, 1 << mn_cap_log(g, pool_log)
    if na + nb > cap:
        if -(-na // 32) + -(-nb // 32) > cap: pa, pb = -(-na // 32), -(-nb // 32)
        else: ka, kb = split_grid_cap(na, nb, cap, 1 << mn_logmin(g)); pa, pb = -(-na // ka), -(-nb // kb)
    if np == 'auto': return (min(pa, pb) if auto_min else pa + pb) > NP3_MAX_TERMS
    return np == 4
def p24_grid_shape(na, nb, g, pool_log=31):
    """rns_dist.c p24_grid_shape / p24_split: (ka, kb) on the points at min(the group's cap, 2^40)"""
    cap = 1 << min(mn_cap_log(g, pool_log), P24_CAP_LOG)
    if p24_pts(na) + p24_pts(nb) <= cap: return 1, 1
    minpts = 1 << mn_logmin(g); best = None
    for i in range(1, 33):
        for j in range(1, 33):
            pa, pb = -(-na // i), -(-nb // j); n = p24_pts(pa) + p24_pts(pb)
            if n > cap: continue
            pts = 1 << 20
            while pts < n: pts <<= 1
            pts = max(pts, minpts); cost = i * j * pts
            if best is None or cost < best[0] or (cost == best[0] and i * j < best[1] * best[2]): best = (cost, i, j)
    return best[1], best[2]

def mn_scratch(na, nb, has_x, g, share_a, share_b, share_c, pool_log=31, logr_delta=0, t_chunk_mb=0, p24=None):
    """rns_dist.c rns_mul_dist_mn_scratch (Phase 12 agent G -- the code's own formula, which binsplit.c's arena layout calls):
    the block-pool bytes per device at the peak of C = A B (+ X) over g nodes, for shares of share_a, share_b, share_c
    limbs.  The grid's largest piece at the group's cap (mn_logn_cap; the plane pools stay at their init size); mn_core's
    buffers: the received sequences rbA, rbB (and rbX on one plane: a grid adds X afterwards by mdb_add_shifted) of at most
    qs = my rows x C limbs, the packed part sb (my share's limbs on APU d's ranks: about a quarter of my part of the piece),
    cx (X, one plane only) and tmp (q; the equal-part path only); then the result exchange's rbO (my window's part), the
    exact spills (~ 4 C limbs per APU + 4 per source rank, whatever g) and the window temporary T (a quarter of the
    window, at most share_c).  Returns (bytes, pieces = ka x kb)."""
    if not na or not nb or g < 2: return 0, 0
    if mn_p24_of(na, nb, g, p24, pool_log):                               # MS: P24's branch (the grid on the points; A's sequence freed before B's)
        ka, kb = p24_grid_shape(na, nb, g, pool_log)
        pa = -(-na // ka); pb = -(-nb // kb); nc = pa + pb
        logn, logR, logC, q = mn_shape(p24_pts(pa) + p24_pts(pb), g)
        nr = 4 * g; R = 1 << logR; C = 1 << logC; rows = -(-R // nr); rl = rows + rows // 3 + 2; qsl = rl * C
        def RT24(ln): return min((-(-p24_pts(ln) // R) + 1) * rl, qsl)
        va = min(share_a, pa); vb = min(share_b, pb)
        sba = va // 4 + va // R * g + 2 * g * rl; sbb = vb // 4 + vb // R * g + 2 * g * rl
        win = min(share_c, nc); tmp = q if (g & (g - 1)) == 0 else 0
        Wt = t_chunk_limbs(t_chunk_mb)
        if Wt and win > Wt: win = Wt
        peak1 = max(RT24(pa) + sba, RT24(pb) + sbb) + tmp + 16 * g
        peak2 = win // 4 + 2 * g * rl + 4 * C + 4 * g + tmp
        return max(peak1, peak2) * 8 + 32 * g * 8 + quarter_bytes(win), ka * kb
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
    Wt = t_chunk_limbs(t_chunk_mb)
    if Wt and win > Wt: win = Wt                                          # Phase 13a M: MN_T_CHUNK_MB -- rbO and T hold one chunk
    peak1 = RT(pa) + RT(pb) + (RT(nc) if xin else 0) + sb + xq + tmp + 16 * g
    peak2 = win // 4 + 2 * g * rows + 4 * C + 4 * g + xq + tmp
    return max(peak1, peak2) * 8 + 32 * g * 8 + quarter_bytes(win), ka * kb

def t_chunk_limbs(mb):
    """rns_dist.c mn_t_chunk_limbs: MN_T_CHUNK_MB per APU -> limbs per node per round (0: off)"""
    return int(mb * 1048576.0 / 8) * 4 if mb and mb > 0 else 0

def tree_need_dev(nq_leaf, g, scratch_out=None, logr_delta=0, form='grid', pool_log=31, groups=None, t_chunk_mb=0, early_free=False, p24=None):
    """the largest tree level's live shares + rns_mul_dist_mn's scratch, per device (bytes); scratch_out[0] = the top
    level's scratch alone (the sharded division's products carry the same), [1] = its per-rank plane q.
    form 'flat': the arena formula of the code before Phase 12 (tree_need_dev_flat above; kept for the before/after tables).
    form 'grid' (Phase 12 agent G: binsplit.c tree_need_dev as merged): the levels follow mn_groups (MN_GROUPS); a level
    of nch children of P nodes is combined by Horner from the top child (mn.c tree_level_k: (P, Q) <- (P_i Q + P, Q_i Q)
    for i = nch - 2 .. 0), its largest product P_0 x Q_run = P leaf shares x (nch - 1) P; the product's scratch is
    mn_scratch (the code's rns_mul_dist_mn_scratch: the pieces at the cap, the exact spills -- O(share) + O(q), no g-term);
    the live shares are the child's pair, the running pair (nch > 2) and the new pair, a quarter each + 1/8.
    early_free (Phase 14 T1, E10a, MN_TREE_EARLY_FREE=1): P_i, P_run freed between the level's two products -- the live set is
    max(2c + 2r + n, c + r + 2n) instead of 2c + 2r + 2n (c, r, n = the child's, the running, the new share's quarter + 1/8)."""
    if form == 'flat': return tree_need_dev_flat(nq_leaf, g, scratch_out, logr_delta)
    best = 0; top_scratch = 0; top_q = 0
    for S, ch in level_children(g, groups):
        gg = S; nch = len(ch); P = ch[0]
        if nch < 2: continue
        nqc = nq_leaf * P + 8; na = nqc; nb = nqc * (nch - 1); nc = na + nb
        share_child = nq_leaf + 8; share_run = -(-nb // gg); share_new = -(-nc // gg)
        scratch, pieces = mn_scratch(na, nb, 1, gg, share_child, share_run, share_new, pool_log, logr_delta, t_chunk_mb, p24)
        c = quarter_bytes(share_child + share_child // 8); r = quarter_bytes(share_run + share_run // 8) if nch > 2 else 0; n = quarter_bytes(share_new + share_new // 8)
        live = max(2 * c + 2 * r + n, c + r + 2 * n) if early_free else 2 * c + 2 * r + 2 * n
        best = max(best, live + scratch); top_scratch = scratch; top_q = mn_shape(min(nc, 1 << mn_cap_log(gg, pool_log)), gg, logr_delta)[3]
    if scratch_out is not None: scratch_out.append(top_scratch); scratch_out.append(top_q)
    return best + best // 16

# ---------------------------------------------------------------- B7ACCT (results/B7ACCT.md): the general map's v-exchange slots
def _partn(R, nr, rho): return R * (rho + 1) // nr - R * rho // nr

@functools.lru_cache(maxsize=None)
def vslot_shape(logR, logC, g, depth=2, chunks=4):
    """rns_dist.c vslot_shape: the layered communicator's v-exchange scratch (comm_layered.c need_vslot / need_vtmp) one general-map
    transform of a 2^logR x 2^logC plane over 4 g ranks grows on an APU thread, bytes (the max over the APU threads).  Per chunk k < K
    (gplan_build's K) and APU d of node r, rho = g d + r: forward A = sum_dd rk(g dd + r, k) x sum_r' cols(g d + r'), B = cols(rho) x
    sum rk(., k); inverse A = sum_dd cols(g dd + r) x sum_r' rk(g d + r', k), B = rk(rho, k) x C (points, x 8 B).  depth >= 2: two slots
    of A + max(A, B) + 4 at the largest exchange; depth 1: 2 A + B + 4; each area in whole 2 MiB."""
    nr = 4 * g; R = 1 << logR; C = 1 << logC; rmin = R // nr
    K = min(max(chunks, 1), 16)
    while K > 1 and rmin // K < 32: K -= 1
    rows = [_partn(R, nr, s) for s in range(nr)]; cols = [_partn(C, nr, s) for s in range(nr)]
    colcol = [sum(cols[g * d:g * d + g]) for d in range(4)]
    scol = [sum(cols[g * dd + r] for dd in range(4)) for r in range(g)]
    need = 0
    for k in range(K):
        rk = [rr * (k + 1) // K - rr * k // K for rr in rows]; tot = sum(rk); rkcol = [sum(rk[g * d:g * d + g]) for d in range(4)]
        for r in range(g):
            srk = rk[r] + rk[g + r] + rk[2 * g + r] + rk[3 * g + r]
            for d in range(4):
                rho = g * d + r
                Af = srk * colcol[d] * 8; Bf = cols[rho] * tot * 8; Ai = scol[r] * rkcol[d] * 8; Bi = rk[rho] * C * 8
                if depth >= 2: need = max(need, Af + max(Af, Bf) + 4, Ai + max(Ai, Bi) + 4)
                else: need = max(need, 2 * Af + Bf + 4, 2 * Ai + Bi + 4)
    need = -(-need // (2 << 20)) * (2 << 20)
    return 2 * need if depth >= 2 else need

def vslot_env():
    """the switches the C function reads: COMM_ALLTOALLV_DEPTH (2), DIST_CHUNKS (8), DIST_GEN (0)"""
    e = os.environ
    return int(e.get('COMM_ALLTOALLV_DEPTH', '2') or 2), int(e.get('DIST_CHUNKS', '8') or 8), e.get('DIST_GEN', '0') not in ('', '0')

def mn_vslot(na, nb, g, pool_log=31, p24=None, depth=None, chunks=None, gen_forced=None):
    """rns_dist.c rns_mul_dist_mn_vslot: one product's v-slots per APU thread (bytes); 0 for a power-of-two g (ntt_dist's transform)"""
    d0, k0, f0 = vslot_env(); depth = d0 if depth is None else depth; chunks = k0 if chunks is None else chunks; gen_forced = f0 if gen_forced is None else gen_forced
    if not na or not nb or g < 2 or ((g & (g - 1)) == 0 and not gen_forced): return 0
    if mn_p24_of(na, nb, g, p24, pool_log):
        ka, kb = p24_grid_shape(na, nb, g, pool_log); pa = -(-na // ka); pb = -(-nb // kb)
        _, logR, logC, _ = mn_shape(p24_pts(pa) + p24_pts(pb), g)
    else:
        cap = 1 << mn_cap_log(g, pool_log); ka = kb = 1
        if na + nb > cap: ka, kb = split_grid_cap(na, nb, cap, 1 << mn_logmin(g))
        pa = -(-na // ka); pb = -(-nb // kb)
        _, logR, logC, _ = mn_shape(pa + pb, g)
    return vslot_shape(logR, logC, g, depth, chunks)

def vslot_resident(nq, g, groups=None, pool_log=31, p24=None, depth=None):
    """binsplit.c binsplit_vslot_bytes: the v-slots resident per APU thread (bytes).  Every general-map group keeps its communicator's
    slots to the run's end (mn.c groups_finalize): the tree's levels add up (each level's largest product P_0 x Q_run, as tree_need_dev;
    a level whose last group is cut by the size: the larger of the two shapes), and the whole machine's group holds max(its tree level's,
    the dm phase's: A_h mu n_Q x n_Q and Q_t r n_Q x (n_Q / 2 + 1)).  Returns dict(per_apu = from the dm phase on, tree_top = during the
    tree's top level, levels = [(level, group, bytes)], dm)."""
    if g < 2: return dict(per_apu=0, tree_top=0, levels=[], dm=0)
    nq_leaf = -(-nq // g); lower = 0; top_tree = 0; levels = []; prev = 1
    for l, S in enumerate(mn_groups(g, groups), 1):
        gg = min(S, g); nch = -(-gg // prev); nqc = nq_leaf * prev + 8
        v = mn_vslot(nqc, nqc * (nch - 1), gg, pool_log, p24, depth) if nch >= 2 else 0
        if gg < g and g % gg:
            gc = g % gg; ncc = -(-gc // prev)
            if ncc >= 2: v = max(v, mn_vslot(nqc, nqc * (ncc - 1), gc, pool_log, p24, depth))
        if gg == g: top_tree = v
        else: lower += v
        if v: levels.append((l, gg, v))
        prev = S
    dm = max(mn_vslot(nq, nq, g, pool_log, p24, depth), mn_vslot(nq, nq // 2 + 1, g, pool_log, p24, depth))
    return dict(per_apu=lower + max(top_tree, dm), tree_top=lower + top_tree, levels=levels, dm=dm)

def vslot_pool_on(env=None):
    """X2 (results/XEFF.md item 2) COMM_LAYER_VSLOT_POOL=1: the v-slots come from the comm pool (comm_sym_alloc): counted in the pool need (shmem_pool vslot=),
    not as hipMalloc'd v-slots (binsplit.c vslot_pool_on)"""
    env = os.environ if env is None else env
    return env.get('COMM_LAYER_VSLOT_POOL', '0') not in ('', '0')

def inter2_on(env=None):
    env = os.environ if env is None else env
    return env.get('COMM_LAYER_INTER2', '0') not in ('', '0')

def vslot_budget_on(env=None):
    """ECALC_VSLOT_BUDGET (binsplit.c binsplit_vslot_budget_on; default 1 since 2026-10-07 (the user); was 0): the v-slots counted in the node budget (room, cache fit, budget check)"""
    env = os.environ if env is None else env
    return env.get('ECALC_VSLOT_BUDGET', '1') not in ('', '0') and not vslot_pool_on(env)   # X2: under VSLOT_POOL the pool counts them

# ---------------------------------------------------------------- the other pools (measured constants where the code has them)
def planes_3q30(pool_log=31, digits=0):
    """rns_mul.c rns_planes_3q30_default (Phase 12 I: the default): the 3 2^k planes on at 2^31 pools below 5e10 digits -- the
    run's digits, i.e. D x g at size g (ecalc.c passes the run's d), so never at 576 nodes"""
    return pool_log >= 31 and digits < 5e10

EC_NP = 3                                # Phase 13b step 0: three primes are the default for decimal limbs (ECALC_NP; binary limbs need 4)
NP3_MAX_TERMS = 58424467928              # crt.c ec_np3_max_terms: floor((p0 p1 p2 - 1) / (10^18 - 1)^2), the largest nc = pa + pb three primes hold

def np_planes(np, g=1, pool_log=31, auto_terms=NP3_MAX_TERMS):
    """Phase 15 NP (crt.c ec_np_planes, rns_mul.c rns_pool0_np): the planes plane pool 0 is made for under ECALC_NP=np -- np itself, or for
    'auto' (the prime count per product) 4 when the largest plane a run of g node-processes forms, 2^(min(31, pool_log) + floor(log2 g)) points,
    exceeds the switch-over (ECALC_NP_AUTO_TERMS, default the three-prime bound), else 3: at the target 4 (pool 0 as ECALC_NP=4), at size 1 3"""
    if np != 'auto': return np
    c = pool_log if 0 < pool_log < 31 else 31
    lg = g.bit_length() - 1 if g > 1 else 0
    return 4 if (1 << (c + lg)) > auto_terms else 3

# ---------------------------------------------------------------- Phase 13b agent D: the plane cap and the product strategy
CAPS = {'2^30': 1 << 30, '3*2^29': 3 << 29, '2^31': 1 << 31, '3*2^30': 3 << 30}   # the four plane caps of PLAN 31 (points)

def cap_name(cap):
    for k, v in CAPS.items():
        if v == cap: return k
    return '%d' % cap

def cap_pool(cap):
    """a plane cap (points) -> (POOL_LOG, 3 2^k planes on): 2^30 = POOL_LOG 30; 3 2^29 = POOL_LOG 30 + RNS_PLANES_3Q30=1;
    2^31 = POOL_LOG 31 (RNS_PLANES_3Q30=0); 3 2^30 = POOL_LOG 31 + RNS_PLANES_3Q30=1 (the code's default below 5e10 digits)"""
    r3 = cap & (cap - 1) != 0
    lg = (cap // 3 if r3 else cap).bit_length() - 1 + (1 if r3 else 0)
    return lg, r3

def code_cap(digits, pool_log=31):
    """the plane cap the code picks by itself: RNS_PLANES_3Q30's size rule (3 2^30 below 5e10 digits of the run at POOL_LOG 31)"""
    return (3 << (pool_log - 1)) if planes_3q30(pool_log, digits) else (1 << pool_log)

def plane_bytes_apu(strategy, n, np=EC_NP):
    """the plane bytes per APU a product of n points needs (results/S13.md, t_strategy measured at P = 4 and 3):
    C four-step (np + 3) n/4 x 8 (14 n at P = 4, 12 n at P = 3); B prime-per-APU 2 n x 8 = 16 n on each busy APU; B4 (B's
    3 P planes spread over the four APUs) 1.5 n x 8 = 12 n"""
    if strategy == 'B': return 16 * n
    if strategy == 'B4': return 12 * n
    return (np + 3) * n * 2

def pool_limbs(pool_log=31, p3q30=False, np=EC_NP, pool1_np=None):
    """(pool 0, pool 1) per APU in limbs: rns_plane_pool_bytes (agent P's one formula), 2 MiB-aligned.  Phase 15 PS: pool1_np = the
    one-node tiers' prime count (default np; 3 under ECALC_NP=auto): at four, pool 1 >= 4 q (the batch-local tier's B planes)"""
    q = (3 << (pool_log - 3)) if p3q30 else (1 << pool_log) // 4
    al = (2 << 20) // 8
    a = np * q; b = max(3 * q + 16, 1 << min(pool_log, 30))
    if (np if pool1_np is None else pool1_np) >= 4: b = max(b, 4 * q)
    return (a + al - 1) // al * al, (b + al - 1) // al * al

def b_planes(form, d, n, np=EC_NP):
    """rns_dist.c b_planes (agent B): the planes' sizes (limbs) of a product of length n on APU d in form B or B4"""
    if form == 'B': return [n, n] if d < np else []
    return [n, n // 2] if d < 3 else [n // 2, n // 2, n // 2]

def b_extra_limbs(form, n, pool_log=31, p3q30=False, np=EC_NP, pool1_np=None):
    """rns_dist.c b_place: the planes that fit neither pool (first fit, whole planes) come from one grow-only hipMalloc buffer
    per APU; returns [extra limbs per APU] (0 everywhere = the form fits the pools: what `auto` requires)"""
    if form == 'B4' and np != 3: form = 'B'
    c0, c1 = pool_limbs(pool_log, p3q30, np, pool1_np); out = []
    for d in range(NR):
        cap = [c0, c1]; used = [0, 0]; ex = 0
        for sz in b_planes(form, d, n, np):
            for r in (0, 1):
                if cap[r] - used[r] >= sz: used[r] += sz; break
            else: ex += sz
        out.append(ex)
    return out

def planes_bytes(pool_log=31, digits=0, p3q30=None, np=None, strategy='C', cap=None, pool1_np=None):
    """rns_mul.c: pool 0 = EC_NP x q limbs with q = 2^pool_log / 4, pool 1 = 3 q + 16 limbs (C4, 2 MiB-aligned), per APU; + the
    contexts.  Phase 13a M (TASKS 1.1): with the 3 2^k planes (the default below 5e10 digits at 2^31 pools, Phase 12 I) pool 0 is
    3 2^(pool_log-1) limbs (24 GiB) and pool 1 3 q + 16 at q = 3 2^(pool_log-3) (18 GiB): 180.4 GB per node instead of 120.3 --
    the model had the old pools (measured 4e10: planes 180.4, RESULTS 77).
    Phase 13b D: pool 0 is np x q (P3: 3/4 of it at three primes), pool 1 stays 3 q + 16 (one prime at a time; measured 4e10 at
    ECALC_NP=3: planes 154.6 GB); cap = the plane cap in points (overrides pool_log / p3q30, see cap_pool); strategy 'B' needs
    16 n bytes per APU at the cap (the pools grow to it: assumed, agent B's layout decides), 'B4' 12 n, 'C' / 'auto' the pools"""
    al = 2 << 20
    np = EC_NP if np is None else np
    if cap is not None: pool_log, p3q30 = cap_pool(cap)
    on = planes_3q30(pool_log, digits) if p3q30 is None else p3q30
    if on:
        q = 3 << (pool_log - 3)
    else:
        q = (1 << pool_log) // 4
    p0 = np * q * 8
    p1 = (3 * q + 16) * 8 if pool_log > 30 else max((3 * q + 16) * 8, 8 << min(pool_log, 30))   # pool 1 is the full pool at POOL_LOG <= 30 (results/A-mem.md, open issue 1)
    if (np if pool1_np is None else pool1_np) >= 4: p1 = max(p1, 4 * q * 8)   # Phase 15 PS (rns_plane_pool_bytes): four one-node primes -- the batch-local tier's
                                                                      # np Mmax L B limbs reach its plane cap 4 q (L = q, Mmax = 1); 3 q + 16 grew inside the bs phase (rc 6)
    p1 = (p1 + al - 1) // al * al
    per = p0 + p1
    extra = 0
    if strategy in ('B', 'B4'):                                       # rns_dist.c (agent B): what the pools lack for the largest product
        extra = 8 * sum(b_extra_limbs(strategy, 4 * q, pool_log, on, np, pool1_np))   # (the plane of the cap, 4 q points) comes from one grow-only
                                                                      # hipMalloc buffer per APU, kept to the end of the dm phase (at the dm peak)
    return NR * per + extra + int((0.61 if pool_log >= 30 else 2.16) * GB)   # + the transform contexts: 0.61 GB at 2^31, 2.16 at 2^29 (measured; 2^30 assumed = 2^31)

HOST_RUNTIME = 7.0 * GB                                  # ROCm runtime + program ("other" 6.9 GB at 4e10, the same at 1e6)
HOST_STAGING = 4 * (1 << 30)                             # the checkpoints' chunk buffer, 1 GiB per APU (H's B2)
HOST_SEEDBUF = 2 * (2 << 30)                             # the two 2 GiB seed buffers, alive during init only
HOST_WRITER = int(0.65 * GB)                             # the writer's two 256 MB chunks + a 128 MB limb buffer
HOST_COMM_PER_PROC = 6.0 * GB                            # measured at 10^9 sizes 2/4: VmHWM 19.3 / 18.9 GB per process (TCP buffers, the comm's pinned slabs)

K_CHUNKS_MEM = 8                                         # DIST_CHUNKS: the slab pipeline's chunks per transform exchange

def host_size1(D):
    """the host HWM of a size-1 run (bytes), fitted on the measured runs (mem summary / VmHWM of the `total` line):
    4e10 12.1 (six runs, NP 3 and 4), 8e10 12.8-13.0, 1e11 13.9-14.0 GB -- 12.1 + 0.031 GB per 10^9 digits above 4e10; below
    4e10 the init value 12.1 (the dm phase's host flows at <= 1e10 -- X and R on the host, 26-40 GB at 1e10 -- are NOT modelled:
    the calibration lists 1e10 as the exception; no ceiling is decided there)"""
    return int((12.1 + 0.031 * max(0.0, D / 1e9 - 40.0)) * GB)

# ---------------------------------------------------------------- Phase 15 (agent MD): the defaults since Phase 14 (ecalc/README.md)
# DB_POOL_VMM=1 (the arena a VMM range), DM_TIGHT following it, MN_TREE_EARLY_FREE=1, MN_T_CHUNK_MB=1024, MDB_SHIFT_CHUNK_MB=1024 (13c),
# COMM_SHMEM_POOL_AUTO=1 with the pool taken from MN_PLAN_ONLY's `plan pool` line (mnrun.sh, docs/TARGET.md 4: the pool = the need rounded up
# to 256 MiB, the device heap exactly the pool).  COMM_SHMEM_ROUND_MB is off in the code; the target's launch line adds 1024 (the user's D2):
# TARGET_LAUNCH.  OLD13 = the forms before Phase 14, for the historical tables (their numbers are unchanged with it).
DEFAULTS15 = dict(tight=True, early_free=True, t_chunk_mb=1024, shift_chunk_mb=1024, pool='plan', vmm=True, round_mb=0, cap=1 << 31, seed_fill=0, out_early=False, arena_room=0.0, dkm=False)   # cap: ECALC_PLANE_CAP 2^31 (13c)
OLD13 = dict(tight=False, early_free=False, t_chunk_mb=0, shift_chunk_mb=0, pool='max', vmm=False, round_mb=0, cap=None, seed_fill=0, out_early=False, pool1_np=3, arena_room=0.0, dkm=False)   # pool1_np 3: pool 1 at 3 q + 16 whatever the primes (the code before Phase 15 PS)
# Phase 15 (agent DOC, the user's decisions of 2026-09-27): DEFAULTS15B = the code's defaults now -- DEFAULTS15 + BS_SEED_FILL=128 (the seed span per run:
# the bs regions hold one more batch level; at size 1 the division then grows the arena, VMM_DM_GROW_FILL) + MN_OUT_EARLY=1 (the part file's host
# buffers held during the division, HOST_EARLY).  DEFAULTS15 stays B0 (the Phase 14 defaults) for the measured Phase 14 rows.
DEFAULTS15B = dict(DEFAULTS15, seed_fill=SEED_FILL, out_early=True)
# Phase 15 (agent DOC2, the user's decisions of 2026-09-28): DEFAULTS15C = the code's defaults now (main B2) -- DEFAULTS15B + BS_ARENA_ROOM=0.16 (binsplit.c
# bs_arena_room: the arenas in whole VMM chunks + 0.16 x the hole in the dm need; at size 1 the division no longer grows the pool: VMM_DM_GROW_FILL not
# applied).  DEFAULTS15B stays B1 (the defaults of 2026-09-27) for RESULTS 86's rows.
ARENA_ROOM = 0.16                                       # BS_ARENA_ROOM's code default since 2026-09-28 (be2eec3; was 0)
DEFAULTS15C = dict(DEFAULTS15B, arena_room=ARENA_ROOM)
# Phase 15 DL (the user's Batch 3 decisions of 2026-09-29): DEFAULTS15D = the code's defaults now -- DEFAULTS15C + NEWTON_DKM=1 with dm_layout following it
# (binsplit.c; NEWTON_DKM=0 in the environment gives C's layout).  MN_P24=2 is the tree's scratch (opts p24; mn_model passes it).  mem_per_node's base.
DKM_CODE = os.environ.get('NEWTON_DKM', '1') != '0'
LEAN_CODE = os.environ.get('DM_MN_LEAN', '0') not in ('', '0')   # int15k: DM_MN_LEAN (off by default) -- the lean mn division set at size > 1 (opts lean)
DEFAULTS15D = dict(DEFAULTS15C, dkm=DKM_CODE, lean=LEAN_CODE)
TARGET_LAUNCH = dict(round_mb=1024)                     # D2 (results/V114.md): the target's launch line
TARGET_NP = 'auto'                                      # the user's decision 1 of 2026-09-28: ECALC_NP=auto on the target's launch line (four primes only for the
                                                        # products over the three-prime bound; pool 0 at four planes at 576, pool 1 at three); history: 4 (the decision
                                                        # of 2026-09-27: three primes cannot hold the target's mn pieces, MN_PLAN_ONLY refuses them, results/P15.md;
                                                        # ECALC_NP=4 is +17.2 GB per node over auto with RNS_POOL1_4Q); the code's default stays 3 (decimal)
TARGET_DIGITS = 3.71e13                                 # CURRENT (2026-10-06, s18-target, the user's decision 2026-10-06 ~19:30 EDT: "3.71e13 is fine", the size
                                                        # is not important while the implementation is built) -- from results/TGTBENCH2.md section 4's lower
                                                        # candidate (the device edge's margin over the proposed 3.76e13): login-node MN_PLAN_ONLY / BS_LAYOUT_ONLY
                                                        # at 576 (verified here 2026-10-06, b7-vslot aa86cac): device_with 363.526 GB vs the 373.44 GB measured
                                                        # edge (9.91 GB under), node 378.785 / 405.612 GB with the general-map v-slots (480 budget: +74.4 GB),
                                                        # pieces (node 0 / critical path) 103 / 107 (1236 products), `plan check` OK; estimate.py --fabric target-m
                                                        # at this D/g: 221.9 s modelled without the output write / 225.1 s with it at --bw 47 (ROCm 7.2.4, MAP_RATE
                                                        # 0.010 measured on the target).
                                                        # history: 4.08e13 (TGT17, 2026-10-06, superseded by this decision -- device 372.12 GB, 1.32 GB under the
                                                        # edge, 282.4 s mod at --bw 47), 5.276e13 (the target from the user's decision of 2026-09-29, Batch 3, to
                                                        # 2026-10-06; Phase 15 TGT's 2026-09-27 23:50 EDT decision was 5.1e13; room kept up to 5.396e13), 5.167e13
                                                        # (int15k's second test size, 2026-10-03), 4.25e13 (Phase 13d D2 - 2026-09-27), 4.4e13 (Phase 13c).  The C
                                                        # plan (MN_PLAN_ONLY, ECALC_NP=4, results/TGT15/): 5.10e13 is the last size at 222 / 242 pieces (5.11 node 0
                                                        # 226, 5.12 critical 246).  TGTBENCH2 (2026-10-06) also proposed 3.76e13 (367.82 GB device, 5.62 under the
                                                        # edge, 222.7 s mod) as the largest layout tier <= 368 GB; the user picked 3.71e13 instead (more margin)
TARGET_BELOW = 3.6e13                                  # s18-target (2026-10-06, login-node MN_PLAN_ONLY on 576, the TGT17 launch line; verified here): one piece
                                                        # step below 3.71e13 in the same arena tier (206.158 GB device layout, 363.526 GB device_with v-slots, both
                                                        # unchanged from 3.5x10^13 to 3.7104x10^13) -- pieces node 0 / critical path 101 / 107 (1236 products)
                                                        # against 103 / 107 at 3.71e13 (the node-0 step is at 3.6160 -> 3.6180 x 10^13); the next arena tier down
                                                        # (201.863 GB device layout) starts at 3.52 -> 3.53 x 10^13 (94 / 102 pieces at <= 3.52e13).  Was 3.99e13
                                                        # (one step below the former 4.08e13 target)
TARGET_NODES = 576
VMM_DM_GROW_FILL = 16.3 * GB                            # MEASURED (RESULTS 86's paired 1e11 series, s24-16 / s24-26, ten runs with BS_SEED_FILL=128, all the same):
                                                        # the device at the dm peak 393.6 GB against 377.3 at init -- the division grows the block pool by 12.9 GB
                                                        # (277.0 -> 289.9) where the fixed span grew 2.1; applied at size 1 with the fill (the arena is the bs
                                                        # regions' there); at size > 1 the arena is the dm need's and the fill leaves it unchanged (ASSUMED: no growth)
HOST_EARLY = int(0.9 * GB)                              # MN_OUT_EARLY (results/IO15.md 6): 2 chunks + the pinned limbs + the O_DIRECT staging at 256 MB, held
                                                        # during the division's low product at size > 1 (modelled from the code's buffers)
POOLS = ('plan', 'auto', 'off', 'max')                  # plan: = the need (256 MiB steps; mnrun.sh / TARGET.md 4); auto: max(COMM_SHMEM_POOL_MB 8192, the need);
                                                        # off: COMM_SHMEM_POOL_AUTO=0, 8192 flat; max: the pre-Phase-15 model (max(8192, the need unrounded))
HOST_SEEDBUF_VMM = 8 << 30                               # DB_POOL_VMM: seeds_stream's two pinned buffers of min(BS_SEED_CHUNK_MB 8192, the largest region's spans) each
                                                        # (binsplit.c; measured 21.5 GB pinned at init = 4 GiB staging + 2 x 8 GiB at 4e10-1.3e11 on one node)
VMM_BS_GROW = 6.0 * GB                                   # MEASURED (size 1, the defaults): the device at the bs phase above the arena + planes mapped at init --
                                                        # 4e10 +5.8 (224.0 vs 218.2), 1e11 +2.1 (365.7 vs 363.6), 1.16e11 +4.5 (404.3 vs 399.8), 1.3e11 +3.1 (438.7 vs 435.6);
                                                        # the largest, rounded up; the cause is not traced (the block pool's in-phase growth under VMM)

def host_size1_vmm(D):
    """the host HWM of a size-1 run on the defaults (DB_POOL_VMM: the seeds through 2 x 8 GiB pinned), fitted on the measured runs:
    4e10 25.8 / 25.9, 1e11 27.0-27.2, 1.16e11 27.6, 1.3e11 27.7 GB (V214, V314, the five-run 1e11 series) -- 25.85 + 0.0208 GB per 10^9
    digits above 4e10"""
    return int((25.85 + 0.0208 * max(0.0, D / 1e9 - 40.0)) * GB)

def seedbuf_vmm(N, nterms, S=256):
    """the two pinned seed buffers under DB_POOL_VMM (bytes): 2 x min(8 GiB, the largest region's span bytes)"""
    per, nspan = seed_limbs(N, nterms, S)
    span = 2 * per * 8                                                  # (Phase 15 DL: in whole spans, as seeds_stream carves them)
    mx = max(span * max(0, min(nspan, -(-(r + 1) * nspan // NR)) - min(nspan, -(-r * nspan // NR))) for r in range(NR))
    b = max(min(HOST_SEEDBUF_VMM, mx), span)
    return 2 * (b // span * span)

def pool_mb_of(need, pool, pool_mb=8192):
    """the SHMEM pool (bytes) the run gets from its need (bytes) under the pool rule (binsplit_shmem_pool_rule)"""
    if pool == 'max': return max(pool_mb << 20, need)                 # the model before Phase 15 (unrounded)
    mb = -(-need // (1 << 20)); mb = -(-mb // 256) * 256              # whole 256 MiB, as `plan pool` prints it
    if pool == 'plan': return mb << 20
    if pool == 'auto': return max(pool_mb, mb) << 20
    return pool_mb << 20                                               # 'off'

def exchange_scratch(nq_total, g, alltoallv, shift_chunk_mb=0):
    """the sharded division's exchange scratch per APU (rns_dist.c mdb_shift / mdb_add_shifted; PLAN 23-4):
    before B7 the padded g x share / 4 limbs per APU (mdb_shift) + g x 512 MB (mdb_add_shifted); after: the counts' own sizes.
    Phase 13a M (TASKS 1.2): MDB_SHIFT_CHUNK_MB=m -- mdb_shift in K rounds (newton_db.c), sb and rb about m MB each per APU"""
    if g == 1: return 0
    share = (nq_total + g - 1) // g
    if alltoallv:
        part = share // 4
        if shift_chunk_mb and shift_chunk_mb > 0:
            ch = int(shift_chunk_mb * 1048576.0 / 8)
            if part > ch: K = -(-part // ch); part = -(-part // K) + 1
        return 2 * part * 8 + (64 << 20)
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

# ---------------------------------------------------------------- Phase 14 P2: the SHMEM pool as the code uses it (measured law)
SHMEM_RING = 256 << 10                                    # COMM_SHMEM_RING_KB: the point-to-point ring per (communicator, source)
SHMEM_MAXID = 1024                                        # comm_shmem.c MAXID: the mailbox rows
SHMEM_SELFTEST = 128 << 20                                # mn_selftest_layered's symmetric buffers at init (measured 128 MiB at 2 PEs, 64 at 4)
SHMEM_MARGIN = 256 << 20                                  # first-fit slack and the small exchanges (spills, counts, reductions): assumed

def mn_stage(na, nb, g, share_a, share_b, share_c, pool_log=31, logr_delta=0, t_chunk_mb=0, parts=None, round_mb=0):
    """Phase 14 P2 (results/P214.md): the SHMEM transport's staging for one mn_core product C = A B over g nodes, per APU thread
    (limbs; send + receive of its largest staged exchange -- the staging is allocated per exchange and released after it, and
    the four APU threads run the same exchange at once, so the pool holds NR of these).  The code's buffers (mn_scratch's
    terms): the result exchange (rns_dist.c: xb in plane pool 1 -> rbO from the block pool, neither in the symmetric pool)
    sends my rows of the piece, RT(nc), and receives my window's part, win / 4 (win = min(share_c, nc): my share of C inside
    the piece); the redistribution of A (B) sends my share's part of the piece, va / 4 (+ 2 g rows), and receives my rows,
    RT(pa); the transform's inter-node stage (comm_layered) q / K each way.  MN_T_CHUNK_MB=m: the result exchange in rounds
    of m MiB per APU each way.  Measured (P214 section 1): the result exchange of the division's A_h mu product is the peak
    at 1e8 / 1e9 / 1e10 on 2 nodes and 1e9 / 1e10 on 4 processes, to the MiB.
    Phase 14 V1: round_mb = COMM_SHMEM_ROUND_MB -- the transport carries each all-to-all in rounds staging at most round_mb
    each way, so every side of every exchange is bounded by it (rns_dist.c rns_mul_dist_mn_stage's RMIN)."""
    if not na or not nb or g < 2: return 0
    cap = 1 << mn_cap_log(g, pool_log); ka = kb = 1
    nr = 4 * g; lg = 0
    while (1 << lg) < nr: lg += 1
    if na + nb > cap and -(-na // 32) + -(-nb // 32) > cap: ka, kb = -(-na // (cap // 2)), -(-nb // (cap // 2))   # beyond the code's 32 x 32 grid (the run would stop there): pieces at the cap, for the model's sweeps
    elif na + nb > cap: ka, kb = split_grid_cap(na, nb, cap, 1 << max(20, 2 * (5 + lg)))
    pa = -(-na // ka); pb = -(-nb // kb); nc = pa + pb
    logn, logR, logC, q = mn_shape(nc, g, logr_delta)
    R = 1 << logR; C = 1 << logC; rows = -(-R // nr); qs = rows * C
    def RT(ln): return min((-(-ln // R) + 1) * rows, qs)
    va = min(share_a, pa); vb = min(share_b, pb)
    win = min(share_c, nc); send = RT(nc)
    Wt = t_chunk_limbs(t_chunk_mb)
    if Wt and win > Wt: win = Wt; send = min(send, Wt // 4 + 2 * g * rows)
    Rl = int(round_mb * 1048576) // 8 if round_mb else 0
    def RM(x): return min(x // g * (g - 1), Rl) if Rl else x       # the peers' part (the rounds' path stages no self slab), at most Rl
    result = RM(send) + RM(win // 4)
    redist = max(RM(RT(pa)) + RM(va // 4 + 2 * g * rows), RM(RT(pb)) + RM(vb // 4 + 2 * g * rows))
    transform = 2 * RM(q // K_CHUNKS_MEM)
    if parts is not None: parts.update(result=result, redist=redist, transform=transform, ka=ka, kb=kb, nc=nc, win=win)
    return max(result, redist, transform)

def shmem_products(nq_total, g, groups=None):
    """the run's mn_core products for the pool law: (name, S, na, nb, share_a, share_b, share_c) -- the tree levels as
    tree_need_dev forms them (the largest product of each level) and the division at g: A_h mu (nq x (k + 1), C of 2 nq
    limbs: newton_db.c newton_mn_divmod; the largest), the reciprocal's last step Q_t r (nq x nq / 2)"""
    nq_leaf = (nq_total + g - 1) // g; out = []
    for S, ch in level_children(g, groups):
        nch = len(ch); P = ch[0]
        if nch < 2: continue
        nqc = nq_leaf * P + 8; na = nqc; nb = nqc * (nch - 1); nc = na + nb
        out.append(('tree %d' % S, S, na, nb, nq_leaf + 8, -(-nb // S), -(-nc // S)))
    sh = -(-nq_total // g)
    out.append(('div A_h mu', g, nq_total, nq_total, sh, sh, -(-2 * nq_total // g)))
    out.append(('recip Q_t r', g, nq_total, nq_total // 2 + 1, sh, -(-(nq_total // 2) // g), -(-(3 * nq_total // 2) // g)))
    return out

def ofi_planned(env=None):
    """Phase 17 OFIMEM: comm_ofi_planned() (comm_ofi.c) -- COMM_OFI if set, else a cxi NIC on this host or COMM_OFI_PLAN_CXI=1 (the plan's
    hint from mnrun.sh).  The target (Cray, cxi) has it on by default: pass COMM_OFI=1 to model it from a host without cxi."""
    env = os.environ if env is None else env
    if env.get('COMM_OFI') is not None: return env['COMM_OFI'] not in ('', '0') and int(env['COMM_OFI'] or 0) != 0
    return os.path.exists('/sys/class/cxi/cxi0') or env.get('COMM_OFI_PLAN_CXI', '0') not in ('', '0')

def round256_mb(b): mb = -(-b // (1 << 20)); return -(-mb // 256) * 256

def shmem_pool(nq_total, g, groups=None, pool_log=31, t_chunk_mb=0, shift_chunk_mb=1024, sym_slabs=False, ring=SHMEM_RING, detail=None, round_mb=0, ofi=False, vslot=0):
    """Phase 14 P2: the symmetric pool a run of g node-processes needs, bytes per node-process (the measured law; P214 section 2):
      staging  = NR x 8 x the largest mn_stage over the run's products (and the division's mdb_shift of t (2 nq) to X (nq): a quarter of
                 my share of each, each at most MDB_SHIFT_CHUNK_MB)
      control  = 4 communicators per level of size S (the level's G->all[d]; the top level's are mn.c's meshes of g), each
                 8 words x S + S x the ring: 4 x (g + sum of the lower levels' S) x (ring + 64 B)  (measured 2 MiB at 2, 6 at 4)
      mailbox  = MAXID x g x 8 B;  + the self-tests' 128 MiB at init (not concurrent with the staging) and a margin.
      sym_slabs (DIST_MN_SYM_SLABS=1, TASKS 2.4): + NR x 3 q x 8 B kept (q = the largest mn_core plane), the transform's staging gone.
      round_mb (COMM_SHMEM_ROUND_MB, Phase 14 V1): every side of every staged exchange at most round_mb.
      ofi (Phase 17 OFIMEM, COMM_OFI): the device staging and the slabs move to one comm_ofi pool per device (detail['ofi_dev_mb']: one
      APU's staging, at least the self-tests' 128 MiB, + its slabs + 256 MiB, in whole 256 MiB); the SHMEM pool keeps 128 MiB of host-op
      staging, the control blocks, the mailbox and the margin (binsplit.c binsplit_shmem_pool_need).
    detail (a dict) receives the parts and the product that sets the staging."""
    if g <= 1: return 0
    best, who, qmax = 0, '', 0
    for name, S, na, nb, sa, sb, sc in shmem_products(nq_total, g, groups):
        pp = {}; st = mn_stage(na, nb, S, sa, sb, sc, pool_log, 0, t_chunk_mb, pp, round_mb)
        if st > best: best, who = st, '%s (%s)' % (name, max(('result', 'redist', 'transform'), key=lambda k: pp[k]))
        cap = 1 << mn_cap_log(S, pool_log); qmax = max(qmax, mn_shape(min(na + nb, cap), S)[3])
    ch = int(shift_chunk_mb * 1048576 / 8) if shift_chunk_mb else 0             # mdb_shift: the division's t (2 nq limbs) >> (k + 1) -> X (nq): my share of t
    s_out, s_in = -(-2 * nq_total // g) // 4, -(-nq_total // g) // 4          # out, my share of X in, a quarter per APU each (measured: send 106 + recv 53 MiB
    K = -(-min(s_out, s_in) // ch) if ch and min(s_out, s_in) > ch else 1       # per APU at 1e9 on 2 nodes, the high node); newton_db.c: K rounds from the
    shift = -(-(s_out + s_in) // K)                                              # shorter side's part (SL), so a side can exceed the chunk (1e10: 1059.6 + 529.8, K 1)
    if round_mb:                                                                 # Phase 14 V1: the transport's rounds bound each side
        Rl = int(round_mb * 1048576) // 8; shift = min(-(-s_out // K) // g * (g - 1), Rl) + min(-(-s_in // K) // g * (g - 1), Rl)
    if shift > best: best, who = shift, 'mdb_shift'
    staging = NR * best * 8
    lv = [S for S in mn_groups(g, groups) if S < g]
    control = 4 * (g + sum(lv)) * (ring + 64)
    if inter2_on(): control += 4 * sum(S for S in [min(S, g) for S in mn_groups(g, groups)] if S > 1 and S & (S - 1)) * (ring + 64)   # X2 COMM_LAYER_INTER2: a second mesh per general-map group
    mailbox = SHMEM_MAXID * g * 8
    sym = NR * 3 * qmax * 8 if sym_slabs else 0
    ofi_dev_mb = 0
    if ofi:
        ofi_dev_mb = round256_mb(max(best * 8, SHMEM_SELFTEST) + sym // 4 + vslot + SHMEM_MARGIN); staging = 0; sym = 0
    if not ofi: sym += NR * vslot   # X2: (no comm_ofi) the slots in the SHMEM pool, one set per APU thread
    need = max(staging, SHMEM_SELFTEST) + control + mailbox + sym + SHMEM_MARGIN
    if detail is not None: detail.update(staging=staging, control=control, mailbox=mailbox, sym=sym, need=need, by=who, per_apu=best * 8, ofi_dev_mb=ofi_dev_mb)
    return need

MEASURED_POOL = [  # (total digits, g, the cap's log (POOL_LOG, or DIST_LOGN_TEST when lower), MN_T_CHUNK_MB, the staging peak MiB of the largest PE, source) -- P214 section 1
    (1e8, 2, 27, 0, 84.8, 'P214 b1a (job 21269), 2 real nodes, SOS'),
    (1e9, 2, 29, 0, 847.7, 'P214 b1a (job 21269), 2 real nodes, SOS'),
    (1e10, 2, 31, 0, 8477.0, 'S13d (job 21104), 2 real nodes, SOS (8479 MiB with the control blocks)'),
    (1e9, 4, 29, 0, 423.9, 'P214 b1b (job 21271), 4 processes on one node, SOS'),
    (1e10, 4, 29, 0, 4238.6, 'P214 b1b (job 21271), 4 processes on one node, SOS'),
    (1e9, 2, 25, 0, 635.8, 'P214 b4 (job 21276), 2 real nodes, DIST_LOGN_TEST=25: the division in 2 x 2 pieces (PE 1; PE 0 529.8)'),
    (1e9, 2, 29, 64, 635.8, 'P214 b4 (job 21276), 2 real nodes, MN_T_CHUNK_MB=64: the mdb_shift sets it (PE 1; PE 0 529.8)'),
    (1e10, 2, 31, 0, 8477.1, 'P214 b5 (job 21279), 2 real nodes, SOS, the pool set by COMM_SHMEM_POOL_AUTO=1 (8960 MiB); the shift 1059.6 + 529.8'),
    # Phase 14 V1 (results/V114.md): the Phase 14 defaults (MN_T_CHUNK_MB=1024), the pool from mnrun.sh's plan; 7th field COMM_SHMEM_ROUND_MB
    (1e9, 2, 31, 1024, 847.7, 'V114 b1 (job 21435), 2 real nodes, SOS host heap, pool 1280 from the plan'),
    (1e10, 2, 31, 1024, 8192.0, 'V114 b2 (job 21439), 2 real nodes, SOS host heap, pool 8704 from the plan'),
    (1e9, 2, 31, 1024, 423.9, 'V114 b3 (job 21442), 2 real nodes, COMM_SHMEM_ROUND_MB=64 (the self slab left out)', 64),
    (1e10, 2, 31, 1024, 2048.0, 'V114 b3 (job 21442), 2 real nodes, COMM_SHMEM_ROUND_MB=256: 84 exchanges in 176 rounds', 256),
    (1e9, 4, 29, 1024, 423.9, 'V114 b5 (job 21452), 4 processes on one node, SOS host heap, the staging regions (8f3b136)'),
    (1e10, 4, 29, 1024, 4238.6, 'V114 b5 (job 21452), 4 processes, the pool 4608 from the plan in regions (b4 without them: fragmented)'),
    (1e10, 4, 29, 1024, 2048.0, 'V114 b4 / b5 (jobs 21448, 21452), 4 processes, COMM_SHMEM_ROUND_MB=256', 256),
]

def pool_target():
    """the pool the target (TARGET_DIGITS; 4.25e13 in Phase 14) needs on 576 nodes (MN_GROUPS 2,4,8,16,32,64,192,576) and the node total with it"""
    groups = '2,4,8,16,32,64,192,576'; D = TARGET_DIGITS / TARGET_NODES
    print('== Phase 14 P2: the %.3ge13 target on 576 nodes (D %.3e per node, MN_GROUPS %s): the SHMEM pool and the node total (GB, modelled)' % (TARGET_DIGITS / 1e13, D, groups))
    for name, o in [('the code (staged exchanges)', dict()), ('MN_T_CHUNK_MB=1024', dict(t_chunk_mb=1024)), ('DIST_MN_SYM_SLABS=1', dict(staging='sym')),
                    ('COMM_SHMEM_ROUND_MB=1024 (V1)', dict(round_mb=1024)), ('MN_T_CHUNK_MB=1024 COMM_SHMEM_ROUND_MB=1024', dict(t_chunk_mb=1024, round_mb=1024)),
                    ('the old model (resident, 8 GiB flat)', dict(staging='resident'))]:
        for tight in (False, True):
            oo = dict(OLD13); oo.update(TARGET576); oo.update(groups=groups, tight=tight); oo.update(o)   # Phase 15: the V1 table's forms (pool 'max')
            r = mem_per_node(int(D), 576, oo); det = {}
            if oo['staging'] in ('code', 'sym'):
                L = dm_layout(e_terms(digits_of_run(D * 576)), 576, 31, True, tight)
                shmem_pool(L['nq'], 576, groups, 31, oo.get('t_chunk_mb', 0), oo.get('shift_chunk_mb', 1024), oo['staging'] == 'sym', detail=det, round_mb=oo.get('round_mb', 0))
            print('  %-44s%s: pool %6.1f (staging %6.1f = 4 x %.2f by %s, control %.2f, sym %.1f) -> node %6.1f (device %6.1f + host %5.1f): %s 480' % (
                name, ' DM_TIGHT' if tight else '         ', r['shmem_pool'] / GB, det.get('staging', 0) / GB, det.get('per_apu', 0) / GB, det.get('by', '-'),
                det.get('control', 0) / GB, det.get('sym', 0) / GB, r['node_peak'] / GB, r['dev_dm'] / GB, r['host_hwm'] / GB, 'fits' if r['node_peak'] <= 480 * GB else 'EXCEEDS'))

def pool_check():
    """the pool law against the measured staging peaks (the largest PE: the pool is the same size on every PE)"""
    print('== Phase 14 P2: the SHMEM pool staging (MiB per node-process, the largest PE): measured vs the law')
    for row in MEASURED_POOL:
        D, g, pl, tmb, meas, src = row[:6]; rmb = row[6] if len(row) > 6 else 0     # Phase 14 V1: + COMM_SHMEM_ROUND_MB
        d = digits_of_run(D); L = dm_layout(e_terms(d), g, pl); det = {}
        shmem_pool(L['nq'], g, None, pl, tmb, detail=det, round_mb=rmb)
        m = det['staging'] / 1048576.0
        print('  %.0e g %d cap log %d T chunk %4d rounds %4d: measured %8.1f  law %8.1f (%+.2f %%) = 4 x %.1f MiB by %-26s control %.1f MiB | %s' % (
            D, g, pl, tmb, rmb, meas, m, 100 * (m / meas - 1), det['per_apu'] / 1048576.0, det['by'], det['control'] / 1048576.0, src))

# ---------------------------------------------------------------- Phase 15 TC: the mn transform cache (rns_dist.c cache_avail)
CACHE_MN_SLOTS_CODE = 2                  # RNS_DIST_CACHE_MN's default
CACHE_FIT_RESERVE = 32e9                 # RNS_DIST_CACHE_FIT_RESERVE_GB's default (32 GB per node)
DIST_LOGN_MAX = 31                       # rns_dist.c: the single-node tier's cap, which sizes the default's slots
def cache_mn_bytes(g, pool_log=31, np=4, slots=CACHE_MN_SLOTS_CODE, fit=False):
    """bytes per node of the mn transform cache: slots x 4 APUs x np x 2^(c - 2) limbs x 8, c = 31 (the default: whatever POOL_LOG) or
    min(31, POOL_LOG) (RNS_DIST_CACHE_FIT: the mn tier's cap, mn_logn_cap: q <= 2^(c - 2)); the planes pool 0 is made for (np).  Only a
    run with an mn grid product takes it (MN_PLAN_ONLY's `plan cache` line says where; at the target: tree level 1)"""
    c = min(DIST_LOGN_MAX, pool_log) if fit else DIST_LOGN_MAX
    return slots * NR * np * (1 << (c - 2)) * 8 if g > 1 else 0

def cache_report():
    """the cache at the target and one step below (TARGET_LAUNCH), and at the single-node test points of results/TC15.md"""
    groups = '2,4,8,16,32,64,192,576'
    print('== Phase 15 TC: the mn transform cache (RNS_DIST_CACHE_MN=%d; GB per node, modelled)' % CACHE_MN_SLOTS_CODE)
    for T in (TARGET_DIGITS, TARGET_BELOW):
        oo = dict(TARGET_LAUNCH, transport='shmem', staging='code', depth=2, groups=groups, np=TARGET_NP); r = mem_per_node(int(T / TARGET_NODES), TARGET_NODES, oo)
        one = r['cache_slot']                                           # (Phase 15 DOC2: the slot at pool 0's planes -- four at the target under auto)
        print('  %.3ge13 on %d, ECALC_NP=%s: node %.1f (no cache) | the default\'s 2 slots %.1f -> node %.1f (1 slot: %.1f) | RNS_DIST_CACHE_FIT: %d slot%s (the code\'s room: 480 - the layout node %.1f - the reserve %.0f = %.1f < one slot %.1f; against this model\'s node %.1f) -> node %.1f' % (
            T / 1e13, TARGET_NODES, TARGET_NP, r['node_peak'] / GB, r['cache_mn'] / GB, r['node_peak_cache'] / GB, (r['node_peak'] + one) / GB,
            r['cache_fit_slots'], '' if r['cache_fit_slots'] == 1 else 's', r['layout_node'] / GB, CACHE_FIT_RESERVE / GB, r['cache_fit_room'] / GB, one / GB, 480 - r['node_peak'] / GB, (r['node_peak'] + r['cache_fit']) / GB))
    for D, g, pl in ((5e8, 2, 25), (2.5e8, 4, 24), (5e9, 2, 29), (2.5e9, 4, 28)):
        print('  %.0e digits at %d node-processes, POOL_LOG=%d (3 primes): the default %.2f per process (12 GiB slots), FIT %.2f per process' % (
            D * g, g, pl, cache_mn_bytes(g, pl, 3) / GB, cache_mn_bytes(g, pl, 3, fit=True) / GB))

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
    o = dict(pool_log=31, tail=True, alltoallv=True, decimal=True, margin=0.0, logr_delta=0, form='grid', groups=None, transport='tcp', pool_mb=8192, staging='code', planes_3q30=None,
             np=EC_NP, strategy='C', cap=None, depth=2, host_fit=True, tail_dead=0, arena_room=0.0, dkm=False, ofi=None); o.update(DEFAULTS15D); o.update(opts or {})   # early_free: Phase 14 T1 (MN_TREE_EARLY_FREE); form: 'grid' is the code after Phase 12 G (the arena request follows rns_mul_dist_mn_scratch); 'flat' = before; tight / tail_dead: Phase 14 L1 (DM_TIGHT, DM_TAIL_DEAD)
    # Phase 15 (agent MD): the defaults are the code's since Phase 14 (DEFAULTS15: tight, early_free, t_chunk_mb 1024, shift_chunk_mb 1024,
    # pool 'plan', vmm); pass OLD13 for the forms before.  vmm (DB_POOL_VMM): the host's seed buffers 2 x 8 GiB, the size-1 host fitted anew
    # (host_size1_vmm), the bs phase's measured growth (VMM_BS_GROW) on the device peak.  pool: POOLS.
    # Phase 13b D: np (ECALC_NP: pool 0 scales np/4), strategy (C | B | B4 | auto: B's 16 n planes), cap (the plane cap in points:
    # sets pool_log and the 3 2^k planes, cap_pool), depth (COMM_ALLTOALLV_DEPTH, the code's default 2 since Phase 13c: the v-slots' form --
    # B7ACCT: vslot_resident, outside the arena, on dev_bs / dev_dm), host_fit (size 1: the host HWM fitted on the measured runs instead of the init constants)
    if o['cap'] is not None: o['pool_log'], o['planes_3q30'] = cap_pool(o['cap'])
    D_total = D * g; d = digits_of_run(D_total); N = e_terms(d); nterms = (N + g - 1) // g
    S = seed_span(N, g, o['seed_fill']) if o['decimal'] else 256                # Phase 15 (2026-09-27): BS_SEED_FILL (128 by default; 0 = 256)
    bs = arena_bs_bytes(N, nterms, decimal=o['decimal'], S=S); bs_total = sum(bs)
    aroom = o['arena_room'] if o['vmm'] else 0.0                            # Phase 15 AS: BS_ARENA_ROOM (needs the VMM pool)
    L = dm_layout(N, g, o['pool_log'], o['decimal'], o['tight'], o['tail_dead'], room=aroom, dkm=o.get('dkm', False), mdev_logl=o.get('mdev_logl', 30), lean=o.get('lean', False))   # dkm: NEWTON_DKM (Phase 15 DL: built -- binsplit.c dm_layout follows it; the code's default since Batch 3); lean: DM_MN_LEAN (int15k)
    p24 = (o.get('p24', 0), o['np'], g, o.get('np_auto_min', False)) if o.get('p24', 0) else None   # MS: MN_P24 (0 = off, the default)
    sc = []; tree = tree_need_dev((L['nq'] + g - 1) // g, g, sc, o['logr_delta'], o['form'], o['pool_log'], o['groups'], o['t_chunk_mb'], o['early_free'], p24) if g > 1 else 0
    if g > 1: L['need_dev'] += sc[0]                                       # the sharded division's products: the same slabs and spills
    vs = vslot_resident(L['nq'], g, o['groups'], o['pool_log'], p24, o['depth'])   # B7ACCT: the general map's v-slots per APU (hipMalloc'd beside the arena)
    vb = NR * vs['per_apu'] if vslot_budget_on() else 0                    # ECALC_VSLOT_BUDGET=1: also in the code's node budget (room, cache fit)
    want = max(L['need_dev'], tree)
    if o['tail']:
        arena = [arena_of(b + (L['hole'] if o['tail'] == 'v1' else 0), want, VMM_CHUNK if aroom > 0 else 0) for b in bs]   # binsplit_pregrow (v2): the bs halves or the dm / tree need per device; the tail is a policy over the last bytes (tail='v1': the hole added to the halves, the batch-1/2 runs of M11.md)
        pool_in_phase = 0
        # Phase 15 int15j (DL15 open issue 2, the user's item 4): the VMM pool maps every arena in whole chunks whether or not the room is on
        # (dbig.c db_vmm_arena_alloc: m0 = ceil(bytes / chunk)), so the node holds the rounded arena; the request (`arena`, the `layout:`
        # line that --check-c compares) stays as binsplit_pregrow asks it.  Before, the room-off node counted the unrounded request (the
        # room-0 ceilings were one chunk per APU too high: DL15).
        arena_mapped = [(a + VMM_CHUNK - 1) // VMM_CHUNK * VMM_CHUNK for a in arena] if o['vmm'] else list(arena)
        pool_total = sum(arena_mapped)
    else:
        arena = bs; arena_mapped = list(bs)
        # Phase 10's behaviour: the pool maps t1's quarter per APU inside the reciprocal, plus the byte deficit
        # (measured 141.0 / 231.8 / 265.7 GB of pool at 4 / 7 / 8e10 = 1.07-1.09 x the dm need; the tree's excess at size > 1)
        pool_in_phase = max(0, int(1.08 * NR * L['need_v2']) - bs_total) if g == 1 else max(0, NR * tree - bs_total) + NR * L['hole']
        pool_total = bs_total + pool_in_phase
    xchg = NR * exchange_scratch(L['nq'], g, o['alltoallv'], o['shift_chunk_mb'])
    # (B7ACCT: the old depth-2 term here -- one more quarter-plane per APU in the block pool -- is replaced by vslot_resident: the whole
    # v-exchange scratch, 2 slots of A + max(A, B) at both depths' forms, hipMalloc'd OUTSIDE the arena, added to dev_bs / dev_dm below)
    npp = np_planes(o['np'], g, o['pool_log'])                          # Phase 15 NP: ECALC_NP=auto -- pool 0's planes by the run's largest group
    p1np = o.get('pool1_np') or (3 if o['np'] == 'auto' else o['np'])  # Phase 15 PS: pool 1 at the one-node tiers' prime count (auto: three)
    planes = planes_bytes(o['pool_log'], d, o['planes_3q30'], npp, o['strategy'], pool1_np=p1np)
    pdet = {}                                                             # (Phase 15 DL: the host terms before the room's decision, which counts them)
    if g > 1 and o['transport'] == 'shmem' and o['staging'] in ('code', 'sym'):   # Phase 14 P2: the measured law (the code as it is; 'sym': DIST_MN_SYM_SLABS=1)
        ofi = ofi_planned() if o.get('ofi') is None else o['ofi']           # Phase 17 OFIMEM: COMM_OFI (None: as the code decides on this host)
        need = shmem_pool(L['nq'], g, o['groups'], o['pool_log'], o['t_chunk_mb'], o['shift_chunk_mb'], o['staging'] == 'sym', detail=pdet, round_mb=o.get('round_mb', 0), ofi=ofi, vslot=vs['per_apu'] if vslot_pool_on() else 0)   # X2: VSLOT_POOL counts the v-slots here
        stg = pdet['staging']; pool = pool_mb_of(need, 'plan' if ofi and o['pool'] == 'auto' else o['pool'], o['pool_mb'])   # Phase 15: the pool rule (POOLS; 'max' = the model before); OFIMEM: under OFI an unset pool = the need
        pdet['shmem_only'] = pool; pool += NR * (pdet['ofi_dev_mb'] << 20)   # OFIMEM: + the four comm_ofi pools (the transport's pools in all)
    else:                                                                 # the hypotheses before Phase 14 (resident: 0 staging, the pool flat at 8 GiB)
        stg = shmem_staging(L['nq'], g, o['groups'], o['pool_log'], o['staging']) if (g > 1 and o['transport'] == 'shmem') else 0
        pool = max(o['pool_mb'] << 20, stg + (100 << 20)) if (g > 1 and o['transport'] == 'shmem') else 0
    comm = (HOST_COMM_PER_PROC + pool) if g > 1 else 0
    seedbuf = seedbuf_vmm(N, nterms, S) if o['vmm'] else HOST_SEEDBUF          # Phase 15: DB_POOL_VMM's two 8 GiB pinned seed buffers (at init)
    host_init = HOST_RUNTIME + HOST_STAGING + seedbuf + comm
    host_dm = HOST_RUNTIME + HOST_STAGING + HOST_WRITER + comm + (HOST_EARLY if (g > 1 and o['out_early']) else 0)
    if g == 1 and o['host_fit']:                                          # Phase 13b D: size 1, the measured host (mem summary):
        host_init = host_size1_vmm(D_total) if o['vmm'] else host_size1(D_total)   # the HWM is at init (staging pinned + other), and grows
        host_dm = min(host_dm, host_init)                                 # slowly with D (seeds); the dm phase stays below it at >= 2e10
    room_node = None
    if aroom > 0 and o['tail']:                                           # Phase 15 AS: the room dropped over the budget (the arenas stay in whole chunks); DL: the node
        rh = room_host(N, g, pool, seedbuf, o['out_early'])               # counted as binsplit.c as_room_fits counts it (this model's bs-phase node)
        room_node = planes + sum(arena) + VMM_BS_GROW + rh
        if not as_room_fits(planes, sum(arena), rh + vb, o.get('node_gb', 480.0)):   # (B7ACCT: + the v-slots under ECALC_VSLOT_BUDGET=1)
            aroom = 0.0; L = dm_layout(N, g, o['pool_log'], o['decimal'], o['tight'], o['tail_dead'], room=0.0, dkm=o.get('dkm', False), mdev_logl=o.get('mdev_logl', 30), lean=o.get('lean', False))
            if g > 1: L['need_dev'] += sc[0]
            want = max(L['need_dev'], tree); arena = [arena_of(b, want, VMM_CHUNK) for b in bs]; arena_mapped = list(arena); pool_total = sum(arena)
    if g > 1 and sc[1] > (1 << o['pool_log']) // 4:                       # (never with the mn tier's cap: kept for a lowered cap)
        planes = NR * (npp * sc[1] * 8 + (3 * sc[1] + 16) * 8) + int(0.61 * GB)
    dev_init = planes + (sum(arena_mapped) if o["tail"] else bs_total)   # Phase 13a M: the arena (bs regions + dm extra) is mapped at init since M11 v2 (measured 4e10: 313.3 GB at init = at the dm peak); int15j: in whole VMM chunks
    # the exchange scratch comes from the block pool (db_pool_alloc): inside the arena while the dm shares + it fit, hipMalloc beyond
    live_dm = NR * L['need_dev'] + xchg
    if live_dm > pool_total: pool_in_phase += live_dm - pool_total; pool_total = live_dm
    dev_dm = planes + pool_total + NR * vs['per_apu']                      # B7ACCT: + the v-slots resident from the dm phase on
    dev_bs = dev_init + (VMM_BS_GROW if o['vmm'] else 0) + NR * vs['tree_top']   # Phase 15: the bs phase's measured growth under VMM; counted with the host HWM; B7ACCT: + the v-slots of the tree's top level
    if g == 1 and o['vmm'] and o['seed_fill'] and o['decimal'] and not aroom > 0:   # Phase 15 (2026-09-27): the fill's division grows the pool at size 1 (measured at 1e11); AS: none with BS_ARENA_ROOM (replayed)
        dev_dm = max(dev_dm, dev_init + VMM_DM_GROW_FILL)
    peak = max(dev_init + host_init, dev_bs + max(host_init, host_dm), dev_dm + host_dm) * (1 + o['margin'])   # (the measured node = device max + host HWM)
    # Phase 15 TC (results/TC15.md): the mn transform cache -- not in node_peak (the numbers the docs cite); the default code's slots
    # (RNS_DIST_CACHE_MN=2 x 16 GiB per APU at four primes) live from the first mn grid product (the tree at the target) to the division's end,
    # so they add to the bs and dm peaks: node_peak_cache.  cache_fit_slots: what RNS_DIST_CACHE_FIT would take against this model's peak.
    cmn = cache_mn_bytes(g, o['pool_log'], npp, CACHE_MN_SLOTS_CODE) if g > 1 else 0
    peak_c = max(dev_init + host_init, dev_bs + cmn + max(host_init, host_dm), dev_dm + cmn + host_dm) * (1 + o['margin'])
    slot_fit = cache_mn_bytes(g, o['pool_log'], npp, 1, fit=True) if g > 1 else 0
    # Phase 15 DOC2: RNS_DIST_CACHE_FIT's bound as the code computes it (rns_dist.c rns_dist_cache_plan / cache_avail): ECALC_NODE_GB less the layout's
    # node (binsplit_node_bytes: the plane pools + the arena + the tables + the host term -- BS_HOST_INIT_BYTES at size 1, room_host's size > 1
    # formula since the B5 fix: runtime + staging + transport + the SHMEM pool + max(the VMM seed buffers, the writer + MN_OUT_EARLY)) less
    # the reserve (32 GB per node), over one slot per node; the run's own check takes its measured init peak where larger (not modelled).
    # cache_fit_room_model: against this model's peak
    layout_node = planes + sum(arena) + (room_host(N, g, pool, seedbuf, o['out_early']) if g > 1 else AS_HOST) + vb   # B7ACCT: vb = the v-slots under ECALC_VSLOT_BUDGET=1
    fit_room = o.get('node_gb', 480) * GB - layout_node - CACHE_FIT_RESERVE
    fit_slots = min(CACHE_MN_SLOTS_CODE, max(0, int(fit_room // slot_fit))) if slot_fit and fit_room > 0 else 0
    return dict(D=D, g=g, N=N, digits=d, nq=L['nq'], t1_quarter=L['t1_quarter'], hole=L['hole'],
                cache_mn=cmn, node_peak_cache=peak_c, cache_fit_slots=fit_slots, cache_fit=fit_slots * slot_fit, cache_slot=slot_fit, cache_fit_room=fit_room, layout_node=layout_node,
                arena_room=aroom, room_node=room_node, dkm=L['dkm'],
                planes=planes, regions_bs=bs_total, arena=sum(arena), arena_mapped=sum(arena_mapped), dm_need=NR * L['need_dev'], tree_need=NR * tree, top_scratch=NR * sc[0] if g > 1 else 0,
                pool_in_phase=pool_in_phase, pool_total=pool_total, exchange=xchg, shmem_staging=stg, shmem_pool=pool, shmem_pool_by=pdet.get('by', ''), shmem_pool_only=pdet.get('shmem_only', pool), ofi_pools=NR * (pdet.get('ofi_dev_mb', 0) << 20),
                dev_init=dev_init, dev_dm=dev_dm, dev_bs=dev_bs, dev_max=max(dev_init, dev_bs, dev_dm), shmem_need=pdet.get("need", 0), host_init=host_init, host_dm=host_dm, host_hwm=max(host_init, host_dm),
                vslot=NR * vs['per_apu'], vslot_tree=NR * vs['tree_top'], vslot_levels=vs['levels'],   # B7ACCT (bytes per node)
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
    # Phase 13a M (TASKS 1.1): the code as merged (3 2^k planes below 5e10 digits, tail v4, zero hipMalloc); 'planes' = the planes line, dev_dm = the device total
    (1e10, 1, dict(planes=180.4, regions=53.4, pool=53.4, dev_dm=234.4, host=16.6, tail=True, p3=True, src='M13 b1 (job 21008): device 234.4 = 180.4 + 53.4 + 0.61, host HWM 16.6')),
    (4e10, 1, dict(planes=180.4, regions=132.3, pool=132.3, dev_dm=313.3, host=12.1, tail=True, p3=True, src='M13 b1 (job 21008) = RESULTS 77: 313.3 = 180.4 + 132.3 + 0.61, host HWM 12.1')),
    (8e10, 1, dict(planes=120.3, regions=248.2, pool=248.2, dev_dm=369.1, host=12.8, tail=True, src='M11 v3/v4 (tail): device 369.1, host 12.8')),
    (1e11, 1, dict(planes=120.3, regions=310.3, pool=310.3, dev_dm=431.2, host=14.0, tail=True, src='M11 v4 (tail): device 431.2, host 14.0 (node 445)')),
    (2.5e9, 4, dict(planes=34.4, regions=19.9, pool=19.9, dev_dm=56.6, host=29.3, tail=True, src='M13 b1 (job 21008) 1e10 at size 4, POOL_LOG=29, per process: 56.6 = 34.4 + 19.9 + 2.3; hipMalloc 0')),
    # Phase 13b D: three primes (P3, closing series of RESULTS 78): pool 0 at 3 q, pool 1 unchanged
    (4e10, 1, dict(planes=154.6, regions=132.3, pool=132.3, dev_dm=287.5, host=12.1, tail=True, p3=True, np=3, src='P3 / RESULTS 78 (jobs 21009, 21039): ECALC_NP=3, 287.5 = 154.6 + 132.3 + 0.61, host HWM 12.1')),
    (1e9, 1, dict(planes=154.6, regions=5.4, pool=5.4, dev_dm=160.6, host=8.7, tail=True, p3=True, np=3, src='P3 b2 (job 21005): ECALC_NP=3 at 1e9, device 160.6 = 154.6 + 5.4 + 0.61 to bs; host 8.7 then (15.7 in dm: host flows below 2e10, not modelled)')),

]

def fmt(b): return '%7.1f' % (b / GB)

def main():
    print('== calibration (GB; model vs measured; "pool" = regions + the pool\'s hipMalloc at the dm peak)')
    print('%-8s %2s | %-22s | %8s %8s %8s %8s | %s' % ('D', 'g', 'item', 'planes', 'regions', 'pool', 'dev_dm', 'source'))
    for D, g, m in MEASURED:
        r = mem_per_node(int(D), g, dict(OLD13, tail=m['tail'], pool_log=29 if g > 1 else 31, planes_3q30=m.get('p3', False), np=m.get('np', 4), host_fit=m['tail'] is not False))   # the code of those runs
        print('%-8.0e %2d | %-22s | %s %s %s %s | %s' % (D, g, 'measured', fmt(m['planes'] * GB), fmt(m['regions'] * GB), fmt(m['pool'] * GB), fmt(m['dev_dm'] * GB), m['src']))
        print('%-8s %2s | %-22s | %s %s %s %s | %s' % ('', '', 'model (tail %s)' % m['tail'], fmt(r['planes']), fmt(r['regions_bs'] if not m['tail'] else r['arena']), fmt(r['pool_total']), fmt(r['dev_dm']),
              'dev_dm %+.1f %%, node peak %+.1f %% (measured %.1f = device + host HWM)' % (100.0 * (r['dev_dm'] / GB / m['dev_dm'] - 1), 100.0 * (r['node_peak'] / GB / (m['dev_dm'] + m['host']) - 1), m['dev_dm'] + m['host'])))
        if m['tail']: continue
        r2 = mem_per_node(int(D), g, dict(OLD13, tail=True, np=4))
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

# ---------------------------------------------------------------- Phase 13a M (TASKS 1.1): the C request against this port, and the one ceiling
def c_layout_check(path, pool_log=31, t_chunk_mb=1024, seed_fill=SEED_FILL, arena_room=ARENA_ROOM, np_mode=None, pool1_4q=1):   # Phase 15 DOC2: BS_ARENA_ROOM 0.16 (the default since 2026-09-28); np_mode None: 'auto' when the file's plan lines say ECALC_NP=auto, else each planes: line's np   # Phase 15 AS: arena_room = the run's BS_ARENA_ROOM   # Phase 15: MN_T_CHUNK_MB=1024 is the code's default (the layout line does not print it); BS_SEED_FILL 128 (2026-09-27; 0 for a log run with BS_SEED_FILL=0)
    """compare the `layout:` lines of `BS_LAYOUT_ONLY=D:g,... ./ecalc 1e6 x` (binsplit.c binsplit_layout_only: the arena request
    of binsplit_pregrow, not allocated) with this file's port, term by term; returns the largest relative difference of the arena"""
    import re
    worst = 0.0; nplanes = 0
    if np_mode is None and 'ECALC_NP=auto' in open(path, errors='replace').read(): np_mode = 'auto'   # Phase 15 DOC2: the launch line's auto (its planes: lines print np 3)
    lines = open(path, errors='replace').read().splitlines(True)
    env = os.environ; nplane_np = {}                                      # Phase 15 DL: the run's environment the layout line does not print (as MN_P24 below)
    mdev_logl = int(env.get('BS_MDEV_LOGL', '30'))
    for i, line in enumerate(lines):
        if line.startswith('planes:'):                                    # Phase 15 PS: the plane pools per cap (binsplit_node_bytes), GB at 2 decimals
            m = re.match(r'planes: D (\S+) g (\d+) np (\d)', line); g = int(m.group(2)); npl = int(m.group(3)); nplane_np[(m.group(1), g)] = npl
            npm = np_mode if np_mode is not None else npl                 # ECALC_NP=auto prints np 3: pool 0 by the group (np_planes), pool 1 at three
            for cap, pb in re.findall(r'cap (\S+?)\*?: planes ([0-9.]+)', line):
                pl, r3 = cap_pool(CAPS[cap]); p0np = np_planes(npm, g, pl)
                p1np = 3 if npm == 'auto' or not pool1_4q else npm
                py = planes_bytes(pl, 0, r3, p0np, 'C', pool1_np=p1np) - int((0.61 if pl >= 30 else 2.16) * GB) + 610000000   # C: BS_TABLES_BYTES 0.61 GB at every cap
                ok = '%.2f' % (py * 1e-9) == pb; nplanes += 1
                if not ok: globals()['_c_layout_bad'] = globals().get('_c_layout_bad', 0) + 1
                print('   planes D %s g %d np %s cap %-6s C %s  model %.2f GB  %s' % (m.group(1), g, npm, cap, pb, py * 1e-9, 'exact' if ok else 'DIFFERS'))
            continue
        if not line.startswith('layout:'): continue
        v = dict((k, float(x)) for k, x in re.findall(r'(\w+(?: \w+)?) ([0-9.e+]+)(?!\w)', line.replace('(', ' ').replace(')', ' ').replace('|', ' ')))
        D, g, N = v['D'], int(v['g']), int(v['N'])
        tight, tdead = int(v.get('tight', 0)), int(v.get('tail_dead', 0))          # Phase 14 L1: the variant the C line was printed under
        ef = int(v.get('early_free', 0))                                          # Phase 14 T1: MN_TREE_EARLY_FREE
        dkm = int(v.get('dkm', 0))                                                # Phase 15 DL: the layout followed NEWTON_DKM ("dkm 1"; absent: today's)
        lean = int(v.get('lean', 0))                                              # int15k: "lean 1" = DM_MN_LEAN's division set
        rl = None                                                                 # DL: the room's decision line after it (`room:`), if any
        for l2 in lines[i + 1:i + 3]:
            if l2.startswith('room:'): rl = dict((k, float(x)) for k, x in re.findall(r'(\w+) ([0-9.e+]+)(?!\w)', l2.replace('(', ' ').replace(')', ' ').replace('|', ' '))); break
            if l2.startswith('layout:'): break
        vl = None                                                                 # B7ACCT: the v-slot line after the planes line (`vslot:`), if any
        for l2 in lines[i + 1:]:                                          # (to the next point: ECALC_VERBOSE=2 interleaves its tree lines)
            if l2.startswith('vslot:'): vl = dict((k, float(x)) for k, x in re.findall(r'(\w+) ([0-9.e+]+)(?!\w)', l2.split('| budget')[0].replace('|', ' '))); break
            if l2.startswith('layout:'): break
        npl = None
        for l2 in lines[i + 1:i + 4]:
            m2 = re.match(r'planes: D \S+ g \d+ np (\d)', l2)
            if m2: npl = int(m2.group(1)); break
        npm = np_mode if np_mode is not None else (npl or EC_NP)
        pl0 = pool_log; r3 = env.get('RNS_PLANES_3Q30', '0') not in ('', '0')        # as_room_fits: POOL_LOG, RNS_PLANES_3Q30 (not the size rule)
        room_planes = planes_bytes(pl0, 0, r3, np_planes(npm, g, pl0), 'C', pool1_np=3 if npm == 'auto' or not pool1_4q else npm) - int((0.61 if pl0 >= 30 else 2.16) * GB) + 610000000
        S_ = seed_span(N, g, seed_fill); nt_ = N // g if g > 1 else N           # (BS_LAYOUT_ONLY: rank 0's range [1, 1 + N / g))
        sb = seedbuf_vmm(N, nt_, S_) if g > 1 else 0
        pool_b = 0; ofi_b = 0
        if g > 1 and env.get('COMM_TRANSPORT') == 'shmem':                        # binsplit.c as_shmem_pool: binsplit_shmem_pool_rule's pool
            pd_ = {}; ofi = ofi_planned(env)                                       # Phase 17 OFIMEM: the comm_ofi pools beside the SHMEM pool
            nd = shmem_pool(dm_layout(N, g, pool_log, True, tight, tdead)['nq'], g, None, pool_log, t_chunk_mb, int(env.get('MDB_SHIFT_CHUNK_MB', '1024')), False, round_mb=int(float(env.get('COMM_SHMEM_ROUND_MB', '0') or 0)), detail=pd_, ofi=ofi,
                            vslot=(vslot_resident(dm_layout(N, g, pool_log, True, tight, tdead)['nq'], g, os.environ.get('MN_GROUPS') or None, pool_log, ((int(os.environ.get('MN_P24', '2') or 0), np_mode if np_mode is not None else EC_NP, g, os.environ.get('ECALC_NP_AUTO_MIN', '0') == '1') if int(os.environ.get('MN_P24', '2') or 0) else None))['per_apu'] if vslot_pool_on(env) else 0))
            mb = -(-nd // (1 << 20)); mb = -(-mb // 256) * 256; have = int(env.get('COMM_SHMEM_POOL_MB', str(mb) if ofi else '8192'))
            pool_b = (mb if env.get('COMM_SHMEM_POOL_AUTO', '1') != '0' and mb > have else have) << 20
            if ofi: ofi_b = NR * (int(env.get('COMM_OFI_POOL_MB', str(pd_['ofi_dev_mb']))) << 20)
        rh = room_host(N, g, pool_b + ofi_b, sb, env.get('MN_OUT_EARLY', '1') != '0')
        ar_room = None
        ch = VMM_CHUNK if arena_room > 0 else 0
        p24m_ = int(os.environ.get('MN_P24', '2') or 0)                           # B7ACCT: the v-slots (binsplit_vslot_bytes), the tree's P24 decision
        p24_ = (p24m_, np_mode if np_mode is not None else EC_NP, g, os.environ.get('ECALC_NP_AUTO_MIN', '0') == '1') if p24m_ else None
        vs = vslot_resident(dm_layout(N, g, pool_log, True, tight, tdead)['nq'], g, os.environ.get('MN_GROUPS') or None, pool_log, p24_) if g > 1 else dict(per_apu=0, tree_top=0)
        vb = NR * vs['per_apu'] if vslot_budget_on() else 0                       # ECALC_VSLOT_BUDGET=1: in as_room_fits' node
        bs = arena_bs_bytes(N, (N + g - 1) // g, S=seed_span(N, g, seed_fill))
        for rm in ([arena_room, 0.0] if arena_room > 0 else [0.0]):             # Phase 15 AS: the room, dropped when the node with it is over the budget
            L = dm_layout(N, g, pool_log, True, tight, tdead, room=rm, dkm=dkm, mdev_logl=mdev_logl, lean=lean); sc = []
            p24m = int(os.environ.get('MN_P24', '2') or 0)                 # (DL: '2', the code's default since Batch 3)  MS: the run's MN_P24 / ECALC_NP_AUTO_MIN (the layout line does not print them)
            p24 = (p24m, np_mode if np_mode is not None else EC_NP, g, os.environ.get('ECALC_NP_AUTO_MIN', '0') == '1') if p24m else None
            tree = tree_need_dev((L['nq'] + g - 1) // g, g, sc, 0, 'grid', pool_log, None, t_chunk_mb, ef, p24) if g > 1 else 0
            need = L['need_dev'] + (sc[0] if g > 1 else 0); want = max(need, tree)
            ar = sum(arena_of(b, want, ch) for b in bs)
            if rm > 0: ar_room = ar
            if rm == 0 or as_room_fits(room_planes, ar, rh + vb, float(env.get('ECALC_NODE_GB', '480'))): break   # (B7ACCT: the budget the C reads -- was 480 always)
        rows = [('nq (limbs)', v['nq'], L['nq']), ('hole', v['hole'], L['hole']), ('dm need / dev', v['dm_need'], need),
                ('top scratch / dev', v['top scratch'], sc[0] if g > 1 else 0), ('tree need / dev', v['tree_need'], tree),
                ('bs regions / node', v['bs regions'], sum(bs)), ('arena / node', v['arena'], ar)]
        if 'v2' in v: rows[3:3] = [('v2 / dev', v['v2'], L['v2']), ('v3 / dev', v['v3'], L['v3']), ('division / dev', v['div'], L['div']), ('jl (limbs)', v['jl'], L['jl'])]
        if 'room' in v: rows[3:3] = [('room / dev', v['room'], L['room']), ('chunk', v['chunk'], ch)]   # Phase 15 AS (DL: the room decided on this model's node, below)
        if rl is not None and ar_room is not None:                              # Phase 15 DL: the room's node, term by term (binsplit.c as_room_fits)
            rows += [('room planes', rl['planes'], room_planes), ('room arena', rl['arena_with_room'], ar_room), ('room seedbuf', rl['seedbuf'], sb),
                     ('room shmem pool', rl['shmem_pool'], pool_b)] + ([('room ofi pools', rl['ofi_pool'], ofi_b)] if 'ofi_pool' in rl else []) + [('room host', rl['host'], rh), ('room node', rl['node'], room_planes + ar_room + int(VMM_BS_GROW) + rh),
                     ('room fits', rl['fits'], 1 if as_room_fits(room_planes, ar_room, rh + vb, float(env.get('ECALC_NODE_GB', '480'))) else 0)]
        if vl is not None:                                                      # B7ACCT: the v-slots, term by term (+ the device / node with them over the room's terms)
            rows += [('vslot / APU', vl['per_apu'], vs['per_apu']), ('vslot tree top', vl['tree_top'], vs['tree_top'])]
            if rl is not None and ar_room is not None:
                rows += [('device + vslots', vl['device_with'], room_planes + ar_room + ofi_b + NR * vs['per_apu']),
                         ('node + vslots', vl['node_with'], room_planes + ar_room + int(VMM_BS_GROW) + rh + NR * vs['per_apu'])]
        print('D %.3g g %d (N %d) tight %d tail_dead %d early_free %d dkm %d:' % (D, g, N, tight, tdead, ef, dkm))
        for name, c, py in rows:
            rel = (py - c) / c if c else 0.0
            print('   %-18s C %16.0f  model %16.0f  %+.4f %%' % (name, c, py, 100 * rel))
            if name == 'arena / node': worst = max(worst, abs(rel))
            if py != c: nbad = globals().setdefault('_c_layout_bad', 0) + 1; globals()['_c_layout_bad'] = nbad
    print('largest arena difference: %.4f %%; %d term(s) not exact (%d plane figures compared)' % (100 * worst, globals().get('_c_layout_bad', 0), nplanes))
    return worst

# ---------------------------------------------------------------- Phase 14 L1: the modelled savings of DM_TIGHT / DM_TAIL_DEAD
VARIANTS = [('V0 (defaults)', dict()), ('DM_TIGHT=1 (E2)', dict(tight=True)), ('DM_TAIL_DEAD=1', dict(tail_dead=1)),
            ('DM_TIGHT=1 DM_TAIL_DEAD=1', dict(tight=True, tail_dead=1)), ('DM_TIGHT=1 DM_TAIL_DEAD=2 (E5 layout, needs the spill)', dict(tight=True, tail_dead=2))]
TARGET576 = dict(transport='shmem', staging='code', shift_chunk_mb=1024, depth=2)   # Phase 14 P2: staging 'code' (the measured pool law; was 'resident', the flat 8 GiB)   # the target's switches (RESULTS 82: 452 GB at 7.38e10 per node)

def savings():
    print('== Phase 14 L1: node peak (GB, modelled) per variant; size 1 at 2^31, three primes, the host fitted; 576 = the target share with %s' % TARGET576)
    sizes = [(1e10, 1), (4e10, 1), (1e11, 1), (1.3e11, 1), (1.68e11, 1), (7.38e10, 576)]
    print('%-52s |' % 'variant' + ''.join(' %9s' % ('%.3gx%d' % (D, g) if g > 1 else '%.3g' % D) for D, g in sizes))
    base = None
    for name, o in VARIANTS:
        row = []
        for D, g in sizes:
            oo = dict(OLD13); oo.update(o); oo.update(TARGET576 if g > 1 else {})   # Phase 15: on the forms of Phase 14 L1 (V0 = before DM_TIGHT)
            r = mem_per_node(int(D), g, oo); row.append((r['node_peak'], r['arena'], r['dm_need'], r['tree_need']))
        if base is None: base = row
        print('%-52s |' % name + ''.join(' %9.1f' % (p[0] / GB) for p in row))
        print('%-52s |' % '   arena (dm need; tree)' + ''.join(' %9s' % ('%.0f(%.0f;%.0f)' % (p[1] / GB, p[2] / GB, p[3] / GB)) for p in row))
        print('%-52s |' % '   saving vs V0' + ''.join(' %9.1f' % ((p[0] - b[0]) / GB) for p, b in zip(row, base)))
    print()
    print('== ceilings: one node (the largest D whose node peak fits 502 / 524 GB) and the 576 share (480 / 502 GB per node), per variant')
    for name, o in VARIANTS:
        o1 = dict(OLD13); o1.update(o); c1 = [max_digits_per_node(nb * GB, 1, o1) for nb in (502, 524)]
        oo = dict(o1); oo.update(TARGET576); c5 = [max_digits_per_node(nb * GB, 576, oo) for nb in (480, 502)]
        print('  %-52s one node %.3e / %.3e; 576: %.3e / %.3e per node = %.3e / %.3e digits' % (name, c1[0], c1[1], c5[0], c5[1], 576 * c5[0], 576 * c5[1]))

CONFIGS = [  # the one ceiling per configuration (TASKS 1.1): name, g, opts
    ('size 1 (one node, the defaults)', 1, dict()),
    ('576, SHMEM, the pool by the measured law (P214)', 576, dict(transport='shmem', staging='code', shift_chunk_mb=0)),
    ('576, SHMEM resident, 8 GiB pool (the pre-P214 model)', 576, dict(transport='shmem', staging='resident')),
    ('576, TCP (no SHMEM pool; aac6-style)', 576, dict(transport='tcp')),
    ('576, SHMEM + MDB_SHIFT_CHUNK_MB=1024 (the code)', 576, dict(transport='shmem', staging='code', shift_chunk_mb=1024)),
    ('576, SHMEM + MN_T_CHUNK_MB=1024', 576, dict(transport='shmem', staging='code', t_chunk_mb=1024)),
    ('576, SHMEM + both at 1024 MB', 576, dict(transport='shmem', staging='code', shift_chunk_mb=1024, t_chunk_mb=1024)),
    ('Phase 15: size 1, the defaults (DEFAULTS15)', 1, dict(p15=True)),
    ('Phase 15: 576, the defaults (SHMEM, pool from the plan)', 576, dict(p15=True, transport='shmem', staging='code', depth=2)),
    ('Phase 15: 576, the defaults + COMM_SHMEM_ROUND_MB=1024 (D2)', 576, dict(p15=True, transport='shmem', staging='code', depth=2, round_mb=1024)),
]

def ceilings():
    print('== the ceiling per configuration (502 GB node; 480 GB = the safe budget), with the node peak split at the ceiling')
    for name, g, o in CONFIGS:
        out = []
        o = dict(OLD13, **o) if not o.get('p15') else dict((k, v) for k, v in o.items() if k != 'p15')   # Phase 15: the old rows on the forms then
        for node in (502 * GB, 480 * GB):
            Dm = max_digits_per_node(node, g, o); r = mem_per_node(Dm, g, o); out.append((Dm, r))
        (D1, r), (D2, _) = out
        print('  %-52s %.2e / %.2e per node (%.2e digits in all): peak %.1f = planes %.1f + pool %.1f [arena %.1f = max(bs %.1f, dm %.1f, tree %.1f); top scratch %.1f] (exchange %.1f) + host %.1f (SHMEM pool %.1f)' % (
            name, D1, D2, D1 * g, r['node_peak'] / GB, r['planes'] / GB, r['pool_total'] / GB, r['arena'] / GB, r['regions_bs'] / GB, r['dm_need'] / GB, r['tree_need'] / GB,
            r['top_scratch'] / GB, r['exchange'] / GB, r['host_hwm'] / GB, r['shmem_pool'] / GB))

# ---------------------------------------------------------------- Phase 14 T1 (E10a): MN_TREE_EARLY_FREE at 576
def early_free_ceilings():
    print('== Phase 14 T1 (E10a): the 576 per-node ceiling (modelled) at 480 / 502 GB, the target switches %s, without / with MN_TREE_EARLY_FREE' % TARGET576)
    for name, o in [('V0', dict()), ('DM_TIGHT=1', dict(tight=True)), ('DM_TIGHT=1 MN_T_CHUNK_MB=1024', dict(tight=True, t_chunk_mb=1024))]:
        for ef in (False, True):
            oo = dict(OLD13); oo.update(o); oo.update(TARGET576); oo['early_free'] = ef
            cs = []
            for nb in (480, 502):
                Dm = max_digits_per_node(nb * GB, 576, oo); r = mem_per_node(Dm, 576, oo)
                cs.append('%.3e per node = %.3e digits (peak %.1f: arena %.1f = max(bs %.1f, dm %.1f, tree %.1f))' % (Dm, 576 * Dm, r['node_peak'] / GB, r['arena'] / GB, r['regions_bs'] / GB, r['dm_need'] / GB, r['tree_need'] / GB))
            print('  %-32s early_free %d: 480 GB: %s\n  %-32s               502 GB: %s' % (name, ef, cs[0], '', cs[1]))

# ---------------------------------------------------------------- Phase 15 (agent MD): the model on the defaults against the measured runs, and the target
MEASURED15 = [  # the Phase 14 defaults: (D per node, g, device at init, device max, host HWM, node used (MemAvailable drop; None: device max + host HWM), opts, source)
    (4e10, 1, 218.2, 224.0, 25.8, None, {}, 'V314 e4_def (job 21436, s24-30): planes 103.1 + regions 114.5 + tables 0.61 (the report then counted the borrowed 53.7 twice)'),
    (1e11, 1, 363.6, 365.7, 27.1, None, {}, 'V214 A3 c2_1e11_def (job 21443, s24-16) = V314 e11_def; the five-run series HWM 27.1-27.2'),
    (1.16e11, 1, 399.8, 404.3, 27.6, None, {}, 'V214 B c2_116_def (job 21447, s24-26)'),
    (1.3e11, 1, 435.6, 438.7, 27.7, None, {}, 'V214 B c2_130_def (job 21447, s24-26)'),
    (5e9, 2, None, None, 27.8, 530.9 - 351.2, dict(transport='shmem', staging='code'), 'V114 b2 (job 21439), 1e10 on 2 real nodes, SOS, pool 8704 from the plan: MemAvailable 530.9 -> min 351.2'),
    (5e9, 2, None, None, 21.3, 530.9 - 357.8, dict(transport='shmem', staging='code', round_mb=256), 'V114 b3 (job 21442), the same with COMM_SHMEM_ROUND_MB=256: pool 2560, MemAvailable min 357.8'),
]
MEASURED15B = [  # Phase 15 (agent DOC): the defaults of 2026-09-27 (BS_SEED_FILL=128 ...), RESULTS 86's paired series (the same columns)
    (1e11, 1, 377.3, 393.6, 27.4, None, {}, 'RESULTS 86 series, every candidate on (= the defaults of 2026-09-27; jobs 21550 / 21563, ten runs, s24-16 / s24-26): planes 103.1 + regions 136.2 + pool 137.4 at init; dm 393.6 (pool 289.9); HWM 27.3-27.6'),
]

def report15():
    print('== Phase 15: the memory model on the defaults (%s) against the measured runs (GB; measured / model; each row on the defaults of its run: [B0], [B1])' % ', '.join('%s=%s' % kv for kv in DEFAULTS15C.items()))
    for D, g, di, dm, hw, used, o, src in [(D, g, di, dm, hw, used, dict(DEFAULTS15, **o), '[B0] ' + src) for D, g, di, dm, hw, used, o, src in MEASURED15] + [(D, g, di, dm, hw, used, dict(DEFAULTS15B, **o), '[B1] ' + src) for D, g, di, dm, hw, used, o, src in MEASURED15B]:
        r = mem_per_node(int(D), g, o)
        node_m = used if used is not None else dm + hw
        print('  %.3g x %d: device init %s / %.1f, device max %s / %.1f, host HWM %.1f / %.1f, node %.1f / %.1f (%+.1f %%)  | %s' % (
            D, g, '%.1f' % di if di else '-', r['dev_init'] / GB, '%.1f' % dm if dm else '-', r['dev_max'] / GB, hw, r['host_hwm'] / GB, node_m, r['node_peak'] / GB,
            100 * (r['node_peak'] / GB / node_m - 1), src))
    D = TARGET_DIGITS / TARGET_NODES; groups = '2,4,8,16,32,64,192,576'   # Phase 15 TGT: the target constant (was 4.25e13)
    print('\n== the target: %.3ge13 digits on 576 nodes (D %.4e per node, MN_GROUPS %s, SHMEM, depth 2, ECALC_NP=%s: the launch line; BS_ARENA_ROOM %.2f: the default), GB per node (modelled)' % (TARGET_DIGITS / 1e13, D, groups, TARGET_NP, ARENA_ROOM))
    for name, o in [('the defaults (COMM_SHMEM_ROUND_MB off)', {}), ('the defaults + COMM_SHMEM_ROUND_MB=1024 (D2: the launch line)', dict(TARGET_LAUNCH)),
                    ('  ... BS_ARENA_ROOM=0 (B1)', dict(TARGET_LAUNCH, arena_room=0.0)),
                    ('  ... ECALC_NP=4 (the launch line of 2026-09-27; RNS_POOL1_4Q)', dict(TARGET_LAUNCH, np=4)),
                    ('  ... ECALC_NP=4 BS_ARENA_ROOM=0 (B1 on its launch line)', dict(TARGET_LAUNCH, np=4, arena_room=0.0)),
                    ('  ... ECALC_NP=3 (refused by the plan check: shown for the memory only)', dict(TARGET_LAUNCH, np=3)),
                    ('  ... the Phase 14 defaults (B0: BS_SEED_FILL=0, MN_OUT_EARLY=0)', dict(TARGET_LAUNCH, seed_fill=0, out_early=False)),
                    ('  ... the pre-Phase-15 host (no VMM seed buffers, no bs growth)', dict(TARGET_LAUNCH, vmm=False)),
                    ('  ... MN_TREE_EARLY_FREE=0', dict(TARGET_LAUNCH, early_free=False)), ('  ... DM_TIGHT=0', dict(TARGET_LAUNCH, tight=False)),
                    ('  ... MN_T_CHUNK_MB=0', dict(TARGET_LAUNCH, t_chunk_mb=0))]:
        oo = dict(transport='shmem', staging='code', depth=2, groups=groups, np=TARGET_NP); oo.update(o); r = mem_per_node(int(D), 576, oo)
        print('  %-66s node %6.1f = max(init %5.1f + %4.1f, bs %5.1f + %4.1f, dm %5.1f + %4.1f); arena %.1f (bs %.1f, dm %.1f, tree %.1f); pool %.0f MiB (need %.0f)' % (
            name, r['node_peak'] / GB, r['dev_init'] / GB, r['host_init'] / GB, r['dev_bs'] / GB, max(r['host_init'], r['host_dm']) / GB, r['dev_dm'] / GB, r['host_dm'] / GB,
            r['arena'] / GB, r['regions_bs'] / GB, r['dm_need'] / GB, r['tree_need'] / GB, r['shmem_pool'] / 2 ** 20, r['shmem_need'] / 2 ** 20))
    print('\n== ceilings (the largest D per node whose node peak fits; digits in all = D x g)')
    for name, g, o in [('one node, the defaults (three primes)', 1, {}), ('576, the defaults, ECALC_NP=%s' % TARGET_NP, 576, dict(transport='shmem', staging='code', depth=2, groups=groups, np=TARGET_NP)),
                       ('576, the launch line (+ COMM_SHMEM_ROUND_MB=1024, ECALC_NP=%s)' % TARGET_NP, 576, dict(TARGET_LAUNCH, transport='shmem', staging='code', depth=2, groups=groups, np=TARGET_NP))]:
        cs = []
        for nb in (480, 502):
            Dm = max_digits_per_node(nb * GB, g, o, hi=4e11); cs.append('%.0f GB: %.2e per node = %.3e digits' % (nb, Dm, Dm * g))
        print('  %-52s %s' % (name, '; '.join(cs)))

if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--p15':
        report15(); sys.exit(0)
    if len(sys.argv) > 1 and sys.argv[1] == '--cache':                  # Phase 15 TC
        cache_report(); sys.exit(0)
    if len(sys.argv) > 1 and sys.argv[1] == '--e10a':
        early_free_ceilings(); sys.exit(0)
    if len(sys.argv) > 2 and sys.argv[1] == '--check-c':
        c_layout_check(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 31, float(sys.argv[4]) if len(sys.argv) > 4 else 1024, int(sys.argv[5]) if len(sys.argv) > 5 else SEED_FILL,
                       float(sys.argv[6]) if len(sys.argv) > 6 else ARENA_ROOM, (sys.argv[7] if sys.argv[7] == 'auto' else int(sys.argv[7])) if len(sys.argv) > 7 else None,
                       int(sys.argv[8]) if len(sys.argv) > 8 else 1)
    elif len(sys.argv) > 1 and sys.argv[1] == '--ceiling':
        ceilings()
    elif len(sys.argv) > 1 and sys.argv[1] == '--savings':
        savings()
    elif len(sys.argv) > 1 and sys.argv[1] == '--pool':
        pool_check(); print(); pool_target()
    else:
        main()

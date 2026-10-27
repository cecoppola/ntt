#!/usr/bin/env python3
"""mem_model.py - the per-node memory model of ecalc (Phase 11, agent M; archive:PLAN §26 row M, item 3).

Device and host bytes of one node-process as functions of the digits per node D and of the group size g
(the number of node-processes the run is spread over), built from the same formulas the code sizes its
allocations with (binsplit.c: e_terms, seed_limbs, region_need, dm_layout, tree_need_dev; rns_mul.c: the
plane pools; rns_dist.c: the sharded exchange's scratch), calibrated against the mem_report tables of
archive:results_merged_2026-10-24/MEMORY.md §M, archive:results_merged_2026-10-24/MEMORY.md §M11 (4, 7, 8 x 10^10 at size 1; 10^10 at size 4).

    mem_per_node(D, g, opts) -> dict      (bytes; opts: pool_log, tail, alltoallv, margin, form, groups, transport ...)
    python3 mem_model.py --cache          the mn transform cache rows at the target (cache_report)
    python3 mem_model.py --check-c FILE [POOL_LOG [MN_T_CHUNK_MB [BS_SEED_FILL [BS_ARENA_ROOM [ECALC_NP [RNS_POOL1_4Q]]]]]]   the C layout (BS_LAYOUT_ONLY) against this port (MN_T_CHUNK_MB 1024, BS_SEED_FILL 128, BS_ARENA_ROOM 0.16 (the default since 2026-09-28),
                                          ECALC_NP 3 / 4 / auto as the run's (auto read from the file's plan lines) (the `planes:` lines: the plane pools at every cap, Phase 15 PS), RNS_POOL1_4Q 1)

mem_per_node's defaults (DEFAULTS) are the code's: DM_TIGHT, MN_TREE_EARLY_FREE, MN_T_CHUNK_MB and MDB_SHIFT_CHUNK_MB 1024, the SHMEM pool from the plan,
DB_POOL_VMM's host and bs terms, ECALC_PLANE_CAP 2^31 (the C default is now `run`: 2^30 up to about 9.6e9 digits per node; the model does not pick it, pass cap explicitly -- every explicit cap is unaffected), BS_SEED_FILL=128, MN_OUT_EARLY, BS_ARENA_ROOM, NEWTON_DKM.  TARGET_LAUNCH is the target's launch line
(XSHMEM_ROUND_MB=1024 = round_mb, the user's D2); the pre-Phase-14 forms (OLD13), the Phase 14 / 15 defaults B0 / B1 and the historical tables are in the archive
(archive:scripts_2026-10-25/mem_model_legacy.py).  --check-c is exact against BS_LAYOUT_ONLY at 4e10, 1e11 and the target's share.
B7ACCT (2026-10-06): vslot_resident -- the general map's v-exchange slots (comm_layered.c, hipMalloc'd beside the arena) on dev_bs / dev_dm;
--check-c compares the C `vslot:` line (per APU, the tree top, device / node with them); the budget as the C (always on).
TARGET_NP = 'auto' (the launch line's ECALC_NP=auto).  The mn transform cache: cache_fit_slots follows the code's RNS_DIST_CACHE_FIT rule
(rns_dist.c rns_dist_cache_plan: ECALC_NODE_GB less the layout's node less the 32 GB reserve, over one slot per node): 0 at the target.
--check-c defaults to BS_ARENA_ROOM 0.14 and reads ECALC_NP=auto from the file's `plan` lines.

mn_model.py (and its estimate subcommand) import mem_per_node for their memory rows.  Every number is "modelled"
unless the calibration table says "measured"; the tables' sources are named in archive:results_merged_2026-10-24/MEMORY.md §M11.  Phase 12 (agent Q):
the tree's g-terms in both forms -- 'flat' (the code at 7aded87) and 'grid' (agent G's gridded top product, archive:PLAN §27) --
and L's level schedule (mn_groups, MN_GROUPS); see archive:results_merged_2026-10-24/TIME_MODEL.md §Q.
"""
import math, sys, functools, os

LN10 = math.log(10.0)
NR = 4                                   # regions of one binary splitting (4 per node, one per APU; ONE per PE with ECALC_PE_PER_GPU)
NRN = 4                                  # devices (APUs / PEs) per node: the per-node multipliers (planes, v-slots, comm pools, the dm numbers' share)
PE_LANES = 0                             # ECALC_PE_PER_GPU=1: PEs per node (ECALC_VPE_LANES, default 4); 0 = off
PPN_PROC = 1                             # S2c (ECALC_PE_PER_GPU=1): processes per node, each with its own runtime + comm (binsplit.c bs_ppn_proc); 1 = one process per node
def lanes(): return PE_LANES or 1        # PEs per node (binsplit.c bs_lanes)
def set_pe(L, ppn_proc=1):
    """S1d (TARGET1): the PE planner, in lockstep with binsplit.c.  ECALC_PE_PER_GPU=1 with L lanes: a node is L identical PEs of one device each; every PE runs
    its own binary splitting over its term range with ONE region, so the per-node figures are the one PE's times L (arena, seed buffers, writer);
    the devices per node are L (planes, v-slots, comm pools, the quarter of the dm numbers); the checkpoint staging is L GiB.  The tree's group
    schedule stays over the nodes (the groups below a node are not modelled until the PE-level products exist).  L = 0: off (the code before S1d)."""
    global NR, NRN, PE_LANES, PPN_PROC, HOST_STAGING, HOST_WRITER, HOST_EARLY
    PE_LANES = int(L); PPN_PROC = int(ppn_proc) if L else 1; NR = 1 if L else 4; NRN = int(L) if L else 4
    HOST_STAGING = (int(L) if L else 4) * (1 << 30) * (2 if L and PPN_PROC > 1 and os.environ.get('RNS_BATCH_SPLIT', '') == '1' else 1); HOST_WRITER = int(0.65 * GB) * lanes(); HOST_EARLY = int(0.9 * GB) * lanes()
def _pe_from_env():
    g = os.environ.get('ECALC_PE_PER_GPU', '')
    if g and g != '0': L = int(os.environ.get('ECALC_VPE_LANES') or 4); set_pe(L, L)   # S2c: one process per PE: the host terms x PEs per node
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
    G = g * lanes()                       # (a PE's range with ECALC_PE_PER_GPU: binsplit.c bs_pe_b1)
    return seed_terms_for(N + 1 if G <= 1 else 1 + N // G, fill)

def seed_limbs(N, nterms, S=256, decimal=True):
    """per (limbs per span, the bound), nspan for a node computing nterms of the N-term series"""
    nspan = (nterms + S - 1) // S
    per = (S * math.ceil(math.log2(N + 2.0)) + 128) // (59 if decimal else 64) + 2
    return per, nspan

def region_of(i, n): r = i * NR // n; return r if r < NR else NR - 1
def place_node(i, n, balance_n=16): return region_of(i, n) if n > balance_n else i % NR

POOL_RULE = 1     # (BS_POOL_RULE removed 2026-10-12: binsplit.c has rule 1 only) Phase 16 R: 1 = the tier prediction from the lower bound on the level's largest Q (binsplit.c level_q_lower); 0 = 0.9 x the bound

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
    a0: the node's first term (rank 0: 1); rule: Phase 16 R's tier prediction (None = the code's, 1; 0 = the old rule, for comparison only)"""
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
    return out * lanes()                  # the node's L PEs are alike (one region each)

def quarter_bytes(limbs): return ((limbs + NRN - 1) // NRN + 4095) // 4096 * 4096 * 8   # one device's share (a quarter on the 4-APU node)
VMM_CHUNK = 2 << 30                      # dbig.c db_pool_vmm_chunk: DB_POOL_VMM_CHUNK_GB (2 GiB)
def as_host(): return 7000000000 * PPN_PROC + (8 << 30)         # binsplit.c binsplit_node_bytes: bs_host_init_bytes (BS_HOST_INIT_BYTES, the runtime x processes per node), size 1 only (size > 1: room_host, B5 fix -- see layout_node)
def room_host(N, g, pool, seedbuf, out_early=True):
    """Phase 15 DL: binsplit.c as_room_fits's host (bytes) -- this model's host HWM: size 1 host_size1_vmm with the digits as log10 N!; size > 1
    max(host_init, host_dm) = the runtime + the staging + the transport + the SHMEM pool + max(the two VMM seed buffers, the writer + MN_OUT_EARLY)"""
    if g == 1:
        D = math.lgamma(N + 1.0) / LN10 / 1e9 - 40.0
        return int((25.85 + 0.0208 * (D if D > 0 else 0.0)) * 1e9) + (PPN_PROC - 1) * 7000000000   # S2c: one more runtime per further process
    base = PPN_PROC * (int(HOST_RUNTIME) + int(HOST_COMM_PER_PROC)) + HOST_STAGING + pool
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

def arena_trim(L, scratch=0, trim_gb=None):
    """binsplit.c dm_layout's BS_ARENA_TRIM_GB (env when trim_gb is None; 0 = off): the dm need with the tree's top scratch added (the C adds it
    inside dm_layout) less the trim, never below the largest live set max(v2, v3); sets L['need_dev'] and L['trim'] (bytes) and returns the need"""
    L['need_dev'] += scratch
    t = float(os.environ.get('BS_ARENA_TRIM_GB', '0') or 0) if trim_gb is None else trim_gb
    t = min(t, 1e4) if t == t else 0                                         # (the C's cap: nan / inf / huge)
    L['trim'] = min(int(t * 1e9), max(0, L['need_dev'] - max(L['v2'], L['v3']))) if t > 0 else 0
    L['need_dev'] -= L['trim']
    return L['need_dev']

def dm_mn_wrap(env=None):
    """P1 / D19 (results/LAUNCH_DEFAULTS.md §DEF): DM_MN_WRAP's value (newton_db.c dm_wrap: 0 off, the fallback; 1 the cap's plane, the default since D22; k >= 20 a plane of 2^k)"""
    v = (env if env is not None else os.environ).get('DM_MN_WRAP', '1') or '1'
    try: return max(0, int(v))
    except ValueError: return 0

def wrap_bytes(nq, k, g, pool_log=31, wrap=None):
    """D19 (results/LAUNCH_DEFAULTS.md §DEF; binsplit.c dm_wrap_bytes): DM_MN_WRAP's extra bytes per device in the mn division's low-product moment
    (newton_db.c mn_wrap_step): Q B^-h folded (Qpp, basis P + 4, held over both steps) and the folded A' (T1, basis P + 4) beside the
    assembly's Rc + Tt (basis w each, the moment Aw + xql was) -- 2 quarters of P + 4 limbs; 0 when the wrap is off or not used
    (mn_wrap_plane: P = 2^cap or 2^k in [2^logmin, 2^cap]; w = n_Q + 2 >= P + 2; at 1, X_hi Q (k1 + 1 + n_Q limbs) over one plane)"""
    v = dm_mn_wrap() if wrap is None else wrap
    if not v or g < 2: return 0
    cap = mn_cap_log(g, pool_log); lmin = mn_logmin(g); lg = cap if v == 1 else v
    if lg < lmin or lg > cap or lg > 62: return 0
    P = 1 << lg; w = nq + 2
    if w < P + 2: return 0
    if v == 1 and (k - k // 2 + 1) + nq <= (1 << cap): return 0
    return 2 * quarter_bytes(-(-(P + 4) // g))

def dm_layout(N, g, pool_log=31, decimal=True, tight=False, anchor=True, room=0.0, dkm=False, mdev_logl=30, lean=False, wrap=None):
    """binsplit.c dm_layout: n_Q, k_mu, t1, the hole (t1's quarter), the dm need per device (bytes).
    Phase 14 L1 (APUMULT_STUDY E2): tight = DM_TIGHT (the reciprocal's r2 at 2 jl + 4 and t1 at Q_t r's size at the last doubling
    jl = ceil(k/2), the top level's pairs freed as consumed);
    the division's own set (S, Q, mu + t or X + xq, a piece) is a term of v2 in every variant (it never
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
    kr = k // 2 + 1 if dkm else k                                              # Phase 15 DKM (results/DIVISION.md §DKM15 1.4): the reciprocal to h = floor(k/2) + 1
    k1 = k - k // 2                                                            # DL: step 1's quotient limbs
    jl = (kr + 1) // 2 + CHAIN_GUARD_CODE if anchor else kr - 1               # NEWTON_CHAIN_GUARD (D25, built in): the last doubling starts at ceil(kr/2) + 1 (binsplit.c dm_layout)
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
    hi = 2 * qp + quarter_bytes(k_s + 1) + quarter_bytes(2 * k_s + 8)
    lo = (qp + quarter_bytes(k_s) + quarter_bytes(nq_s + 2) + quarter_bytes(nq_s + k_s + 8)) if tight else (2 * qp + quarter_bytes(k_s) + quarter_bytes(nq_s + k_s + 8))
    if dkm:                                                                    # DKM: step 1's high product Q + S + mu (h) + t (k); step 2's Q + X_hi (k/2)
        h_s = (k // 2 + 1 + g - 1) // g                                        # + R1 (n_Q) + mu + t; step 2's low product Q + X + Aw + xq (n_Q + k/2) + X_lo
        hi = max(2 * qp + quarter_bytes(h_s + 1) + quarter_bytes(k_s + 8), qp + quarter_bytes(h_s) + quarter_bytes(nq_s + 2) + quarter_bytes(h_s + 1) + quarter_bytes(k_s + 8))
        lo = qp + quarter_bytes(k_s) + quarter_bytes(nq_s + 2) + quarter_bytes(nq_s + h_s + 8) + quarter_bytes(h_s)   # X, Aw, X_lo Q (n_Q + k/2), X_lo
        if lean: lo = 3 * quarter_bytes(nq_s + 2) + quarter_bytes(k_s + 1) + quarter_bytes(h_s + 1)   # int15k (DM_MN_LEAN, size > 1): Qw + Aw + xql (w each) + X (k; the X0 form) + X_lo (h)
    wr = wrap_bytes(nq, k, g, pool_log, wrap) if lean else 0                  # D19: DM_MN_WRAP's Qpp + T1 (env DM_MN_WRAP when wrap is None, as the C reads it)
    lo += wr
    div = quarter_bytes(piece) + max(hi, lo)
    v2 = max(v2, div)
    v2 += min(v2 // 8, 1 << 30)
    inn = quarter_bytes(nq_s // 2 + nq_s // 20 + 8); out = qp
    top = 4 * inn + 2 * out if not tight else max(4 * inn + out, 2 * inn + 2 * out)
    top += top // 8 + hole                                      # v3: the top bs levels beside the tail
    need = max(v2, top)
    room_b = int(room * float(hole)) if room > 0 else 0                  # Phase 15 AS: BS_ARENA_ROOM=<room> (with the VMM pool): room x the hole in the dm need
    need += room_b
    return dict(nq=nq, k=k, tcap=tcap, hole=hole, thresh=hole - hole * 3 // 8, need_dev=need, need_v2=v2, v2=v2, v3=top, div=div, jl=jl, t1_quarter=quarter_bytes(tcap), room=room_b, dkm=dkm, lean=lean, wrap=wr)

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

NP2CAP = os.environ.get("MN_NONPOW2_CAP", "") or "auto"   # rns_mul.c rns_np2cap: MN_NONPOW2_CAP unset/auto = on at digits per node >= MN_NONPOW2_MIN, 1 = on, 0 = off
NP2MIN = float(os.environ.get("MN_NONPOW2_MIN", "") or 4.5e10)   # results/DIVISION.md §NPCAP: crossover about 4.5e10 digits per node (mod)
NP2_D = 0.0                                                   # the run's digits per node (set by mem_per_node); 0 = unknown: auto counts as on, as the C does before the digits are set
def np2_on(g):
    """rns_mul.c rns_np2cap: the run's group size g not a power of two, and the switch on (auto: digits per node >= MN_NONPOW2_MIN)"""
    if g < 3 or not g & (g - 1) or NP2CAP not in ("auto", "1"): return False
    return NP2CAP != "auto" or NP2_D <= 0 or NP2_D >= NP2MIN
def np2_q(q, g):
    """rns_mul.c rns_q_slot: the rank plane the pools are made for: 43/32 of q = 2^(pool_log-2) under MN_NONPOW2_CAP in a non-pow2 run (g)"""
    return q + q * 11 // 32 if np2_on(g) else q

def mn_cap_log(g, pool_log=31):
    """rns_dist.c mn_logn_cap: 2^(min(31, pool_log) + floor(log2 g)) points per piece over g nodes (one less where the
    rounding of R / nr would not fit the pools: never for g <= 2304)"""
    lgt = 0
    while (2 << lgt) <= g: lgt += 1
    c = min(31, pool_log); logn = c + lgt
    if g & (g - 1):
        qcap = np2_q(1 << (c - 2), g)
        qm = mn_shape(1 << logn, g)[3]
        if qm > qcap: logn -= 1
        elif np2_on(g) and mn_shape(1 << (logn + 1), g)[3] <= qcap: logn += 1   # MN_NONPOW2_CAP: the next power of two fits
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
    nodes it exceeds the node by itself above ~2e10 digits per node (archive:results_merged_2026-10-24/MEMORY.md §M11)."""
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

class NoGrid(ValueError):
    """a product that no grid of at most 32 x 32 pieces holds within the plane cap: the C code's `grid: ... limbs at ... points` fatal"""

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
    if best is None: raise NoGrid("no grid of at most 32 x 32 pieces: %d x %d limbs at %d points" % (na, nb, cap))
    return best[1], best[2]

# MS (Phase 15): P24's branch of rns_mul_dist_mn_scratch (MN_P24, results/PRIMES_STRATEGY.md §P2415; rns_dist.c mn_p24_of / p24_grid_shape / p24_split).
# p24 = (mode, np, g_run): mode 2 = every mn product when pool 0 holds four planes (rns_pool0_np: np_planes(np, g_run) >= 4);
# 1 = the products whose 18-digit grid's largest piece runs four primes (ECALC_NP=4: all; auto: pa + pb over the three-prime bound).  +34 MB of dm need per device at 5.1e13 on 576, measured by the layout.
P24_CAP_LOG = 40
def p24_pts(n): return -(-3 * n // 4)
def mn_logmin(g):
    nr = 4 * g; lg = 0
    while (1 << lg) < nr: lg += 1
    return max(20, 2 * (5 + lg))
def mn_p24_of(na, nb, g, p24, pool_log=31):
    """rns_dist.c mn_p24_of"""
    if not p24 or not p24[0]: return False
    mode, np, g_run = p24
    if mode >= 2: return np_planes(np, g_run, pool_log) >= 4
    pa, pb, cap = na, nb, 1 << mn_cap_log(g, pool_log)
    if na + nb > cap:
        if -(-na // 32) + -(-nb // 32) > cap: pa, pb = -(-na // 32), -(-nb // 32)
        else: ka, kb = split_grid_cap(na, nb, cap, 1 << mn_logmin(g)); pa, pb = -(-na // ka), -(-nb // kb)
    if np == 'auto': return pa + pb > NP3_MAX_TERMS
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
    if best is None: raise NoGrid("no grid of at most 32 x 32 pieces: %d x %d limbs at %d points (P24)" % (na, nb, cap))
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

# ---------------------------------------------------------------- B7ACCT (results/GRID_GROUPS.md §B7ACCT): the general map's v-exchange slots
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
    # S36 (binsplit.c): one pair per device handed from level to level -> the largest level, not the sum
    maxlow = max([v for (_, gg, v) in levels if gg < g] or [0])
    return dict(per_apu=max(maxlow, top_tree, dm), tree_top=max(maxlow, top_tree), levels=levels, dm=dm, summed=lower + max(top_tree, dm))

# ---------------------------------------------------------------- the other pools (measured constants where the code has them)
def planes_3q30(pool_log=31, digits=0):
    """the 3 2^k plane pools (RNS_PLANES_3Q30 and its size rule) were removed from the code (SIMP, results/CODE_REDUCTION.md §SIMP): never on.
    The p3q30 / cap arguments below keep modelling the old runs measured with them"""
    return False

EC_NP = 3                                # Phase 13b step 0: three primes are the default for decimal limbs (ECALC_NP; binary limbs need 4)
NP3_MAX_TERMS = 58424467928              # crt.c ec_np3_max_terms: floor((p0 p1 p2 - 1) / (10^18 - 1)^2), the largest nc = pa + pb three primes hold

def np_planes(np, g=1, pool_log=31, auto_terms=NP3_MAX_TERMS):
    """Phase 15 NP (crt.c ec_np_planes, rns_mul.c rns_pool0_np): the planes plane pool 0 is made for under ECALC_NP=np -- np itself, or for
    'auto' (the prime count per product): D18 (the default since 2026-10-12) 4 at every g > 1 (every multi-node product in P24); at size 1
    4 when 2^(min(31, pool_log)) points exceed the switch-over (ECALC_NP_AUTO_TERMS, default the three-prime bound), else 3.
    The three-prime multi-node rule (ECALC_NP_MN3) was removed 2026-10-19 (D56)"""
    if np != 'auto': return np
    if g > 1: return 4
    c = pool_log if 0 < pool_log < 31 else 31
    lg = g.bit_length() - 1 if g > 1 else 0
    return 4 if (1 << (c + lg)) > auto_terms else 3

# ---------------------------------------------------------------- Phase 13b agent D: the plane cap and the product strategy
CAPS = {'2^30': 1 << 30, '3*2^29': 3 << 29, '2^31': 1 << 31, '3*2^30': 3 << 30}   # the four plane caps of archive:PLAN §31 (points)

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
    for d in range(NRN):
        cap = [c0, c1]; used = [0, 0]; ex = 0
        for sz in b_planes(form, d, n, np):
            for r in (0, 1):
                if cap[r] - used[r] >= sz: used[r] += sz; break
            else: ex += sz
        out.append(ex)
    return out

def planes_bytes(pool_log=31, digits=0, p3q30=None, np=None, strategy='C', cap=None, pool1_np=None, g=0):
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
        q = np2_q((1 << pool_log) // 4, g)   # MN_NONPOW2_CAP (g = the run's group size)
    p0 = np * q * 8
    p1 = (3 * q + 16) * 8 if pool_log > 30 else max((3 * q + 16) * 8, 8 << min(pool_log, 30))   # pool 1 is the full pool at POOL_LOG <= 30 (archive:results_merged_2026-10-24/MEMORY.md §A-mem, open issue 1)
    if (np if pool1_np is None else pool1_np) >= 4: p1 = max(p1, 4 * q * 8)   # Phase 15 PS (rns_plane_pool_bytes): four one-node primes -- the batch-local tier's
                                                                      # np Mmax L B limbs reach its plane cap 4 q (L = q, Mmax = 1); 3 q + 16 grew inside the bs phase (rc 6)
    p1 = (p1 + al - 1) // al * al
    per = p0 + p1
    extra = 0
    if strategy in ('B', 'B4'):                                       # rns_dist.c (agent B): what the pools lack for the largest product
        extra = 8 * sum(b_extra_limbs(strategy, 4 * q, pool_log, on, np, pool1_np))   # (the plane of the cap, 4 q points) comes from one grow-only
                                                                      # hipMalloc buffer per APU, kept to the end of the dm phase (at the dm peak)
    return NRN * per + extra + int((0.61 if pool_log >= 30 else 2.16) * GB)   # + the transform contexts: 0.61 GB at 2^31, 2.16 at 2^29 (measured; 2^30 assumed = 2^31)

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

# ---------------------------------------------------------------- the defaults of the code (ecalc/README.md): DB_POOL_VMM=1 (the arena a VMM range), DM_TIGHT
# following it, MN_TREE_EARLY_FREE=1, MN_T_CHUNK_MB=1024, MDB_SHIFT_CHUNK_MB=1024, the pool rule (the pool raised to the need) with the pool taken from MN_PLAN_ONLY's
# `plan pool` line (mnrun.sh: the pool = the need rounded up to 256 MiB, the device heap exactly the pool), BS_SEED_FILL=128, MN_OUT_EARLY=1,
# BS_ARENA_ROOM=ARENA_ROOM (binsplit.c bs_arena_room: the arenas in whole VMM chunks + that fraction of the hole in the dm need), NEWTON_DKM=1 with
# dm_layout following it (MN_P24=2 is the tree's scratch: opts p24; mn_model passes it) and the lean mn division.  exchange rounds (round_mb; the code's XSHMEM_ROUND_MB) are off in the
# default; the target's launch line adds 1024 (TARGET_LAUNCH, the user's D2).  mem_per_node's base.
ARENA_ROOM = 0.14                                       # BS_ARENA_ROOM's code default (was 0.16 from 2026-09-28, be2eec3; 0.14: memfix, one chunk less per APU at 2 n and 4 n)
DKM_CODE = True                                                    # NEWTON_DKM: built in (switch removed 2026-10-12)
CHAIN_GUARD_CODE = 1                                                                                                                  # the chain guard (D25; the NEWTON_CHAIN_GUARD=0 fallback was removed 2026-10-23, Q87)
LEAN_CODE = True                                                   # int15k: DM_MN_LEAN, built in since 2026-10-12 -- the lean mn division set at size > 1 (opts lean)
DEFAULTS = dict(tight=True, early_free=True, t_chunk_mb=1024, shift_chunk_mb=1024, pool='plan', vmm=True, round_mb=0, cap=1 << 31, seed_fill=SEED_FILL, out_early=True,
                arena_room=ARENA_ROOM, dkm=DKM_CODE, lean=LEAN_CODE)   # cap: ECALC_PLANE_CAP 2^31 (13c); the C default is `run` since 2026-10-25 (2^30 where the largest division product fits its plane, else 2^31): the model still assumes 2^31 when cap is not given, so it over-predicts the plane pools of a default run in that range; explicit caps are exact
TARGET_LAUNCH = dict(round_mb=1024)                     # D2 (results/SHMEM.md): the target's launch line
TARGET_NP = 'auto'                                      # the user's decision 1 of 2026-09-28: ECALC_NP=auto on the target's launch line (four primes only for the
                                                        # products over the three-prime bound; pool 0 at four planes at 576, pool 1 at three); history: 4 (the decision
                                                        # of 2026-09-27: three primes cannot hold the target's mn pieces, MN_PLAN_ONLY refuses them, results/PRIMES_STRATEGY.md §P15;
                                                        # ECALC_NP=4 is +17.2 GB per node over auto with RNS_POOL1_4Q); the code's default stays 3 (decimal)
TARGET_DIGITS = 3.82e13                                 # CURRENT: the user decision of 2026-10-12 (3.82e13 digits on 576 nodes, results/TARGET.md §TGT). Before it (3.71e13):
                                                        # (2026-10-06, s18-target, the user's decision 2026-10-06 ~19:30 EDT: "3.71e13 is fine", the size
                                                        # is not important while the implementation is built) -- from results/TARGET.md §TGTBENCH2 section 4's lower
                                                        # candidate (the device edge's margin over the proposed 3.76e13): login-node MN_PLAN_ONLY / BS_LAYOUT_ONLY
                                                        # at 576 (verified here 2026-10-06, b7-vslot aa86cac): device_with 363.526 GB vs the 373.44 GB measured
                                                        # edge (9.91 GB under), node 378.785 / 405.612 GB with the general-map v-slots (480 budget: +74.4 GB),
                                                        # pieces (node 0 / critical path) 103 / 107 (1236 products), `plan check` OK; mn_model.py estimate --fabric target-m
                                                        # at this D/g: 221.9 s modelled without the output write / 225.1 s with it at --bw 47 (ROCm 7.2.4, MAP_RATE
                                                        # 0.010 measured on the target).
                                                        # history: 4.08e13 (TGT17, 2026-10-06, superseded by this decision -- device 372.12 GB, 1.32 GB under the
                                                        # edge, 282.4 s mod at --bw 47), 5.276e13 (the target from the user's decision of 2026-09-29, Batch 3, to
                                                        # 2026-10-06; Phase 15 TGT's 2026-09-27 23:50 EDT decision was 5.1e13; room kept up to 5.396e13), 5.167e13
                                                        # (int15k's second test size, 2026-10-03), 4.25e13 (Phase 13d D2 - 2026-09-27), 4.4e13 (Phase 13c).  The C
                                                        # plan (MN_PLAN_ONLY, ECALC_NP=4, archive:results_merged_2026-10-25/TARGET_removed.md (TGT15)): 5.10e13 is the last size at 222 / 242 pieces (5.11 node 0
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
HOST_EARLY = int(0.9 * GB)                              # MN_OUT_EARLY (results/OUTPUT_IO.md §IO15 6): 2 chunks + the pinned limbs + the O_DIRECT staging at 256 MB, held
                                                        # during the division's low product at size > 1 (modelled from the code's buffers)
HOST_SEEDBUF_VMM = 8 << 30                               # DB_POOL_VMM: seeds_stream's two pinned buffers of min(BS_SEED_CHUNK_MB 8192, the largest region's spans) each
                                                        # (binsplit.c; measured 21.5 GB pinned at init = 4 GiB staging + 2 x 8 GiB at 4e10-1.3e11 on one node)
VMM_BS_GROW = 6.0 * GB                                   # MEASURED (size 1, the defaults): the device at the bs phase above the arena + planes mapped at init --
                                                        # 4e10 +5.8 (224.0 vs 218.2), 1e11 +2.1 (365.7 vs 363.6), 1.16e11 +4.5 (404.3 vs 399.8), 1.3e11 +3.1 (438.7 vs 435.6);
                                                        # the largest, rounded up; the cause is not traced (the block pool's in-phase growth under VMM)

def host_size1_vmm(D):
    """the host HWM of a size-1 run on the defaults (DB_POOL_VMM: the seeds through 2 x 8 GiB pinned), fitted on the measured runs:
    4e10 25.8 / 25.9, 1e11 27.0-27.2, 1.16e11 27.6, 1.3e11 27.7 GB (V214, archive:results_merged_2026-10-24/SINGLE_NODE.md §V314, the five-run 1e11 series) -- 25.85 + 0.0208 GB per 10^9
    digits above 4e10"""
    return int((25.85 + 0.0208 * max(0.0, D / 1e9 - 40.0)) * GB)

def seedbuf_vmm(N, nterms, S=256):
    """the two pinned seed buffers under DB_POOL_VMM (bytes): 2 x min(8 GiB, the largest region's span bytes)"""
    per, nspan = seed_limbs(N, nterms, S)
    span = 2 * per * 8                                                  # (Phase 15 DL: in whole spans, as seeds_stream carves them)
    mx = max(span * max(0, min(nspan, -(-(r + 1) * nspan // NR)) - min(nspan, -(-r * nspan // NR))) for r in range(NR))
    b = max(min(HOST_SEEDBUF_VMM, mx), span)
    return 2 * (b // span * span) * lanes()   # (per PE with ECALC_PE_PER_GPU: binsplit.c as_host_bytes)

def pool_mb_of(need, pool, pool_mb=8192):
    """the SHMEM pool (bytes) the run gets from its need (bytes) under the pool rule (binsplit_shmem_pool_rule)"""
    if pool == 'max': return max(pool_mb << 20, need)                 # the model before Phase 15 (unrounded)
    mb = -(-need // (1 << 20)); mb = -(-mb // 256) * 256              # whole 256 MiB, as `plan pool` prints it
    if pool == 'plan': return mb << 20
    if pool == 'auto': return max(pool_mb, mb) << 20
    return pool_mb << 20                                               # 'off'

def exchange_scratch(nq_total, g, alltoallv, shift_chunk_mb=0):
    """the sharded division's exchange scratch per APU (rns_dist.c mdb_shift / mdb_add_shifted; archive:PLAN §23-4):
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
    """the removed plain SHMEM transport's staging in its symmetric pool (comm_shmem.c, removed in Q103; kept as a model of the 'tcp'/'xshmem'
    hypotheses, the xshmem engine has no such staging: every exchange was copied through a send
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
    if staging == 'per_exchange': return NRN * max(per_level)
    return NRN * sum(per_level)

# ---------------------------------------------------------------- Phase 14 P2: the SHMEM pool as the code used it (measured law; the xshmem planner keeps its pool rule and device pools)
SHMEM_RING = 256 << 10                                    # the removed transport's COMM_SHMEM_RING_KB: the point-to-point ring per (communicator, source)
SHMEM_MAXID = 1024                                        # comm_shmem.c (removed, Q103) MAXID: the mailbox rows
SHMEM_SELFTEST = 128 << 20                                # mn_selftest_layered's symmetric buffers at init (measured 128 MiB at 2 PEs, 64 at 4)
SHMEM_MARGIN = 256 << 20                                  # first-fit slack and the small exchanges (spills, counts, reductions): assumed

def mn_stage(na, nb, g, share_a, share_b, share_c, pool_log=31, logr_delta=0, t_chunk_mb=0, parts=None, round_mb=0):
    """Phase 14 P2 (results/SHMEM.md): the SHMEM transport's staging for one mn_core product C = A B over g nodes, per APU thread
    (limbs; send + receive of its largest staged exchange -- the staging is allocated per exchange and released after it, and
    the four APU threads run the same exchange at once, so the pool holds NR of these).  The code's buffers (mn_scratch's
    terms): the result exchange (rns_dist.c: xb in plane pool 1 -> rbO from the block pool, neither in the symmetric pool)
    sends my rows of the piece, RT(nc), and receives my window's part, win / 4 (win = min(share_c, nc): my share of C inside
    the piece); the redistribution of A (B) sends my share's part of the piece, va / 4 (+ 2 g rows), and receives my rows,
    RT(pa); the transform's inter-node stage (comm_layered) q / K each way.  MN_T_CHUNK_MB=m: the result exchange in rounds
    of m MiB per APU each way.  Measured (P214 section 1): the result exchange of the division's A_h mu product is the peak
    at 1e8 / 1e9 / 1e10 on 2 nodes and 1e9 / 1e10 on 4 processes, to the MiB.
    Phase 14 V1: round_mb (the code: XSHMEM_ROUND_MB) -- the transport carries each all-to-all in rounds staging at most round_mb
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
    """Phase 17 OFIMEM: whether the device pools are modelled -- the C planner always counts them under xshmem (pass ofi=1 / COMM_TRANSPORT=xshmem
    in the env); this host test (a cxi NIC on this host, or COMM_OFI_PLAN_CXI=1) is the default when the caller does not say."""
    env = os.environ if env is None else env
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
      round_mb (XSHMEM_ROUND_MB, Phase 14 V1): every side of every staged exchange at most round_mb.
      ofi (Phase 17 OFIMEM; always on under xshmem): the device staging and the slabs move to one device pool per APU (COMM_OFI_POOL_MB) (detail['ofi_dev_mb']: one
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
    staging = NRN * best * 8
    lv = [S for S in mn_groups(g, groups) if S < g]
    control = NRN * (g + sum(lv)) * (ring + 64)
    mailbox = SHMEM_MAXID * g * 8
    sym = NRN * 3 * qmax * 8 if sym_slabs else 0
    ofi_dev_mb = 0
    if ofi:
        stg = max(best * 8, SHMEM_SELFTEST)
        dv = max(stg, vslot) + sym // 4 + SHMEM_MARGIN   # M3: release -> the larger, not the sum
        ofi_dev_mb = round256_mb(dv); staging = 0; sym = 0
    stg0 = max(staging, SHMEM_SELFTEST)
    if not ofi: sym += NRN * vslot   # X2: (no device pools) the slots in the SHMEM pool, one set per APU thread; SHM: the sum also under M3's release (the removed transport's
                                    # staging regions are staging-only, the slots come from the general area: binsplit.c binsplit_shmem_pool_need)
    need = stg0 + control + mailbox + sym + SHMEM_MARGIN
    if detail is not None: detail.update(staging=staging, control=control, mailbox=mailbox, sym=sym, need=need, by=who, per_apu=best * 8, ofi_dev_mb=ofi_dev_mb)
    return need




# ---------------------------------------------------------------- Phase 15 TC: the mn transform cache (rns_dist.c cache_avail)
CACHE_MN_SLOTS_CODE = 2                  # the mn transform cache's slots (rns_dist.c)
CACHE_FIT_RESERVE = 32e9                 # rns_dist.c cache_fit_reserve (32 GB per node)
DIST_LOGN_MAX = 31                       # rns_dist.c: the single-node tier's cap, which sizes the default's slots
def cache_mn_bytes(g, pool_log=31, np=4, slots=CACHE_MN_SLOTS_CODE, fit=False):
    """bytes per node of the mn transform cache: slots x 4 APUs x np x 2^(c - 2) limbs x 8, c = 31 (the default: whatever POOL_LOG) or
    min(31, POOL_LOG) (RNS_DIST_CACHE_FIT: the mn tier's cap, mn_logn_cap: q <= 2^(c - 2)); the planes pool 0 is made for (np).  Only a
    run with an mn grid product takes it (MN_PLAN_ONLY's `plan cache` line says where; at the target: tree level 1)"""
    c = min(DIST_LOGN_MAX, pool_log) if fit else DIST_LOGN_MAX
    return slots * NRN * np * np2_q(1 << (c - 2), g) * 8 if g > 1 else 0

def cache_report():
    """the cache at the target and one step below (TARGET_LAUNCH), and at the single-node test points of results/CACHE.md"""
    groups = '2,4,8,16,32,64,192,576'
    print('== Phase 15 TC: the mn transform cache (%d slots; GB per node, modelled)' % CACHE_MN_SLOTS_CODE)
    for T in (TARGET_DIGITS, TARGET_BELOW):
        oo = dict(TARGET_LAUNCH, transport='xshmem', staging='code', depth=2, groups=groups, np=TARGET_NP); r = mem_per_node(int(T / TARGET_NODES), TARGET_NODES, oo)
        one = r['cache_slot']                                           # (Phase 15 DOC2: the slot at pool 0's planes -- four at the target under auto)
        print('  %.3ge13 on %d, ECALC_NP=%s: node %.1f (no cache) | the default\'s 2 slots %.1f -> node %.1f (1 slot: %.1f) | RNS_DIST_CACHE_FIT: %d slot%s (the code\'s room: 480 - the layout node %.1f - the reserve %.0f = %.1f < one slot %.1f; against this model\'s node %.1f) -> node %.1f' % (
            T / 1e13, TARGET_NODES, TARGET_NP, r['node_peak'] / GB, r['cache_mn'] / GB, r['node_peak_cache'] / GB, (r['node_peak'] + one) / GB,
            r['cache_fit_slots'], '' if r['cache_fit_slots'] == 1 else 's', r['layout_node'] / GB, CACHE_FIT_RESERVE / GB, r['cache_fit_room'] / GB, one / GB, 480 - r['node_peak'] / GB, (r['node_peak'] + r['cache_fit']) / GB))
    for D, g, pl in ((5e8, 2, 25), (2.5e8, 4, 24), (5e9, 2, 29), (2.5e9, 4, 28)):
        print('  %.0e digits at %d node-processes, POOL_LOG=%d (3 primes): the default %.2f per process (12 GiB slots), FIT %.2f per process' % (
            D * g, g, pl, cache_mn_bytes(g, pl, 3) / GB, cache_mn_bytes(g, pl, 3, fit=True) / GB))

# ---------------------------------------------------------------- the model
def mem_per_node(D, g=1, opts=None):
    """FIT (D26, results/LAUNCH_DEFAULTS.md §FIT): DM_MN_WRAP automatic as the C decides it (binsplit.c binsplit_fit) -- when opts has no 'wrap' and the
    environment no DM_MN_WRAP, at g > 1: the wrap's arm if its device peak (dev_max) is within ECALC_DEVICE_GB (373.44) less
    ECALC_DEVICE_MARGIN_GB (5.0), else the arm without it; r['wrap'] says which.  (This model's dev_max is not the C's device: the C
    also counts the device pools (COMM_OFI_POOL_MB) as device memory; the C's `plan fit` lines are the source of truth.)"""
    global NP2_D
    d0 = NP2_D; NP2_D = float(D)                                       # MN_NONPOW2_CAP auto: the rule reads the digits per node
    try: return _mem_per_node_wrap(D, g, opts)
    finally: NP2_D = d0

def _mem_per_node_wrap(D, g, opts):
    o = dict(opts or {})
    if o.get('wrap') is not None or 'DM_MN_WRAP' in os.environ or g < 2:
        r = _mem_per_node(D, g, o); r['wrap'] = o['wrap'] if o.get('wrap') is not None else dm_mn_wrap(); return r
    r1 = _mem_per_node(D, g, dict(o, wrap=1))
    lim = (float(os.environ.get('ECALC_DEVICE_GB', '373.44') or 373.44) - float(os.environ.get('ECALC_DEVICE_MARGIN_GB', '5.0') or 5.0)) * GB
    if r1['dev_max'] <= lim: r1['wrap'] = 1; return r1
    r0 = _mem_per_node(D, g, dict(o, wrap=0)); r0['wrap'] = 0; r0['dev_max_wrap1'] = r1['dev_max']; return r0

def _mem_per_node(D, g=1, opts=None):
    """bytes per node-process (one per node, four APUs) for D digits per node in a run of g node-processes.
    opts: pool_log (31), tail (True: decision 5's layout, item 1), alltoallv (True: L's B7, merged in Phase 11),
          decimal (True), margin (0.0: a fraction added to the device total), logr_delta (DIST_LOGR_DELTA),
          form ('flat': today's g-sized spill buffers; 'grid': agent G's O(share) spills -- Phase 12),
          groups (MN_GROUPS: a list or string; None = the code's default schedule),
          transport ('tcp' | 'xshmem': the pool model, now of the xshmem planner -- the larger of COMM_SHMEM_POOL_MB (pool_mb, 8192)
          and the staging the transport needs (shmem_staging: staging = 'cached' (the code) | 'per_exchange' | 'resident'),
          in the node's HBM whether host-registered or a device heap).  Returns a dict with the parts and the peaks."""
    o = dict(pool_log=31, tail=True, alltoallv=True, decimal=True, margin=0.0, logr_delta=0, form='grid', groups=None, transport='tcp', pool_mb=8192, staging='code', planes_3q30=None,
             np=EC_NP, strategy='C', cap=None, depth=2, host_fit=True, arena_room=0.0, dkm=False, ofi=None); o.update(DEFAULTS); o.update(opts or {})   # early_free: Phase 14 T1 (MN_TREE_EARLY_FREE); form: 'grid' is the code after Phase 12 G (the arena request follows rns_mul_dist_mn_scratch); 'flat' = before; tight: Phase 14 L1 (DM_TIGHT)
    # Phase 15 (agent MD): the defaults are the code's since Phase 14 (DEFAULTS: tight, early_free, t_chunk_mb 1024, shift_chunk_mb 1024,
    # pool 'plan', vmm); the forms before Phase 14 are in the archive.  vmm (DB_POOL_VMM): the host's seed buffers 2 x 8 GiB, the size-1 host fitted anew
    # (host_size1_vmm), the bs phase's measured growth (VMM_BS_GROW) on the device peak.  pool: POOLS.
    # Phase 13b D: np (ECALC_NP: pool 0 scales np/4), strategy (C | B | B4 | auto: B's 16 n planes), cap (the plane cap in points:
    # sets pool_log and the 3 2^k planes, cap_pool), depth (COMM_ALLTOALLV_DEPTH, the code's default 2 since Phase 13c: the v-slots' form --
    # B7ACCT: vslot_resident, outside the arena, on dev_bs / dev_dm), host_fit (size 1: the host HWM fitted on the measured runs instead of the init constants)
    if o['cap'] is not None: o['pool_log'], o['planes_3q30'] = cap_pool(o['cap'])
    D_total = D * g; d = digits_of_run(D_total); N = e_terms(d); nterms = (N + g * lanes() - 1) // (g * lanes())   # (a PE's terms with ECALC_PE_PER_GPU)
    S = seed_span(N, g, o['seed_fill']) if o['decimal'] else 256                # Phase 15 (2026-09-27): BS_SEED_FILL (128 by default; 0 = 256)
    bs = arena_bs_bytes(N, nterms, decimal=o['decimal'], S=S); bs_total = sum(bs)
    aroom = o['arena_room'] if o['vmm'] else 0.0                            # Phase 15 AS: BS_ARENA_ROOM (needs the VMM pool)
    L = dm_layout(N, g, o['pool_log'], o['decimal'], o['tight'], room=aroom, dkm=o.get('dkm', False), mdev_logl=o.get('mdev_logl', 30), lean=o.get('lean', False), wrap=o.get('wrap'))   # dkm: NEWTON_DKM (Phase 15 DL: built -- binsplit.c dm_layout follows it; the code's default since Batch 3); lean: DM_MN_LEAN (int15k)
    p24 = (o.get('p24', 0), o['np'], g) if o.get('p24', 0) else None   # MS: MN_P24 (0 = off, the default)
    sc = []; tree = tree_need_dev((L['nq'] + g - 1) // g, g, sc, o['logr_delta'], o['form'], o['pool_log'], o['groups'], o['t_chunk_mb'], o['early_free'], p24) if g > 1 else 0
    arena_trim(L, sc[0] if g > 1 else 0)                                   # (BS_ARENA_TRIM_GB after the scratch) the sharded division's products: the same slabs and spills
    vs = vslot_resident(L['nq'], g, o['groups'], o['pool_log'], p24, o['depth'])   # B7ACCT: the general map's v-slots per APU (hipMalloc'd beside the arena)
    vb = 0                                                                 # the v-slots are in the comm pool (X2), not in the code's node budget (room, cache fit)
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
        pool_in_phase = max(0, int(1.08 * NRN * L['need_v2']) - bs_total) if g == 1 else max(0, NRN * tree - bs_total) + NRN * L['hole']
        pool_total = bs_total + pool_in_phase
    xchg = NRN * exchange_scratch(L['nq'], g, o['alltoallv'], o['shift_chunk_mb'])
    # (B7ACCT: the old depth-2 term here -- one more quarter-plane per APU in the block pool -- is replaced by vslot_resident: the whole
    # v-exchange scratch, 2 slots of A + max(A, B) at both depths' forms, hipMalloc'd OUTSIDE the arena, added to dev_bs / dev_dm below)
    npp = np_planes(o['np'], g, o['pool_log'])                          # Phase 15 NP: ECALC_NP=auto -- pool 0's planes by the run's largest group
    p1np = o.get('pool1_np') or (3 if o['np'] == 'auto' else o['np'])  # Phase 15 PS: pool 1 at the one-node tiers' prime count (auto: three)
    planes = planes_bytes(o['pool_log'], d, o['planes_3q30'], npp, o['strategy'], pool1_np=p1np, g=g)
    pdet = {}                                                             # (Phase 15 DL: the host terms before the room's decision, which counts them)
    if g > 1 and o['transport'] == 'xshmem' and o['staging'] in ('code', 'sym'):   # Phase 14 P2: the measured law (the code as it is; 'sym': DIST_MN_SYM_SLABS=1)
        ofi = ofi_planned() if o.get('ofi') is None else o['ofi']           # Phase 17 OFIMEM: the device pools (None: ofi_planned() on this host; the C always counts them under xshmem)
        need = shmem_pool(L['nq'], g, o['groups'], o['pool_log'], o['t_chunk_mb'], o['shift_chunk_mb'], o['staging'] == 'sym', detail=pdet, round_mb=o.get('round_mb', 0), ofi=ofi, vslot=vs['per_apu'])   # X2: the pool counts the v-slots here
        stg = pdet['staging']; pool = pool_mb_of(need, 'plan' if ofi and o['pool'] == 'auto' else o['pool'], o['pool_mb'])   # Phase 15: the pool rule (POOLS; 'max' = the model before); OFIMEM: under OFI an unset pool = the need
        pdet['shmem_only'] = pool; pool += NRN * (pdet['ofi_dev_mb'] << 20)   # OFIMEM: + the four device pools (the transport's pools in all)
    else:                                                                 # the hypotheses before Phase 14 (resident: 0 staging, the pool flat at 8 GiB)
        stg = shmem_staging(L['nq'], g, o['groups'], o['pool_log'], o['staging']) if (g > 1 and o['transport'] == 'xshmem') else 0
        pool = max(o['pool_mb'] << 20, stg + (100 << 20)) if (g > 1 and o['transport'] == 'xshmem') else 0
    comm = (PPN_PROC * HOST_COMM_PER_PROC + pool) if g > 1 else 0
    seedbuf = seedbuf_vmm(N, nterms, S) if o['vmm'] else HOST_SEEDBUF          # Phase 15: DB_POOL_VMM's two 8 GiB pinned seed buffers (at init)
    host_init = PPN_PROC * HOST_RUNTIME + HOST_STAGING + seedbuf + comm
    host_dm = PPN_PROC * HOST_RUNTIME + HOST_STAGING + HOST_WRITER + comm + (HOST_EARLY if (g > 1 and o['out_early']) else 0)
    if g == 1 and o['host_fit']:                                          # Phase 13b D: size 1, the measured host (mem summary):
        host_init = (host_size1_vmm(D_total) if o['vmm'] else host_size1(D_total)) + (PPN_PROC - 1) * HOST_RUNTIME   # (S2c: + a runtime per further process) the HWM is at init (staging pinned + other), and grows
        host_dm = min(host_dm, host_init)                                 # slowly with D (seeds); the dm phase stays below it at >= 2e10
    room_node = None
    if aroom > 0 and o['tail']:                                           # Phase 15 AS: the room dropped over the budget (the arenas stay in whole chunks); DL: the node
        rh = room_host(N, g, pool, seedbuf, o['out_early'])               # counted as binsplit.c as_room_fits counts it (this model's bs-phase node)
        room_node = planes + sum(arena) + VMM_BS_GROW + rh
        if not as_room_fits(planes, sum(arena), rh + vb, o.get('node_gb', 480.0)):   # (B7ACCT: + the v-slots under ECALC_VSLOT_BUDGET=1)
            aroom = 0.0; L = dm_layout(N, g, o['pool_log'], o['decimal'], o['tight'], room=0.0, dkm=o.get('dkm', False), mdev_logl=o.get('mdev_logl', 30), lean=o.get('lean', False), wrap=o.get('wrap'))
            arena_trim(L, sc[0] if g > 1 else 0)
            want = max(L['need_dev'], tree); arena = [arena_of(b, want, VMM_CHUNK) for b in bs]; arena_mapped = list(arena); pool_total = sum(arena)
    if g > 1 and sc[1] > (1 << o['pool_log']) // 4:                       # (never with the mn tier's cap: kept for a lowered cap)
        planes = NRN * (npp * sc[1] * 8 + (3 * sc[1] + 16) * 8) + int(0.61 * GB)
    dev_init = planes + (sum(arena_mapped) if o["tail"] else bs_total)   # Phase 13a M: the arena (bs regions + dm extra) is mapped at init since M11 v2 (measured 4e10: 313.3 GB at init = at the dm peak); int15j: in whole VMM chunks
    # the exchange scratch comes from the block pool (db_pool_alloc): inside the arena while the dm shares + it fit, hipMalloc beyond
    live_dm = NRN * L['need_dev'] + xchg
    if live_dm > pool_total: pool_in_phase += live_dm - pool_total; pool_total = live_dm
    dev_dm = planes + pool_total + NRN * vs['per_apu']                      # B7ACCT: + the v-slots resident from the dm phase on
    dev_bs = dev_init + (VMM_BS_GROW if o['vmm'] else 0) + NRN * vs['tree_top']   # Phase 15: the bs phase's measured growth under VMM; counted with the host HWM; B7ACCT: + the v-slots of the tree's top level
    if g == 1 and o['vmm'] and o['seed_fill'] and o['decimal'] and not aroom > 0:   # Phase 15 (2026-09-27): the fill's division grows the pool at size 1 (measured at 1e11); AS: none with BS_ARENA_ROOM (replayed)
        dev_dm = max(dev_dm, dev_init + VMM_DM_GROW_FILL)
    # ECALC_FIT_PER_APU (diag 2026-10-26): the busiest APU, binsplit.c's dev_apu_max -- region 0 holds the top batch levels, so the largest region's
    # arena (not the node mean) + a quarter of the planes + the v-slots + the device pool; the phase max (dm: v-slots; bs: + the growth, the tree top's)
    _am = max(arena_mapped if o["tail"] else bs); _ofi_apu = pdet.get('ofi_dev_mb', 0) << 20
    dev_apu_max = planes // NRN + _am + _ofi_apu + max(vs['per_apu'], (int(VMM_BS_GROW) // NRN if o['vmm'] else 0) + vs['tree_top'])
    peak = max(dev_init + host_init, dev_bs + max(host_init, host_dm), dev_dm + host_dm) * (1 + o['margin'])   # (the measured node = device max + host HWM)
    # Phase 15 TC (results/CACHE.md): the mn transform cache -- not in node_peak (the numbers the docs cite); the default code's slots
    # (2 slots x 16 GiB per APU at four primes) live from the first mn grid product (the tree at the target) to the division's end,
    # so they add to the bs and dm peaks: node_peak_cache.  cache_fit_slots: what RNS_DIST_CACHE_FIT would take against this model's peak.
    cmn = cache_mn_bytes(g, o['pool_log'], npp, CACHE_MN_SLOTS_CODE) if g > 1 else 0
    peak_c = max(dev_init + host_init, dev_bs + cmn + max(host_init, host_dm), dev_dm + cmn + host_dm) * (1 + o['margin'])
    slot_fit = cache_mn_bytes(g, o['pool_log'], npp, 1, fit=True) if g > 1 else 0
    # Phase 15 DOC2: RNS_DIST_CACHE_FIT's bound as the code computes it (rns_dist.c rns_dist_cache_plan / cache_avail): ECALC_NODE_GB less the layout's
    # node (binsplit_node_bytes: the plane pools + the arena + the tables + the host term -- BS_HOST_INIT_BYTES at size 1, room_host's size > 1
    # formula since the B5 fix: runtime + staging + transport + the SHMEM pool + max(the VMM seed buffers, the writer + MN_OUT_EARLY)) less
    # the reserve (32 GB per node), over one slot per node; the run's own check takes its measured init peak where larger (not modelled).
    # cache_fit_room_model: against this model's peak
    layout_node = planes + sum(arena) + (room_host(N, g, pool, seedbuf, o['out_early']) if g > 1 else as_host()) + vb   # B7ACCT: vb = the v-slots under ECALC_VSLOT_BUDGET=1
    fit_room = o.get('node_gb', 480) * GB - layout_node - CACHE_FIT_RESERVE
    fit_slots = min(CACHE_MN_SLOTS_CODE, max(0, int(fit_room // slot_fit))) if slot_fit and fit_room > 0 else 0
    return dict(D=D, g=g, N=N, digits=d, nq=L['nq'], t1_quarter=L['t1_quarter'], hole=L['hole'],
                cache_mn=cmn, node_peak_cache=peak_c, cache_fit_slots=fit_slots, cache_fit=fit_slots * slot_fit, cache_slot=slot_fit, cache_fit_room=fit_room, layout_node=layout_node,
                arena_room=aroom, room_node=room_node, dkm=L['dkm'],
                planes=planes, regions_bs=bs_total, arena=sum(arena), arena_mapped=sum(arena_mapped), dm_need=NRN * L['need_dev'], tree_need=NRN * tree, top_scratch=NRN * sc[0] if g > 1 else 0,
                pool_in_phase=pool_in_phase, pool_total=pool_total, exchange=xchg, shmem_staging=stg, shmem_pool=pool, shmem_pool_by=pdet.get('by', ''), shmem_pool_only=pdet.get('shmem_only', pool), ofi_pools=NRN * (pdet.get('ofi_dev_mb', 0) << 20),
                dev_init=dev_init, dev_dm=dev_dm, dev_bs=dev_bs, dev_max=max(dev_init, dev_bs, dev_dm), dev_apu_max=dev_apu_max, shmem_need=pdet.get("need", 0), host_init=host_init, host_dm=host_dm, host_hwm=max(host_init, host_dm),
                vslot=NRN * vs['per_apu'], vslot_tree=NRN * vs['tree_top'], vslot_levels=vs['levels'],   # B7ACCT (bytes per node)
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
                py = planes_bytes(pl, 0, r3, p0np, 'C', pool1_np=p1np, g=g) - int((0.61 if pl >= 30 else 2.16) * GB) + 610000000   # C: BS_TABLES_BYTES 0.61 GB at every cap
                ok = '%.2f' % (py * 1e-9) == pb; nplanes += 1
                if not ok: globals()['_c_layout_bad'] = globals().get('_c_layout_bad', 0) + 1
                print('   planes D %s g %d np %s cap %-6s C %s  model %.2f GB  %s' % (m.group(1), g, npm, cap, pb, py * 1e-9, 'exact' if ok else 'DIFFERS'))
            continue
        if not line.startswith('layout:'): continue
        v = dict((k, float(x)) for k, x in re.findall(r'(\w+(?: \w+)?) ([0-9.e+]+)(?!\w)', line.replace('(', ' ').replace(')', ' ').replace('|', ' ')))
        D, g, N = v['D'], int(v['g']), int(v['N'])
        tight = int(v.get('tight', 0))          # Phase 14 L1: the variant the C line was printed under
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
        room_planes = planes_bytes(pl0, 0, r3, np_planes(npm, g, pl0), 'C', pool1_np=3 if npm == 'auto' or not pool1_4q else npm, g=g) - int((0.61 if pl0 >= 30 else 2.16) * GB) + 610000000
        S_ = seed_span(N, g, seed_fill); nt_ = N // (g * lanes()) if g * lanes() > 1 else N           # (BS_LAYOUT_ONLY: rank 0's range [1, 1 + N / g))
        sb = seedbuf_vmm(N, nt_, S_) if g > 1 else 0
        pool_b = 0; ofi_b = 0
        if g > 1 and env.get('COMM_TRANSPORT') == 'xshmem':                       # (binsplit.c xsh_on) the device pools beside the SHMEM pool; rounds from XSHMEM_ROUND_MB
            pd_ = {}; ofi = 1                                       # Phase 17 OFIMEM: the device pools beside the SHMEM pool
            nd = shmem_pool(dm_layout(N, g, pool_log, True, tight)['nq'], g, None, pool_log, t_chunk_mb, int(env.get('MDB_SHIFT_CHUNK_MB', '1024')), False, round_mb=int(float(env.get('XSHMEM_ROUND_MB', '0') or 0)), detail=pd_, ofi=ofi,
                            vslot=(vslot_resident(dm_layout(N, g, pool_log, True, tight)['nq'], g, os.environ.get('MN_GROUPS') or None, pool_log, ((int(os.environ.get('MN_P24', '2') or 0), np_mode if np_mode is not None else EC_NP, g) if int(os.environ.get('MN_P24', '2') or 0) else None))['per_apu']))
            mb = -(-nd // (1 << 20)); mb = -(-mb // 256) * 256; have = int(env.get('COMM_SHMEM_POOL_MB', str(mb) if ofi else '8192'))
            pool_b = (mb if mb > have else have) << 20   # (the pool rule always raises the pool)
            if ofi: ofi_b = NRN * (int(env.get('COMM_OFI_POOL_MB', str(pd_['ofi_dev_mb']))) << 20)
        rh = room_host(N, g, pool_b + ofi_b, sb, env.get('MN_OUT_EARLY', '1') != '0')
        ar_room = None
        ch = VMM_CHUNK if arena_room > 0 else 0
        p24m_ = int(os.environ.get('MN_P24', '2') or 0)                           # B7ACCT: the v-slots (binsplit_vslot_bytes), the tree's P24 decision
        p24_ = (p24m_, np_mode if np_mode is not None else EC_NP, g) if p24m_ else None
        vs = vslot_resident(dm_layout(N, g, pool_log, True, tight)['nq'], g, os.environ.get('MN_GROUPS') or None, pool_log, p24_) if g > 1 else dict(per_apu=0, tree_top=0)
        vb = 0                                                                    # the v-slots are in the comm pool, not in as_room_fits' node
        bs = arena_bs_bytes(N, (N + g * lanes() - 1) // (g * lanes()), S=seed_span(N, g, seed_fill))
        def arm(w):                                                           # FIT (D26): one wrap arm, the room decided as below (binsplit.c fit_arm)
            ar_room = None
            for rm in ([arena_room, 0.0] if arena_room > 0 else [0.0]):             # Phase 15 AS: the room, dropped when the node with it is over the budget
                L = dm_layout(N, g, pool_log, True, tight, room=rm, dkm=dkm, mdev_logl=mdev_logl, lean=lean, wrap=w); sc = []
                p24m = int(os.environ.get('MN_P24', '2') or 0)                 # (DL: '2', the code's default since Batch 3)  MS: the run's MN_P24 (the layout line does not print them)
                p24 = (p24m, np_mode if np_mode is not None else EC_NP, g) if p24m else None
                tree = tree_need_dev((L['nq'] + g - 1) // g, g, sc, 0, 'grid', pool_log, None, t_chunk_mb, ef, p24) if g > 1 else 0
                need = arena_trim(L, sc[0] if g > 1 else 0); want = max(need, tree)
                ar = sum(arena_of(b, want, ch) for b in bs)
                if rm > 0: ar_room = ar
                if rm == 0 or as_room_fits(room_planes, ar, rh + vb, float(env.get('ECALC_NODE_GB', '480'))): break   # (B7ACCT: the budget the C reads -- was 480 always)
            dev_ = room_planes + ar + ofi_dev                                    # FIT: binsplit.c fit_arm -- device = max(dm, bs phase), node
            return dict(L=L, sc=sc, tree=tree, need=need, ar=ar, ar_room=ar_room, dev=max(dev_ + NRN * vs['per_apu'], dev_ + int(VMM_BS_GROW) + NRN * vs['tree_top']),
                        node=room_planes + ar + int(VMM_BS_GROW) + rh + NRN * vs['per_apu'])
        ofi_dev = 0 if env.get('COMM_OFI_POOL') == 'host' else ofi_b
        lim_ = float(env.get('ECALC_DEVICE_GB', '373.44') or 373.44) * 1e9; mrg_ = float(env.get('ECALC_DEVICE_MARGIN_GB', '5.0') or 5.0) * 1e9
        if g < 2: w_ = dm_mn_wrap(env); A1 = A0 = A = arm(w_)
        else:
            A1, A0 = arm(1), arm(0)
            if 'DM_MN_WRAP' in env: w_ = dm_mn_wrap(env); A = A1 if w_ == 1 else A0 if w_ == 0 else arm(w_)
            else: w_ = 1 if A1['L']['wrap'] > 0 and A1['dev'] <= lim_ - mrg_ else 0; A = A1 if w_ else A0   # the automatic rule (binsplit_fit)
        L, sc, tree, need, ar, ar_room = A['L'], A['sc'], A['tree'], A['need'], A['ar'], A['ar_room']
        fl = None                                                                 # FIT: the C `fit:` line of this point, if any
        for l2 in lines[i + 1:]:
            if l2.startswith('fit:'): fl = dict((k, float(x)) for k, x in re.findall(r'(\w+) ([0-9.e+]+)(?!\w)', l2.replace('|', ' '))); break
            if l2.startswith('layout:'): break
        rows = [('nq (limbs)', v['nq'], L['nq']), ('hole', v['hole'], L['hole']), ('dm need / dev', v['dm_need'], need),
                ('top scratch / dev', v['top scratch'], sc[0] if g > 1 else 0), ('tree need / dev', v['tree_need'], tree),
                ('bs regions / node', v['bs regions'], sum(bs)), ('arena / node', v['arena'], ar)]
        if 'v2' in v: rows[3:3] = [('v2 / dev', v['v2'], L['v2']), ('v3 / dev', v['v3'], L['v3']), ('division / dev', v['div'], L['div']), ('jl (limbs)', v['jl'], L['jl'])]
        if 'wrap' in v or L.get('wrap'): rows[3:3] = [('wrap / dev', v.get('wrap', 0), L['wrap'])]   # D19: DM_MN_WRAP's term (env DM_MN_WRAP, as the C)
        if 'room' in v: rows[3:3] = [('room / dev', v['room'], L['room']), ('chunk', v['chunk'], ch)]   # Phase 15 AS (DL: the room decided on this model's node, below)
        if rl is not None and ar_room is not None:                              # Phase 15 DL: the room's node, term by term (binsplit.c as_room_fits)
            rows += [('room planes', rl['planes'], room_planes), ('room arena', rl['arena_with_room'], ar_room), ('room seedbuf', rl['seedbuf'], sb),
                     ('room shmem pool', rl['shmem_pool'], pool_b)] + ([('room ofi pools', rl['ofi_pool'], ofi_b)] if 'ofi_pool' in rl else []) + [('room host', rl['host'], rh), ('room node', rl['node'], room_planes + ar_room + int(VMM_BS_GROW) + rh),
                     ('room fits', rl['fits'], 1 if as_room_fits(room_planes, ar_room, rh + vb, float(env.get('ECALC_NODE_GB', '480'))) else 0)]
        if vl is not None:                                                      # B7ACCT: the v-slots, term by term (+ the device / node with them over the room's terms)
            rows += [('vslot / APU', vl['per_apu'], vs['per_apu']), ('vslot tree top', vl['tree_top'], vs['tree_top'])]
            if rl is not None and ar_room is not None:
                rows += [('device + vslots', vl['device_with'], room_planes + ar_room + ofi_b + NRN * vs['per_apu']),
                         ('node + vslots', vl['node_with'], room_planes + ar_room + int(VMM_BS_GROW) + rh + NRN * vs['per_apu'])]
        if fl is not None:                                                      # FIT (D26): the fit's device / node and the wrap decision, term by term
            rows += [('fit wrap', fl['wrap'], w_), ('fit device', fl['device'], A['dev']), ('fit node', fl['node'], A['node']),
                     ('fit fits', fl['fits'], 1 if A['dev'] <= lim_ and A['node'] <= float(env.get('ECALC_NODE_GB', '480')) * 1e9 else 0)]
            if g > 1: rows += [('fit device wrap 1', fl['device_wrap1'], A1['dev']), ('fit device wrap 0', fl['device_wrap0'], A0['dev'])]
        print('D %.3g g %d (N %d) tight %d early_free %d dkm %d:' % (D, g, N, tight, ef, dkm))
        for name, c, py in rows:
            rel = (py - c) / c if c else 0.0
            print('   %-18s C %16.0f  model %16.0f  %+.4f %%' % (name, c, py, 100 * rel))
            if name == 'arena / node': worst = max(worst, abs(rel))
            if py != c: nbad = globals().setdefault('_c_layout_bad', 0) + 1; globals()['_c_layout_bad'] = nbad
    print('largest arena difference: %.4f %%; %d term(s) not exact (%d plane figures compared)' % (100 * worst, globals().get('_c_layout_bad', 0), nplanes))
    return worst

TARGET576 = dict(transport='xshmem', staging='code', shift_chunk_mb=1024, depth=2)   # Phase 14 P2: staging 'code' (the measured pool law; was 'resident', the flat 8 GiB)   # the target's switches (RESULTS 82: 452 GB at 7.38e10 per node)







_pe_from_env()                                   # ECALC_PE_PER_GPU=1 in the environment = the PE planner (binsplit.c bs_nrn / bs_lanes)
if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--cache':                  # Phase 15 TC
        cache_report()
    elif len(sys.argv) > 2 and sys.argv[1] == '--check-c':
        c_layout_check(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 31, float(sys.argv[4]) if len(sys.argv) > 4 else 1024, int(sys.argv[5]) if len(sys.argv) > 5 else SEED_FILL,
                       float(sys.argv[6]) if len(sys.argv) > 6 else ARENA_ROOM, (sys.argv[7] if sys.argv[7] == 'auto' else int(sys.argv[7])) if len(sys.argv) > 7 else None,
                       int(sys.argv[8]) if len(sys.argv) > 8 else 1)
    else:
        sys.exit("usage: mem_model.py --check-c FILE [POOL_LOG [MN_T_CHUNK_MB [BS_SEED_FILL [BS_ARENA_ROOM [ECALC_NP [RNS_POOL1_4Q]]]]]] | --cache   (the library: mem_per_node)")

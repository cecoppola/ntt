#!/usr/bin/env python3
"""mem_model.py - the per-node memory model of ecalc (Phase 11, agent M; PLAN.md 26 row M, item 3).

Device and host bytes of one node-process as functions of the digits per node D and of the group size g
(the number of node-processes the run is spread over), built from the same formulas the code sizes its
allocations with (binsplit.c: e_terms, seed_limbs, region_need, dm_layout, tree_need_dev; rns_mul.c: the
plane pools; rns_dist.c: the sharded exchange's scratch), calibrated against the mem_report tables of
results/M.md, results/M11.md (4, 7, 8 x 10^10 at size 1; 10^10 at size 4).

    mem_per_node(D, g, opts) -> dict      (bytes; opts: pool_log, tail, alltoallv, margin ...)
    python3 mem_model.py                  prints the calibration table, the ceilings per node and the 576-node digits

Agent X's mn_model.py imports mem_per_node for its memory rows.  Every number is "modelled" unless the
calibration table says "measured"; the tables' sources are named in results/M11.md.
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
    return dict(nq=nq, k=k, tcap=tcap, hole=hole, thresh=hole - hole * 3 // 8, need_dev=need, t1_quarter=quarter_bytes(tcap))

def tree_need_dev(nq_leaf, g, scratch_out=None, logr_delta=0):
    """binsplit.c tree_need_dev: the largest tree level's live shares + rns_mul_dist_mn's scratch, per device (bytes);
    scratch_out[0] = the top level's scratch alone (the sharded division's products carry the same)"""
    best = 0; L = 0; top_scratch = 0
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

# ---------------------------------------------------------------- the other pools (measured constants where the code has them)
def planes_bytes(pool_log=31):
    """rns_mul.c: pool 0 = EC_NP (4) x q limbs with q = 2^pool_log / 4, pool 1 = 3 q + 16 limbs (C4), per APU; + the contexts"""
    q = (1 << pool_log) // 4
    per_apu = 4 * q * 8 + (3 * q + 16) * 8
    return NR * per_apu + int(0.61 * GB)                # 120.3 + 0.6 GB tables at 2^31 (measured 120.3 / 0.61)

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

# ---------------------------------------------------------------- the model
def mem_per_node(D, g=1, opts=None):
    """bytes per node-process (one per node, four APUs) for D digits per node in a run of g node-processes.
    opts: pool_log (31), tail (True: decision 5's layout, item 1), alltoallv (False: L's B7 not in), decimal (True),
          margin (0.0: a fraction added to the device total).  Returns a dict with the parts and the peaks."""
    o = dict(pool_log=31, tail=True, alltoallv=False, decimal=True, margin=0.0, logr_delta=0); o.update(opts or {})   # logr_delta: DIST_LOGR_DELTA (A6), -3..3: the spill buffers are 2 g C x 4 limbs per APU, C = n / R
    D_total = D * g; d = digits_of_run(D_total); N = e_terms(d); nterms = (N + g - 1) // g
    bs = arena_bs_bytes(N, nterms, decimal=o['decimal']); bs_total = sum(bs)
    L = dm_layout(N, g, o['pool_log'], o['decimal'])
    sc = []; tree = tree_need_dev((L['nq'] + g - 1) // g, g, sc, o['logr_delta']) if g > 1 else 0
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
        pool_in_phase = max(0, int(1.08 * NR * L['need_dev']) - bs_total) if g == 1 else max(0, NR * tree - bs_total) + NR * L['hole']
        pool_total = bs_total + pool_in_phase
    xchg = NR * exchange_scratch(L['nq'], g, o['alltoallv'])
    planes = planes_bytes(o['pool_log'])
    if g > 1 and sc[1] > (1 << o['pool_log']) // 4:                       # the top level's slice q = n / (4 g) exceeds the pool's: rns_dpool grows pool 0 (4 q) and pool 1 (3 q + 16) on demand
        planes = NR * (4 * sc[1] * 8 + (3 * sc[1] + 16) * 8) + int(0.61 * GB)
    dev_init = planes + bs_total
    dev_dm = planes + pool_total + xchg
    host_init = HOST_RUNTIME + HOST_STAGING + HOST_SEEDBUF + (HOST_COMM_PER_PROC if g > 1 else 0)
    host_dm = HOST_RUNTIME + HOST_STAGING + HOST_WRITER + (HOST_COMM_PER_PROC if g > 1 else 0)
    peak = max(dev_init + host_init, dev_dm + host_dm) * (1 + o['margin'])
    return dict(D=D, g=g, N=N, digits=d, nq=L['nq'], t1_quarter=L['t1_quarter'], hole=L['hole'],
                planes=planes, regions_bs=bs_total, arena=sum(arena), dm_need=NR * L['need_dev'], tree_need=NR * tree,
                pool_in_phase=pool_in_phase, pool_total=pool_total, exchange=xchg,
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
]

def fmt(b): return '%7.1f' % (b / GB)

def main():
    print('== calibration (GB; model vs measured; "pool" = regions + the pool\'s hipMalloc at the dm peak)')
    print('%-8s %2s | %-22s | %8s %8s %8s %8s | %s' % ('D', 'g', 'item', 'planes', 'regions', 'pool', 'dev_dm', 'source'))
    for D, g, m in MEASURED:
        r = mem_per_node(int(D), g, dict(tail=m['tail']))
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
    for node, label in [(502 * GB, '502 GB node'), (480 * GB * 0.95, '480 GB with 5 % margin (456 GB)')]:
        for g in [1, 4, 64, 576]:
            for tail in [True, False]:
                for a2a in ([False, True] if g > 1 else [False]):
                    Dm = max_digits_per_node(node, g, dict(tail=tail, alltoallv=a2a))
                    r = mem_per_node(Dm, g, dict(tail=tail, alltoallv=a2a))
                    print('  %-32s g %4d tail %-5s alltoallv %-5s: max D per node %.1e (node peak %.1f GB: device %.1f + host %.1f; exchange %.1f) -> %d nodes: %.2e digits' % (
                        label, g, tail, a2a, Dm, r['node_peak'] / GB, r['dev_dm'] / GB, r['host_dm'] / GB, r['exchange'] / GB, g, Dm * g))
    print()
    print('== 576 nodes (PLAN 25): the maximum digits = 576 x the per-node ceiling at g = 576')
    for tail, a2a in [(True, True), (True, False), (False, False)]:
        Dm = max_digits_per_node(502 * GB, 576, dict(tail=tail, alltoallv=a2a))
        print('  tail %-5s alltoallv %-5s: D per node %.1e -> %.2e digits over 576 nodes' % (tail, a2a, Dm, 576 * Dm))

if __name__ == '__main__':
    main()

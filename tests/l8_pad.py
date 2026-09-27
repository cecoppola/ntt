#!/usr/bin/env python3
"""L8 (Phase 15, throwaway): transform lengths at 10^11 on one node, their share of the transform time, the time-weighted
padding (chosen length / needed length) and the modelled saving for the length families
    today {2^k, 3 2^k} | +5 | +15 | +5,15 | +5,7 | +5,7,15
before and after BS_SEED_TERMS tuning (V3 item 2), with and without the fused radix pass (Batch 2 item 3, "2a").

Inputs (all in the repository):
  results/V314/e11_def.log, e11_s168.log   measured 10^11 runs (V3): the batch tier's levels (max_nl, ntt s), the mdev levels,
                                           the recip / division product times
  results/L815/L815_plan1*.txt             MN_PLAN_ONLY=1e11:1 (S = 256, 168, 236): every product above the batch tier with its
                                           length (the code's own decisions)
Cost model (per point of one transform on one APU, ps):
  c2(k)  the 2^k part: MEASURED (results/V314/bench.log "fwd 2^k x3" over 3 2^29 points; k = 31 from V3's 0.87 ratio),
         linear in k between the measured points
  cr     one radix-r pass: r = 3 MEASURED 16.3 ms per 3 2^29 points = 10.1 ps (memory-bound, 1.58 TB/s, flat in k);
         r = 5, 7, 15 MODELLED equal (one read + one write per point; the arithmetic stays under the memory time, §4)
  fused  (item 2a): the pass folded into the first 2^k pass, 70 % of it recovered (V3's ASSUMED recovery): cr = 3.0 ps
A product's transform time (modelled): 3 transforms (2 forward, 1 inverse) x pieces x wall(L); wall = cost(L) in the B form
(each prime on its own APU), 3/4 cost(L) in the C form (one 2^31-class plane spread over the four APUs).
The batch tier: 5 transforms per pair (the shared B once) x 3 primes / 4 APUs per level (checked against the measured ntt
lines: within 0.6-1.0x, the small levels' overheads not modelled)."""
import re, os, sys, math

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
C2 = {10: 10.53, 12: 12.77, 17: 20.29, 22: 32.20, 26: 32.93, 28: 40.72, 29: 42.25, 31: 45.4}
KS = sorted(C2)
def c2(k):
    if k <= KS[0]: return C2[KS[0]]
    if k >= KS[-1]: return C2[KS[-1]]
    for a, b in zip(KS, KS[1:]):
        if a <= k <= b: return C2[a] + (C2[b] - C2[a]) * (k - a) / (b - a)
CR_PASS = 10.1
FAMS = [('today-costpick', (1, 3)), ('+5', (1, 3, 5)), ('+15', (1, 3, 15)), ('+5,15', (1, 3, 5, 15)), ('+5,7', (1, 3, 5, 7)), ('+5,7,15', (1, 3, 5, 7, 15))]
B_CAP, C_CAP = 3 << 29, 1 << 31            # the B form's per-APU pool (12 GiB) and DIST_LOGN_MAX
KMIN = 10                                   # NTT_LOGN_MIN (the 2^k part of every length)

def cr(r, fused, r15=1):
    if r == 1: return 0.0
    p = CR_PASS * (0.3 if fused else 1.0)
    return p * (r15 if r == 15 else 1)
def cost(L, fused=False, r15=1):
    r = L
    while r % 2 == 0: r //= 2
    k = int(round(math.log2(L // r)))
    return L * (c2(k) + cr(r, fused, r15)) * 1e-12
def wall(L, fused=False):
    return cost(L, fused) * (0.75 if L > B_CAP else 1.0)
def lens(fam, cap):
    out = []
    for r in fam:
        k = KMIN
        while (r << k) <= cap: out.append(r << k); k += 1
    return sorted(out)
def code_pick(nc):                          # rns_mul.c pick_len (today's rule)
    logn = max(KMIN, math.ceil(math.log2(nc)))
    if logn - 2 >= KMIN and nc <= (3 << (logn - 2)): return 3 << (logn - 2)
    return 1 << logn
def pick(nc, fam, cap=C_CAP, fused=False, rule='cost'):
    if rule == 'code' and fam == (1, 3): return code_pick(nc)
    c = [L for L in lens(fam, cap) if L >= nc]
    return min(c, key=lambda L: (wall(L, fused), L)) if c else None

# ---- the measured runs ---------------------------------------------------------------------------------------------------
def batch_levels(log):
    lv = []
    for ln in open(os.path.join(ROOT, 'results/V314', log)):
        m = re.match(r'bs: level\s+(\d+) (batch|mdev)\s+(\d+) pairs\s+max_nl\s+(\d+)\s.*?([\d.]+) s\s+\(batch [\d.]+: scatter [\d.]+ ntt ([\d.]+)', ln)
        if m: lv.append(dict(l=int(m[1]), tier=m[2], pairs=int(m[3]), nl=int(m[4]), t=float(m[5]), ntt=float(m[6])))
    return lv
def batch_model(pairs, L, fused=False):     # the level's ntt wall (s): 5 transforms per pair x 3 primes / 4 APUs
    return pairs * 5 * 3 * cost(L, fused) / 4

# ---- the plan's products above the batch tier ----------------------------------------------------------------------------
def plan_products(fn):
    P = []
    for ln in open(os.path.join(ROOT, 'results/L815', fn)):
        if not ln.startswith('plan '): continue
        ph = ln.split()[1]
        m = re.search(r'dist_db (\d+) limbs \| one plane of (3\*)?2\^(\d+) points \((B|C) form', ln)
        if m:
            L = (3 if m[2] else 1) << int(m[3]); P.append(dict(ph=ph, what=ln[5:60].strip(), grid=0, nc=int(m[1]), L=L, pieces=1, form=m[4])); continue
        m = re.search(r'dist_db (\d+) x (\d+) limbs: (\d+) x (\d+) pieces of (\d+) \+ (\d+), (\d+) formed.*\| (3\*)?2\^(\d+) points per piece \((B|C) form', ln)
        if m:
            L = (3 if m[8] else 1) << int(m[9])
            P.append(dict(ph=ph, what=ln[5:60].strip(), grid=1, na=int(m[1]), nb=int(m[2]), ka=int(m[3]), kb=int(m[4]), nc=int(m[5]) + int(m[6]), L=L, pieces=int(m[7]), form=m[10]))
    return P

def grid_best(na, nb, fam, fused=False, ovh=0.68 / 1.587e9):
    """the cheapest full grid (ka x kb pieces, every piece formed) under the family: transforms 3 wall(L) + the per-piece
    non-transform time, MODELLED as proportional to the piece's limbs (0.68 s per 1.587e9 limbs: the division's measured
    26.1 s over 28 pieces less their modelled transforms)"""
    best = None
    for ka in range(1, 17):
        for kb in range(1, 17):
            pa, pb = -(-na // ka), -(-nb // kb); L = pick(pa + pb, fam, C_CAP, fused)
            if L is None: continue
            t = ka * kb * (3 * wall(L, fused) + ovh * (pa + pb))
            if best is None or t < best[0]: best = (t, ka, kb, L, pa + pb)
    return best

def tstr(L):
    r = L; k = 0
    while r % 2 == 0: r //= 2; k += 1
    return ('%d*2^%d' % (r, k)) if r > 1 else '2^%d' % k

def analyse(tag, log, plan, fused=False, verbose=True):
    lv = batch_levels(log); P = plan_products(plan)
    out = {}
    if verbose: print('\n==== %s (%s, %s)%s ====' % (tag, log, plan, ' -- fused radix pass (2a)' if fused else ''))
    # the batch tier
    rows = []
    for x in lv:
        if x['tier'] != 'batch': continue
        nc = 2 * x['nl']; Lc = code_pick(nc)
        rows.append((x, nc, Lc))
    if verbose:
        print('batch tier: level, pairs, needed nc (2 max_nl), today\'s length (pad), measured ntt s, modelled ntt s; best length per family')
        for x, nc, Lc in rows:
            s = ' '.join('%s:%s(%.3f)' % (n.split()[0], tstr(pick(nc, f, B_CAP, fused)), pick(nc, f, B_CAP, fused) / nc) for n, f in FAMS[1:])
            print('  %2d %9d nc %10d  %-9s pad %.3f  ntt %.2f (model %.2f) | %s' % (x['l'], x['pairs'], nc, tstr(Lc), Lc / nc, x['ntt'], batch_model(x['pairs'], Lc), s))
    for name, fam in [('today code', (1, 3))] + FAMS:
        rule = 'code' if name == 'today code' else 'cost'
        tw = pw = tm = 0.0
        for x, nc, Lc in rows:
            L = pick(nc, fam, B_CAP, fused, rule)
            # the measured ntt time scaled by the modelled cost ratio (the length change only)
            t = x['ntt'] * cost(L, fused) / cost(Lc, False)
            tw += t; pw += t * L / nc; tm += t
        out.setdefault(name, {})['batch'] = (tm, pw / tw)
    # the products above the batch tier (plan): one-plane and grids
    if verbose: print('above the batch tier (plan): phase, product, nc per piece, pieces, today\'s length (pad), modelled transform s; per family')
    for name, fam in [('today code', (1, 3))] + FAMS:
        tw = pw = 0.0
        for p in P:
            if p['nc'] < 1 << 20: continue                     # the recip chain's tiny products (2^20 floor): no weight
            # the family may only shorten a product's length within its form (B: <= 3 2^29 per APU; C: <= 2^31); a grid keeps its
            # pieces (at the cap the piece count cannot fall: no family adds a length above 3 2^29 in the B pool)
            L = p['L'] if name == 'today code' else pick(p['nc'], fam, B_CAP if p['form'] == 'B' else C_CAP, fused)
            if L is None or L > p['L']: L = p['L']
            t = p['pieces'] * 3 * wall(L, fused); tw += t; pw += t * L / p['nc']
        out[name]['above'] = (tw, pw / tw)
    base_above = sum(p['pieces'] * 3 * wall(p['L'], False) for p in P if p['nc'] >= 1 << 20)   # today, unfused (the code as it is)
    if verbose:
        for p in P:
            if p['nc'] < 1 << 20: continue
            cap = B_CAP if p['form'] == 'B' else C_CAP
            s = ' '.join('%s:%s' % (n.split()[0], tstr(min(pick(p['nc'], f, cap, fused) or p['L'], p['L'], key=lambda L: wall(L, fused)))) for n, f in FAMS[1:])
            print('  %-6s %-44s nc %11d x%-3d %-9s pad %.3f  tr %.2f s | %s' % (p['ph'], p['what'][:44], p['nc'], p['pieces'], tstr(p['L']), p['L'] / p['nc'], p['pieces'] * 3 * wall(p['L'], fused), s))
    print('summary %s%s (modelled transform seconds; time-weighted padding; savings against the code as it is, unfused):' % (tag, ' -- FUSED radix pass (2a)' if fused else ''))
    b0, a0 = sum(x['ntt'] for x, nc, Lc in rows), base_above
    for name, _ in [('today code', 0)] + FAMS:
            b, a = out[name]['batch'], out[name]['above']
            tot, totp = b[0] + a[0], (b[0] * b[1] + a[0] * a[1]) / (b[0] + a[0])
            print('  %-14s batch %.2f s pad %.3f | above %.2f s pad %.3f | all %.2f s pad %.3f | saving vs today %.2f s (batch %.2f, above %.2f)' % (
                name, b[0], b[1], a[0], a[1], tot, totp, b0 + a0 - tot, b0 - b[0], a0 - a[0]))
    return out

# ---- the batch tier under any seed span S (modelled): max_nl scales with S, one length per level --------------------------
def batch_for_S(S, fam, fused=False, N=10433891470, m256=143.0, mdev_log=30, rule='cost'):
    spans = -(-N // S); nl = m256 * S / 256.0; tot = pad = 0.0; l = 1; pairs = spans // 2
    while pairs >= 1:
        nc = int(2 * nl * 2 ** (l - 1)) + 1
        if nc > (1 << mdev_log): break
        L = pick(nc, fam, B_CAP, fused, rule); t = batch_model(pairs, L, fused)
        tot += t; pad += t * L / nc; l += 1; pairs //= 2
    return tot, pad / tot if tot else 1.0

if __name__ == '__main__':
    for fused in (False, True):
        analyse('10^11, S = 256 (default)', 'e11_def.log', 'L815_plan1.txt', fused, verbose=not fused)
        analyse('10^11, S = 168 (V3 item 2, measured best)', 'e11_s168.log', 'L815_plan1_s168.txt', fused, verbose=not fused)
    for fused in (False, True):
        print('\n==== the batch tier by seed span S (modelled, 10^11)%s: ntt s / padding; the best S in [160, 320] per family ====' % (' -- fused' if fused else ''))
        for name, fam in [('today code', (1, 3))] + FAMS:
            rule = 'code' if name == 'today code' else 'cost'
            r256 = batch_for_S(256, fam, fused, rule=rule)
            best = min(((batch_for_S(S, fam, fused, rule=rule), S) for S in range(160, 321)), key=lambda x: x[0][0])
            print('  %-12s S=256: %.2f s pad %.3f | best S=%d: %.2f s pad %.3f' % (name, r256[0], r256[1], best[1], best[0][0], best[0][1]))

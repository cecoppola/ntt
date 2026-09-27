#!/usr/bin/env python3
"""T2 (Phase 15): which BS_SEED_TERMS fill the batch tier's transform lengths -- the prediction behind BS_SEED_FILL.

binsplit.c's level loop, reproduced (sizes only, modelled):
  spans of S terms over [a0, b1) (single node: [1, N+1)); a node of level l (0-based) covers 2^l spans; its Q has
  floor(log10 Q / 18) + 1 decimal limbs (mn_plan.c pq_of); P <= Q in size.
  level l's tier: batch while 2 max_nl + 1 <= 2^BS_MDEV_LOGL (30), else mdev (dist_db products).
  a batch level uses ONE length for all its products: pick_len(maxnc) (rns_mul.c): 2^k with k = ceil(log2 maxnc) >= 10,
  or 3 2^(k-2) when k - 2 >= 10 and maxnc <= 3 2^(k-2).
The largest seed has m1 = ceil(S log10(kmax) / 18) limbs (a little less); a node of level l has <= 2^l m1, so a level's
products have maxnc <= 2^(l+1) m1:
  * m1 <= 2^j       -> every product from 2^10 points up fills a 2^k length, and the batch tier's top level (inputs <= 2^29)
                        ends exactly at the mdev threshold (2^30): the "2^k family" (m1 = 128: S = floor(128 18 / log10 kmax))
  * m1 <= 3 2^j     -> fills 3 2^k from 3 2^10 up: the "3 2^k family" (m1 = 96 or 192)
  anything else pads by up to 1.5x (m1 just above 2^j) or 1.33x (just above 3 2^j); S = 256 at 10^11 has m1 = 143 (1.34x).
The rule: S = the largest S whose last span has <= BS_SEED_FILL limbs -- depends on log10(kmax), so one S cannot fit every size.

Usage: t2_seed_model.py [digits ...]            the table of candidates per size (default 4e10 1e11)
       t2_seed_model.py --levels digits S       the level table for one run
       t2_seed_model.py --fit                   the cost model fitted to V3's five logs (results/V314/*.log) and checked
"""
import math, sys, os, re, glob

LN10 = math.log(10.0)
MDEV_LOGL = 30

def e_terms(d):
    t = d + 50.0; lo, hi = 1, 2
    while math.lgamma(hi + 1) / LN10 < t: hi *= 2
    while hi - lo > 1:
        m = (lo + hi) // 2
        if math.lgamma(m + 1) / LN10 < t: lo = m
        else: hi = m
    return lo if math.lgamma(lo + 1) / LN10 >= t else hi

def digits_run(d):                       # ecalc computes to the next multiple of 18
    return (d + 17) // 18 * 18

def lg10_q(a, b):                        # log10 prod_{k=a}^{b-1} k
    if b <= a: return 0.0
    return (math.lgamma(b) - math.lgamma(a)) / LN10

def limbs(lg): return int(math.floor(lg / 18.0)) + 1

def pick_len(nc):
    logn = max(10, math.ceil(math.log2(nc)) if nc > 1 else 0)
    if logn - 2 >= 10 and nc <= 3 << (logn - 2): return 3 << (logn - 2)
    return 1 << logn

def seed_terms_for(fill, a0, b1):
    """binsplit.c bs_seed_fill_terms: the largest S whose span [b1 - S, b1) has <= fill limbs (an upper bound on every span)"""
    lk = math.log10(b1 - 1)
    S = max(1, int(fill * 18.0 / lk))
    while S > 1 and limbs(lg10_q(b1 - S, b1)) > fill: S -= 1
    while limbs(lg10_q(b1 - (S + 1), b1)) <= fill: S += 1
    return S

def tree(N, S, a0=1, b1=None):
    """the level loop: per level (tier, npairs, max_nl, maxnc, L, fill, per-APU products, out limbs)"""
    if b1 is None: b1 = N + 1
    nspan = (b1 - a0 + S - 1) // S
    def node(l, i):                      # node i of level l: spans [i 2^l, (i+1) 2^l)
        s0, s1 = i << l, min((i + 1) << l, nspan)
        lo, hi = a0 + s0 * S, min(a0 + s1 * S, b1)
        return limbs(lg10_q(lo, hi))
    out = []; n = nspan; l = 0
    while n > 1:
        npairs = n // 2
        cand = range(max(0, n - 4), n)
        max_nl = max(node(l, i) for i in cand)
        # the largest product: the last full pair (or the cut last one)
        pairs = [p for p in range(max(0, npairs - 2), npairs)]
        maxnc = max(node(l, 2 * p) + node(l, 2 * p + 1) for p in pairs)
        tier = 'batch' if 2 * max_nl + 1 <= (1 << MDEV_LOGL) else 'mdev'
        L = pick_len(maxnc) if tier == 'batch' else 0
        # the mdev tier's cost grows with na nb (dist_db: pieces of <= the plane cap on each side), P and Q: 2 products per pair;
        # every pair but the last is full-size (sizes of the last two pairs; the cut one exactly)
        full = node(l, 2 * pairs[0]) * node(l, 2 * pairs[0] + 1); last = node(l, 2 * pairs[-1]) * node(l, 2 * pairs[-1] + 1)
        outl = 2.0 * (full * (npairs - 1) + last) if npairs > 1 else 2.0 * last
        out.append(dict(level=l + 1, tier=tier, npairs=npairs, odd=n & 1, max_nl=max_nl, maxnc=maxnc, L=L,
                        fill=maxnc / L if L else 0.0, per_apu=-(-npairs // 4), out=outl))
        n = npairs + (n & 1); l += 1
    return nspan, out

def summary(N, S):
    nspan, lv = tree(N, S)
    m1 = limbs(lg10_q(N + 1 - S, N + 1))
    b = [x for x in lv if x['tier'] == 'batch']; m = [x for x in lv if x['tier'] == 'mdev']
    # batch cost ~ the transform points each APU handles: per-APU pairs x 2 products x L (the pair shares B; a constant factor)
    bpts = sum(x['per_apu'] * 2 * x['L'] for x in b)
    ideal = sum(x['per_apu'] * 2 * x['maxnc'] for x in b)
    deep = [x['fill'] for x in b if x['maxnc'] > 2048]
    return dict(S=S, m1=m1, nspan=nspan, top=nspan / 2 ** math.ceil(math.log2(nspan)), nb=len(b), nm=len(m),
                bpts=bpts, pad=bpts / ideal if ideal else 0, deep_fill=min(deep) if deep else 0,
                mout=sum(x['out'] for x in m), low=sum(x['per_apu'] * 2 * x['L'] for x in b if x['maxnc'] <= 1024))

# ---- the cost model, fitted to V3's logs (seeds wait, batch, mdev from each run's bs line) -----------------------------------
def v3_runs():
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'results', 'V314')
    runs = []
    for f in sorted(glob.glob(os.path.join(here, 'e*.log'))):
        t = open(f, errors='replace').read()
        m = re.search(r'^== ecalc: e to (\d+) digits', t, re.M); s = re.search(r'spans of (\d+)', t)
        b = re.search(r'^bs +([\d.]+) s .*\(seeds ([\d.]+) school [\d.]+ batch ([\d.]+) mdev ([\d.]+)', t, re.M)
        if m and s and b: runs.append(dict(f=os.path.basename(f), d=int(m.group(1)), S=int(s.group(1)), bs=float(b.group(1)),
                                           seeds=float(b.group(2)), batch=float(b.group(3)), mdev=float(b.group(4))))
    return runs

def fit():
    runs = v3_runs()
    rows = []
    for r in runs:
        N = e_terms(digits_run(r['d'])); s = summary(N, r['S']); r.update(s); r['N'] = N; rows.append(r)
    # batch = kb * bpts + c0 * nspan (the per-level layout / launch work grows with the span count); mdev = km * mout
    import itertools
    def lsq(xs, ys):                     # 2-parameter least squares through the origin
        a11 = sum(x[0] * x[0] for x in xs); a12 = sum(x[0] * x[1] for x in xs); a22 = sum(x[1] * x[1] for x in xs)
        b1 = sum(x[0] * y for x, y in zip(xs, ys)); b2 = sum(x[1] * y for x, y in zip(xs, ys)); det = a11 * a22 - a12 * a12
        return ((b1 * a22 - b2 * a12) / det, (a11 * b2 - a12 * b1) / det)
    kb, c0 = lsq([(r['bpts'], r['nspan']) for r in rows], [r['batch'] for r in rows])
    km, cl = lsq([(r['mout'], r['nm']) for r in rows], [r['mdev'] for r in rows])   # mdev = km sum(na nb) + cl per mdev level
    print(f"fit (V3's logs, measured -> modelled): batch = {kb:.3e} s/point x per-APU points + {c0:.3e} s/span; "
          f"mdev = {km:.3e} s per (limb x limb) of the products + {cl:.2f} s per mdev level")
    print(f"{'log':14s} {'S':>4s} {'m1':>4s} {'nb':>3s} {'nm':>3s} {'pad':>5s} {'batch':>6s} {'model':>6s} {'mdev':>6s} {'model':>6s} {'seeds':>6s}")
    for r in rows:
        print(f"{r['f']:14s} {r['S']:4d} {r['m1']:4d} {r['nb']:3d} {r['nm']:3d} {r['pad']:5.2f} {r['batch']:6.1f} {kb * r['bpts'] + c0 * r['nspan']:6.1f} "
              f"{r['mdev']:6.1f} {km * r['mout'] + cl * r['nm']:6.1f} {r['seeds']:6.1f}")
    return kb, c0, km, cl

def table(d, kb=None, c0=None, km=None, cl=None):
    N = e_terms(digits_run(d))
    lk = math.log10(N)
    print(f"\n{d:.3g} digits: N = {N} terms, log10 kmax = {lk:.4f}, seed limbs per term {lk / 18:.4f}")
    cands = {256: 'default', 168: 'V3', 236: 'V3 (4e10)', 176: 'V3 (4e10)'}
    for f in (64, 96, 128, 192, 256):
        S = seed_terms_for(f, 1, N + 1); cands.setdefault(S, f'fill {f}'); cands.setdefault(S + 3, f'fill {f} + 3 terms (control: spills)')
    print(f"{'S':>4s} {'m1':>4s} {'nspan':>10s} {'top':>5s} {'batch':>5s} {'mdev':>4s} {'pad':>5s} {'deep':>5s} {'mdev G^2':>10s}" + ("  model bs-seeds" if kb else "") + "  what")
    for S in sorted(cands):
        s = summary(N, S)
        mod = f"  {kb * s['bpts'] + c0 * s['nspan'] + km * s['mout'] + cl * s['nm']:14.1f}" if kb else ""
        print(f"{S:4d} {s['m1']:4d} {s['nspan']:10d} {s['top']:5.2f} {s['nb']:5d} {s['nm']:4d} {s['pad']:5.2f} {s['deep_fill']:5.2f} {s['mout'] / 1e18:10.2f}{mod}  {cands[S]}")

def levels(d, S):
    N = e_terms(digits_run(d)); nspan, lv = tree(N, S)
    print(f"{d:.3g} digits, S = {S}: N {N}, {nspan} spans")
    for x in lv:
        print(f"level {x['level']:2d} {x['tier']:5s} {x['npairs']:9d} pairs{'+1' if x['odd'] else '  '} max_nl {x['max_nl']:10d} maxnc {x['maxnc']:11d} "
              f"L {x['L']:11d} fill {x['fill']:.3f}")

if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['--levels']: levels(int(float(a[1])), int(a[2])); sys.exit()
    k = fit() if '--fit' in a or not a else (None, None, None, None)
    ds = [int(float(x)) for x in a if not x.startswith('--')] or [40000000000, 100000000000]
    for d in ds: table(d, *k)

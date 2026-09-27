#!/usr/bin/env python3
"""L8 (Phase 15, throwaway): the length families over one-node sizes 4e10 .. 1.4e11 (40 log-spaced sizes) and seed spans S
(160 .. 320), from the code's own plan (tests/l8_sweep.sh -> results/L815/sweep.tgz) and l8_pad.py's cost model (MODELLED).
Per size: the modelled transform seconds of the batch tier (one length per level: nc = 2 max_nl, max_nl = S log10(N) / 18
rounded up, doubling per level, up to the mdev tier at 2 max_nl > 2^30) and of every product above it (the plan's lengths; a
family may only shorten a product's length within its form, grids keep their pieces).
  S = 256    the default seed span
  tuned      each family with its own best S of the ten (the transform time only: V3 item 2 as a length knob)
Savings against today's code at S = 256 unfused; 'fused' = Batch 2 item 3 (the radix pass folded, 70 % recovered, ASSUMED)."""
import os, sys, re, math, tarfile, io
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l8_pad as M

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
TGZ = os.path.join(ROOT, 'results/L815/sweep.tgz')
SS = (160, 176, 192, 208, 224, 240, 256, 272, 288, 320)

def load():
    plans = {}
    with tarfile.open(TGZ) as t:
        for m in t.getmembers():
            g = re.search(r'plan_([\d.e+]+)_(\d+)\.txt$', m.name)
            if g: plans[(float(g[1]), int(g[2]))] = t.extractfile(m).read().decode()
    return plans

def parse(txt):
    N = int(re.search(r' N (\d+) terms', txt)[1]); P = []
    for ln in txt.splitlines():
        if not ln.startswith('plan '): continue
        ph = ln.split()[1]
        m = re.search(r'dist_db (\d+) limbs \| one plane of (3\*)?2\^(\d+) points \((B|C) form', ln)
        if m: P.append(dict(nc=int(m[1]), L=(3 if m[2] else 1) << int(m[3]), pieces=1, form=m[4], ph=ph)); continue
        m = re.search(r'dist_db (\d+) x (\d+) limbs: (\d+) x (\d+) pieces of (\d+) \+ (\d+), (\d+) formed.*\| (3\*)?2\^(\d+) points per piece \((B|C) form', ln)
        if m: P.append(dict(nc=int(m[5]) + int(m[6]), L=(3 if m[8] else 1) << int(m[9]), pieces=int(m[7]), form=m[10], ph=ph))
    return N, P

def total(N, S, P, fam, fused, code):
    spans = -(-N // S); nl1 = math.ceil(S * math.log10(N) / 18); pairs = spans // 2; l = 1; tb = pb = 0.0
    while pairs >= 1:
        nc = 2 * nl1 * 2 ** (l - 1)
        if nc + 1 > 1 << 30: break
        L = M.code_pick(nc) if code else M.pick(nc, fam, M.B_CAP, fused)
        t = M.batch_model(pairs, L, fused); tb += t; pb += t * L / nc if nc >= 2048 else 0.0
        l += 1; pairs //= 2
    ta = pa = 0.0
    for p in P:
        if p['nc'] < 1 << 20: continue
        L = p['L'] if code else M.pick(p['nc'], fam, M.B_CAP if p['form'] == 'B' else M.C_CAP, fused)
        if L is None or L > p['L']: L = p['L']
        t = p['pieces'] * 3 * M.wall(L, fused); ta += t; pa += t * L / p['nc']
    return tb, ta, pa / ta

if __name__ == '__main__':
    plans = load(); Ds = sorted({d for d, s in plans})
    fams = [('today (code)', (1, 3), True)] + [(n, f, False) for n, f in M.FAMS]
    res = {}
    for d in Ds:
        for s in SS:
            N, P = parse(plans[(d, s)])
            for fused in (False, True):
                for name, fam, code in fams:
                    if code and fused: continue
                    res[(d, s, fused, name)] = total(N, s, P, fam, fused, code)
    base = lambda d: sum(res[(d, 256, False, 'today (code)')][:2])
    print('modelled transform seconds per size, saving against today at S = 256 unfused (mean over the 40 sizes, min .. max)')
    for fused in (False, True):
        for name, fam, code in fams:
            if code and fused: continue
            s256 = [base(d) - sum(res[(d, 256, fused, name)][:2]) for d in Ds]
            tuned = []; bestS = []
            for d in Ds:
                t, s = min((sum(res[(d, s, fused, name)][:2]), s) for s in SS); tuned.append(base(d) - t); bestS.append(s)
            # the family's own gain once today is tuned too (item 2 first, then the family)
            tt = [min(sum(res[(d, s, False, 'today (code)')][:2]) for s in SS) for d in Ds]
            after = [tt[i] - (base(d) - tuned[i]) for i, d in enumerate(Ds)]
            print('  %-15s %-6s S=256: %5.2f s (%5.2f .. %5.2f) | own best S: %5.2f s (%5.2f .. %5.2f) | beyond tuned today: %5.2f s (%5.2f .. %5.2f)' % (
                name, 'fused' if fused else '', sum(s256) / len(Ds), min(s256), max(s256), sum(tuned) / len(Ds), min(tuned), max(tuned),
                sum(after) / len(Ds), min(after), max(after)))
    print('\nper size (unfused): D, today S=256 total s | saving at S=256 for +5, +15, +5,15, +5,7,15 | tuned today saving | beyond tuned today for +5, +5,15, +5,7,15')
    for d in Ds:
        b = base(d); tt = min(sum(res[(d, s, False, 'today (code)')][:2]) for s in SS)
        g = lambda n: b - sum(res[(d, 256, False, n)][:2])
        a = lambda n: tt - min(sum(res[(d, s, False, n)][:2]) for s in SS)
        print('  %.3g  %6.2f | %5.2f %5.2f %5.2f %5.2f | %5.2f | %5.2f %5.2f %5.2f' % (d, b, g('+5'), g('+15'), g('+5,15'), g('+5,7,15'), b - tt, a('+5'), a('+5,15'), a('+5,7,15')))
    print('\ntime-weighted padding of the products above the batch tier (today, S=256), per size: min %.3f mean %.3f max %.3f' % (
        min(res[(d, 256, False, 'today (code)')][2] for d in Ds), sum(res[(d, 256, False, 'today (code)')][2] for d in Ds) / len(Ds), max(res[(d, 256, False, 'today (code)')][2] for d in Ds)))
    for name in ('+5', '+15', '+5,15', '+5,7', '+5,7,15'):
        v = [res[(d, 256, False, name)][2] for d in Ds]
        print('  %-8s min %.3f mean %.3f max %.3f' % (name, min(v), sum(v) / len(v), max(v)))

#!/usr/bin/env python3
"""as_pool_replay.py -- AS15 (Phase 15 Batch 2): replay the block pool of one run from its DB_POOL_TRACE=1 lines.

A port of dbig.c's ext_insert / ext_take (the reserved tail, DM_TIGHT's packing, the pinned tail) / vmm_make_room, driven by the
trace's events per device: V (the VMM arena), I (a donation), A (a block taken: size, and the address the code chose), F (a block
freed), T (the tail set), P / N (the pack / pin flags), R (a remap the code made).  Checked mode: every block's address must be the
one the code chose (the port is exact).  What-if mode: the same request sequence against another layout -- arena bytes per device
(the donations inside the arena scaled to it: the second half's donation ends at the arena's end), the tail's bytes -- and the
remaps (count, chunks) it would make.

    python3 as_pool_replay.py TRACE                     checked replay; the remaps per device
    python3 as_pool_replay.py TRACE --arena-add GB[,GB,GB,GB] [--tail-add GB] [--quiet]   what-if
"""
import sys, argparse

class Pool:
    def __init__(s):
        s.ext = []            # [p, bytes, reg], address order
        s.tail = None         # (p, end, bytes, thresh)
        s.n_tail = s.n_spill = 0
        s.vmm = None          # dict(base_reg, C, nslot, h=[bool]*nslot, m0, bytes)
        s.live = {}           # p -> bytes
        s.remaps = []         # (need, nf, m, slot0)
        s.growth = 0

    def insert(s, p, b, reg):
        e = s.ext; i = 0
        while i < len(e) and e[i][0] < p: i += 1
        mprev = i > 0 and e[i-1][2] == reg and e[i-1][0] + e[i-1][1] == p
        mnext = i < len(e) and e[i][2] == reg and p + b == e[i][0]
        if mprev and mnext: e[i-1][1] += b + e[i][1]; del e[i]
        elif mprev: e[i-1][1] += b
        elif mnext: e[i][0] = p; e[i][1] += b
        else: e.insert(i, [p, b, reg])

    def outside_tail(s, x):
        if not s.tail: return x[1]
        a, b = x[0], x[0] + x[1]; ta, tb = s.tail[0], s.tail[1]
        if b <= ta or a >= tb: return x[1]
        return x[1] - (min(b, tb) - max(a, ta))

    def carve_at(s, i, p, need):
        e = s.ext; a, b, reg = e[i][0], e[i][0] + e[i][1], e[i][2]
        if s.tail and p + need == s.tail[1]: s.n_tail += 1
        if p == a:
            e[i][0] += need; e[i][1] -= need
            if not e[i][1]: del e[i]
        else:
            e[i][1] = p - a
            if p + need < b: s.insert(p + need, b - (p + need), reg)
        return p, reg

    def take(s, need, pack, pin):
        e = s.ext; best = -1; T = s.tail
        if pin and T:
            ta, tb = T[0], T[1]; bp = 0
            for i, x in enumerate(e):
                a, b = x[0], x[0] + x[1]; b = min(b, tb); a = max(a, ta)
                if b > a and b - a >= need and (best < 0 or b > bp): best, bp = i, b
            if best >= 0: return s.carve_at(best, bp - need, need)
            best = -1
        if pack and T and need >= T[3]:
            ta, tb = T[0], T[1]
            if pack >= 2:
                for i, x in enumerate(e):
                    if x[0] + x[1] == tb and x[1] >= need: return s.carve_at(i, x[0] + x[1] - need, need)
            bp = 0
            for i, x in enumerate(e):
                a, b = x[0], x[0] + x[1]
                if b > ta and a < tb: b = ta if a < ta else a
                if b > a and b - a >= need and (best < 0 or b > bp): best, bp = i, b
            if best >= 0: return s.carve_at(best, bp - need, need)
            for i, x in enumerate(e):
                if x[1] >= need and (best < 0 or x[0] + x[1] > e[best][0] + e[best][1]): best = i
            if best >= 0: return s.carve_at(best, e[best][0] + e[best][1] - need, need)
        if T and need >= T[3]:
            for i, x in enumerate(e):
                if x[0] + x[1] == T[1] and x[1] >= need:
                    p = x[0] + x[1] - need; reg = x[2]; x[1] -= need; s.n_tail += 1
                    if not x[1]: del e[i]
                    return p, reg
        excl = bool(T) and need < T[3]; spilled = False
        for pss in range(2):
            if best >= 0: break
            for i, x in enumerate(e):
                us = s.outside_tail(x) if excl else x[1]
                if us >= need and (best < 0 or us < (s.outside_tail(e[best]) if excl else e[best][1])): best = i
            if best < 0 and excl: s.n_spill += 1; spilled = True
            excl = False
        if best < 0: return None, None
        x = e[best]
        if spilled and T and x[0] + x[1] == T[1]:
            p = x[0] + x[1] - need; reg = x[2]; x[1] -= need
            if not x[1]: del e[best]
            return p, reg
        p = x[0]; reg = x[2]; x[0] += need; x[1] -= need
        if not x[1]: del e[best]
        return p, reg

    def make_room(s, need):
        v = s.vmm
        if not v: return False
        C = v['C']; m = -(-need // C); fr = []
        for k in range(v['nslot']):
            if len(fr) >= m: break
            if v['h'][k]:
                p = k * C
                if any(p >= x[0] and p + C <= x[0] + x[1] for x in s.ext): fr.append(k)
        for k in fr:
            s.remove(k * C, C); v['h'][k] = False
        slot0 = -1; run = 0
        for k in range(v['nslot']):
            run = 0 if v['h'][k] else run + 1
            if run == m: slot0 = k - m + 1; break
        if slot0 < 0:
            for k in fr: v['h'][k] = True; s.insert(k * C, C, v['reg'])
            return False
        for k in range(slot0, slot0 + m): v['h'][k] = True
        if m > len(fr): s.growth += m - len(fr)
        s.insert(slot0 * C, m * C, v['reg'])
        s.remaps.append((need, len(fr), m, slot0))
        return True

    def remove(s, p, b):
        e = s.ext
        for i, x in enumerate(e):
            if p >= x[0] and p + b <= x[0] + x[1]:
                a, bb, reg = x[0], x[0] + x[1], x[2]
                if p == a:
                    x[0] += b; x[1] -= b
                    if not x[1]: del e[i]
                else:
                    x[1] = p - a
                    if p + b < bb: s.insert(p + b, bb - (p + b), reg)
                return
        raise RuntimeError('ext_remove: not free')

DUMP = None
ROUND = False
CHUNK = 0
THRESH = 0.0
def dump(pl, d, what):
    G = 1e9; C = pl.vmm['C'] if pl.vmm else 1
    lv = sorted((p, n) for (p, n, r) in pl.live.values() if p < (1 << 58))
    print('   [APU%d %s] tail %s' % (d, what, ('%.2f-%.2f' % (pl.tail[0] / G, pl.tail[1] / G)) if pl.tail else '-'))
    print('     free: ' + ' '.join('%.2f+%.2f' % (x[0] / G, x[1] / G) for x in pl.ext if x[0] < (1 << 58)))
    print('     live: ' + ' '.join('%.2f+%.2f' % (p / G, n / G) for p, n in lv))

def replay(path, arena_add=None, tail_add=0, check=True, quiet=False, until=None):
    pools = {}; pack = 0; pin = 0; nmis = 0; nA = 0; phase = 'init'; events = []
    GBb = 1e9; arena_old = {}; arena_new = {}; mism_first = None
    def P(d): return pools.setdefault(d, Pool())
    for ln in open(path):
        if not ln.startswith('dbtrace:'): continue
        f = ln.split()[1:]; op = f[0]
        if op == 'P': pack = int(f[1]); continue
        if op == 'N': pin = int(f[1]); continue
        d = int(f[1]); pl = P(d)
        if op == 'V':
            by, C, m0, nslot, reg = int(f[2]), int(f[3]), int(f[4]), int(f[5]), int(f[6])
            add = int(arena_add[d] * GBb) if arena_add else 0
            add = (add + (2 << 20) - 1) // (2 << 20) * (2 << 20)
            if CHUNK: C = CHUNK
            nb = by + add
            if ROUND: nb = -(-nb // C) * C                              # the arena in whole chunks (the tail ends at the last chunk's end)
            m0n = -(-nb // C); ns = int(m0n * 3.0) + 1
            arena_old[d] = by; arena_new[d] = nb
            pl.vmm = dict(C=C, nslot=ns, h=[True] * m0n + [False] * (ns - m0n), m0=m0n, bytes=nb, reg=reg)
        elif op == 'I':
            p, b, reg = int(f[2]), int(f[3]), int(f[4])
            if pl.vmm and p < (1 << 59):
                ob = arena_old[d]; nb = arena_new[d]; C = pl.vmm['C']
                if p + b == ob: b = nb - p                               # the second half's donation: to the new arena's end
                elif p == ob: p = nb; b = pl.vmm['m0'] * C - nb         # the last chunk's remainder
                if b <= 0: continue
            pl.insert(p, b, reg)
        elif op == 'T':
            p, b, th = int(f[2]), int(f[3]), int(f[4])
            if pl.vmm and arena_new.get(d) is not None and p < (1 << 59):
                end = arena_new[d]; b += int(tail_add * GBb); p = end - b
                if THRESH: th = int(b * THRESH)
            pl.tail = (p, p + b, b, th); pl.n_tail = pl.n_spill = 0
        elif op == 'A':
            need, p_code = int(f[2]), int(f[3]); nA += 1
            if DUMP is not None and d == DUMP and need >= 2e9: dump(pl, d, 'before block %d: %.2f GB, pack %d pin %d' % (nA, need / 1e9, pack, pin))
            p, reg = pl.take(need, pack, pin)
            if p is None and pl.make_room(need):
                if DUMP is not None and d == DUMP: print('     -> REMAP')
                p, reg = pl.take(need, pack, pin)
            if p is None: reg = ('h', nA); p = (1 << 58) + nA * (1 << 40); events.append((d, 'hipMalloc', need))
            if check and p != p_code:
                nmis += 1
                if mism_first is None: mism_first = (nA, d, need, p, p_code)
                if not quiet and nmis <= 5: print('MISMATCH at block %d: APU%d %.3f GB -> replay %x, code %x' % (nA, d, need / GBb, p, p_code))
            pl.live[p_code] = (p, need, reg)
        elif op == 'F':
            p_code, b = int(f[2]), int(f[3])
            if DUMP is not None and d == DUMP and b >= 2e9: print('   free %.2f GB (code %.2f)' % (b / 1e9, p_code / 1e9))
            p, need, reg = pl.live.pop(p_code)
            if isinstance(reg, tuple): continue                          # a hipMalloc block: its own region, never reused here
            pl.insert(p, b, reg)
        elif op == 'R':
            pass
    return pools, nA, nmis, mism_first, events

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('trace'); ap.add_argument('--arena-add', default=None); ap.add_argument('--tail-add', type=float, default=0.0)
    ap.add_argument('--quiet', action='store_true'); ap.add_argument('--dump', type=int, default=None); ap.add_argument('--round-chunks', action='store_true'); ap.add_argument('--chunk-mb', type=float, default=0); ap.add_argument('--thresh', type=float, default=0.0); a = ap.parse_args()
    global DUMP, ROUND, CHUNK, THRESH; THRESH = a.thresh; DUMP = a.dump; ROUND = a.round_chunks; CHUNK = int(a.chunk_mb * 1048576) if a.chunk_mb else 0
    add = None
    if a.arena_add is not None:
        v = [float(x) for x in a.arena_add.split(',')]; add = v * 4 if len(v) == 1 else v
    pools, nA, nmis, mf, ev = replay(a.trace, add, a.tail_add, check=add is None and a.tail_add == 0 and not a.round_chunks, quiet=a.quiet)
    print('%s: %d blocks; %s' % (a.trace, nA, ('%d mismatches (first: %s)' % (nmis, mf)) if (add is None and a.tail_add == 0 and not a.round_chunks) else 'what-if arena +%s GB, tail +%.2f GB' % (a.arena_add, a.tail_add)))
    tot = 0; totc = 0
    for d in sorted(pools):
        pl = pools[d]; r = pl.remaps; tot += len(r); totc += sum(x[1] for x in r)
        print('  APU%d: arena %.2f GB, %d remaps (%s), growth %d chunks' % (d, pl.vmm['bytes'] / 1e9 if pl.vmm else 0, len(r),
              ', '.join('%.2f GB: %d moved' % (x[0] / 1e9, x[1]) for x in r), pl.growth))
    print('  total %d remaps, %d chunks moved; hipMalloc %d' % (tot, totc, len(ev)))

if __name__ == '__main__':
    main()

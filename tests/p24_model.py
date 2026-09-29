#!/usr/bin/env python3
"""P24 (Phase 15 Batch 3): mn_model's P24 term at the target, every number MODELLED (mn_model / mem_model on their measured inputs;
the fabric ASSUMED: 100 GB/s per APU, 2 us).  Run from ecalc/:

    python3 ../tests/p24_model.py [T ...]          the walls and pieces at T total digits (default 5.1e13 and 5.55e13) on 576 nodes:
                                                    ECALC_NP 4 / auto x cache slots 0 / 2 x MN_P24 0 / 1 / 2 (x MN_MODEL_P24_F 1.0 / 1.1)
    python3 ../tests/p24_model.py sweep LO HI STEP  the P24 sweep (auto, cache 0, MN_P24 0 / 1 / 2): walls and critical-path pieces
"""
import sys, os
sys.path.insert(0, '.')
import mn_model as M, mem_model, estimate as E

G = 576

def design(np_mn):
    return M.Design(np=3, strategy='auto', cap=mem_model.CAPS['2^31'], chunk='both', depth=2, modmul=1, chunk_mb=M.CHUNK_MB,
                    p15=True, round_mb=1024, out_overlap=None, p15b=True, np_mn=np_mn, packed=True)

def clear():
    M._PC.clear()
    for f in (M.split_grid, M.plane_pts):
        f.cache_clear()
    M.NP_STATS.update(n3=0, n4=0)

def est(T, np_mn, slots, p24, f=1.0, wbw=0.6):
    M.CACHE_MN_SLOTS = slots; M.P24 = p24; M.P24_F = f; clear()
    fab = M.Fabric(M.TARGET.name, M.TARGET.bw, M.TARGET.lat, group=64, layers=2, taper=1.0, write_bw=wbw)
    e = E.estimate(G, T / G, 'grid', None, fab, 'model', staging='code', design=design(np_mn))
    clear(); p = M.plan(G, T, design(np_mn))
    return e, p

def line(tag, e, p):
    return ('%-34s | %6.1f | %6.1f | levels %5.1f recip %5.1f div %5.1f | pieces tree %3d recip %3d div %3d = %3d | node %5.1f GB'
            % (tag, e['nowrite_s'], e['wall_s'], e['levels'], e['recip'], e['div'], p['tree'], p['recip'], p['div'], p['tree'] + p['recip'] + p['div'], e['node_gb']))

def main():
    a = sys.argv[1:]
    if a and a[0] == 'sweep':
        lo, hi, st = float(a[1]), float(a[2]), float(a[3]); T = lo
        print('# P24 sweep on %d nodes (modelled): digits | wall no write / with | critical-path pieces, for auto cache 0: P24 0 | P24 1 | P24 2' % G)
        while T <= hi * 1.0000001:
            cells = []
            for p24 in (0, 1, 2):
                e, p = est(T, 'auto', 0, p24); cells.append('%6.1f / %6.1f %3d' % (e['nowrite_s'], e['wall_s'], p['tree'] + p['recip'] + p['div']))
            print('%.4e | %s | node %.1f GB' % (T, ' | '.join(cells), e['node_gb'])); sys.stdout.flush()
            T += st
        return
    Ts = [float(x) for x in a] or [5.1e13, 5.55e13]
    for T in Ts:
        print('== %.4g digits on %d nodes (modelled; the fabric assumed): tag | no write | with write (0.6 GB/s) | phases | pieces (node 0, critical path) | node' % (T, G))
        for np_mn in (4, 'auto'):
            for slots in (0, 2):
                for p24 in (0, 1, 2):
                    if np_mn == 4 and p24 == 2: continue            # (= 1 under ECALC_NP=4)
                    for f in ((1.0, 1.1) if p24 else (1.0,)):
                        e, p = est(T, np_mn, slots, p24, f)
                        print(line('NP %-4s cache %d P24 %d F %.2f' % (np_mn, slots, p24, f), e, p)); sys.stdout.flush()

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""MS (Phase 15): the candidate sizes on 576 nodes with MN_P24=2 NEWTON_DKM=1 on the target's launch line (ECALC_NP=auto,
RNS_DIST_CACHE_FIT=1, COMM_SHMEM_ROUND_MB=1024), every number MODELLED (mn_model / mem_model on their measured inputs; the fabric
ASSUMED 100 GB/s per APU, 2 us; the write at 0.6 GB/s; the mn cache at 0 slots; P24's surcharge 1.025, measured by P24).
Run from ecalc/ with the switches in the environment (the model reads them at import):

    MN_P24=2 MN_MODEL_DKM=1 MN_MODEL_P24_F=1.025 python3 ../tests/ms15_candidates.py [T ...]
    ... python3 ../tests/ms15_candidates.py --maxfit          the largest T per node budget (480 GB), room 0.16 / 0, DKM's dm_layout off / on

Columns: T | wall without / with the write | node 0's pieces (tree + recip + div; mn_model.plan = the C plan at the priced rows) |
node GB at BS_ARENA_ROOM 0.16 (arena; 'r' = the room kept, as binsplit.c as_room_fits decides) | at 0 | the same with DKM's dm_layout
(not built)."""
import sys
sys.path.insert(0, '.')
import mn_model as M, mem_model as MM, estimate as E

G = 576; GB = 1e9

def node(T, room, dkm=False):
    o = dict(MM.OLD13); o.update(form='grid', groups=None, transport='shmem', pool_log=31, staging='code')
    o.update(M.DEFAULT15C(arena_room=room).at_g(G).mem_opts(T)); o.update(p24=M.P24, np_auto_min=M.NP_AUTO_MIN, dkm=dkm)
    return MM.mem_per_node(int(T / G), G, o)

def clear():
    M._PC.clear()
    for f in (M.split_grid, M.plane_pts): f.cache_clear()

def walls(T):
    M.CACHE_FORCE = 0; clear()
    fab = M.Fabric(M.TARGET.name, M.TARGET.bw, M.TARGET.lat, group=64, layers=2, taper=1.0, write_bw=0.6)
    e = E.estimate(G, T / G, 'grid', None, fab, 'model', staging='code', design=M.DEFAULT15C())
    clear(); p = M.plan(G, T, M.DEFAULT15C())
    return e, p

def maxfit(room, dkm, budget=480.0, lo=4.9e13, hi=6.0e13, step=0.002e13):
    """the ends of the fitting ranges (the node is not monotonic: the room's whole chunks, the room dropped by the code over its own check)"""
    out = []; prev = None; T = lo
    while T <= hi:
        f = node(T, room, dkm)['node_peak'] / GB <= budget
        if prev and not f:
            a, b = T - step, T
            while b - a > G * 1e5:
                m = (a + b) / 2
                if node(m, room, dkm)['node_peak'] / GB <= budget: a = m
                else: b = m
            out.append(a)
        prev = f; T += step
    return out

def main():
    a = sys.argv[1:]
    print('# MN_P24=%d MN_MODEL_DKM=%d MN_MODEL_P24_F=%.3f ECALC_NP_AUTO_MIN=%d (modelled)' % (M.P24, M.DKM, M.P24_F, M.NP_AUTO_MIN))
    if a and a[0] == '--maxfit':
        for room in (0.16, 0.0):
            for dkm in (False, True):
                xs = maxfit(room, dkm)
                print('room %.2f dm_layout(DKM) %d: fits up to %s' % (room, dkm, ', '.join('%.5e (node %.2f GB)' % (x, node(x, room, dkm)['node_peak'] / GB) for x in xs)))
                sys.stdout.flush()
        return
    Ts = [float(x) for x in a] or [5.1e13, 5.55e13]
    print('%-11s | %7s %7s | %-17s | %-22s | %-22s | %-16s | %-16s' % ('T', 'nowrite', 'write', 'pieces t+r+d', 'node room .16', 'node room 0', 'DKM-layout .16', 'DKM-layout 0'))
    for T in Ts:
        e, p = walls(T)
        cells = []
        for room, dkm in ((0.16, False), (0.0, False), (0.16, True), (0.0, True)):
            r = node(T, room, dkm)
            cells.append('%6.2f (%6.2f %s)' % (r['node_peak'] / GB, r['arena'] / GB, 'r' if r['arena_room'] > 0 else '-') if not dkm else '%6.2f' % (r['node_peak'] / GB))
        print('%.5e | %7.1f %7.1f | %3d = %2d+%2d+%2d | %s' % (T, e['nowrite_s'], e['wall_s'], p['tree'] + p['recip'] + p['div'], p['tree'], p['recip'], p['div'], ' | '.join(cells)))
        sys.stdout.flush()

if __name__ == '__main__':
    main()

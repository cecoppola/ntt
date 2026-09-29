#!/usr/bin/env python3
"""DL (Phase 15 Batch 3): the node against the total digits with binsplit.c's dm_layout following NEWTON_DKM and the room check on the
model's node (as_room_fits = mem_model.as_room_fits), every number MODELLED (mem_model on its measured inputs).  Run from ecalc/:

    MN_P24=2 python3 ../tests/dl15_ceilings.py [--g 576|1] [--lo T --hi T --step T]

576: the target's launch line (ECALC_NP=auto, RNS_DIST_CACHE_FIT=1, COMM_SHMEM_ROUND_MB=1024, COMM_TRANSPORT=shmem), the design of
tests/ms15_candidates.py.  Columns per T: node GB and arena GB for (a) the code as built: room 0.16 decided by the check, DKM layout;
(b) the same with NEWTON_DKM=0 (today's layout, the new check); (c) BS_ARENA_ROOM=0 with the DKM layout ('-' = the arena not in whole
chunks, as the layout requests it; the VMM pool maps whole chunks anyway: '+c' is that node).  'r' = the room kept.
Then the edges: the largest T whose node fits 480 GB with the room kept, where the room turns off, the largest T that fits at all."""
import sys, argparse
sys.path.insert(0, '.')
import mn_model as M, mem_model as MM

GB = 1e9

def node(T, g, room=0.16, dkm=True):
    if g > 1:
        o = dict(MM.OLD13); o.update(form='grid', groups=None, transport='shmem', pool_log=31, staging='code')
        o.update(M.DEFAULT15C(arena_room=room).at_g(g).mem_opts(T)); o.update(p24=M.P24, np_auto_min=M.NP_AUTO_MIN, dkm=dkm)
    else:
        o = dict(MM.DEFAULTS15D, arena_room=room, dkm=dkm)
    return MM.mem_per_node(int(T / g), g, o)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--g', type=int, default=576)
    ap.add_argument('--lo', type=float, default=None); ap.add_argument('--hi', type=float, default=None); ap.add_argument('--step', type=float, default=None)
    a = ap.parse_args(); g = a.g
    lo = a.lo or (5.0e13 if g > 1 else 0.9e11); hi = a.hi or (5.8e13 if g > 1 else 1.6e11); step = a.step or (0.01e13 if g > 1 else 0.01e11)
    print('# g %d, MN_P24=%d (modelled; GB per node)' % (g, M.P24))
    print('# T | built: node arena room | NEWTON_DKM=0: node arena room | BS_ARENA_ROOM=0 (DKM): node arena')
    T = lo; rows = []
    while T <= hi * (1 + 1e-9):
        r1 = node(T, g); r0 = node(T, g, dkm=False); rz = node(T, g, room=0.0); rc = node(T, g, room=1e-12)   # rc: a room of 0 bytes, the arenas in whole chunks
        rows.append((T, r1, r0, rz, rc))
        print('%.4e | %7.2f %7.2f %s | %7.2f %7.2f %s | %7.2f %7.2f +c %7.2f' % (T, r1['node_peak'] / GB, r1['arena'] / GB, 'r' if r1['arena_room'] > 0 else '-',
              r0['node_peak'] / GB, r0['arena'] / GB, 'r' if r0['arena_room'] > 0 else '-', rz['node_peak'] / GB, rz['arena'] / GB, rc['node_peak'] / GB)); sys.stdout.flush()
        T += step
    def edges(k, name):
        fit_room = [t for t, *rs in rows if rs[k]['arena_room'] > 0 and rs[k]['node_peak'] <= 480 * GB]
        off = [t for t, *rs in rows if rs[k]['arena_room'] == 0]
        fit = [t for t, *rs in rows if rs[k]['node_peak'] <= 480 * GB]
        print('%s: largest T with the room kept and fitting %s; the room first off at %s; largest T fitting %s' % (name,
              '%.4e' % max(fit_room) if fit_room else '-', '%.4e' % min(off) if off else '-', '%.4e' % max(fit) if fit else '-'))
    edges(0, 'built (DKM layout, the new check)'); edges(1, 'NEWTON_DKM=0 (the new check)'); edges(2, 'BS_ARENA_ROOM=0 (DKM layout, arena as laid out)')
    fitc = [t for t, *rs in rows if rs[3]['node_peak'] <= 480 * GB]
    print('BS_ARENA_ROOM=0 (DKM layout) with the arenas counted in whole VMM chunks: largest T fitting %s' % ('%.4e' % max(fitc) if fitc else '-'))

if __name__ == '__main__':
    main()

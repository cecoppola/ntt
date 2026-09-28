#!/usr/bin/env python3
"""SC15 (Phase 15, desk work): what-if variants of mn_model at the target 5.1e13 on 576 nodes.  Throwaway; every number it prints
is MODELLED (mn_model / mem_model on their measured inputs; the fabric ASSUMED: 100 GB/s per APU, 2 us per message).  Run from ecalc/:

    python3 ../tests/sc15_model.py base        the by-phase lines, ECALC_NP=4 and auto (= estimate.py --np-mn 4 / auto --verbose)
    python3 ../tests/sc15_model.py np          the prime-count variants under auto (the switch-over on min(pa, pb); a larger three-prime bound)
    python3 ../tests/sc15_model.py pack        four primes at 24 digits per transform point in the mn tier (P24)
    python3 ../tests/sc15_model.py r3          3.2^k planes in the mn tier (the 192 level's plane fills its pool: 3.2^37 instead of 2^38)
    python3 ../tests/sc15_model.py groups      MN_GROUPS schedules
    python3 ../tests/sc15_model.py fabric      the assumed fabric terms: 50 / 100 / 200 GB/s per APU, 2 / 5 us
    python3 ../tests/sc15_model.py sweep LO HI STEP [variant]   the walls and pieces over total digits (variant: base | pack | r3 | pack+r3 | min)
"""
import sys, math, inspect, functools
sys.path.insert(0, '.')
import mn_model as M, mem_model, estimate as E

G = 576
T0 = 5.1e13

def design(np_mn='auto'):
    return M.Design(np=3, strategy='auto', cap=mem_model.CAPS['2^31'], chunk='both', depth=2, modmul=1, chunk_mb=M.CHUNK_MB,
                    p15=True, round_mb=1024, out_overlap=None, p15b=True, np_mn=np_mn, packed=True)

def clear():
    M._PC.clear()
    for f in (M.split_grid, M.plane_pts):
        try: f.cache_clear()
        except AttributeError: pass
    M.NP_STATS.update(n3=0, n4=0)

def est(T=T0, np_mn='auto', groups=None, fab=None, wbw=0.6):
    clear()
    fab = fab or M.Fabric(M.TARGET.name, M.TARGET.bw, M.TARGET.lat, group=64, layers=2, taper=1.0, write_bw=wbw)
    e = E.estimate(G, T / G, 'grid', groups, fab, 'model', staging='code', design=design(np_mn))
    return e

def line(tag, e, extra=''):
    return ('%-44s | %6.1f | %6.1f | init %4.1f seed %4.1f batch %4.1f top %4.1f levels %5.1f recip %4.1f div %4.1f out %4.1f | node %5.1f GB%s'
            % (tag, e['nowrite_s'], e['wall_s'], e['init'], e['seed_wait'], e['batch'], e['top'], e['levels'], e['recip'], e['div'], e['out'], e['node_gb'], extra))

def pieces(T, np_mn='auto', groups=None):
    clear(); p = M.plan(G, T, design(np_mn), groups=groups); return p

# ---------------------------------------------------------------------------------------------------------------------------
# variant hooks
# ---------------------------------------------------------------------------------------------------------------------------
_orig_piece_np = M.piece_np
_orig_piece_cost = M.piece_cost
_orig__product_cost = M._product_cost
_orig_plane_pts = M.plane_pts
VAR = dict(minnp=False, bound=None, pack=None, r3=False, r3f=1.03, slot1=False)

def set_var(**kw):
    VAR.update(minnp=False, bound=None, pack=None, r3=False, r3f=1.03, slot1=False); VAR.update(kw)
    M.NP_AUTO_TERMS = VAR['bound'] or mem_model.NP3_MAX_TERMS
    clear()

# (1) the switch-over on min(pa, pb) (the coefficient's real term count) instead of pa + pb: piece_cost knows la, lb
def piece_cost_v(fab, pts, g, na, nb, nc, fwd=2, with_x=False, form="grid", grid=False):
    if VAR['minnp']:
        M._SC_NC = min(na, nb)
    else:
        M._SC_NC = nc
    return _pc_body(fab, pts, g, na, nb, nc, fwd, with_x, form, grid)

def piece_np_v(dz, nc):
    if VAR['pack']:                                      # P24: every mn piece at four primes (24 digits per point)
        if dz is None or dz.legacy: return M.EC_NP
        if getattr(dz, 'np_auto', False) or dz.np == 4: M.NP_STATS['n4'] += 1; return 4
        return dz.np
    return _orig_piece_np(dz, getattr(M, '_SC_NC', nc) if VAR['minnp'] else nc)

# the piece body with two edits: (a) under P24 the operand redistributions and the result move limbs (8 B per limb = the point
# count / pack), not points; (b) a 3.2^k plane costs r3f x its points in the local passes
_src = inspect.getsource(_orig_piece_cost)
_src = _src.replace('def piece_cost(', 'def _pc_body(', 1)
_src = _src.replace('for limbs in ([na] if fwd >= 1 else []) + ([nb] if fwd >= 2 else []) + ([nc] if with_x else []) + [nc]:',
                    'for limbs in [x / (VAR["pack"] or 1.0) for x in ([na] if fwd >= 1 else []) + ([nb] if fwd >= 2 else []) + ([nc] if with_x else []) + [nc]]:')
_src = _src.replace('t_loc = t31 * scale + 0.005', 't_loc = t31 * scale * (VAR["r3f"] if (VAR["r3"] and (int(pts) & (int(pts) - 1))) else 1.0) + 0.005')
_src = _src.replace('C = 1 << (mem_model.mn_shape(pts, g)[2])', 'C = 1 << (mem_model.mn_shape(int(pts), g)[2])')
assert 'VAR["pack"]' in _src and 'VAR["r3f"]' in _src
exec(compile(_src, 'sc15_pc_body', 'exec'), M.__dict__)
M.__dict__['VAR'] = VAR
_pc_body = M._pc_body

# (2) P24: the mn products' operands in points = limbs x 18/24 (the cuts too); the shifted add of with_x moves limbs
def _product_cost_v(fab, na, nb, g, lowcut=0, highcut=None, with_x=False, cache=True, form="grid"):
    pk = VAR['pack']
    if not pk: return _orig__product_cost(fab, na, nb, g, lowcut, highcut, with_x, cache, form)
    sa, sb = int(math.ceil(na * pk)), int(math.ceil(nb * pk))
    lc = int(lowcut * pk) if lowcut else 0
    hc = int(math.ceil(highcut * pk)) if highcut is not None else None
    c = _orig__product_cost(fab, sa, sb, g, lc, hc, False, cache, form)
    if with_x:                                            # as _product_cost's with_x term, on the limbs
        nc = na + nb
        t1, nic1, glob1, msgs1 = fab.a2a(8 * nc / (4 * g), g, 1)
        if M.DZ is not None and M.DZ.t_mb:
            W = mem_model.t_chunk_limbs(M.DZ.t_mb); t1 += max(0, -(-(nc // g) // W) - 1) * M.round_cost(fab, g)
        c.t += t1 + fab.coll(g); c.t_exposed += t1 + fab.coll(g); c.nic += nic1; c.glob += glob1; c.msgs += msgs1; c.xfers += 1
    return c

# (3) 3.2^k planes in the mn tier: the plane is the smallest 2^k or 3.2^k >= nc (and the shape's minimum); the cap = the largest
# such length whose per-APU share fits the pool's 2^29 points (192: 3.2^37 = 2^29 x 768; 576: still 2^40 -- 3.2^38 is smaller)
@functools.lru_cache(maxsize=None)
def plane_pts_v(nc, g):
    p2 = _orig_plane_pts(nc, g)
    if not VAR['r3']: return p2
    n3 = 3 << 20
    while n3 < nc: n3 <<= 1
    lo = 1 << mem_model.mn_shape(nc, g)[0]
    cand = [x for x in (p2, n3) if x >= lo and x >= nc]
    return min(cand)

_orig_cap_log = mem_model.mn_cap_log
def cap_points(g, pool_log=31):
    c2 = 1 << _orig_cap_log(g, pool_log)
    if not VAR['r3']: return c2
    lim = (1 << min(31, pool_log)) * g                    # 2^29 per APU x 4 g APUs
    k = 0
    while (3 << (k + 1)) <= lim: k += 1
    return max(c2, 3 << k)

_src2 = inspect.getsource(_orig__product_cost).replace('def _product_cost(', 'def _product_cost_r3(', 1)
_src2 = _src2.replace('cap = 1 << mem_model.mn_cap_log(g, 31 if DZ is None or DZ.legacy else DZ.pool_log())',
                      'cap = SC_CAP(g, 31 if DZ is None or DZ.legacy else DZ.pool_log())')
_src2 = _src2.replace('                if i > 0 and j > 0: fwd = 1\n',
                      '                if i > 0 and j > 0: fwd = 1\n            elif cache and SC_SLOT1(la + lb) and i > 0 and not first: fwd = 1\n')
assert 'SC_CAP' in _src2 and 'SC_SLOT1' in _src2
M.__dict__['SC_CAP'] = cap_points
# (4) one slot, for the three-prime pieces only (the hypothesis: plane pool 1, 12 GiB per APU = 3 x 2^29 x 8 B, idle in the mn phases):
# B's piece j held across i
def slot1(nc):
    return VAR.get('slot1') and not VAR['pack'] and nc <= M.NP_AUTO_TERMS
M.__dict__['SC_SLOT1'] = slot1
exec(compile(_src2, 'sc15_pc2', 'exec'), M.__dict__)
_orig__product_cost = M._product_cost_r3                 # (identical to the original while VAR['r3'] is off)

M.piece_cost = piece_cost_v
M.piece_np = piece_np_v
M._product_cost = _product_cost_v
M.plane_pts = plane_pts_v
M.split_grid.cache_clear()

# ---------------------------------------------------------------------------------------------------------------------------
def run_base():
    print('# the target 5.1e13 on 576, by phase (modelled; the fabric assumed; write 0.6 GB/s per node)')
    print('%-44s | %6s | %6s |' % ('variant', 'nowr', 'write'))
    set_var()
    for np_mn in (4, 'auto'):
        e = est(np_mn=np_mn); p = pieces(T0, np_mn)
        print(line('ECALC_NP=%s' % np_mn, e, ' | pieces %d + %d + %d = %d | exposed fabric %.1f s | levels %s' % (
            p['tree_max'], p['recip'], p['div'], p['tree_max'] + p['recip'] + p['div'], e['exposed_s'], p['levels_max'])))
    set_var(); e3 = est(np_mn=3); print(line('ECALC_NP=3 (not runnable: the plan refuses)', e3))

def np_counts():
    """pieces priced at four primes on the critical path (the model's memo counts a product once: the counts are by product shape)"""
    return dict(M.NP_STATS)

def run_np():
    print('# ECALC_NP=auto variants at 5.1e13 (modelled): the switch-over test and the three-prime bound')
    rows = [('auto as built (pa + pb > 5.84e10)', dict()),
            ('auto, min(pa, pb) > 5.84e10', dict(minnp=True)),
            ('auto, pa + pb > 8.00e10 (15 | p - 1 set)', dict(bound=80.0e9)),
            ('auto, pa + pb > 8.25e10 (best 3 | p - 1 set)', dict(bound=82.5e9)),
            ('auto, min(pa, pb) > 8.25e10', dict(minnp=True, bound=82.5e9)),
            ('three primes everywhere (bound ignored)', dict(bound=1e30))]
    for tag, kw in rows:
        set_var(**kw); e = est(); st = np_counts()
        print(line(tag, e, ' | product shapes priced at 4 / 3: %d / %d' % (st['n4'], st['n3'])))
    set_var()

def run_pack():
    print('# P24: four primes at 24 digits per transform point in the mn tier (pack 18/24 = 0.75), against auto and ECALC_NP=4 (modelled)')
    set_var(); b = est(); print(line('auto (B1 + NP)', b))
    set_var(); b4 = est(np_mn=4); print(line('ECALC_NP=4', b4))
    for pk, tag in ((0.75, 'P24 (4 primes, 24 digits per point)'),):
        set_var(pack=pk); e = est(); p = pieces(T0)
        print(line(tag, e, ' | pieces %d + %d + %d = %d levels %s' % (p['tree_max'], p['recip'], p['div'], p['tree_max'] + p['recip'] + p['div'], p['levels_max'])))
        set_var(pack=pk, r3=True); e = est(); p = pieces(T0)
        print(line(tag + ' + R3 planes', e, ' | pieces %d + %d + %d = %d levels %s' % (p['tree_max'], p['recip'], p['div'], p['tree_max'] + p['recip'] + p['div'], p['levels_max'])))
    set_var()

def run_r3():
    print('# R3: 3.2^k planes in the mn tier (192 level: 3.2^37 fills the per-APU pool), r3f = the per-point price of a 3.2^k plane (modelled)')
    set_var(); b = est(); p = pieces(T0)
    print(line('auto (B1 + NP)', b, ' | pieces %d + %d + %d levels %s' % (p['tree_max'], p['recip'], p['div'], p['levels_max'])))
    for f in (1.0, 1.03, 1.06):
        set_var(r3=True, r3f=f); e = est(); p = pieces(T0)
        print(line('R3, r3f %.2f' % f, e, ' | pieces %d + %d + %d levels %s' % (p['tree_max'], p['recip'], p['div'], p['levels_max'])))
    set_var()

SCHED = [('3x3 (the launch line)', '2,4,8,16,32,64,192,576'), ('9-way top', '2,4,8,16,32,64,576'),
         ('binary + 576', None if False else '2,4,8,16,32,64,128,256,512,576'),
         ('3-way first', '3,6,12,24,48,96,192,576'), ('2,4,8,16,32,96,192,576', '2,4,8,16,32,96,192,576'),
         ('2,4,8,16,48,144,576', '2,4,8,16,48,144,576'), ('4,16,64,192,576', '4,16,64,192,576'), ('2,4,8,16,32,64,288,576', '2,4,8,16,32,64,288,576')]

def run_groups():
    print('# MN_GROUPS at 5.1e13, auto (modelled)')
    set_var()
    for tag, spec in SCHED:
        try:
            e = est(groups=spec); p = pieces(T0, groups=spec)
        except Exception as ex:
            print('%-44s | %s' % (tag, ex)); continue
        print(line(tag, e, ' | pieces %d + %d + %d levels %s' % (p['tree_max'], p['recip'], p['div'], p['levels_max'])))

def run_fabric():
    print('# the assumed fabric at 5.1e13, auto (modelled)')
    set_var()
    for bw in (50.0, 100.0, 200.0):
        for lat in (2e-6, 5e-6):
            fab = M.Fabric(M.TARGET.name, bw, lat, group=64, layers=2, taper=1.0, write_bw=0.6)
            e = est(fab=fab); print(line('%.0f GB/s per APU, %.0f us' % (bw, lat * 1e6), e, ' | exposed %.1f s' % e['exposed_s']))
    for taper in (0.5, 0.25):
        fab = M.Fabric(M.TARGET.name, 100.0, 2e-6, group=64, layers=2, taper=taper, write_bw=0.6)
        e = est(fab=fab); print(line('100 GB/s, 2 us, global taper %.2f' % taper, e, ' | exposed %.1f s' % e['exposed_s']))
    for wbw in (0.6, 0.8, 1.2, 2.4):
        e = est(wbw=wbw); print(line('write %.1f GB/s per node' % wbw, e))

def run_sweep(lo, hi, step, var='base'):
    kw = dict(base={}, pack=dict(pack=0.75), r3=dict(r3=True), min=dict(minnp=True), **{'pack+r3': dict(pack=0.75, r3=True)})[var]
    print('# sweep %s: total digits, pieces (tree_max + recip + div), walls without / with the write at 0.6 GB/s, node GB (modelled)' % var)
    n = int(round((hi - lo) / step)); prev = None
    for i in range(n + 1):
        T = lo + i * step
        set_var(**kw); p = pieces(T); e = est(T)
        tot = p['tree_max'] + p['recip'] + p['div']
        mark = '' if prev is None or prev == tot else '   <- step %d -> %d' % (prev, tot)
        print('%.4e  %3d + %2d + %2d = %3d | %6.1f | %6.1f | %5.1f GB | %.2f us/digit-per-node%s' % (T, p['tree_max'], p['recip'], p['div'], tot, e['nowrite_s'], e['wall_s'],
              e['node_gb'], e['nowrite_s'] / (T / G) * 1e9, mark))
        prev = tot
    set_var()

if __name__ == '__main__':
    a = sys.argv[1:] or ['base']
    dict(base=run_base, np=run_np, pack=run_pack, r3=run_r3, groups=run_groups, fabric=run_fabric)[a[0]]() if a[0] != 'sweep' else \
        run_sweep(float(a[1]), float(a[2]), float(a[3]), a[4] if len(a) > 4 else 'base')

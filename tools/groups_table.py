#!/usr/bin/env python3
"""groups_table.py - c09: the best MN_GROUPS schedule for every usable node count N (576 down to 544 by default).

"Exclude k bad nodes, no spares": the run then has N = 576 - k nodes.  The code's default schedule (the powers of two dividing N, then the
odd part's prime factors) is very slow at awkward N; an explicit MN_GROUPS whose last group is the (uneven) N is accepted by the planner.
This script searches those schedules with the standing model (ecalc/estimate.py -> mn_model.py; pure Python, CPU only) on the target line:
    profile `target` (layv factor 0.655), MN_T_CHUNK_MB=2048 (MDB_SHIFT_CHUNK_MB stays 1024), shared v-slot pair (X2, always on), RNS_DIST_CACHE_FIT=1,
    ECALC_NP=auto, BS_ARENA_ROOM=0.16, ECALC_PLANE_CAP=2^31, COMM_SHMEM_ROUND_MB=1024, the same total digits whatever N.
Memory rule (results/B01.md, S52): device peak (model) + 0.5 GB (X2, the user's figure) + 2.1 GB (rank 0) must stay <= 373.44 - 10 GB.

Steps (all reproducible; times modelled):
  1. tools/groups_table.py search  [--digits 3.71e13 3.79e13] [--nmin 544 --nmax 576] --out DIR   -> DIR/cand_<digits>.csv (every feasible schedule,
     sorted by modelled wall) and DIR/plan_cmds.txt (one `MN_PLAN_ONLY=<digits>:<N> MN_GROUPS=<g> ./ecalc` line per top candidate)
  2. on a node with the host binary:  tools/groups_table.py plancheck --cmds DIR/plan_cmds.txt --ecalc ecalc/ecalc --out DIR/plan_results.txt
  3. tools/groups_table.py table --out DIR [--plan DIR/plan_results.txt] -> DIR/groups_table.csv, DIR/groups_table.md (best / next-best, round-down advice)
The plan check needs the C binary; the search needs only Python 3.
"""
import argparse, os, sys, csv, itertools, subprocess, re, functools

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'ecalc'))
import mn_model as M, mem_model, estimate as E

EDGE = M.TARGET_DEVICE_EDGE_GB                                       # 373.44 GB (measured on the target)
X2_GB, RANK0_GB, RULE_GB = 0.5, 2.1, 10.0
# L4 (2026-10-11): GT_PROFILE selects the fabric profile (target = T06's layv factor 0.655, as c09; target_l4 = the L4 refit's factor); GT_RELEASE_GB is the
# device memory the always-on v-slot release returns per node (the user adopted it 2026-10-11: 8.59 GB, results/C06.md) -- subtracted from the device peak; 0 = off (c09's table)
PROFILE = os.environ.get('GT_PROFILE', 'target')
RELEASE_GB = float(os.environ.get('GT_RELEASE_GB', '0'))
LAYV = os.environ.get('GT_LAYV')                                       # L4: override the profile's layv factor (sensitivity)

def setup(profile=None):
    prof = M.apply_profile(profile or PROFILE)
    design = M.Design(np=3, strategy='auto', cap=mem_model.CAPS['2^31'], chunk='both', depth=2, modmul=1, chunk_mb=1024, p15=True, out_overlap='none',
                      p15b=True, np_mn='auto', packed=True, p15c=True, arena_room=0.16, cache_fit=True)
    design.t_mb = 2048                                               # MN_T_CHUNK_MB=2048, the shift chunk stays 1024
    orig = M.Design.at_g
    def at_g(self, g):                                               # at_g rebuilds the design and would drop t_mb
        d = orig(self, g); d.t_mb = self.t_mb; return d
    M.Design.at_g = at_g
    M.TWREC_G = True; M.CACHE_FORCE = None
    fab = M.Fabric(M.TARGET.name, prof.bw, prof.lat, group=64, layers=2, taper=1.0, write_bw=prof.write_bw, staging_bw=None, fall_off=0.0, v_factor=float(LAYV) if LAYV else prof.v_factor)
    return fab, design

def chains(N, extra=3, maxratio=12, maxpow=256):
    """candidate MN_GROUPS lists for N nodes (before the final N, which may be an uneven last group): the powers of two 2..2^a (a = 0..8, <= maxpow,
    all below N) followed by 0..`extra` further levels, each a multiple (ratio 2..maxratio) of the previous, the top one at least N/maxratio.
    Every level but the last is a proper divisor chain; the last level N is clipped (uneven) when it is not a multiple of the level below."""
    out = set()
    def rec(c, left):
        if (not c or N / c[-1] <= maxratio): out.add(tuple(c))
        if left == 0: return
        for r in range(2, maxratio + 1):
            v = c[-1] * r
            if v >= N: break
            rec(c + [v], left - 1)
    pw = []; v = 2
    while v <= maxpow and v < N: pw.append(v); v *= 2
    for a in range(0, len(pw) + 1): rec(pw[:a], extra if a else extra + 1) if a else rec_empty(N, extra, maxratio, out)
    return sorted(out)

def rec_empty(N, extra, maxratio, out):
    """no power-of-two prefix: start from any v1 in 2..maxratio"""
    def rec(c, left):
        if N / c[-1] <= maxratio: out.add(tuple(c))
        if left == 0: return
        for r in range(2, maxratio + 1):
            v = c[-1] * r
            if v >= N: break
            rec(c + [v], left - 1)
    for v1 in range(2, maxratio + 1):
        if v1 < N: rec([v1], extra)

def candidates(N):
    """the search space: one extra level beyond the power-of-two prefix with ratio <= 12, or two with ratio <= 8"""
    return sorted(set(chains(N, 1, 12)) | set(chains(N, 2, 8)))

def default_sched(N): return tuple(mem_model.mn_groups(N))

def top_ratio(N, groups):
    """largest / smallest child of the top level (the groups of the level below, the last one clipped)"""
    if not groups or len(groups) < 2: return 1.0
    P = groups[-2]; ch = [min(P, N - k * P) for k in range((N + P - 1) // P)]
    return max(ch) / min(ch)

def evaluate(fab, design, N, T, groups):
    try: e = E.estimate(N, T / N, 'grid', None if groups is None else ','.join(map(str, groups)), fab, 'model', design=design)
    except (ValueError, ZeroDivisionError, KeyError, AssertionError): return None
    dev = e['device_gb'] - RELEASE_GB; peak = dev + X2_GB + RANK0_GB
    return dict(N=N, T=T, groups=','.join(map(str, groups)) if groups else 'default', wall=e['nowrite_s'], device=dev, peak=peak, margin=EDGE - peak,
                topratio=top_ratio(N, groups or default_sched(N)), node=e['node_gb'], pieces=e['pieces'], fits=e['node_gb'] <= M.NODE_GB_MARGIN)

def work(args):
    T, N, keep = args; fab, design = _CTX
    cands = [None] + [c + (N,) for c in candidates(N)]
    seen = set(); best = []; dflt = default_sched(N)
    for c in cands:
        key = dflt if c is None else c
        if key in seen: continue
        seen.add(key)
        r = evaluate(fab, design, N, T, c)
        if r is None: continue
        r['default'] = key == dflt
        best.append(r)
    ok = sorted([r for r in best if r['margin'] >= RULE_GB and r['fits']], key=lambda r: r['wall'])
    d = [r for r in best if r['default']]
    for r in best: r['mem_ok'] = int(r['margin'] >= RULE_GB and r['fits'])
    nomem = sorted([r for r in best if not r['mem_ok']], key=lambda r: r['wall'])[:2] if not ok else []
    bal = [r for r in ok if r['topratio'] <= 4.0][:2]
    rows = nomem + ok[:keep] + [r for r in bal if r not in ok[:keep]] + ([d[0]] if d and d[0] not in ok[:keep] and d[0] not in nomem else [])
    msg = 'T %.3g N %d: %d candidates, %d pass memory; best %s %.1f s (default %s)' % (T, N, len(best), len(ok), ok[0]['groups'] if ok else '-', ok[0]['wall'] if ok else 0, '%.1f s' % d[0]['wall'] if d else '-')
    return T, N, rows, ok[:keep], msg

def search(a):
    import multiprocessing as mp
    global _CTX
    os.makedirs(a.out, exist_ok=True); _CTX = setup(); cmds = []
    jobs = [(T, N, a.keep) for T in a.digits for N in range(a.nmax, a.nmin - 1, -1)]
    with mp.Pool(a.jobs) as pool: res = pool.map(work, jobs, chunksize=1)     # fork: _CTX is inherited
    for T in a.digits:
        rows = []
        for T2, N, r, top, msg in res:
            if T2 != T: continue
            print(msg); rows += r
            for x in top + [q for q in r if q not in top and not q['default']]: cmds.append('MN_PLAN_ONLY=%d:%d MN_GROUPS=%s' % (round(T), N, '' if x['default'] else x['groups']))
        with open(os.path.join(a.out, 'cand_%.3g_%d-%d.csv' % (T, a.nmin, a.nmax)), 'w', newline='') as f:
            w = csv.writer(f); w.writerow(['N', 'digits', 'groups', 'wall_s', 'device_gb', 'peak_gb', 'margin_gb', 'node_gb', 'pieces', 'default', 'top_ratio', 'mem_ok'])
            for r in rows: w.writerow([r['N'], '%.3g' % r['T'], r['groups'], '%.1f' % r['wall'], '%.1f' % r['device'], '%.1f' % r['peak'], '%.1f' % r['margin'], '%.1f' % r['node'], r['pieces'], int(r['default']), '%.1f' % r['topratio'], r['mem_ok']])
    with open(os.path.join(a.out, 'plan_cmds.txt'), 'w') as f: f.write('\n'.join(sorted(set(cmds), key=lambda s: (int(re.search(r':(\d+)', s).group(1)), s))) + '\n')

def maxwork(args):
    N, = args; fab, design = _CTX; out = {}
    for name, g in (('default', None), ('b04form', tuple([2 ** i for i in range(1, 7) if 2 ** i < N][:6]) + ((192,) if N > 192 else ()) + (N,))):
        best = None
        for k in range(0, 60):                                    # 3.85e13 down to 3.26e13 in 0.01e13 steps
            T = (3.85 - 0.01 * k) * 1e13
            r = evaluate(fab, design, N, T, g)
            if r and r['margin'] >= RULE_GB and r['fits']: best = (T, r['margin'], r['wall']); break
        out[name] = best
    return N, out

def maxdigits(a):
    import multiprocessing as mp
    global _CTX
    _CTX = setup()
    with mp.Pool(a.jobs) as pool: res = pool.map(maxwork, [(N,) for N in range(a.nmax, a.nmin - 1, -1)], chunksize=1)
    with open(a.out, 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['N', 'max_digits_default', 'margin_default_gb', 'wall_default_s', 'max_digits_b04form', 'margin_b04form_gb', 'wall_b04form_s'])
        for N, o in res:
            row = [N]
            for k in ('default', 'b04form'): row += ['%.3g' % o[k][0], '%.1f' % o[k][1], '%.1f' % o[k][2]] if o[k] else ['-', '-', '-']
            w.writerow(row)

def plancheck(a):
    from multiprocessing.pool import ThreadPool
    def one(line):
        env = dict(os.environ); line = line.strip()
        for t in line.split():
            k, v = t.split('=', 1); env[k] = v
        if env.get('MN_GROUPS') == '': del env['MN_GROUPS']
        env.update(dict(x.split('=', 1) for x in a.env))
        try: p = subprocess.run([a.ecalc], env=env, capture_output=True, text=True, timeout=300, stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired: return '%s\tTIMEOUT' % line
        txt = p.stdout + p.stderr
        m = re.search(r'plan (check [^\n]*?: OK|REFUSED[^\n]*|check[^\n]*)', txt)
        ok = bool(m) and ': OK' in m.group(0)
        return '%s\t%s\t%s' % (line, 'OK' if ok else 'FAIL', (m.group(0) if m else 'no plan line rc=%d' % p.returncode)[:100])
    lines = [l for l in open(a.cmds).read().split('\n') if l.strip()]
    res = ThreadPool(a.jobs).map(one, lines)
    open(a.out, 'w').write('\n'.join(res) + '\n'); print('%d plan checks, %d OK' % (len(res), sum('\tOK\t' in r for r in res)))

def table(a):
    plan = {}
    if a.plan and os.path.exists(a.plan):
        for line in open(a.plan):
            f = line.rstrip('\n').split('\t')
            if len(f) >= 2:
                m = re.match(r'MN_PLAN_ONLY=(\d+):(\d+)(?: MN_GROUPS=(\S*))?', f[0]); plan[(int(m.group(1)), int(m.group(2)), m.group(3) or 'default')] = f[1]
    maxd = {}
    mf = os.path.join(a.out, 'maxdigits.csv')
    if os.path.exists(mf):
        for r in csv.DictReader(open(mf)): maxd[int(r['N'])] = r
    data = {}
    for fn in sorted(os.listdir(a.out)):
        if not fn.startswith('cand_'): continue
        for r in csv.DictReader(open(os.path.join(a.out, fn))):
            r['N'] = int(r['N']); r['wall'] = float(r['wall_s']); r['T'] = float(r['digits']); r['mem_ok'] = int(r['mem_ok'])
            r['plan'] = plan.get((round(r['T']), r['N'], r['groups']), 'not run' if plan else 'n/a')
            data.setdefault(r['T'], []).append(r)
    res = {}
    for T, rows in data.items():
        for N in {r['N'] for r in rows}:
            rr = [r for r in rows if r['N'] == N]
            good = sorted([r for r in rr if r['mem_ok'] and r['plan'] in ('OK', 'n/a')], key=lambda r: r['wall'])
            bal = [r for r in good if float(r['top_ratio']) <= 4.0]
            res[(T, N)] = dict(good=good, bal=bal[0] if bal else None, default=next((r for r in rr if int(r['default'])), None),
                               nomem=sorted([r for r in rr if not r['mem_ok']], key=lambda r: r['wall'])[:1])
    fab, design = setup(); std = {}
    for (T, N) in res:                                                # the standing form 2,4,8,16,32,64,192,N (b04 / the 576 line) as the conservative reference
        c = tuple(v for v in (2, 4, 8, 16, 32, 64, 192) if v < N) + (N,)
        r = evaluate(fab, design, N, T, c); std[(T, N)] = r
    with open(os.path.join(a.out, 'groups_table.csv'), 'w', newline='') as f, open(os.path.join(a.out, 'groups_table.md'), 'w') as m:
        w = csv.writer(f); w.writerow(['digits', 'N', 'best_MN_GROUPS', 'wall_s', 'margin_gb', 'plan_check', 'next_MN_GROUPS', 'next_wall_s', 'balanced_MN_GROUPS', 'balanced_wall_s',
                                       'default_wall_s', 'std_form_wall_s', 'std_form_margin_gb', 'round_down_to', 'round_down_wall_s', 'advice', 'max_digits_at_N_default_sched'])
        for T in sorted(data):
            Ns = sorted({N for (T2, N) in res if T2 == T}, reverse=True)
            m.write('\n### %.3g digits (modelled, no part-file write)\n\n| N | best MN_GROUPS | wall s | margin GB | plan | next-best (wall s) | balanced-top best (wall s) | default (wall s) | std form 2,..,64,192,N (wall s / margin GB) | advice |\n|---|---|---|---|---|---|---|---|---|---|\n' % T)
            for N in Ns:
                d = res[(T, N)]; g = d['good']; de = d['default']
                if not g:
                    nm = d['nomem'][0] if d['nomem'] else None
                    adv = 'no schedule keeps the 10 GB margin at this size; fastest ignoring memory: %s (%.1f s, margin %s GB)' % ((nm['groups'], nm['wall'], nm['margin_gb']) if nm else ('-', 0, '-'))
                    mx = maxd.get(N)
                    if mx and mx['max_digits_default'] != '-': adv += '; largest size with margin >= 10 GB at N: %s (default sched)' % mx['max_digits_default']
                    cand = [(n2, res[(T, n2)]['good'][0]['wall']) for n2 in Ns if n2 > N and res[(T, n2)]['good']]
                    if cand: n2, w2 = min(cand, key=lambda x: x[1]); adv += '; use %d instead (%.1f s)' % (n2, w2)
                    w.writerow(['%.3g' % T, N, 'INFEASIBLE', '', '', '', '', '', '', '', de['wall_s'] if de else '', '%.1f' % std[(T, N)]['wall'], '%.1f' % std[(T, N)]['margin'], '', '', adv, mx['max_digits_default'] if mx else ''])
                    m.write('| %d | infeasible (memory) | - | - | - | - | - | %s | %.1f / %.1f | %s |\n' % (N, de['wall_s'] if de else '-', std[(T, N)]['wall'], std[(T, N)]['margin'], adv)); continue
                b = g[0]; nx = g[1] if len(g) > 1 else None; ba = d['bal']
                rd = min(((n2, res[(T, n2)]['good'][0]['wall']) for n2 in Ns if n2 < N and res[(T, n2)]['good']), key=lambda x: x[1], default=None)
                adv = ''
                if rd and rd[1] < b['wall'] - 0.05: adv = 'round down to %d (%.1f s, -%.1f s)' % (rd[0], rd[1], b['wall'] - rd[1])
                mx = maxd.get(N)
                w.writerow(['%.3g' % T, N, b['groups'], '%.1f' % b['wall'], b['margin_gb'], b['plan'], nx['groups'] if nx else '', nx['wall_s'] if nx else '', ba['groups'] if ba else '', ba['wall_s'] if ba else '',
                            de['wall_s'] if de else '', '%.1f' % std[(T, N)]['wall'], '%.1f' % std[(T, N)]['margin'], rd[0] if adv else '', '%.1f' % rd[1] if adv else '', adv, mx['max_digits_default'] if mx else ''])
                m.write('| %d | `%s` | %.1f | %s | %s | %s | %s | %s | %.1f / %.1f | %s |\n' % (N, b['groups'], b['wall'], b['margin_gb'], b['plan'], ('`%s` (%s)' % (nx['groups'], nx['wall_s'])) if nx else '-',
                        ('`%s` (%s)' % (ba['groups'], ba['wall_s'])) if ba else '-', de['wall_s'] if de else '-', std[(T, N)]['wall'], std[(T, N)]['margin'], adv or '-'))
    print(open(os.path.join(a.out, 'groups_table.md')).read())

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('search'); s.add_argument('--digits', type=float, nargs='+', default=[3.71e13, 3.79e13]); s.add_argument('--nmin', type=int, default=544)
    s.add_argument('--nmax', type=int, default=576); s.add_argument('--keep', type=int, default=5); s.add_argument('--out', default='c09_out'); s.add_argument('--jobs', type=int, default=4); s.set_defaults(f=search)
    p = sub.add_parser('plancheck'); p.add_argument('--cmds', required=True); p.add_argument('--ecalc', default='ecalc/ecalc'); p.add_argument('--jobs', type=int, default=8); p.add_argument('--out', required=True)
    p.add_argument('--env', nargs='*', default=[], help='extra VAR=value for every plan call (the target line: ECALC_NP=auto MN_T_CHUNK_MB=2048 ...)'); p.set_defaults(f=plancheck)
    m = sub.add_parser('maxdigits'); m.add_argument('--nmin', type=int, default=544); m.add_argument('--nmax', type=int, default=576); m.add_argument('--jobs', type=int, default=4); m.add_argument('--out', default='maxdigits.csv'); m.set_defaults(f=maxdigits)
    t = sub.add_parser('table'); t.add_argument('--out', default='c09_out'); t.add_argument('--plan', default=None); t.set_defaults(f=table)
    a = ap.parse_args(); a.f(a)

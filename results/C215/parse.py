#!/usr/bin/env python3
"""C215: the run logs (~/C215/logs/<tag>.log|.sha, copied here) -> one table row per run, and the series statistics.
   usage: parse.py <logdir>"""
import sys, os, re, glob, statistics as st, math

REF = {'100000000000': '578f5efb0ff2b9af6b681a375c9ff39197f55cb7', '40000000000': '43c84e49678df6d9010028b207c6a1b57f1a89d3'}

def num(pat, s, g=1, f=float):
    m = re.search(pat, s, re.M); return f(m.group(g)) if m else None

def parse(path):
    s = open(path, errors='replace').read(); tag = os.path.basename(path)[:-4]
    r = dict(tag=tag)
    r['node'] = (s.splitlines() or [''])[0].strip()
    r['total'] = num(r'^total\s+([\d.]+) s', s); r['init'] = num(r'init ([\d.]+); other', s)
    r['bs'] = num(r'^bs\s+([\d.]+) s', s); r['dm'] = num(r'^dm\s+([\d.]+) s', s); r['recip'] = num(r'recip ([\d.]+) s; corr', s)
    m = re.search(r'corrections (\d+)/(\d+)', s); r['corr'] = '%s/%s' % m.groups() if m else '?'
    r['ncorr'] = sum(map(int, m.groups())) if m else None
    r['T1'] = num(r'^T1\s+([\d.]+) s', s); r['dc'] = num(r'^dc\s+([\d.]+) s', s)
    r['wait'] = num(r'waiting for the writer ([\d.]+)\)', s)
    r['redo'] = 'redoing the digits' in s
    r['wall'] = num(r'^C2WALL ([\d.]+) rc', s); r['rc'] = num(r'^C2WALL [\d.]+ rc (\d+)', s, f=int)
    r['verify'] = 'VERIFY OK' in s and 'FAILED' not in s
    r['dist_db'] = len(re.findall(r'^\s+dist_db ', s, re.M))
    r['C'] = len(re.findall(r'C form|\(C\)', s))
    r['sha'] = None; r['hash_s'] = None
    if os.path.exists(path[:-4] + '.sha'):
        t = open(path[:-4] + '.sha').read()
        r['sha'] = num(r'^([0-9a-f]{40})\s+-', t, f=str); r['hash_s'] = num(r'^hash (\d+) s', t)
    return r

def ms(xs):
    xs = [x for x in xs if x is not None]
    if not xs: return (float('nan'), float('nan'), 0)
    return (st.mean(xs), st.stdev(xs) if len(xs) > 1 else float('nan'), len(xs))

def diff(a, b):
    """mean(b) - mean(a) and its standard error (Welch)"""
    (ma, sa, na), (mb, sb, nb) = ms(a), ms(b)
    se = math.sqrt((sa ** 2 / na if na > 1 else 0) + (sb ** 2 / nb if nb > 1 else 0))
    return mb - ma, se

if __name__ == '__main__':
    d = sys.argv[1]; rows = [parse(p) for p in sorted(glob.glob(os.path.join(d, '*.log')), key=os.path.getmtime) if not os.path.basename(p).startswith('node_')]
    print('| run | node | total | init | bs | dm (recip) | corrections | T1 | dc (writer wait) | wall | sha1 | digits |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|')
    for r in rows:
        dg = '100000000000' if r['tag'][:2] in ('s_', 'n_') else '40000000000' if r['tag'].startswith('z4_') else None
        ok = ('identical' if r['sha'] == REF.get(dg) else 'DIFFERS') if r['sha'] and dg else ('-' if not r['sha'] else 'see pair')
        f = lambda x, p=2: '-' if x is None else ('%.*f' % (p, x))
        print('| %s | %s | %s | %s | %s | %s (%s) | %s%s | %s | %s (%s) | %s | %s | %s%s |' % (r['tag'], r['node'].replace('ppac-pl1-', ''), f(r['total']), f(r['init'], 1), f(r['bs'], 1), f(r['dm'], 1), f(r['recip'], 1),
              r['corr'], ' redo' if r['redo'] else '', f(r['T1']), f(r['dc'], 1), f(r['wait'], 1), f(r['wall'], 1), (r['sha'] or '-')[:8], ok, '' if r['verify'] and r['rc'] == 0 else ' **rc %s / VERIFY?**' % r['rc']))
    by = lambda pre: [r for r in rows if r['tag'].startswith(pre)]
    print()
    for key in ('total', 'wall', 'bs', 'dm', 'recip', 'T1', 'dc'):
        line = []
        for pre in ('s_off', 's_on', 's_tw', 'n_off', 'n_on'):
            m, s, n = ms([r[key] for r in by(pre)])
            line.append('%s %.2f ± %.2f (n %d)' % (pre, m, s, n))
        dd, se = diff([r[key] for r in by('s_off')], [r[key] for r in by('s_on')])
        dt, set_ = diff([r[key] for r in by('s_on')], [r[key] for r in by('s_tw')])
        dn, sen = diff([r[key] for r in by('n_off')], [r[key] for r in by('n_on')])
        print('%-6s %s | on-off (file) %+.2f ± %.2f | tw-on %+.2f ± %.2f | on-off (no file) %+.2f ± %.2f' % (key, '; '.join(line), dd, se, dt, set_, dn, sen))
    # the correction effect: C2 off runs split by their correction count
    print()
    for pre in ('s_off', 's_on', 'n_off', 'n_on', 's_tw'):
        g = {}
        for r in by(pre): g.setdefault(r['ncorr'], []).append(r)
        for k, rs in sorted(g.items(), key=lambda x: (x[0] is None, x[0])):
            print('%-6s corrections %s: n %d, total %.2f, dm %.2f, T1 %.2f, wall %.1f' % (pre, k, len(rs), *[ms([r[x] for r in rs])[0] for x in ('total', 'dm', 'T1', 'wall')]))

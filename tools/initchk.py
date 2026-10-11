#!/usr/bin/env python3
"""initchk.py -- slow-host check from a multi-node ecalc run's own log (no cluster access, no dependencies).

Policy (adopted 2026-10-11): flag a node whose first-run init time is > THR x (default 3) the allocation median.
Reads ONE log (srun --label output as made by mnrun.sh MNRUN_LABEL=1: every line "<rank>: text") or a DIRECTORY of
per-node logs (one file = one node).  Per node it extracts, when printed:
  rns_init   "tl <t> peer access done"  (ECALC_INIT_TL=1: seconds from "rns_init begins" to the end of rns_init)
             fallback "init: rns_init <t> s" (ECALC_VERBOSE>=2)
  map        longest "APU<d> arena: N chunks mapped in <t> s" (arena mapping; ECALC_INIT_TL=1)
  region     "init: region pools <t> s"
  transport  "the transport opened before rns_init (<t> s)"
  upload     "recip ts2 node N host H ... db_from_bi(upload) +<t>"  (NEWTON_MN_RECIP_TS=2; host-attributed, the
             stage where the slow host aac7-n01 stalled 20-25 s; not part of init proper)
Ratio = value / median over nodes, per metric.  A node is FLAGged when the ratio of rns_init (or, if absent, init total)
exceeds THR, or of upload/map/region/transport, and the excess over the median is at least --min-excess seconds.
Without rank labels (plain interleaved log) the per-node values are listed anonymously (node ?1..?n, sorted) and a flag
names the metric and value but not the host; use MNRUN_LABEL=1 to get host names.
Exit codes: 0 no flag, 3 flagged, 2 could not parse (no per-node init data, or unreadable input).
"""
import os, re, sys, argparse, statistics

LAB = re.compile(r'^(\d+): ?(.*)$')
RE_TL_PEER = re.compile(r'^tl\s+([\d.]+)\s+peer access done')
RE_TL_REL = re.compile(r'^tl\s+([\d.]+)\s+the background mapping is released')
RE_TL_BEGIN = re.compile(r'^tl\s+([\d.]+)\s+rns_init begins')
RE_MAP = re.compile(r'^tl\s+[\d.]+\s+APU\d+ arena: \d+ chunks mapped in ([\d.]+) s')
RE_REGION = re.compile(r'init: region pools ([\d.]+) s')
RE_TRANS = re.compile(r'the transport opened before rns_init \(([\d.]+) s\)')
RE_RNSI = re.compile(r'init: rns_init ([\d.]+) s, region pools')
RE_RES = re.compile(r'^RESULT ecalc init s ([\d.]+)')
RE_UP = re.compile(r'^recip ts2 node (\d+) host (\S+):.*db_from_bi\(upload\) \+([\d.]+)')
RE_ENV = re.compile(r'aac7env: node (\S+) task (\d+) of')
RE_META = re.compile(r'META .*host=(\S+)')
METRICS = ['rns_init', 'map', 'region', 'transport', 'upload']


class Node:
    def __init__(self, key):
        self.key = key; self.host = None; self.v = {m: None for m in METRICS}; self.total = None

    def put(self, m, x, mode):
        if self.v[m] is None or (mode == 'max' and x > self.v[m]):
            self.v[m] = x


def feed(n, line):
    m = RE_TL_PEER.match(line)
    if m:
        n.put('rns_init', float(m.group(1)), 'first'); return
    m = RE_TL_REL.match(line)
    if m and n.v['rns_init'] is None:
        n.put('rns_init', float(m.group(1)), 'first'); return
    m = RE_MAP.match(line)
    if m:
        n.put('map', float(m.group(1)), 'max'); return
    m = RE_REGION.search(line)
    if m:
        n.put('region', float(m.group(1)), 'first')
    m = RE_TRANS.search(line)
    if m:
        n.put('transport', float(m.group(1)), 'first')
    m = RE_RNSI.search(line)
    if m:
        n.total = float(m.group(1))
    m = RE_UP.match(line)
    if m:
        n.put('upload', float(m.group(3)), 'first'); n.host = m.group(2)


def parse_file(path, nodes, labelled_hint=None):
    """Returns True if rank labels were used."""
    with open(path, errors='replace') as f:
        lines = f.read().splitlines()
    nlab = sum(1 for l in lines[:4000] if LAB.match(l))
    labelled = nlab > 50
    env_host = {}
    if labelled:
        for l in lines:
            m = LAB.match(l)
            if not m:
                continue
            r, t = int(m.group(1)), m.group(2)
            n = nodes.setdefault(r, Node(r))
            mm = RE_ENV.search(t)
            if mm and int(mm.group(2)) == r:
                n.host = n.host or mm.group(1)
            mm = RE_META.search(t)
            if mm and r == 0 and not n.host:
                n.host = mm.group(1)
            feed(n, t)
        return True
    # unlabelled: per-node values are anonymous; collect repeated metrics as lists
    return False


def parse_unlabelled(path):
    """Interleaved log without labels: each node prints its own peer-access/region/transport line once."""
    lists = {m: [] for m in METRICS}
    totals = []
    host_of_upload = []
    envs = {}
    with open(path, errors='replace') as f:
        for l in f:
            l = l.rstrip('\n')
            mm = RE_ENV.search(l)
            if mm:
                envs[int(mm.group(2))] = mm.group(1)
            for rx, m in ((RE_TL_PEER, 'rns_init'),):
                x = rx.match(l)
                if x:
                    lists[m].append(float(x.group(1)))
            x = RE_MAP.match(l)
            if x:
                lists['map'].append(float(x.group(1)))
            x = RE_REGION.search(l)
            if x:
                lists['region'].append(float(x.group(1)))
            x = RE_TRANS.search(l)
            if x:
                lists['transport'].append(float(x.group(1)))
            x = RE_RNSI.search(l)
            if x:
                totals.append(float(x.group(1)))
            x = RE_UP.match(l)
            if x:
                lists['upload'].append(float(x.group(3))); host_of_upload.append((float(x.group(3)), x.group(2)))
    if not lists['rns_init'] and totals:
        lists['rns_init'] = totals
    return lists, host_of_upload, envs


def med(xs):
    return statistics.median(xs) if xs else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('path', help='multi-node log (MNRUN_LABEL=1) or directory of per-node logs')
    ap.add_argument('--threshold', type=float, default=3.0, help='flag when value > THR x median (default 3)')
    ap.add_argument('--min-excess', type=float, default=2.0, help='and value - median >= this many seconds (default 2)')
    ap.add_argument('--quiet', action='store_true', help='print FLAG lines and the verdict only')
    a = ap.parse_args()
    p = a.path
    if not os.path.exists(p):
        print('initchk: cannot read', p); return 2
    rows = []   # (name, host, {metric: value})
    note = ''
    anon = False
    try:
        if os.path.isdir(p):
            files = sorted(f for f in os.listdir(p) if os.path.isfile(os.path.join(p, f)))
            for f in files:
                n = Node(f)
                with open(os.path.join(p, f), errors='replace') as fh:
                    for l in fh:
                        l = l.rstrip('\n'); m = LAB.match(l)
                        if m: l = m.group(2)
                        mm = RE_ENV.search(l)
                        if mm and not n.host: n.host = mm.group(1)
                        mm = RE_META.search(l)
                        if mm and not n.host: n.host = mm.group(1)
                        feed(n, l)
                if any(x is not None for x in n.v.values()) or n.total is not None:
                    if n.v['rns_init'] is None and n.total is not None:
                        n.v['rns_init'] = n.total
                    rows.append((f, n.host or f, dict(n.v)))
        else:
            nodes = {}
            if parse_file(p, nodes):
                for r in sorted(nodes):
                    n = nodes[r]
                    if n.v['rns_init'] is None and n.total is not None:
                        n.v['rns_init'] = n.total
                    if any(x is not None for x in n.v.values()):
                        rows.append(('rank%d' % r, n.host or 'rank%d' % r, dict(n.v)))
            else:
                lists, ups, envs = parse_unlabelled(p)
                note = 'log has no rank labels: each column is sorted on its own, so a row is NOT one node; only upload values carry a host name (use MNRUN_LABEL=1 for names on every metric)'
                anon = True
                lists['map'] = []   # per-APU lines, not per node: not usable without labels
                k = max((len(v) for v in lists.values()), default=0)
                for m in lists:
                    lists[m] = sorted(lists[m])
                uph = [h for _, h in sorted(ups)]
                for i in range(k):
                    row = {m: (lists[m][i] if i < len(lists[m]) else None) for m in METRICS}
                    rows.append(('?%d' % (i + 1), uph[i] if i < len(uph) else '?', row))
    except OSError as e:
        print('initchk: cannot read', e); return 2
    if not rows or not any(r[2]['rns_init'] is not None for r in rows):
        # upload-only data is still a usable host check
        if not any(r[2]['upload'] is not None for r in rows):
            print('initchk: no per-node init data found in %s (need ECALC_INIT_TL=1 or ECALC_VERBOSE>=2 output)' % p)
            return 2
    # upload lines are host-attributed: for labelled/anon logs rebuild their rows by host
    meds = {m: med([r[2][m] for r in rows if r[2][m] is not None]) for m in METRICS}
    cnt = {m: sum(1 for r in rows if r[2][m] is not None) for m in METRICS}
    flags = []
    out = []
    hdr = '%-10s %-14s %9s %8s %8s %9s %8s %7s' % ('node', 'host', 'rns_init', 'map', 'region', 'transport', 'upload', 'ratio')
    out.append(hdr)

    def f(x):
        return '-' if x is None else '%.2f' % x
    for name, host, v in rows:
        worst = 0.0; wm = None
        for m in METRICS:
            if v[m] is None or not meds[m] or cnt[m] < 3:
                continue
            ratio = v[m] / meds[m]
            if ratio > a.threshold and v[m] - meds[m] >= a.min_excess:
                flags.append((name, host if (not anon or m == 'upload') else '?', m, v[m], meds[m], ratio))
            # headline ratio: rns_init if present, else the worst metric
            if m == 'rns_init' or (v['rns_init'] is None and ratio > worst):
                worst = ratio; wm = m
        out.append('%-10s %-14s %9s %8s %8s %9s %8s %7s' % (name, host[:14], f(v['rns_init']), f(v['map']), f(v['region']),
                   f(v['transport']), f(v['upload']), ('%.2f' % worst + ('' if wm in (None, 'rns_init') else '(' + wm + ')')) if wm else '-'))
    out.append('%-10s %-14s %9s %8s %8s %9s %8s' % ('median', '', f(meds['rns_init']), f(meds['map']), f(meds['region']),
               f(meds['transport']), f(meds['upload'])))
    if not a.quiet:
        print('initchk: %s (threshold %.1fx median, min excess %.1f s, nodes with rns_init: %d)' % (p, a.threshold, a.min_excess, cnt['rns_init']))
        if note: print('initchk: NOTE', note)
        print('\n'.join(out))
        for m in METRICS:
            if 0 < cnt[m] < 3:
                print('initchk: NOTE only %d node(s) report %s: no median, no flag possible for it' % (cnt[m], m))
    for name, host, m, x, md, ratio in flags:
        print('FLAG initchk: %s host %s: %s %.2f s = %.1fx the median %.2f s (> %.1fx)' % (name, host, m, x, ratio, md, a.threshold))
    if flags:
        print('initchk: FLAGGED %d (exit 3): policy -> re-run this allocation without the flagged host(s) (-x / MNRUN_EXCLUDE_HOSTS)' % len(flags))
        return 3
    print('initchk: OK, no node above %.1fx the median' % a.threshold)
    return 0


if __name__ == '__main__':
    sys.exit(main())

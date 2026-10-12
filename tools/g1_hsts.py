#!/usr/bin/env python3
"""g1_hsts.py LOG...: analyse COMM_HS_TS=1 'hs_ts' lines (absolute CLOCK_REALTIME us) of the reciprocal's gather.
Per run: per (comm, seq) exchange, which PE enters/publishes last, which signals arrive late, by how much."""
import re, sys, collections
R = re.compile(r'hs_ts pe (\d+) c (\S+) n (\d+) seq (\d+) (\S+) (\S+) peer (-?\d+) t_us (\d+)')
N = re.compile(r'hs_ts node (\d+) host (\S+): gather window opens t_us (\d+)')
W = re.compile(r'hs_ts node (\d+): gather window closes t_us (\d+)')
for fn in sys.argv[1:]:
    ex = collections.defaultdict(lambda: collections.defaultdict(list))   # (pe-of-comm key) -> per pe events
    host = {}; opens = {}; closes = {}
    for ln in open(fn, errors='replace'):
        m = N.search(ln)
        if m: host[int(m[1])] = m[2]; opens[int(m[1])] = int(m[3]); continue
        m = W.search(ln)
        if m: closes[int(m[1])] = int(m[2]); continue
        m = R.search(ln)
        if m:
            pe, c, n, seq, what, ev, peer, t = int(m[1]), m[2], int(m[3]), int(m[4]), m[5], m[6], int(m[7]), int(m[8])
            ex[(c, seq, n)][pe].append((ev, peer, t))
    print('== %s: nodes %s' % (fn, ' '.join('%d:%s' % (k, host[k]) for k in sorted(host))))
    if opens:
        t0 = min(opens.values())
        print('  window open (ms after the first node): ' + ' '.join('node%d +%.1f' % (k, (opens[k] - t0) / 1e3) for k in sorted(opens)))
        print('  window length (s): ' + ' '.join('node%d %.3f' % (k, (closes.get(k, 0) - opens[k]) / 1e6) for k in sorted(opens)))
    # exchanges are keyed by comm pointer (private to a PE), so group by (seq, n) over all pes
    grp = collections.defaultdict(dict)
    for (c, seq, n), d in ex.items():
        for pe, evs in d.items(): grp[(seq, n)][(pe, c)] = evs
    for (seq, n), d in sorted(grp.items()):
        first = {}; pub = {}; roff = {}; put = {}; sig = {}; exit_ = {}
        for (pe, c), evs in d.items():
            for ev, peer, t in evs:
                if ev == 'entry': first[pe] = t
                elif ev == 'pub1': pub[pe] = t
                elif ev == 'roff_ok': roff[(pe, peer)] = t
                elif ev == 'put_done': put[(pe, peer)] = t
                elif ev == 'sig_ok': sig[(pe, peer)] = t
                elif ev == 'exit': exit_[pe] = t
        if len(first) < 2: continue
        t0 = min(first.values())
        dur = {pe: exit_[pe] - first[pe] for pe in exit_ if pe in first}
        print(' exchange seq %d n %d (PEs %d): entry spread %.1f ms, latest entry PE %d (+%.1f ms); max duration %.1f ms (PE %d), min %.1f ms' % (
            seq, n, len(first), (max(first.values()) - t0) / 1e3, max(first, key=first.get), (max(first.values()) - t0) / 1e3,
            max(dur.values(), default=0) / 1e3, max(dur, key=dur.get) if dur else -1, min(dur.values(), default=0) / 1e3))
        if dur and max(dur.values()) > 200000:   # a stalled exchange (> 0.2 s): detail
            lastpub = max(pub, key=pub.get) if pub else -1
            print('   STALL: last publish PE %d at +%.1f ms; latest signals (sig_ok minus the sender\'s put_done, ms):' % (lastpub, (pub.get(lastpub, t0) - t0) / 1e3))
            rows = sorted(((sig[(r, s)] - put[(s, r)]) / 1e3, r, s) for (r, s) in sig if (s, r) in put)
            for d_, r, s in rows[-6:]: print('     receiver PE %d from sender PE %d: %.1f ms after the put' % (r, s, d_))
            rs = sorted(((roff[(s, r)] - pub.get(r, roff[(s, r)])) / 1e3, s, r) for (s, r) in roff if r in pub)
            for d_, s, r in rs[-4:]: print('     sender PE %d saw PE %d\'s offset %.1f ms after PE %d published' % (s, r, d_, r))

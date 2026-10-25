#!/bin/bash
# mem02 (TQ07) - t_nodechk on every host of the allocation (1 task per node, all at once): per-APU 1 GB upload times, MemAvailable, hugepages,
# then a cold-path pass (8 GB per APU) that reproduces ecalc's init on a fresh process.  Writes nodechk.tsv, flagged_hosts.txt (upload > 3x
# median or MemAvailable < 90 % of median), and the aggregate numbers.
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
PN=1; ecalc_have 1 || PN=2
T=$(ecalc_tool $PN tests/t_nodechk)
[ -x "$T" ] || { tgt_kv note "no t_nodechk in the build tree"; ecalc_end SKIP; }
NH=$(echo "$HOSTS" | tr ',' '\n' | grep -c .); [ "$NH" -ge 1 ] || NH=1
COLD=${MEM02_COLD_GB:-8}; [ "${TGT_SITE:-}" = aac7 ] && COLD=2
TMO=3 ecalc_srun nodechk $PN "$NH" "$T" 1 3
TMO=4 ecalc_srun nodechk_cold $PN "$NH" "$T" 1 1 "$COLD"
grep -a '^NODECHK ' raw/nodechk.log | sed 's/^NODECHK //' > nodechk.tsv
grep -a '^NODECHK_COLD ' raw/nodechk_cold.log | sed 's/^NODECHK_COLD //' > nodechk_cold.tsv
[ -s nodechk.tsv ] || { tgt_kv note "no NODECHK line"; ecalc_end FAIL; }
python3 - "$OUT" <<'PY'
import sys, re, statistics as st
out = sys.argv[1]
def rows(f):
    r = []
    try:
        for l in open(f):
            d = dict(w.split('=', 1) for w in l.split() if '=' in w)
            if 'host' in d: r.append(d)
    except OSError: pass
    return r
def f(x):
    try: return float(x)
    except Exception: return None
kv = []
def put(k, v): kv.append("%s=%s" % (k, v))
R = rows(out + '/nodechk.tsv'); C = rows(out + '/nodechk_cold.tsv')
ups = {d['host']: [f(v) for k, v in d.items() if re.fullmatch(r'apu\d+_s', k) and f(v) is not None] for d in R}
allu = [x for v in ups.values() for x in v]
med = st.median(allu) if allu else 0
av = {d['host']: (f(d.get('MemAvailable_kB')) or 0) / 1e6 for d in R}
mav = st.median(av.values()) if av else 0
flag = []
for h in ups:
    why = []
    if med and max(ups[h] or [0]) > 3 * med: why.append('upload %.3f s > 3x median %.3f s' % (max(ups[h]), med))
    if mav and av[h] < 0.9 * mav: why.append('MemAvailable %.1f GB < 90%% of median %.1f' % (av[h], mav))
    if why: flag.append(h + ' ' + '; '.join(why))
open(out + '/flagged_hosts.txt', 'w').write('\n'.join(flag) + ('\n' if flag else ''))
put('m.nodes_checked', '%d' % len(R))
if allu:
    put('m.upload_1gb_median_s', '%.4f s' % med); put('m.upload_1gb_min_s', '%.4f s' % min(allu)); put('m.upload_1gb_max_s', '%.4f s' % max(allu))
    put('m.upload_rate_median_gbs', '%.2f GB/s' % (1.0 / med if med else 0))
if av:
    put('m.memavail_median_gb', '%.1f GB' % mav); put('m.memavail_min_gb', '%.1f GB' % min(av.values())); put('m.memavail_max_gb', '%.1f GB' % max(av.values()))
hp = [f(d.get('HugePages_Total')) for d in R if f(d.get('HugePages_Total')) is not None]
if hp: put('e.hugepages_total_max', '%d' % max(hp))
put('m.nodes_flagged', '%d' % len(flag))
ct = [f(v) for d in C for k, v in d.items() if re.fullmatch(r'apu\d+_total_s', k) and f(v) is not None]
if ct:
    put('m.cold_total_median_s', '%.2f s' % st.median(ct)); put('m.cold_total_max_s', '%.2f s' % max(ct))
    for ph in ('create', 'map', 'access', 'zero', 'up'):
        x = [f(v) for d in C for k, v in d.items() if re.fullmatch(r'apu\d+_%s_s' % ph, k) and f(v) is not None]
        if x: put('m.cold_%s_median_s' % ph, '%.2f s' % st.median(x))
    tt = [f(d.get('touch_s')) for d in C if f(d.get('touch_s')) is not None]
    if tt: put('m.cold_touch_median_s', '%.2f s' % st.median(tt)); put('m.cold_touch_max_s', '%.2f s' % max(tt))
open(out + '/_mem02.kv', 'w').write('\n'.join(kv) + '\n')
print('per-APU 1 GB upload median %.3f s (%.1f GB/s), min %.3f, max %.3f over %d node(s); MemAvailable median %.0f GB; %d node(s) flagged (flagged_hosts.txt)' %
        (med, 1.0 / med if med else 0, min(allu or [0]), max(allu or [0]), len(R), mav, len(flag))))
PY
[ -f _mem02.kv ] && { while IFS= read -r l; do tgt_kv "${l%%=*}" "${l#*=}"; done < _mem02.kv; rm -f _mem02.kv; }
n=$(grep -c . flagged_hosts.txt); ms=$(grep '^m.upload_1gb_median_s=' summary.txt | cut -d= -f2); mx=$(grep '^m.upload_1gb_max_s=' summary.txt | cut -d= -f2); ma=$(grep '^m.memavail_median_gb=' summary.txt | cut -d= -f2)
ecalc_q TQ07 "per-APU 1 GB host-to-device upload median $ms, max $mx over $(wc -l < nodechk.tsv) node(s); MemAvailable median $ma; $n node(s) flagged (flagged_hosts.txt)"
ecalc_end PASS

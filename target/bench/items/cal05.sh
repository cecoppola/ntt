#!/bin/bash
# cal05 - put latency and rate by distance (TQ74): t_xshmem lat+stream on 2-node launches h0 with h1, h2, h4, h8, h15 of a 16-node allocation (index distance; the host names of every pair are in the summary for the topology fit).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
PN=2; ecalc_have 2 || PN=1; need_path $PN
HL=($(echo "$HOSTS" | tr ',' ' ')); [ "${#HL[@]}" -ge 2 ] || skip "needs 2 hosts"
for j in 1 2 4 8 15; do
    [ "$j" -lt "${#HL[@]}" ] || continue; tleft_ok 0.85 || break
    HOSTS_SUB="${HL[0]},${HL[$j]}" XT_TMO=3 xt_run "pair$j" $PN 2 "lat stream" XT_MAXMB=64 XT_REPS=3 || note "pair $j rc $?"
    xt_kv "pair$j" lat || true; xt_kv "pair$j" stream || true; tgt_kv e.pair$j.hosts "${HL[0]} ${HL[$j]}"
    R="$R d$j:lat $(awk -F'\t' '/pingpong/ { print $2; exit }' "$OUT/raw/pair$j.lat.tsv" 2>/dev/null)us/stream $(xt_peak pair$j stream)GB/s;"
done
tgt_kv q.TQ74 "pingpong us and stream peak node GB/s by index distance:$R"
OPT_NO_RUNS_OK=1; finish

#!/bin/bash
# net10 - node placement (TQ51): t_xshmem a2a on 8 adjacent hosts (first 8 of the allocation) vs 8 scattered hosts (every second of 16), one launch each; a second pass reverses the order.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
PN=2; ecalc_have 2 || PN=1; need_path $PN
[ "$(hosts_n)" -ge 16 ] || skip "needs 16 hosts"
ADJ=$(ecalc_hosts 0 8); SCA=$(echo "$HOSTS" | tr ',' '\n' | grep . | head -16 | awk 'NR % 2 == 1' | paste -sd, -)
tgt_kv e.adjacent "$ADJ"; tgt_kv e.scattered "$SCA"
for r in adj1 sca1 sca2 adj2; do
    tleft_ok 0.85 || break
    case $r in adj*) HS=$ADJ;; *) HS=$SCA;; esac
    HOSTS_SUB=$HS XT_TMO=5 xt_run "$r" $PN 8 a2a XT_MAXMB=64 XT_REPS=3 || note "$r rc $?"; xt_kv "$r" a2a || true
done
A=$(echo "$(xt_peak adj1 a2a) $(xt_peak adj2 a2a)" | awk '{ print ($1 + $2) / 2 }'); S=$(echo "$(xt_peak sca1 a2a) $(xt_peak sca2 a2a)" | awk '{ print ($1 + $2) / 2 }')
tgt_kv m.adjacent_peak_node_gbs "$A GB/s"; tgt_kv m.scattered_peak_node_gbs "$S GB/s"
tgt_kv q.TQ51 "a2a peak per node: adjacent $A GB/s, scattered $S GB/s (8 nodes, path $PN, order adj sca sca adj)"
OPT_NO_RUNS_OK=1; finish

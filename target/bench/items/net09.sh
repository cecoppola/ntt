#!/bin/bash
# net09 - slow launch mode (TQ50): 8 independent t_xshmem a2a launches on 4 nodes, same hosts; peak node GB/s per launch; a launch below 85 % of the best counts as slow (aac7: 21 % of 4-node launches hard-slow).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
PN=2; ecalc_have 2 || PN=1; need_path $PN
tool $PN tests/t_xshmem >/dev/null || skip "no t_xshmem"
N=${NET09_NODES:-4}; [ "$(hosts_n)" -ge $N ] || skip "needs $N hosts"
V=; BEST=0
for i in 1 2 3 4 5 6 7 8; do
    tleft_ok 0.85 || { note "stopped after $((i - 1)) launches"; break; }
    XT_TMO=4 xt_run "l$i" $PN $N a2a XT_MAXMB=16 XT_REPS=3; rc=$?
    xt_kv "l$i" a2a || true; P=$(xt_peak "l$i" a2a); V="$V ${P:-0}"
    tgt_kv m.launch${i}_a2a_peak_node_gbs "${P:-NA} GB/s"; tgt_kv m.launch${i}_rc "$rc"
done
read -r SLOW NN MX MN <<< "$(echo $V | awk '{ for (i = 1; i <= NF; i++) { if ($i > mx) mx = $i; if (i == 1 || $i < mn) mn = $i } for (i = 1; i <= NF; i++) if ($i < 0.85 * mx) s++; print s + 0, NF, mx, mn }')"
tgt_kv m.slow_launches "$SLOW of $NN"; tgt_kv m.best_node_gbs "$MX GB/s"; tgt_kv m.worst_node_gbs "$MN GB/s"
tgt_kv q.TQ50 "$SLOW of $NN launches slow (below 85 % of the best $MX GB/s per node; worst $MN) on $N nodes, path $PN"
OPT_NO_RUNS_OK=1; [ "$NN" -ge 1 ] && [ "$MX" != 0 ] || OPT_EXTRA_FAIL=1; finish

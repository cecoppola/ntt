#!/bin/bash
# cal03 - 2/4/8-node phase split at the per-node share (TQ72): ecalc path 2, 6.6e10 digits per node (planner-shrunk), MN_WAIT_STATS and the init timeline on, all inside one 8-node allocation.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 2
for n in 2 4 8; do
    [ "$(hosts_n)" -ge $n ] || { note "no $n hosts"; continue; }
    tleft_ok 0.75 || { note "stopped before n=$n: time budget"; break; }
    D=$(fitd $n C "$(dig 6.6e10 $n)")
    one "N$n" 1 C $n "$D" MN_WAIT_STATS=1 ECALC_INIT_TL=1
    tl_kv "$OUT/raw/n${n}_r1.log" "tl_n$n" || true; wait_kv "$OUT/raw/n${n}_r1.log" "wait_n$n" 4 || true
    ROW="$ROW n$n:${E_TOTAL:-NA}s(dm ${E_DM:-NA})"
done
tgt_kv q.TQ72 "total s by node count:$ROW; per-node share 6.6e10 unless cut by the planner (e.nN.digits)"
finish

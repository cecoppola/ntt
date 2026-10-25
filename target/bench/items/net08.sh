#!/bin/bash
# net08 - path 1 (P) vs path 2 (C) at 4, 8, 16 nodes inside one 16-node allocation, 2e9 digits per node, one run per cell (TQ40); the arms of one node count are paired by the order P C.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 1; need_path 2
for n in 4 8 16; do
    [ "$(hosts_n)" -ge $n ] || { note "no $n hosts"; continue; }
    tleft_ok 0.8 || { note "stopped before n=$n: time budget"; break; }
    D=$(fitd "$n" C "$(dig 2e9 $n)")
    ecalc_run "p$n" P "$n" "$D" || true; rec "p$n" "P$n" 1
    ecalc_run "c$n" C "$n" "$D" || true; rec "c$n" "C$n" 1
    ARMS_ONLY="P$n C$n" arms_report "P$n" total m.n$n; R="$R n$n:${AB_LINE:-none};"
done
tgt_kv q.TQ40 "C minus P total:$R"
finish

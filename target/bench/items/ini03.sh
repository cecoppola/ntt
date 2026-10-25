#!/bin/bash
# ini03 - init, path 1 (P) vs path 2 (C) at 2 nodes, ABBA (TQ36): ECALC_INIT_TL=1; the metric is the init time (aac7: path 2 +8 s).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 1; need_path 2
D=$(dig 1e10 2); i=0
for a in P C C P; do
    i=$((i + 1)); tleft_ok 0.85 || break
    ecalc_run "${a}_r$i" "$a" 2 "$D" ECALC_INIT_TL=1 || true; rec "${a,,}_r$i" "$a" "$i"; tl_kv "$OUT/raw/${a,,}_r$i.log" "tl_${a,,}$i" || true
done
arms_report P init m.init; A1=$AB_LINE; arms_report P total m.tot
tgt_kv q.TQ36 "init: $A1 (C minus P); total: $AB_LINE; 2 nodes, $D digits"
finish

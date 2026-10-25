#!/bin/bash
# scl01 (TQ20) - which path is faster at 2 nodes, 1e10: A = path 1 layout D, B = path 2 (C), ABBA twice (A B B A A B B A) on the same 2 nodes.
# Without path 2 the same order runs A only (an A/A drift control).  Digits are not hashed (cor02 does that).
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
ecalc_have 1 || { tgt_kv note "no path-1 binary"; ecalc_end SKIP; }
ecalc_refresh 900 && P2=1 || P2=0
D=$(ecalc_digits 1e10); TA=; TB=; N=0; BAD=0
# warm-up (discarded, tag r0_warm, not in TA/TB): the first run on these hosts is cold and would bias the sequence
ecalc_run r0_warm D 2 "$D" > /dev/null; ecalc_load r0_warm; ecalc_kv r0_warm
for s in A B B A A B B A; do N=$((N + 1)); sh=D; [ $s = B ] && [ $P2 = 1 ] && sh=C
    ecalc_run "r${N}_$s" $sh 2 "$D" > /dev/null; ecalc_load "r${N}_$s"; ecalc_kv "r${N}_$s"
    if ecalc_ok && [ -n "$E_TOTAL" ]; then case $s in A) TA="$TA $E_TOTAL";; B) TB="$TB $E_TOTAL";; esac; else BAD=1; fi
done
tgt_kv e.pathB "$([ $P2 = 1 ] && echo path2-C || echo path1-D-control)"
read -r MA MB DF PC <<< "$(ecalc_stat "$TA" "$TB")"
[ -n "$MA" ] && { tgt_kv m.mean_A_s "$MA s"; tgt_kv m.mean_B_s "$MB s"; tgt_kv m.B_minus_A_s "$DF s"; tgt_kv m.B_vs_A_pct "$PC %"; }
ecalc_q TQ20 "2 nodes $D digits, ABBA x2: A (path 1 D) mean $MA s [${TA# }], B ($([ $P2 = 1 ] && echo 'path 2' || echo 'path 1 control')) mean $MB s [${TB# }]; B vs A $PC % ($DF s)"
[ -n "$TA" ] || ecalc_end FAIL
[ $BAD = 1 ] && ecalc_end PARTIAL
[ $P2 = 0 ] && ! ecalc_aac7 && ecalc_end PARTIAL
[ $P2 = 0 ] && tgt_kv note "path 2 absent (aac7): A/A control only"
ecalc_end PASS

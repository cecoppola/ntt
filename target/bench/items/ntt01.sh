#!/bin/bash
# ntt01 (TQ18) - 1-node phase times at 1e10 (ladder L0 baseline): layout D and P on path 1, path 2 (C); order D P C D (one repeat of D as drift check).
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
ecalc_have 1 || { tgt_kv note "no path-1 binary"; ecalc_end SKIP; }
ecalc_refresh 900 >/dev/null 2>&1
D=$(ecalc_digits 1e10); BAD=0; SUM=
for a in "d D" "p P" "c C" "d2 D"; do set -- $a
    ecalc_run "n_$1" "$2" 1 "$D" > /dev/null; ecalc_load "n_$1"; ecalc_kv "n_$1"
    SUM="$SUM; $1: $(ecalc_phases) [$(ecalc_ok && echo ok || { [ "$E_SKIP" = 1 ] && echo skipped || echo BAD; })]"
    ecalc_ok || { [ "$E_SKIP" = 1 ] || { [ "$2" = C ] || BAD=1; }; }
done
ecalc_q TQ18 "1 node, $D digits (s):${SUM#;}"
[ $BAD = 1 ] && ecalc_end FAIL
ecalc_end PASS

#!/bin/bash
# ntt02 (TQ19) - the per-node share on one node: 6.6e10 digits (aac7 reference D 74.2 s, P 83.8 s) in D, P (path 1) and C (path 2).
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
ecalc_have 1 || { tgt_kv note "no path-1 binary"; ecalc_end SKIP; }
ecalc_refresh 900 >/dev/null 2>&1
D=$(ecalc_digits 6.6e10); BAD=0; SUM=; BEST=; BT=
for a in "d D" "p P" "c C"; do set -- $a
    TMO=14 ecalc_run "s_$1" "$2" 1 "$D" > /dev/null; ecalc_load "s_$1"; ecalc_kv "s_$1"
    SUM="$SUM; $1: $(ecalc_phases) dev ${E_DEVPEAK_GB:-NA} GB host ${E_HOSTPEAK_GB:-NA} GB [$(ecalc_ok && echo ok || { [ "$E_SKIP" = 1 ] && echo skipped || echo BAD; })]"
    if ecalc_ok && [ -n "$E_TOTAL" ] && { [ -z "$BT" ] || awk -v a="$E_TOTAL" -v b="$BT" 'BEGIN { exit !(a < b) }'; }; then BT=$E_TOTAL; BEST=$1; fi
    ecalc_ok || { [ "$E_SKIP" = 1 ] || { [ "$2" = C ] || BAD=1; }; }
done
tgt_kv e.fastest_arm "${BEST:-none}"
ecalc_q TQ19 "1 node, $D digits (s):${SUM#;}; fastest: ${BEST:-none} ${BT:+($BT s)}"
[ $BAD = 1 ] && ecalc_end FAIL
ecalc_end PASS

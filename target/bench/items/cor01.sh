#!/bin/bash
# cor01 (TQ06) - 1-node correctness gate: ecalc 1e9 (D path 1, P path 1, C path 2) and 1e10 (D, C), VERIFY OK and the digit sha against the
# known references (1e9 bf9f4b08758b40d9, 1e10 3fc025473586ed22).  Counts are NOT scaled on aac7 (cheap).  FAIL only when a path-1 arm is
# wrong; path 2 trouble (or absent) gives PARTIAL.
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
ecalc_have 1 || { tgt_kv note "no path-1 binary"; ecalc_end SKIP; }
ecalc_refresh 900 && P2=1 || P2=0
tgt_kv e.path2_available "$P2"
BAD1=0; BAD2=0; SK2=0; SUM=; HD=; HC=
arm() {   # TAG SHAPE DIGITS
    WRITE=1 ecalc_run "$1" "$2" 1 "$3" > /dev/null; ecalc_load "$1"; ecalc_kv "$1"
    SUM="$SUM; $(ecalc_brief "$1")"
    if [ "$E_SKIP" = 1 ]; then SK2=1; elif ! ecalc_ok; then [ "$2" = C ] && BAD2=1 || BAD1=1; fi
    case $2$3 in D1000000000) HD=$E_HASH;; C1000000000) HC=$E_HASH;; esac
}
arm d_1e9 D 1000000000
arm c_1e9 C 1000000000
arm p_1e9 P 1000000000
arm d_1e10 D 10000000000
arm c_1e10 C 10000000000
[ -n "$HD" ] && [ -n "$HC" ] && { [ "$HD" = "$HC" ] && tgt_kv e.paths_equal_1e9 yes || tgt_kv e.paths_equal_1e9 no; }
ecalc_q TQ06 "1 node:${SUM#;} (refs 1e9 bf9f4b08758b40d9, 1e10 3fc025473586ed22)"
if [ $BAD1 = 1 ]; then tgt_kv note "a path-1 arm is wrong: do not run the ladder before this is understood"; ecalc_end FAIL
elif [ $BAD2 = 1 ] || [ $SK2 = 1 ]; then ecalc_end PARTIAL
else ecalc_end PASS; fi

#!/bin/bash
# cor02 (TQ12) - 2-node digit identity: ecalc 1e10 on 2 nodes in D (path 1), P (path 1) and C (path 2), digit sha against the reference
# 3fc025473586ed22 and against each other.  Not scaled on aac7.  FAIL when a path-1 arm is wrong; path 2 trouble gives PARTIAL.
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
ecalc_have 1 || { tgt_kv note "no path-1 binary"; ecalc_end SKIP; }
ecalc_refresh 900 && tgt_kv e.path2_available 1 || tgt_kv e.path2_available 0
BAD1=0; SOFT=0; SUM=; HS=
for a in "d_2n D" "p_2n P" "c_2n C"; do set -- $a
    WRITE=1 ecalc_run "$1" "$2" 2 10000000000 > /dev/null; ecalc_load "$1"; ecalc_kv "$1"
    SUM="$SUM; $(ecalc_brief "$1")"; HS="$HS $E_HASH"
    if [ "$E_SKIP" = 1 ]; then SOFT=1; elif ! ecalc_ok; then [ "$2" = C ] && SOFT=1 || BAD1=1; fi
done
U=$(echo $HS | tr ' ' '\n' | grep . | sort -u | wc -l)
tgt_kv e.distinct_hashes "$U"
ecalc_q TQ12 "2 nodes 1e10:${SUM#;}; distinct digit hashes: $U (1 = identical on all arms; ref 3fc025473586ed22)"
[ $BAD1 = 1 ] && ecalc_end FAIL
[ "$U" -gt 1 ] && ecalc_end FAIL
[ $SOFT = 1 ] && ecalc_end PARTIAL
ecalc_end PASS

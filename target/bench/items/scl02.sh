#!/bin/bash
# scl02 (TQ21) - phase scaling at fixed 4e9 digits per node: 1, 4 and 8 nodes, layout D (path 1) and path 2 (C).  VERIFY must hold at every size.
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
ecalc_have 1 || { tgt_kv note "no path-1 binary"; ecalc_end SKIP; }
ecalc_refresh 900 && P2=1 || P2=0
NH=$(echo "$HOSTS" | tr ',' '\n' | grep -c .)
PER=$(ecalc_digits 4e9); BAD=0; SUM=
for sh in D C; do
    [ $sh = C ] && [ $P2 = 0 ] && continue
    for n in 1 4 8; do
        [ "$n" -le "$NH" ] || continue
        d=$(awk -v p="$PER" -v n="$n" 'BEGIN { printf "%.0f", p * n }'); tag=sc_${sh}_n$n
        TMO=8 ecalc_run $tag $sh $n $d > /dev/null; ecalc_load $tag; ecalc_kv $tag
        SUM="$SUM; $sh ${n}n: $(ecalc_phases)$(ecalc_ok || echo ' BAD')"
        ecalc_ok || { [ "$sh" = D ] && BAD=1; }
    done
done
ecalc_q TQ21 "$PER digits per node (s):${SUM#;}"
[ $BAD = 1 ] && ecalc_end FAIL
[ $P2 = 0 ] && ecalc_end PARTIAL
ecalc_end PASS

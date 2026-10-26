#!/bin/bash
# net05 (TQ17) - 16, 32, 64 nodes: does the launch start cleanly (FI_UNIVERSE_SIZE / PMI bootstrap) and how do small-message latency and the
# all-to-all (8 MiB messages: the heap per PE stays small) scale.  Path 1 at 16/32/64, then path 2 at 64/32/16.  Launch time = wall of the run.
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
NH=$(echo "$HOSTS" | tr ',' '\n' | grep -c .)
ecalc_refresh 600 && P2=1 || P2=0
QS=; QL=; RES=
one() {   # PATH NODES
    local pn=$1 n=$2 tag=big_p$1_n$2
    [ "$n" -le "$NH" ] || { tgt_kv e.$tag "skipped (only $NH hosts)"; return; }
    ecalc_have $pn || return
    TMO=5 XT_MAXMB=8 XT_REPS=3 XT_TIMEOUT=200 ecalc_xt $tag $pn $n "lat a2a"
    [ "$X_SKIP" = 1 ] && return
    local a an l; a=$(ecalc_xt_col $tag a2a 7 ' 8388608$' max); an=$(ecalc_xt_col $tag a2a 9 ' 8388608$' max); l=$(ecalc_xt_col $tag lat 3 'pingpong.* 8 B$' first)
    tgt_kv e.${tag}_result "$X_RESULT"; tgt_kv m.${tag}_launch_wall_s "$X_WALL_S s"; tgt_kv m.${tag}_rc "$X_RC"
    [ -n "$a" ] && tgt_kv m.${tag}_a2a8m_pe_gbs "$a GB/s"; [ -n "$an" ] && tgt_kv m.${tag}_a2a8m_node_gbs "$an GB/s"; [ -n "$l" ] && tgt_kv m.${tag}_lat_8b_us "$l us"
    grep -a -c 'REFUSED\|Abort\|abort\|PMI.*fail' raw/$tag.log* > /dev/null 2>&1 && tgt_kv e.${tag}_errors "$(zcat -f raw/$tag.log* | grep -a -m3 'REFUSED\|Abort\|abort\|PMI.*fail' | cut -c1-120 | tr '\n' '/')"
    QS="$QS; p$pn ${n}n ${a:-NA}"; QL="$QL; p$pn ${n}n ${l:-NA} us (launch ${X_WALL_S}s, $X_RESULT)"; RES="$RES $X_RESULT"
}
for n in 16 32 64; do one 1 $n; done
[ $P2 = 1 ] && for n in 64 32 16; do one 2 $n; done
ecalc_q TQ17 "a2a 8 MiB per PE (GB/s):${QS#;}; 8 B ping-pong and launch:${QL#;}"
case "$RES" in *PASS*) ;; *) ecalc_end FAIL;; esac
case "$RES" in *FAIL*|*NONE*) ecalc_end PARTIAL;; esac
ecalc_end PASS

#!/bin/bash
# net04 (TQ16) - all-to-all rate per PE vs node count: t_xshmem a2a (64 MiB max) on 2, 4, 8 nodes of the 8-node allocation, path 1 then path 2.
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
ecalc_refresh 900 && P2=1 || P2=0
QS=; RES=
for pn in 1 2; do
    [ $pn = 2 ] && [ $P2 = 0 ] && continue
    ecalc_have $pn || continue
    base=
    for n in 2 4 8; do tag=a2a_p${pn}_n$n
        TMO=4 XT_MAXMB=64 XT_REPS=3 ecalc_xt $tag $pn $n "a2a"
        [ "$X_SKIP" = 1 ] && continue
        a=$(ecalc_xt_col $tag a2a 7 ' 67108864$' max); an=$(ecalc_xt_col $tag a2a 9 ' 67108864$' max)
        tgt_kv e.${tag}_result "$X_RESULT"; tgt_kv m.${tag}_wall_s "$X_WALL_S s"
        [ -n "$a" ] && { tgt_kv m.${tag}_pe_gbs "$a GB/s"; tgt_kv m.${tag}_node_gbs "$an GB/s"; [ -z "$base" ] && base=$a
            tgt_kv m.${tag}_vs_2n_pct "$(awk -v a="$a" -v b="$base" 'BEGIN { printf "%.1f", b ? 100 * a / b : 0 }') %"; }
        QS="$QS; p$pn ${n}n ${a:-NA} GB/s per PE"; RES="$RES $X_RESULT"
        zcat -f raw/$tag.log* 2>/dev/null | sed -E 's/^ *[0-9]+: //' | grep -a '^CTRSUM' > raw/$tag.ctr
    done
done
ecalc_q TQ16 "a2a 64 MiB per PE (GB/s) by nodes:${QS#;}; fall-off = ratio to the 2-node value in m.*_vs_2n_pct"
case "$RES" in *PASS*) ;; *) ecalc_end FAIL;; esac
case "$RES" in *FAIL*|*NONE*) ecalc_end PARTIAL;; esac
ecalc_end PASS

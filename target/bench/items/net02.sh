#!/bin/bash
# net02 (TQ13 TQ14 TQ15) - t_xshmem lat, stream, a2a on 2 nodes x 4 PEs (off-node pairs), 64 MiB max message, per path and lane count:
# p1 lanes 1 / 2 (libfabric), p2 lanes 1 / 2 (cray-shmem), run one after the other on the same 2 nodes.  Per-NIC counters (CTRSUM) are kept.
# Metrics per arm: lat 8 B and 4 KiB (us per hop, mean), stream peak GB/s per PE (and per node), a2a 64 MiB GB/s per PE (and per node).
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
ecalc_refresh 900 && P2=1 || P2=0
RES=; QA=; QL=; QS=
arm() {   # TAG PATH LANES
    local tag=$1 pn=$2 ln=$3
    ecalc_have $pn || { tgt_kv e.$tag "skipped (no binary of path $pn)"; return; }
    TMO=5 XT_MAXMB=64 XT_REPS=3 ecalc_xt "$tag" "$pn" 2 "lat stream a2a" XSHMEM_LANES=$ln
    [ "$X_SKIP" = 1 ] && { tgt_kv e.$tag "skipped"; return; }
    tgt_kv e.${tag}_result "$X_RESULT"; tgt_kv m.${tag}_wall_s "$X_WALL_S s"
    local l8 l4k sp spn a2 a2n
    l8=$(ecalc_xt_col $tag lat 3 'pingpong.* 8 B$' first); l4k=$(ecalc_xt_col $tag lat 3 'pingpong.* 4096 B$' first)
    sp=$(ecalc_xt_col $tag stream 6 'off-node' max); spn=$(ecalc_xt_col $tag stream 8 'off-node' max)
    a2=$(ecalc_xt_col $tag a2a 7 ' 67108864$' max); a2n=$(ecalc_xt_col $tag a2a 9 ' 67108864$' max)
    [ -n "$l8" ] && tgt_kv m.${tag}_lat_8b_us "$l8 us"; [ -n "$l4k" ] && tgt_kv m.${tag}_lat_4k_us "$l4k us"
    [ -n "$sp" ] && tgt_kv m.${tag}_stream_pe_gbs "$sp GB/s"; [ -n "$spn" ] && tgt_kv m.${tag}_stream_node_gbs "$spn GB/s"
    [ -n "$a2" ] && tgt_kv m.${tag}_a2a64m_pe_gbs "$a2 GB/s"; [ -n "$a2n" ] && tgt_kv m.${tag}_a2a64m_node_gbs "$a2n GB/s"
    zcat -f raw/$tag.log* 2>/dev/null | sed -E 's/^ *[0-9]+: //' | grep -a -E '^(CTRSUM|MAP pe)' > raw/$tag.ctr
    QL="$QL; $tag ${l8:-NA}/${l4k:-NA} us"; QS="$QS; $tag ${sp:-NA} GB/s per PE (${spn:-NA} node)"; QA="$QA; $tag ${a2:-NA} GB/s per PE (${a2n:-NA} node)"
    RES="$RES $X_RESULT"
}
arm xp1_l1 1 1; arm xp1_l2 1 2; arm xp2_l1 2 1; arm xp2_l2 2 2
ecalc_q TQ13 "stream peak, off-node, 64 MiB max:${QS#;} (counters in raw/*.ctr)"
ecalc_q TQ14 "lat 8 B / 4 KiB per hop:${QL#;}"
ecalc_q TQ15 "a2a 64 MiB per PE:${QA#;}"
case "$RES" in *PASS*) ;; *) ecalc_end FAIL;; esac
case "$RES" in *FAIL*|*NONE*) ecalc_end PARTIAL;; esac
ecalc_end PASS

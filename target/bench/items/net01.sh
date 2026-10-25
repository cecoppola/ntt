#!/bin/bash
# net01 (TQ11) - the SHMEM contract on 2 nodes x 4 PEs: t_xshmem "correct" sections (basic exch deadlock layer limit tail) on path 1 (libfabric
# backend) and path 2 (cray-shmem), with SHMEM_ENV_DISPLAY on path 2 (allocator names, heap, NIC/context settings), plus the contexts-on-NICs check
# (MAP lines).  Not scaled.  PASS when path 1 passes and path 2 passes; path 2 absent or failing gives PARTIAL (path 1 failing: FAIL).
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
ecalc_refresh 900 && P2=1 || P2=0
S1=NA; S2=NA
if ecalc_have 1; then TMO=6 XT_MAXMB=16 ecalc_xt xt_p1 1 2 "basic exch deadlock layer limit tail"; S1=$X_RESULT; tgt_kv e.xt_p1_result "$X_RESULT"; tgt_kv m.xt_p1_wall_s "$X_WALL_S s"; fi
if [ $P2 = 1 ]; then TMO=6 XT_MAXMB=16 ecalc_xt xt_p2 2 2 "basic exch deadlock layer limit tail" SHMEM_ENV_DISPLAY=1; S2=$X_RESULT; tgt_kv e.xt_p2_result "$X_RESULT"; tgt_kv m.xt_p2_wall_s "$X_WALL_S s"
    grep -a -E 'SHMEM_|XT_SYMMETRIC|Allocator|allocator|CXI|cxi' raw/xt_p2.log | sed -E 's/^ *[0-9]+: //' | sort -u | head -80 > raw/xt_p2.envdisplay.txt
    tgt_kv m.envdisplay_lines "$(grep -c . raw/xt_p2.envdisplay.txt) lines"
else tgt_kv e.xt_p2_result "SKIP (bld02 absent or failed)"; fi
for t in xt_p1 xt_p2; do [ -f raw/$t.log ] || [ -f raw/$t.log.gz ] || continue
    nf=$(zcat -f raw/$t.log* 2>/dev/null | grep -a -c '<<< FAIL'); tgt_kv m.${t}_fail_lines "$nf lines"
    zcat -f raw/$t.log* 2>/dev/null | sed -E 's/^ *[0-9]+: //' | grep -a '^MAP pe' | sort -u > raw/$t.map; done
ecalc_q TQ11 "t_xshmem correct sections, 2 nodes x 4 PEs: path 1 (libfabric) $S1, path 2 (cray-shmem) $S2; SHMEM_ENV_DISPLAY in raw/xt_p2.envdisplay.txt; PE/NIC maps in raw/xt_p*.map"
[ "$S1" = FAIL ] && ecalc_end FAIL
[ "$S1" = PASS ] && [ "$S2" = PASS ] && ecalc_end PASS
[ "$S1" = NA ] && [ "$S2" = PASS ] && ecalc_end PARTIAL
ecalc_end PARTIAL

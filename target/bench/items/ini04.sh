#!/bin/bash
# ini04 - path 2 symmetric heap on THP (XSHMEM_CRAY_HEAP_THP 0/1) and heap slack (TQ37), 2 nodes, ECALC_INIT_TL=1.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 2
ARMV[A]="XSHMEM_CRAY_HEAP_THP=0"; ARMV[B]="XSHMEM_CRAY_HEAP_THP=1"; ARMV[S]="XSHMEM_CRAY_HEAP_THP=0 XSHMEM_HEAP_SLACK_MB=1024"
D=$(dig 1e10 2)
runseq C 2 "$D" "A B S B A" ECALC_INIT_TL=1 || true
for r in a_r1 b_r2 s_r3; do tl_kv "$OUT/raw/$r.log" "tl_$r" || true; done
arms_report A init m.init; I=$AB_LINE; arms_report A total m.tot
tgt_kv q.TQ37 "init: $I; total: $AB_LINE (THP=1 is B, slack 1024 MB is S); 2 nodes"
finish

#!/bin/bash
# net14 - XSHMEM_PEER 0/1 (same-node put path through shmem_ptr) on path 2, ecalc 1e10 at 1 node and at 2 nodes, ABBA each (TQ43).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 2
ARMV[A1]="XSHMEM_PEER=0"; ARMV[B1]="XSHMEM_PEER=1"; ARMV[A2]="XSHMEM_PEER=0"; ARMV[B2]="XSHMEM_PEER=1"
runseq C 1 "$(dig 1e10)" "A1 B1 B1 A1" || true
runseq C 2 "$(dig 1e10 2)" "A2 B2 B2 A2" || true
ARMS_ONLY="A1 B1" arms_report A1 total m.n1; L1=$AB_LINE; ARMS_ONLY="A2 B2" arms_report A2 total m.n2
tgt_kv q.TQ43 "PEER 1 minus 0: 1 node $L1; 2 nodes $AB_LINE"
finish

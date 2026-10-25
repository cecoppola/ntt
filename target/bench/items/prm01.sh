#!/bin/bash
# prm01 - MN_T_CHUNK_MB 1024 vs 2048 on 8 nodes (TQ44), 4e9 digits per node, ABBA, path 2.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
ARMV[A]="MN_T_CHUNK_MB=1024"; ARMV[B]="MN_T_CHUNK_MB=2048"
N=8; ab_main C $N "$(fitd $N C "$(dig 4e9 $N)")" "A B B A" A TQ44

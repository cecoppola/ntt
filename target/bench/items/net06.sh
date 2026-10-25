#!/bin/bash
# net06 - XSHMEM_LANES 1 vs 2 inside ecalc, path 2, 2 nodes, ABBA (TQ38).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
ARMV[L1]="XSHMEM_LANES=1"; ARMV[L2]="XSHMEM_LANES=2"
ab_main C 2 "$(dig 1e10 2)" "L1 L2 L2 L1" L1 TQ38

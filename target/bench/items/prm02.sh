#!/bin/bash
# prm02 - XSHMEM_ROUND_MB 128 vs 512 at 2 nodes (TQ45), 1e10, ABBA, path 2.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
ARMV[A]="XSHMEM_ROUND_MB=128"; ARMV[B]="XSHMEM_ROUND_MB=512"
ab_main C 2 "$(dig 1e10 2)" "A B B A" A TQ45

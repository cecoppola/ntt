#!/bin/bash
# scl04 - MN_GROUPS schedules at 64 nodes (TQ53): default vs 4,16,64 vs 8,64, 2e9 digits per node, one run each.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
ARMV[A]=""; ARMV[B]="MN_GROUPS=4,16,64"; ARMV[C]="MN_GROUPS=8,64"
N=64; ab_main C $N "$(fitd $N C "$(dig 2e9 $N)")" "A B C" A TQ53

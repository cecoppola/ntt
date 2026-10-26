#!/bin/bash
# prm06 - ECALC_BG_THREADS 24 / 48 / 96 (and BS_SEED_THREADS) at 1 node, layout D (TQ49).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
ARMV[A]="ECALC_BG_THREADS=48"; ARMV[B]="ECALC_BG_THREADS=24"; ARMV[C]="ECALC_BG_THREADS=96"
need_path 1
ab_main D 1 "$(fitd 1 D "$(dig $TGT_SHARE)")" "A B C C B A" A TQ49

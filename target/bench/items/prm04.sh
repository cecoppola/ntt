#!/bin/bash
# prm04 - ECALC_NP auto vs 3 vs 4 primes per node at 1 node (TQ47), campaign share TGT_SHARE (8e9, Q120), layout D (aac7 at the old 6.6e10: NP=3 -0.19 s, NP=4 +7.5 s).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
ARMV[A]=""; ARMV[B]="ECALC_NP=3"; ARMV[C]="ECALC_NP=4"
need_path 1
ab_main D 1 "$(fitd 1 D "$(dig $TGT_SHARE)" ECALC_NP=4)" "A B C C B A" A TQ47

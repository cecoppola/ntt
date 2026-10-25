#!/bin/bash
# prm04 - ECALC_NP auto vs 3 vs 4 primes per node at 1 node (TQ47), large per-node size, layout D (aac7 6.6e10: NP=3 -0.19 s, NP=4 +7.5 s).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
ARMV[A]=""; ARMV[B]="ECALC_NP=3"; ARMV[C]="ECALC_NP=4"
need_path 1
ab_main D 1 "$(fitd 1 D "$(dig 6.6e10)" ECALC_NP=4)" "A B C C B A" A TQ47

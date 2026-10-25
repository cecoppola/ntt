#!/bin/bash
# prm07 - launch arms of path 2 (TQ42): MNRUN_PPG_GPUBIND none|task x MNRUN_PPG_CPUBIND none|ldom, ecalc 1e10 on 2 nodes, palindromic order.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
ARMV[A]="MNRUN_PPG_GPUBIND=none MNRUN_PPG_CPUBIND=none"; ARMV[B]="MNRUN_PPG_GPUBIND=none MNRUN_PPG_CPUBIND=ldom"
ARMV[C]="MNRUN_PPG_GPUBIND=task MNRUN_PPG_CPUBIND=none"; ARMV[D]="MNRUN_PPG_GPUBIND=task MNRUN_PPG_CPUBIND=ldom"
ab_main C 2 "$(dig 1e10 2)" "A B C D D C B A" A TQ42

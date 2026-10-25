#!/bin/bash
# prm03 - DM_MN_WRAP 0 vs 1 on 8 nodes (TQ46), 4e9 digits per node, ABBA, path 2; the planner verdict of both arms is recorded.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 2
N=8; D=$(fitd $N C "$(dig 4e9 $N)" DM_MN_WRAP=1)
for w in 0 1; do ecalc_plan $N "$D" C DM_MN_WRAP=$w; tgt_kv m.plan_dev_wrap${w}_gb "$P_DEV_GB GB"; tgt_kv e.plan_fit_wrap$w "$P_FIT"; done
ARMV[A]="DM_MN_WRAP=0"; ARMV[B]="DM_MN_WRAP=1"
ab_main C $N "$D" "A B B A" A TQ46

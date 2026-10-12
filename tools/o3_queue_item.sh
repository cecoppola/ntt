#!/bin/bash
# aac7q queue item (O3, results/O3.md): P1 step 2 gate, 2 nodes. Gate A = standard (no profile, aac7 line) with --build (also runs the registry and
# profile checks); gate B = same ecalc, --line none --profile aac7 (profile supplies the environment). Both must be PASS / digits identical.
# OWNER=o3
# NODES=2
# EST_MIN=20
# APPROVED=yes
# EXCLUSIVE=0
set -u
J=${SLURM_JOB_ID:-}; WT=$HOME/ntt-wt/o3; OUT=$HOME/o3/Q; mkdir -p $OUT; rm -f $OUT/O3_DONE; cd $WT || exit 2
unset OUT_; env -u OUT true
tools/gate.sh --build --nodes 2 --hold $J --out $OUT/gateA --timeout 600 > $OUT/gateA.log 2>&1; echo "A rc=$?" > $OUT/summary.txt
tools/gate.sh --nodes 2 --hold $J --line none --profile aac7 --out $OUT/gateB --timeout 600 > $OUT/gateB.log 2>&1; echo "B rc=$?" >> $OUT/summary.txt
cat $OUT/gateA/gate.txt $OUT/gateB/gate.txt $OUT/gateB/config_src.txt >> $OUT/summary.txt 2>&1
squeue -s -j $J >> $OUT/summary.txt 2>&1
touch $OUT/O3_DONE

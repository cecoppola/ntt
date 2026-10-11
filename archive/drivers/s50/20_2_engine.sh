#!/bin/bash
# OWNER=T5
# EST_MIN=30
# S50 queue item: engine gates with DIST_GATHER_TILE=<TILE> exported: mnaccept.sh --only unit,e9,full on the hold (t_newton, t_mul, e9 in both bases, 1e11 at size 1 vs ~/ref/e_1e11.out).
H=$SLURM_JOB_ID; O=$HOME/batch1011/T5/s50; WT=$HOME/ntt-wt/s50; mkdir -p $O
T=$(cat $O/TILE 2>/dev/null || echo skip); [ "$T" = skip ] && { echo "S50: TILE=skip: engine gates skipped"; exit 0; }
cd $WT/ecalc || exit 2
export DIST_GATHER_TILE=$T
timeout 2400 ./mnaccept.sh $H --only unit,e9,full < /dev/null > $O/engine_gates.log 2>&1; echo "rc=$? tile=$T" >> $O/engine_gates.log
squeue -s -j $H -h | grep -c . > $O/steps_left_engine.txt
exit 0

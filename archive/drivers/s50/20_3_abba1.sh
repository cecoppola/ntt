#!/bin/bash
# OWNER=T5
# EST_MIN=45
# S50 queue item: 1-node ABBA (6.441e10 digits), A = DIST_GATHER_TILE=0, B = tile (env TILE, default 32), stop rule p<0.05 / CI half-width 1.5 s / 5 rounds, stop by D.
H=$SLURM_JOB_ID; O=$HOME/batch1011/T5/s50; WT=$HOME/ntt-wt/s50; mkdir -p $O
T=$(cat $O/TILE 2>/dev/null || echo skip); [ "$T" = skip ] && { echo "S50: no tile passed probe+gate (TILE=$T): skipped"; exit 0; }
WT=$WT MAXR=5 MINR=3 HALF_S=1.5 MARK=ABBA1_DONE STOP_EPOCH=1791723386 ARM_A=DIST_GATHER_TILE=0 ARM_B=DIST_GATHER_TILE=$T \
  bash $WT/archive/drivers/s50/s50_abba.sh $H $O/abba1_$(date +%H%M) 1 < /dev/null
exit 0

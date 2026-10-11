#!/bin/bash
# OWNER=T5
# EST_MIN=70
# S50 queue item: 4-node ABBA (6.441e10 digits/node), A = DIST_GATHER_TILE=0, B = tile (file TILE), stop rule as the 1-node item, <= 4 rounds, stop by D.
H=$SLURM_JOB_ID; O=$HOME/batch1011/T5/s50; WT=$HOME/ntt-wt/s50; mkdir -p $O
T=$(cat $O/TILE 2>/dev/null || echo skip); [ "$T" = skip ] && { echo "S50: no tile passed probe+gate (TILE=$T): skipped"; exit 0; }
WT=$WT MAXR=4 MINR=3 HALF_S=2.0 MARK=ABBA4_DONE STOP_EPOCH=1791723386 ARM_A=DIST_GATHER_TILE=0 ARM_B=DIST_GATHER_TILE=$T \
  bash $WT/archive/drivers/s50/s50_abba.sh $H $O/abba4_$(date +%H%M) 4 < /dev/null
exit 0

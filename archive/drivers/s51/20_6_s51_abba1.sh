#!/bin/bash
# OWNER=T5
# EST_MIN=50
# S51 (T5b) queue item b: 1-node ABBA at 6.441e10 digits, A = base, B = the kept S51 switches (file ARM_B written by item a), stop rule p<0.05 / CI half-width 1.5 s / 5 rounds, stop by D.
H=${SLURM_JOB_ID:-0}; WT=${WT:-$HOME/ntt-wt/s51}; O=${O:-$HOME/batch1011/T5/s51}; mkdir -p $O
B=$(cat $O/ARM_B 2>/dev/null); [ "$B" = "DBIG_S51_STATS=1" ] || [ -z "$B" ] && { echo "S51: no kernel kept (ARM_B='$B'): skipped"; exit 0; }
WT=$WT MAXR=5 MINR=3 HALF_S=1.5 MARK=ABBA1_DONE STOP_EPOCH=${STOP_EPOCH:-1791723386} ARM_A="DBIG_S51_STATS=1" ARM_B="$B" \
  bash $WT/archive/drivers/s51/s51_abba.sh $H $O/abba1 1 < /dev/null
exit 0

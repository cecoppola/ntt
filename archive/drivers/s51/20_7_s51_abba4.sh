#!/bin/bash
# OWNER=T5
# EST_MIN=60
# S51 (T5b) queue item c: 4-node ABBA (6.441e10 digits/node), A = base, B = the kept S51 switches (file ARM_B), 3 rounds (stop rule as item b), stop by D.
H=${SLURM_JOB_ID:-0}; WT=${WT:-$HOME/ntt-wt/s51}; O=${O:-$HOME/batch1011/T5/s51}; mkdir -p $O
B=$(cat $O/ARM_B 2>/dev/null); [ "$B" = "DBIG_S51_STATS=1" ] || [ -z "$B" ] && { echo "S51: no kernel kept (ARM_B='$B'): skipped"; exit 0; }
WT=$WT MAXR=3 MINR=3 HALF_S=2.0 MARK=ABBA4_DONE STOP_EPOCH=${STOP_EPOCH:-1791723386} ARM_A="DBIG_S51_STATS=1" ARM_B="$B" \
  bash $WT/archive/drivers/s51/s51_abba.sh $H $O/abba4 4 < /dev/null
exit 0

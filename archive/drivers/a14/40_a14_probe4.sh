#!/bin/bash
# OWNER=a14
# NODES=4
# EST_MIN=12
# APPROVED=coordinator-2026-10-11
# a14: 4-node launch-group probe (results/A14.md): 30+ base launches of t_comm --bw 4 100 64, NCs snapshots around each.  Needs SLURM_JOB_ID = the runner's 4-node allocation.
# STUB=1: dry run (no srun) -> checks the script logic only.  Outputs: ~/sess1011c/a14/R4/ (launches.tsv, DONE).  Exits by itself; never cancels the allocation.
S=$HOME/sess1011c/a14; mkdir -p $S/R4
if [ "${STUB:-0}" = 1 ]; then echo "STUB ok: would run a14_drive.sh NN=4 base for 540 s on \$SLURM_JOB_ID"; bash -n $S/a14_drive.sh && bash -n $S/wrap.sh && ls $HOME/ntt-acc/ecalc/tests/t_comm; exit 0; fi
: ${SLURM_JOB_ID:?need the runner allocation}
export HOLD=$SLURM_JOB_ID NN=4 WT=$HOME/ntt-acc OUT=$S/R4 ROUNDS=60 DEADLINE=$(( $(date +%s) + 540 )) CONFIGS="base|" KEEP_HOLD=1
bash $S/a14_drive.sh > $S/R4.out 2>&1
awk -F'\t' 'NR>1{g=($8>$9)?"A":"B"; n[g]++; s[g]+=$5} END{for(k in n) printf "%s n=%d mean64=%.2f\n",k,n[k],s[k]/n[k]}' $S/R4/launches.tsv > $S/R4/summary.txt.tmp && mv $S/R4/summary.txt.tmp $S/R4/summary.txt

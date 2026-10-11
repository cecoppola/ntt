#!/bin/bash
# OWNER=a14
# NODES=4
# EST_MIN=12
# APPROVED=coordinator-2026-10-11
# a14: 4-node pin trial (results/A14.md): 10 rounds x {TC_LOW_LATENCY, NUM_NICS=1}, t_comm --bw 4 100 64, MCU group + rate per launch.  Needs SLURM_JOB_ID = the runner allocation.
# STUB=1: dry run (no srun) -> checks the script logic only.  Outputs: ~/sess1011c/a14/P4/ (launches.tsv, DONE).  Exits by itself; never cancels the allocation.
S=$HOME/sess1011c/a14; mkdir -p $S/P4
if [ "${STUB:-0}" = 1 ]; then echo "STUB ok: would run a14_drive.sh NN=4 tcll+nn1 interleaved x10 for 540 s on \$SLURM_JOB_ID"; bash -n $S/a14_drive.sh && bash -n $S/wrap.sh && ls $HOME/ntt-acc/ecalc/tests/t_comm; exit 0; fi
: ${SLURM_JOB_ID:?need the runner allocation}
export HOLD=$SLURM_JOB_ID NN=4 WT=$HOME/ntt-acc OUT=$S/P4 ROUNDS=10 DEADLINE=$(( $(date +%s) + 540 )) CONFIGS="tcll|SHMEM_OFI_DEFAULT_TCLASS=TC_LOW_LATENCY;nn1|SHMEM_OFI_NUM_NICS=1" KEEP_HOLD=1
bash $S/a14_drive.sh > $S/P4.out 2>&1
awk -F"\t" 'NR>1{g=($8>$9)?"A":"B"; k=$3" "g; n[k]++; s[k]+=$5} END{for(k in n) printf "%s n=%d mean64=%.2f\n",k,n[k],s[k]/n[k]}' $S/P4/launches.tsv > $S/P4/summary.txt.tmp && mv $S/P4/summary.txt.tmp $S/P4/summary.txt

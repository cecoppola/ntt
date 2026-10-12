#!/bin/bash
# aac7q item (reusable): t_mn_grid in decimal (LIMB_BASE=10) and binary (LIMB_BASE=2) limbs at several node counts.
# Copy to ~/aac7q/queue/<NN>_<owner>_mngrid.sh; the coordinator adds APPROVED.  Parameters (edit the two lines below or set
# them in the queue copy): WT = a built worktree's ecalc dir (built with make -B SHMEM_CRAY=1 ...: see the build note), SIZES.
# OWNER=h2
# NODES=6
# EST_MIN=45
# APPROVED=
set -u
WT=${WT:-$HOME/ntt-wt/h2/ecalc}; SIZES=${SIZES:-"2 3 6 4 5"}; CAP=${CAP_S:-2400}; OUTD=${OUTD:-$HOME/h2}
J=$SLURM_JOB_ID
cd "$WT" || exit 2
source aac7env.sh >/dev/null 2>&1
O=$OUTD; rm -rf "$O"; mkdir -p "$O"
T0=$(date +%s)
echo "start $(date -Is) hold $J nodes $(squeue -j $J -h -o %N) wt $WT $(git -C $WT log --oneline -1)" > $O/summary.txt
for p in $SIZES; do
  for b in 10 2; do
    [ $(( $(date +%s) - T0 )) -gt $CAP ] && { echo "p=$p base=$b SKIPPED (time cap)" >> $O/summary.txt; continue; }
    s=$(date +%s)
    SLURM_JOB_ID=$J timeout 420 ./mnrun.sh $p env COMM_TRANSPORT=shmem LIMB_BASE=$b ./tests/t_mn_grid 1 28 > $O/grid_p${p}_b$b.log 2>&1; rc=$?
    e=$(date +%s)
    echo "p=$p base=$b rc=$rc wall=$((e-s))s ok=$(grep -ac 'VERIFY OK' $O/grid_p${p}_b$b.log) fail=$(grep -ac 'VERIFY FAILED' $O/grid_p${p}_b$b.log)" >> $O/summary.txt
  done
done
echo "end $(date -Is)" >> $O/summary.txt
squeue -s -j $J -h >> $O/summary.txt
touch $O/DONE

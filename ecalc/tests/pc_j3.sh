#!/bin/bash -l
# PC15 batch j3 (Phase 15 Batch 3 PC, results/PC15.md): the gates -- ./mnaccept.sh --only <steps> with an environment, then (optionally)
# 4 x 10^10 at size 1 against results/e_4e10.out (the reference evicted from the page cache first).
#   usage: setsid nohup bash -l ~/ntt-PC15/ecalc/tests/pc_j3.sh <tag> <steps> <4e10: 0|1> [VAR=val ...] > ~/PC15/<tag>.out 2>&1 &
module load rocm >/dev/null 2>&1
TAG=$1 STEPS=$2 BIG=$3; shift 3
L=~/PC15/$TAG; mkdir -p $L
cd ~/ntt-PC15/ecalc || exit 1
echo "== $TAG: $(git log --oneline -1) $(date); mnaccept --only $STEPS with: $*; 4e10: $BIG"
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J PC --parsable --wrap "sleep 2700")
echo "job $J queued $(date)"
trap 'scancel $J 2>/dev/null' EXIT
while :; do s=$(squeue -j $J -h -o %T 2>/dev/null); [ "$s" = RUNNING ] && break; [ -z "$s" ] && { echo "job $J gone"; exit 1; }; sleep 20; done
echo "job $J running on $(squeue -j $J -h -o %N) $(date)"; T0=$(date +%s)
export SLURM_JOB_ID=$J
env "$@" ./mnaccept.sh $J --only $STEPS 2>&1 | tee $L/mnaccept.log | grep -a -E "PASS|FAIL|summary|passed|failed"
echo "mnaccept done $(date), $(( $(date +%s) - T0 )) s"
if [ "$BIG" = 1 ]; then
  N() { srun --jobid=$J -N1 --overlap bash -c "$*"; }
  N "mkdir -p /tmp/pc15; rm -rf /tmp/pc15/*; python3 -c \"import os; f=os.open('$HOME/ntt/ecalc/results/e_4e10.out', os.O_RDONLY); os.posix_fadvise(f, 0, 0, os.POSIX_FADV_DONTNEED)\""
  t=$(date +%s)
  srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $PWD && env $* ECALC_VERBOSE=1 ./ecalc 40000000000 /tmp/pc15/e4e10.txt" > $L/e4e10.log 2>&1; rc=$?
  cmpr=$(N "$PWD/digcmp.sh /tmp/pc15/e4e10.txt $HOME/ntt/ecalc/results/e_4e10.out; rm -rf /tmp/pc15/e4e10.txt*")
  echo "4e10 rc $rc $cmpr wall $(( $(date +%s) - t )) s | $(grep -a '^total' $L/e4e10.log | tail -1 | sed 's/  */ /g' | cut -c1-100) | $(grep -ac 'VERIFY OK' $L/e4e10.log) VERIFY OK"
fi
echo "== $TAG done $(date), $(( $(date +%s) - T0 )) s on the node"

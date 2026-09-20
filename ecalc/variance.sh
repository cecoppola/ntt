#!/bin/bash
# Phase 5 task 1: N identical 4e10 runs with amd-smi sampling -> results/variance/
#   srun --jobid=<id> -N1 --gpus=4 bash variance.sh [N]
module load rocm; cd "$(dirname "$0")" || exit 1      # any clone
N=${1:-5}; OUT=results/variance${LIMB_BASE:+_b$LIMB_BASE}${VARIANCE_TAG:+_$VARIANCE_TAG}; mkdir -p $OUT   # VARIANCE_TAG: e.g. dev for NEWTON_DEVICE=1 runs
for i in $(seq 1 $N); do
  ( while true; do echo "$(date +%s) $(amd-smi metric --clock --power --temperature --json 2>/dev/null | tr -d '\n ')"; sleep 2; done ) > $OUT/run$i.smi &
  SP=$!
  ./ecalc 40000000000 > $OUT/run$i.log 2>&1
  kill $SP 2>/dev/null; wait $SP 2>/dev/null
  echo "run $i: $(grep -aE '^total' $OUT/run$i.log); $(grep -ao 'VmHWM [0-9.]* GB' $OUT/run$i.log | tail -1); $(grep -a VERIFY $OUT/run$i.log)"
done

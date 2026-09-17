#!/bin/bash
# Phase 4 acceptance sweep (PLAN.md 9): every unit test, the end-to-end ladder
# with all checks, per-phase times vs Table I, peak RSS.  Run on the node:
#   srun --jobid=<id> -N1 --gpus=4 bash accept.sh        -> results/accept_b<base>/   (LIMB_BASE=10 for decimal limbs)
module load rocm; cd ~/ntt/ecalc
BASE=${LIMB_BASE:-2}; export LIMB_BASE=$BASE
OUT=results/accept_b$BASE; mkdir -p $OUT
echo "== acceptance sweep, LIMB_BASE=$BASE, $(hostname), $(date -Is), $(git -C .. rev-parse --short HEAD) ==" | tee $OUT/summary.txt
for t in t_params; do (cd ..; ./tests/$t) > $OUT/$t.log 2>&1; echo "$t: $(grep -E 'VERIFY' $OUT/$t.log | tail -1)" | tee -a $OUT/summary.txt; done
TESTS=("t_modarith 1000" "t_ntt 31" "t_mul 20" "t_mul 0 batch" "t_crt 30" "t_newton 26" "t_bs" "t_verify"); [ "$BASE" = 2 ] && TESTS+=("t_dec long")
for t in "${TESTS[@]}"; do
  n=${t%% *}; ./tests/$t > $OUT/${t// /_}.log 2>&1
  echo "$t: $(grep -E 'VERIFY' $OUT/${t// /_}.log | tail -1)" | tee -a $OUT/summary.txt
done
for d in 1000000 10000000 100000000 1000000000; do
  ./ecalc $d $OUT/e_$d.out > $OUT/ecalc_$d.log 2>&1
  cmp -s $OUT/e_$d.out ref/e_$d.txt && c="identical to ref" || c="DIFFERS from ref"
  echo "e $d: $(grep -E '^total' $OUT/ecalc_$d.log | sed 's/ *(.*//'); $(grep -E 'VERIFY' $OUT/ecalc_$d.log); $c" | tee -a $OUT/summary.txt
  rm -f $OUT/e_$d.out
done
for d in 10000000000 40000000000; do
  ECALC_VERBOSE=2 ./ecalc $d > $OUT/ecalc_$d.log 2>&1
  echo "e $d: $(grep -E '^total' $OUT/ecalc_$d.log); $(grep -E '^(T1|T2) ' $OUT/ecalc_$d.log | tr '\n' ';'); $(grep VERIFY $OUT/ecalc_$d.log)" | tee -a $OUT/summary.txt
done
echo "done $(date -Is)" | tee -a $OUT/summary.txt

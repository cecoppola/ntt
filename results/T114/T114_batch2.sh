#!/bin/bash
# T114 node batch 2: the per-product live windows (commit 3545dd8+); unattended, cancels its job at exit. Logs in ~/T114/b2_*.
cd ~/ntt-T114/ecalc || exit 1
B=~/T114; S=$B/batch2.txt
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J T1b --parsable --wrap "sleep 2700")
echo "$(date) job $J submitted" >> $S
trap 'scancel $J; echo "$(date) job $J cancelled (exit)" >> $S' EXIT
while [ "$(squeue -j $J -h -o %T)" != RUNNING ]; do sleep 20; [ -z "$(squeue -j $J -h -o %T)" ] && { echo "$(date) job $J gone" >> $S; exit 1; }; done
echo "$(date) job $J running on $(squeue -j $J -h -o %N); $(git log --oneline -1)" >> $S
R10=~/ntt/ecalc/results/e_1e10.out
cmpp() { srun --jobid=$J -N1 --overlap bash -c "cat $1.part* > $1.all; cmp -s $1.all $2 && echo identical || echo DIFFERS; rm -rf $1 $1.*"; }
run() { local name=$1 p=$2 d=$3 ref=$4; shift 4; local t0=$(date +%s)
  SLURM_JOB_ID=$J timeout 900 ./mnrun.sh $p env "$@" ./ecalc $d /tmp/T114_$name > $B/$name.log 2>&1; local rc=$?
  local c=$(cmpp /tmp/T114_$name $ref)
  echo "$(date +%T) $name: size $p $d [$*] rc $rc, $c, $(( $(date +%s) - t0 )) s, VERIFY OK x$(grep -ac 'VERIFY OK' $B/$name.log)" >> $S; }
run b2_s4_ef1 4 10000000000 $R10 MN_TREE_EARLY_FREE=1 POOL_LOG=29 ECALC_LIVE=1
run b2_s4_ef0 4 10000000000 $R10 POOL_LOG=29 ECALC_LIVE=1
run b2_s4g4_ef1 4 10000000000 $R10 MN_TREE_EARLY_FREE=1 MN_GROUPS=4 POOL_LOG=29 ECALC_LIVE=1
run b2_s4g4_ef0 4 10000000000 $R10 MN_GROUPS=4 POOL_LOG=29 ECALC_LIVE=1
./mnaccept.sh $J --only unit,mn > $B/mnaccept_ef0.log 2>&1; echo "$(date +%T) mnaccept ef0 rc $? (failures)" >> $S
mv results/mnaccept/$J results/mnaccept/${J}_ef0
echo "$(date) batch done" >> $S

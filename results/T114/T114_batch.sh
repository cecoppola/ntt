#!/bin/bash
# T114 node batch: runs unattended, waits for its own job, cancels it at the end. Logs in ~/T114/.
cd ~/ntt-T114/ecalc || exit 1
B=~/T114; mkdir -p $B; S=$B/batch.txt
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J T1 --parsable --wrap "sleep 2700")
echo "$(date) job $J submitted" >> $S
trap 'scancel $J; echo "$(date) job $J cancelled (exit)" >> $S' EXIT
while [ "$(squeue -j $J -h -o %T)" != RUNNING ]; do sleep 20; [ -z "$(squeue -j $J -h -o %T)" ] && { echo "$(date) job $J gone" >> $S; exit 1; }; done
echo "$(date) job $J running on $(squeue -j $J -h -o %N); $(git log --oneline -1)" >> $S
R10=~/ntt/ecalc/results/e_1e10.out; R9=~/ntt/ecalc/ref/e_1000000000.txt
cmpp() { srun --jobid=$J -N1 --overlap bash -c "cat $1.part* > $1.all; cmp -s $1.all $2 && echo identical || echo DIFFERS; rm -rf $1 $1.*"; }
run() { local name=$1 p=$2 d=$3 ref=$4; shift 4; local t0=$(date +%s)
  SLURM_JOB_ID=$J timeout 900 ./mnrun.sh $p env "$@" ./ecalc $d /tmp/T114_$name > $B/$name.log 2>&1; local rc=$?
  local c=$(cmpp /tmp/T114_$name $ref)
  echo "$(date +%T) $name: size $p $d [$*] rc $rc, $c, $(( $(date +%s) - t0 )) s, VERIFY OK x$(grep -ac 'VERIFY OK' $B/$name.log)" >> $S; }
run e10_s4_ef1 4 10000000000 $R10 MN_TREE_EARLY_FREE=1 POOL_LOG=29 ECALC_LIVE=1
run e10_s4_ef0 4 10000000000 $R10 POOL_LOG=29 ECALC_LIVE=1
run e9_s2_ef1 2 1000000000 $R9 MN_TREE_EARLY_FREE=1 POOL_LOG=29 ECALC_LIVE=1
run e9_s3_ef1 3 1000000000 $R9 MN_TREE_EARLY_FREE=1 POOL_LOG=29 ECALC_LIVE=1
run e9_s3g3_ef1 3 1000000000 $R9 MN_TREE_EARLY_FREE=1 MN_GROUPS=3 POOL_LOG=29 ECALC_LIVE=1 ECALC_VERBOSE=1
run e10_s4g4_ef1 4 10000000000 $R10 MN_TREE_EARLY_FREE=1 MN_GROUPS=4 POOL_LOG=29 ECALC_LIVE=1 ECALC_VERBOSE=1
run e10_s4g4_ef0 4 10000000000 $R10 MN_GROUPS=4 POOL_LOG=29 ECALC_LIVE=1 ECALC_VERBOSE=1
echo "$(date +%T) mnaccept with MN_TREE_EARLY_FREE=1" >> $S
MN_TREE_EARLY_FREE=1 ./mnaccept.sh $J --only unit,mn > $B/mnaccept_ef1.log 2>&1; echo "$(date +%T) mnaccept ef1 rc $? (failures)" >> $S
mv results/mnaccept/$J results/mnaccept/${J}_ef1
./mnaccept.sh $J --only unit,mn > $B/mnaccept_ef0.log 2>&1; echo "$(date +%T) mnaccept ef0 rc $? (failures)" >> $S
echo "$(date) batch done" >> $S

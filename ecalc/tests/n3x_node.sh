#!/bin/bash
# Phase 15 N3x: one unattended node batch.  Usage: bash tests/n3x_node.sh <step list> [node]
#   steps: hash (t_ntt hash: the old tree, the new one off and on, diffed), hashcfg (NTT_MODMUL=0, NTT_PLAN=0,
#          NTT_B16_BODY=0, NTT_B1R=0: old vs new-on), bench (t_ntt r3bench), units (t_ntt3, t_mul, t_newton, t_dist, t_ntt 4c at 2^24)
# Allocates its own job (-J N3x, <= 45 min), runs the steps with srun, cancels the job at the end.
# Logs in ~/N3x15/<step>.log.
set -u
STEPS=${1:-hash,bench}; NODE=${2:-}
D=~/N3x15; mkdir -p $D
NEW=~/ntt-N3x15/ecalc; OLD=~/n3x_old/ecalc
W=""; [ -n "$NODE" ] && W="-w $NODE"
J=$(sbatch -p PPAC_MI300A_SPX -N1 $W --gpus=4 -t 0:45:00 -J N3x --parsable --wrap "sleep 2700")
echo "job $J (steps $STEPS) submitted $(date)" | tee -a $D/jobs.log
while [ "$(squeue -h -j $J -o %T)" != "RUNNING" ]; do sleep 20; done
echo "job $J running on $(squeue -h -j $J -o %N) $(date)" | tee -a $D/jobs.log
run() { srun --jobid=$J -N1 -n1 --gpus=4 bash -lc "module load rocm >/dev/null 2>&1; $*"; }
echo "commit $(git -C ~/ntt-N3x15 log --oneline -1)" | tee -a $D/jobs.log
for s in ${STEPS//,/ }; do
  case $s in
  hash)
    run "cd $OLD && tests/t_ntt_old hash" > $D/hash_old.log 2>&1
    run "cd $NEW && NTT_R3_FUSE=0 tests/t_ntt hash" > $D/hash_off.log 2>&1
    run "cd $NEW && NTT_R3_FUSE=1 tests/t_ntt hash" > $D/hash_on.log 2>&1
    for f in off on; do echo "old vs $f: $(grep -c '^H ' $D/hash_old.log) / $(grep -c '^H ' $D/hash_$f.log) lines, $(diff <(grep '^H ' $D/hash_old.log) <(grep '^H ' $D/hash_$f.log) | grep -c '^>') differ"; done | tee $D/hash_summary.log ;;
  hashcfg)
    for cfg in NTT_MODMUL=0 NTT_PLAN=0 NTT_B16_BODY=0 NTT_B1R=0; do
      run "cd $OLD && $cfg tests/t_ntt_old hash 31 29" > $D/hash_old_$cfg.log 2>&1
      run "cd $NEW && $cfg NTT_R3_FUSE=1 tests/t_ntt hash 31 29" > $D/hash_on_$cfg.log 2>&1
    done
    {
      for cfg in NTT_MODMUL=0 NTT_PLAN=0 NTT_B16_BODY=0 NTT_B1R=0; do echo "$cfg old vs on: $(grep -c '^H ' $D/hash_old_$cfg.log) / $(grep -c '^H ' $D/hash_on_$cfg.log) lines, $(diff <(grep '^H ' $D/hash_old_$cfg.log) <(grep '^H ' $D/hash_on_$cfg.log) | grep -c '^>') differ"; done; } | tee $D/hashcfg_summary.log ;;
  bench) run "cd $NEW && tests/t_ntt r3bench 10 29" > $D/bench.log 2>&1 ;;
  units)
    run "cd $NEW && NTT_R3_FUSE=1 tests/t_ntt3" > $D/t_ntt3.log 2>&1
    run "cd $NEW && NTT_R3_FUSE=1 tests/t_ntt 24 4c" > $D/t_ntt4c.log 2>&1
    run "cd $NEW && NTT_R3_FUSE=1 tests/t_mul 20" > $D/t_mul.log 2>&1
    run "cd $NEW && NTT_R3_FUSE=1 tests/t_newton 20" > $D/t_newton.log 2>&1
    run "cd $NEW && NTT_R3_FUSE=1 tests/t_dist" > $D/t_dist.log 2>&1
    grep -H "VERIFY" $D/t_ntt3.log $D/t_ntt4c.log $D/t_mul.log $D/t_newton.log $D/t_dist.log | tail -20 > $D/units_summary.log ;;
  esac
  echo "step $s done $(date)" | tee -a $D/jobs.log
done
scancel $J
echo "job $J cancelled $(date)" | tee -a $D/jobs.log

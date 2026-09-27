#!/bin/bash
# Phase 15 N3x: one unattended node batch.  Usage: bash tests/n3x_node.sh <step list> [node]
#   steps: timing (10^11 off/on x3 + the with-file pair, sha1), gates (mnaccept unit,e9,mn + 10^11 digcmp, switches on), hash (t_ntt hash: the old tree, the new one off and on, diffed), hashcfg (NTT_MODMUL=0, NTT_PLAN=0,
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
  timing)   # 10^11, same node: off / on alternating x3 without the digit file, one fuse + RNS_R3_MINK=11 run, then the pair with the file
    REF=~/ntt/ecalc/results/e_1e11.out; WANT=$(cut -c1-40 ~/V214/e_1e11.sha1)
    one() {   # one <tag> <env> [outfile]: a run, its total and process wall
      local tag=$1 env=$2 f=${3:-}
      run "cd $NEW; python3 -c \"import os; fd=os.open('$REF', os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\"; rm -rf /tmp/n3x_e11*; t0=\$(date +%s.%N); env $env ./ecalc 100000000000 $f; t1=\$(date +%s.%N); echo \"PROCESS WALL \$(echo \$t1 \$t0 | awk '{printf \"%.2f\", \$1 - \$2}') s\"" > $D/e11_$tag.log 2>&1
      echo "$tag ($env${f:+, file}): $(grep -a '^total' $D/e11_$tag.log | head -1 | cut -c1-60); $(grep -a 'PROCESS WALL' $D/e11_$tag.log); $(grep -a 'VERIFY\|T2 ' $D/e11_$tag.log | head -1 | cut -c1-80); $(grep -ac 'NTT_R3_FUSE=1' $D/e11_$tag.log) fuse notices" | tee -a $D/timing_summary.log
    }
    for i in 1 2 3; do one off$i NTT_R3_FUSE=0; one on$i NTT_R3_FUSE=1; done
    one mink NTT_R3_FUSE=1\ RNS_R3_MINK=11
    for c in off on; do
      [ $c = on ] && e=NTT_R3_FUSE=1 || e=NTT_R3_FUSE=0
      one file_$c $e /tmp/n3x_e11.out
      run "S=\$(~/ntt-N3x15/tools/unpack_digits /tmp/n3x_e11.out | sha1sum | cut -c1-40); echo \"sha1 \$S ref $WANT\"; [ \"\$S\" = \"$WANT\" ] && echo SHA1 IDENTICAL || echo SHA1 DIFFERS; rm -rf /tmp/n3x_e11*" > $D/e11_file_${c}_sha1.log 2>&1
      echo "file_$c digits: $(tail -1 $D/e11_file_${c}_sha1.log)" | tee -a $D/timing_summary.log
    done ;;
  verbose)  # 10^11 with RNS_VERBOSE=1, off and on: the transform time by tier and length (tests/n3x_lengths.py)
    for c in 0 1; do run "cd $NEW; RNS_VERBOSE=1 NTT_R3_FUSE=$c ./ecalc 100000000000" > $D/e11_verbose$c.log 2>&1; done
    python3 $NEW/tests/n3x_lengths.py $D/e11_verbose0.log $D/e11_verbose1.log > $D/lengths.log 2>&1
    grep -a '^total' $D/e11_verbose0.log $D/e11_verbose1.log >> $D/lengths.log ;;
  gates)    # NTT_R3_FUSE=1 (and RNS_R3_MINK=11): mnaccept unit,e9,mn; 10^11 compared by digcmp.sh
    (cd $NEW && NTT_R3_FUSE=1 RNS_R3_MINK=11 ./mnaccept.sh $J --only unit,e9,mn) > $D/mnaccept.log 2>&1
    grep -E "^(PASS|FAIL)" $D/mnaccept.log | tee $D/gates_summary.log
    echo "fuse notices in the mnaccept logs: $(grep -rl 'NTT_R3_FUSE=1' $NEW/results/mnaccept/$J/ 2>/dev/null | wc -l) files" | tee -a $D/gates_summary.log
    run "cd $NEW; rm -rf /tmp/n3x_g11*; NTT_R3_FUSE=1 RNS_R3_MINK=11 ./ecalc 100000000000 /tmp/n3x_g11.out; echo DIGCMP \$(./digcmp.sh /tmp/n3x_g11.out ~/ntt/ecalc/results/e_1e11.out); rm -rf /tmp/n3x_g11*" > $D/g11.log 2>&1
    echo "10^11 fuse+mink: $(grep -a '^total' $D/g11.log | head -1 | cut -c1-40); $(grep -a DIGCMP $D/g11.log); $(grep -ac 'NTT_R3_FUSE=1' $D/g11.log) fuse notices" | tee -a $D/gates_summary.log ;;
  esac
  echo "step $s done $(date)" | tee -a $D/jobs.log
done
scancel $J
echo "job $J cancelled $(date)" | tee -a $D/jobs.log

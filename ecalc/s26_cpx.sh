#!/bin/bash
# S26 CPX: the CPU-only / unit regression on the new build, on the CPX hold 12332 NIC16B (1 node in CPX mode: 24 GPU partitions, 4 cxi NICs).  NOT armed by its author.
#   Usage: nohup bash ~/s25/s26_cpx.sh > ~/s26/CPX.nohup 2>&1 &    (copy s26_cpx.sh s25_lib.sh there first; the worktree must stay at BUILD_REV)
#   Part 1 (host only, certain): tests/t_roots, t_primes (CPU mode), t_seed, and tests/b_seed64 (S23 chain X: 10-node and 576-node shares, 192 threads) x2 each.
#   Part 2 (GPU tests on a CPX node, INFORMATIONAL: the ecalc tests expect 4 APUs; a failure here is recorded, not an alarm): t_crt, t_dec, t_bs, t_modarith, t_ntt 24, t_mul 20,
#          t_newton 20, t_dbig 0, t_verify with HIP_VISIBLE_DEVICES=0..3 (four CPX partitions) -- each once, 20 min cap.
#   Then it stops (no idle process stays).  The CPX node has no useful 10-node/network role: nothing else is run.
# Outputs in ~/s26/CPX: summary.txt, results.txt (table), log/, ALERT; marker ~/s26/CPX_DONE.
J=12332; BUILD_REV=9d7e9461
HD=$(cd "$(dirname "$0")" && pwd); WT=$HOME/ntt-wt/s25; E=$WT/ecalc; OUT=$HOME/s26/CPX
mkdir -p $OUT/log; rm -f $OUT/ALERT $HOME/s26/CPX_DONE
source $WT/tools/rundriver.sh; rd_init $OUT DONE_UNUSED; RD_MARKER=$HOME/s26/CPX_DONE; rm -f $OUT/DONE_UNUSED
source $HD/s25_lib.sh
check_binary 'ECALC_INIT_TL' "tests/t_roots tests/t_primes tests/t_seed tests/b_seed64 tests/t_crt tests/t_dec tests/t_bs tests/t_modarith tests/t_ntt tests/t_mul tests/t_newton tests/t_dbig tests/t_verify"
cd $E || fail "cd $E"; source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || fail "no MNRUN_MODULES"
[ "$(squeue -j $J -h -o %T 2>/dev/null)" = RUNNING ] || fail "hold $J not RUNNING"
NODE=$(squeue -j $J -h -o %N); [ "$(left_s $J)" -ge 5400 ] || fail "hold $J has $(left_s $J) s left, need >= 5400"
rd_health $J $NODE || fail "$NODE unhealthy at start"
PRE="module unload rocm/7.0.3 >/dev/null 2>&1; module load $MNRUN_MODULES; cd $E;"
: > $OUT/results.txt; nfail=0
# step <label> <expected_s> <info 0|1> <ok-regex> <env words> <command...>
step() { local lab=$1 x=$2 info=$3 re=$4 envw=$5; shift 5
  [ "$(left_s $J)" -ge $(( 2 * x + 900 + 1800 )) ] || { echo "$lab: SKIPPED (hold ending)" >> $OUT/results.txt; return; }
  rd_run $lab $x srun --jobid=$J -N1 -c 192 --overlap bash -lc "$PRE env $envw $*"; local rc=$?
  if [ $rc = 0 ] && grep -aqE "$re" $RD_LAST_LOG && ! grep -aq 'VERIFY FAILED' $RD_LAST_LOG; then echo "$lab: PASS ($(grep -aE "$re" $RD_LAST_LOG | tail -1 | cut -c1-110))  ${RD_LAST_WALL}s" >> $OUT/results.txt
  else echo "$lab: $( [ $info = 1 ] && echo 'FAIL (informational, CPX)' || echo FAIL ) rc $rc: $(tail -2 $RD_LAST_LOG | tr '\n' ' ' | cut -c1-200)" >> $OUT/results.txt
       if [ $info = 0 ]; then nfail=$((nfail+1)); alert "$lab: rc $rc (log $RD_LAST_LOG)"; fi; fi; }
for r in 1 2; do
  step roots_$r 60 0 'VERIFY OK' "" ./tests/t_roots
  step primes_$r 120 0 'VERIFY OK' "" ./tests/t_primes
  step seed_$r 120 0 'VERIFY OK' "" ./tests/t_seed
  step bseed10_$r 240 0 '^R2 done: all identical' "" ./tests/b_seed64 64410000000 10 0,5 192 20000
  step bseed576_$r 240 0 '^R2 done: all identical' "" ./tests/b_seed64 64410000000 576 0,288 192 20000
done
for t in "t_crt" "t_dec" "t_bs" "t_modarith" "t_ntt 24" "t_mul 20" "t_newton 20" "t_dbig 0" "t_verify"; do
  step "gpu_${t// /_}" 1200 1 'VERIFY OK' "HIP_VISIBLE_DEVICES=0,1,2,3" "./tests/$t"
done
{ echo; echo "S26 CPX done $(TZ=America/New_York date), host-only failures: $nfail"; } >> $OUT/results.txt
RD_VERDICT="SUCCESS: host-only failures $nfail; table $OUT/results.txt (GPU-on-CPX steps are informational)"
[ $nfail = 0 ] || RD_VERDICT="FAILED: $nfail host-only steps failed; table $OUT/results.txt"

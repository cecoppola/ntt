#!/bin/bash
# tools/h4_aac6.sh - H4 (PLAN 28.5) validation driver for aac6, fire-and-forget on the login node (results/H4.md).
#   setsid nohup bash ~/ntt-h4/tools/h4_aac6.sh > ~/h4/driver.out 2>&1 < /dev/null &
# Sleeps until H4_START (epoch; default 03:35 Eastern 2026-10-12, after the nightly CI window 00:00-03:30), then three
# single-node holds of <= 45 min (PPAC_MI300A_SPX), each cancelled by this script when its steps end (never left pending:
# a hold that is not RUNNING within PEND_MAX is cancelled and the phase recorded as FAILED):
#   A  tools/gate.sh --build --nodes 1 (1e9 digits identical to the reference), then the five rewritten tests:
#      t_dbig 24, t_mul 20, t_crt 30 (ECALC_NP=3 and 4), t_newton 26 (host flow and NEWTON_DEVICE=1), t_mn_grid at 2 node-processes
#   B  mnaccept.sh --only unit,e9,mn          (the full unit set + t_mn_grid, 10^9 at size 1, sizes 2-4 on one node)
#   C  mnaccept.sh --only ckpt,recheck,corr
# Output: ~/h4/ (summary.txt, log/<step>.log, gate1/, mnaccept summaries copied in), done-marker ~/h4/H4_DONE.
set -u
CLONE=${H4_CLONE:-$HOME/ntt-h4}; OUT=${H4_OUT:-$HOME/h4}; PART=${H4_PART:-PPAC_MI300A_SPX}
START=${H4_START:-$(TZ=America/New_York date -d "2026-10-12 03:35" +%s)}
PEND_MAX=${PEND_MAX:-2400}; PHASE_MAX=${PHASE_MAX:-2500}       # seconds: a phase's steps stop starting after PHASE_MAX of its 45-min hold
E=$CLONE/ecalc; mkdir -p $OUT/log; SUM=$OUT/summary.txt; rm -f $OUT/H4_DONE
J=; NPASS=0; NFAIL=0; T0=
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $SUM; }
pass() { NPASS=$((NPASS + 1)); say "PASS $1: $2"; }
fail() { NFAIL=$((NFAIL + 1)); say "FAIL $1: $2"; }
release() { [ -n "$J" ] && { scancel $J 2>/dev/null; say "hold $J cancelled"; J=; }; }
finish() { release; say "done: $NPASS PASS, $NFAIL FAIL"; echo "$([ $NFAIL = 0 ] && echo SUCCESS || echo FAILED) pass=$NPASS fail=$NFAIL $(date +%s)" > $OUT/H4_DONE; }
trap 'say "driver interrupted"; finish; exit 1' INT TERM
hold() { # name -> J (RUNNING) or 1
  J=$(sbatch -p $PART -N1 --gpus=4 -t 0:45:00 -J h4$1 --parsable --wrap "sleep 2700" < /dev/null 2>>$OUT/log/sbatch.err); J=${J%%;*}
  [ -n "$J" ] || { fail "hold $1" "sbatch failed: $(tail -1 $OUT/log/sbatch.err)"; return 1; }
  local w=0 st; while :; do st=$(squeue -j $J -h -o %T 2>/dev/null); [ "$st" = RUNNING ] && break
    [ -z "$st" ] && { fail "hold $1" "job $J vanished"; J=; return 1; }
    [ $w -ge $PEND_MAX ] && { scancel $J; fail "hold $1" "job $J pending > ${PEND_MAX}s, cancelled"; J=; return 1; }
    sleep 20; w=$((w + 20)); done
  T0=$(date +%s); say "phase $1: hold $J running on $(squeue -j $J -h -o %N)"; return 0; }
left() { echo $(( PHASE_MAX - ($(date +%s) - T0) )); }
R() { local to=$1; shift; timeout $to srun --jobid=$J -N1 -n1 --mem=0 --gpus=4 --overlap bash -lc "module load rocm; cd $E; $*" < /dev/null; }
# test <name> <timeout> <cmd...>: rc 0, a VERIFY OK line and no VERIFY FAILED
test1() { local name=$1 to=$2; shift 2; local log=$OUT/log/$name.log t1=$(date +%s) rc v
  [ $(left) -gt 120 ] || { fail "$name" "no time left in the hold"; return; }
  R $to "$@" > $log 2>&1; rc=$?; v=$(grep -a 'VERIFY' $log | tail -1 | cut -c1-120)
  if [ $rc -eq 0 ] && grep -aq 'VERIFY OK' $log && ! grep -aq 'VERIFY FAILED' $log; then pass "$name" "$v; $(( $(date +%s) - t1 )) s"
  else fail "$name" "rc $rc; ${v:-no VERIFY line}; $(grep -a -m1 'FAILED\|abort\|error\|LIMB_BASE' $log | cut -c1-160)"; fi; }
# ---- wait for the window
say "H4 aac6 driver: clone $CLONE $(git -C $CLONE log --oneline -1 2>/dev/null | cut -c1-60); start at $(TZ=America/New_York date -d @$START '+%F %H:%M ET')"
while [ $(date +%s) -lt $START ]; do s=$(( START - $(date +%s) )); sleep $(( s > 600 ? 600 : s )); done
[ -x $E/ecalc ] || say "note: $E/ecalc not built yet (gate.sh --build builds it)"
# ---- phase A: gate + the five tests
if hold A; then
  log=$OUT/log/gate1.log; t1=$(date +%s)
  bash $CLONE/tools/gate.sh --build --nodes 1 --hold $J --line none --out $OUT/gate1 > $log 2>&1; rc=$?
  g=$(cat $OUT/gate1/gate.txt 2>/dev/null)
  if [ $rc -eq 0 ]; then pass "gate 1 node" "$g"; else fail "gate 1 node" "rc $rc: ${g:-$(tail -2 $log | tr '\n' ' ')}"; fi
  test1 t_dbig_24 1200 ./tests/t_dbig 24
  test1 t_mul_20 1500 ./tests/t_mul 20
  test1 t_crt_30_np3 900 ECALC_NP=3 ./tests/t_crt 30
  test1 t_crt_30_np4 900 ECALC_NP=4 ./tests/t_crt 30
  test1 t_newton_26_host 1500 ./tests/t_newton 26
  test1 t_newton_26_dev 1500 NEWTON_DEVICE=1 ./tests/t_newton 26
  { log=$OUT/log/t_mn_grid_2.log; t1=$(date +%s)
    if [ $(left) -gt 300 ]; then
      ( cd $E && SLURM_JOB_ID=$J timeout 1200 ./mnrun.sh 2 env ./tests/t_mn_grid 1 28 ) > $log 2>&1 < /dev/null; rc=$?
      ok=$(grep -ac 'VERIFY OK' $log)
      if [ $rc -eq 0 ] && [ $ok -ge 1 ] && ! grep -aq 'VERIFY FAILED' $log; then pass "t_mn_grid 2 procs" "$ok VERIFY OK; $(( $(date +%s) - t1 )) s"
      else fail "t_mn_grid 2 procs" "rc $rc, $ok VERIFY OK; $(grep -a -m1 'VERIFY FAILED\|error\|abort' $log | cut -c1-160)"; fi
    else fail "t_mn_grid 2 procs" "no time left in the hold"; fi; }
  release
fi
# ---- phases B, C: mnaccept
mna() { # phase steps
  hold $1 || return
  local log=$OUT/log/mnaccept_$1.log t1=$(date +%s)
  ( cd $E && ECALC_REF=$HOME/ntt/ecalc/ref timeout 2600 ./mnaccept.sh $J --only $2 ) > $log 2>&1 < /dev/null; local rc=$?
  cp $E/results/mnaccept/$J/summary.txt $OUT/mnaccept_$1_summary.txt 2>/dev/null
  local np=$(grep -ac '^PASS' $log) nf=$(grep -ac '^FAIL' $log)
  if [ $rc -eq 0 ] && [ $nf -eq 0 ] && [ $np -ge 1 ]; then pass "mnaccept $2" "$np steps; $(( $(date +%s) - t1 )) s"
  else fail "mnaccept $2" "rc $rc, $np PASS, $nf FAIL: $(grep -a '^FAIL' $log | head -3 | cut -c1-160 | tr '\n' '|')"; fi
  release; }
mna B unit,e9,mn
mna C ckpt,recheck,corr
finish

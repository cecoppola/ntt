#!/bin/bash
# tools/h4c_aac6.sh - H4c (PLAN 28.5) fallback correctness driver for aac6 CPX nodes, fire-and-forget on the login node.
#   setsid nohup bash ~/ntt-h4c/tools/h4c_aac6.sh > ~/h4c/driver.out 2>&1 < /dev/null &
# Sleeps until H4C_START (default 03:31 Eastern next occurrence), then two holds in parallel (SH5_MI300A_CPX, not s12-12), each
# <= 45 min, each cancelled by this script when its phase ends.  CPX env: RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30 (as o4_aac6.sh).
#   A  rewritten tests at larger sizes: t_dbig 24, t_crt 30 (NP 3, 4), t_newton 26 (host, NEWTON_DEVICE=1), t_mn_grid (2 procs)
#   B  LIMB_BASE=2 refusal, mnaccept --only e9, then the mnaccept unit list one test at a time (timed)
# Output ~/h4c/ (summary.txt, log/*.log, phase_A.txt, phase_B.txt); done-marker ~/h4c/H4C_DONE.
set -u
CLONE=${H4C_CLONE:-$HOME/ntt-h4c}; OUT=${H4C_OUT:-$HOME/h4c}; PART=SH5_MI300A_CPX; EXCL=aac6-sh5cpx-b
AT=${H4C_START:-03:31}; START=$(TZ=America/New_York date -d "$AT" +%s); [ $START -lt $(date +%s) ] && START=$((START + 86400))
PEND_MAX=${PEND_MAX:-2400}; PHASE_MAX=${PHASE_MAX:-2500}
E=$CLONE/ecalc; mkdir -p $OUT/log; SUM=$OUT/summary.txt; rm -f $OUT/H4C_DONE $OUT/phase_?.txt
exec 9>$HOME/h4c.lock; flock -n 9 || { echo "already running"; exit 1; }
ENVS="RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30"
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" >> $PF; }
phase() { # name, uses: PF J T0 NPASS NFAIL
  local P=$1; PF=$OUT/phase_$P.txt; J=; NPASS=0; NFAIL=0; : > $PF
  pass() { NPASS=$((NPASS + 1)); say "PASS $1: $2"; }
  fail() { NFAIL=$((NFAIL + 1)); say "FAIL $1: $2"; }
  release() { [ -n "$J" ] && { scancel $J 2>/dev/null; say "hold $J cancelled"; J=; }; }
  trap 'release' EXIT
  J=$(sbatch -p $PART -x $EXCL -N1 --gpus=4 -t 0:45:00 -J h4c$P --parsable --wrap "sleep 2700" < /dev/null 2>>$OUT/log/sbatch_$P.err); J=${J%%;*}
  [ -n "$J" ] || { fail "hold $P" "sbatch failed: $(tail -1 $OUT/log/sbatch_$P.err)"; return; }
  local w=0 st; while :; do st=$(squeue -j $J -h -o %T 2>/dev/null); [ "$st" = RUNNING ] && break
    [ -z "$st" ] && { fail "hold $P" "job $J vanished"; J=; return; }
    [ $w -ge $PEND_MAX ] && { fail "hold $P" "job $J pending > ${PEND_MAX}s, cancelled"; return; }
    sleep 20; w=$((w + 20)); done
  T0=$(date +%s); say "phase $P: hold $J running on $(squeue -j $J -h -o %N)"
  left() { echo $(( PHASE_MAX - ($(date +%s) - T0) )); }
  R() { local to=$1; shift; timeout $to srun --jobid=$J -N1 -n1 --mem=0 --gpus=4 --overlap bash -lc "module load rocm; cd $E; export $ENVS; $*" < /dev/null; }
  # test1 <name> <timeout> <cmd...>: rc 0, VERIFY OK, no VERIFY FAILED; records time and the number of "residues" lines
  test1() { local name=$1 to=$2; shift 2; local lf=$(left); [ $lf -gt 90 ] || { fail "$name" "no time left in the hold"; return; }
    [ $to -gt $((lf - 30)) ] && to=$((lf - 30))
    local log=$OUT/log/$name.log t1=$(date +%s) rc v; R $to "$@" > $log 2>&1; rc=$?; v=$(grep -a 'VERIFY' $log | tail -1 | cut -c1-100)
    local rs=$(grep -ac 'residue' $log) dt=$(( $(date +%s) - t1 ))
    if [ $rc -eq 0 ] && grep -aq 'VERIFY OK' $log && ! grep -aq 'VERIFY FAILED' $log; then pass "$name" "$v; ${dt} s; residue-text lines $rs"
    else fail "$name" "rc $rc; ${v:-no VERIFY line}; ${dt} s; $(grep -a -m1 'FAILED\|abort\|error\|out of memory\|OOM' $log | cut -c1-160)"; fi; }
  if [ $P = A ]; then
    test1 t_dbig_24 600 ./tests/t_dbig 24
    test1 t_crt_30_np3 300 ECALC_NP=3 ./tests/t_crt 30
    test1 t_crt_30_np4 300 ECALC_NP=4 ./tests/t_crt 30
    test1 t_newton_26_host 700 ./tests/t_newton 26
    test1 t_newton_26_dev 700 NEWTON_DEVICE=1 ./tests/t_newton 26
    { log=$OUT/log/t_mn_grid_2.log; t1=$(date +%s)
      if [ $(left) -gt 300 ]; then
        ( cd $E && export $ENVS && SLURM_JOB_ID=$J timeout 600 ./mnrun.sh 2 env ./tests/t_mn_grid 1 28 ) > $log 2>&1 < /dev/null; rc=$?
        ok=$(grep -ac 'VERIFY OK' $log)
        if [ $rc -eq 0 ] && [ $ok -ge 1 ] && ! grep -aq 'VERIFY FAILED' $log; then pass "t_mn_grid 2 procs" "$ok VERIFY OK; $(( $(date +%s) - t1 )) s"
        else fail "t_mn_grid 2 procs" "rc $rc, $ok VERIFY OK; $(( $(date +%s) - t1 )) s; $(grep -a -m1 'VERIFY FAILED\|error\|abort' $log | cut -c1-160)"; fi
      else fail "t_mn_grid 2 procs" "no time left in the hold"; fi; }
  else
    log=$OUT/log/limb_base2.log; R 120 "LIMB_BASE=2 ./ecalc 1000000 /dev/null" > $log 2>&1; rc=$?
    if [ $rc -ne 0 ] && grep -aq 'only decimal limbs' $log; then pass "LIMB_BASE=2 refused" "rc $rc; $(grep -a -m1 'only decimal' $log | cut -c1-110)"
    else fail "LIMB_BASE=2 refused" "rc $rc; $(head -c 200 $log | tr '\n' ' ')"; fi
    log=$OUT/log/mnaccept_e9.log; t1=$(date +%s)
    ( cd $E && export $ENVS && ECALC_REF=$HOME/ntt/ecalc/ref timeout 1200 ./mnaccept.sh $J --only e9 ) > $log 2>&1 < /dev/null; rc=$?
    if [ $rc -eq 0 ] && grep -aq '^PASS e9' $log; then pass "mnaccept e9 (digits vs reference)" "$(grep -a '^PASS e9' $log | cut -c1-120); $(( $(date +%s) - t1 )) s"
    else fail "mnaccept e9 (digits vs reference)" "rc $rc; $(grep -a '^FAIL\|ERROR\|not running' $log | head -2 | cut -c1-160 | tr '\n' '|'); $(( $(date +%s) - t1 )) s"; fi
    test1 unit_t_dbig_0 300 ./tests/t_dbig 0
    test1 unit_t_mul_20 700 ./tests/t_mul 20
    test1 unit_t_newton_20 600 ./tests/t_newton 20
    test1 unit_t_verify 300 ./tests/t_verify
    test1 unit_t_out 300 ./tests/t_out
    test1 unit_t_patch 300 ./tests/t_patch
    test1 unit_t_bs 300 ./tests/t_bs
    test1 unit_t_ntt_24 500 ./tests/t_ntt 24
  fi
  say "phase $P done: $NPASS PASS, $NFAIL FAIL"; release
}
echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') H4c driver: clone $CLONE $(git -C $CLONE log --oneline -1 | cut -c1-60); start $(TZ=America/New_York date -d @$START '+%F %H:%M ET')" > $SUM
while [ $(date +%s) -lt $START ]; do s=$(( START - $(date +%s) )); sleep $(( s > 600 ? 600 : s )); done
phase A & PA=$!
phase B & PB=$!
wait $PA $PB
cat $OUT/phase_A.txt $OUT/phase_B.txt >> $SUM
NF=$(grep -c '^FAIL' $SUM); NP=$(grep -c '^PASS' $SUM)
echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') done: $NP PASS, $NF FAIL (t_ntt 24 may fail only its speed check on CPX, see S44)" >> $SUM
echo "$([ $NF = 0 ] && echo SUCCESS || echo FAILED) pass=$NP fail=$NF $(date +%s)" > $OUT/H4C_DONE

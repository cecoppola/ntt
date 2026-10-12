#!/bin/bash
# tools/h6_aac6.sh - H6: the h4 test-cost reruns on aac6 (results/H4.md 7), fire-and-forget on the login node:
#   setsid nohup bash ~/ntt-h6/tools/h6_aac6.sh > ~/h6/driver.out 2>&1 < /dev/null &
# Three single-node holds (PPAC_MI300A_SPX, <= 45 min each, run in parallel when nodes are free, each cancelled by this
# script when its steps end; a hold not RUNNING within PEND_MAX is cancelled and its steps recorded as FAILED):
#   A  (h4 clone)   t_mul 20 (batch groups by residues), t_newton 26 host, t_mn_grid 1 28 at 2 node-processes
#   B  (h4 clone)   NEWTON_DEVICE=1 t_newton 26
#   C  (main clone) LIMB_BASE=10 NEWTON_DEVICE=1 t_newton 26 (the pre-removal code in decimal, for comparison)
# Every test line of the logs carries the seconds since the test started ([Ns]), so section times can be read off.
# Output ~/h6/ (summary.txt, log/), done-marker ~/h6/H6_DONE (SUCCESS|FAILED pass=N fail=M).
set -u
H4=${H6_H4:-$HOME/ntt-h6}; MAIN=${H6_MAIN:-$HOME/ntt-h6m}; OUT=${H6_OUT:-$HOME/h6}; PART=${H6_PART:-PPAC_MI300A_SPX}
PEND_MAX=${PEND_MAX:-10800}; PHASE_MAX=${PHASE_MAX:-2600}
mkdir -p $OUT/log; SUM=$OUT/summary.txt; rm -f $OUT/H6_DONE $OUT/res_*
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $SUM; }
# phase <name> <clone> <steps...>: one hold; each step "name|timeout|command"
phase() { local P=$1 C=$2 E=$2/ecalc; shift 2; local J T0 st w=0 np=0 nf=0
  res() { echo "$1" >> $OUT/res_$P; say "$1"; }
  J=$(sbatch -p $PART -N1 --gpus=4 -t 0:45:00 -J h6$P --parsable --wrap "sleep 2700" < /dev/null 2>>$OUT/log/sbatch.err); J=${J%%;*}
  [ -n "$J" ] || { res "FAIL hold $P: sbatch failed"; return; }
  while :; do st=$(squeue -j $J -h -o %T 2>/dev/null); [ "$st" = RUNNING ] && break
    [ -z "$st" ] && { res "FAIL hold $P: job $J vanished"; return; }
    [ $w -ge $PEND_MAX ] && { scancel $J; res "FAIL hold $P: job $J pending > ${PEND_MAX}s, cancelled"; return; }
    sleep 30; w=$((w + 30)); done
  T0=$(date +%s); say "phase $P: hold $J running on $(squeue -j $J -h -o %N), clone $(git -C $C log --oneline -1 | cut -c1-50)"
  local s name to cmd log t1 rc v ok
  for s in "$@"; do IFS='|' read -r name to cmd <<< "$s"; log=$OUT/log/$name.log; t1=$(date +%s)
    [ $(( PHASE_MAX - (t1 - T0) )) -gt 300 ] || { res "FAIL $name: no time left in the hold"; continue; }
    if [ "${cmd%% *}" = MNGRID ]; then
      ( cd $E && SLURM_JOB_ID=$J timeout $to ./mnrun.sh 2 env ./tests/t_mn_grid 1 28 ) > $log 2>&1 < /dev/null; rc=$?
      ok=$(grep -ac 'VERIFY OK' $log)
      if [ $rc -eq 0 ] && [ $ok -ge 1 ] && ! grep -aq 'VERIFY FAILED' $log; then res "PASS $name: $ok VERIFY OK; $(( $(date +%s) - t1 )) s"
      else res "FAIL $name: rc $rc, $ok VERIFY OK; $(( $(date +%s) - t1 )) s; $(grep -a -m1 'VERIFY FAILED\|FATAL\|Killed' $log | cut -c1-140)"; fi
    else
      timeout $to srun --jobid=$J -N1 -n1 --mem=0 --gpus=4 --overlap bash -lc "module load rocm; cd $E; set -o pipefail; t0=\$(date +%s); env $cmd 2>&1 | while IFS= read -r l; do echo \"[\$(( \$(date +%s) - t0 ))s] \$l\"; done" > $log 2>&1 < /dev/null; rc=$?
      v=$(grep -a 'VERIFY' $log | tail -1 | cut -c1-100); v=${v#*] }
      if [ $rc -eq 0 ] && grep -aq 'VERIFY OK' $log && ! grep -aq 'VERIFY FAILED' $log; then res "PASS $name: $v; $(( $(date +%s) - t1 )) s"
      else res "FAIL $name: rc $rc; ${v:-no VERIFY line}; $(( $(date +%s) - t1 )) s; last section: $(grep -a '\] --' $log | tail -1 | cut -c1-90)"; fi
    fi
  done
  scancel $J; say "hold $J cancelled"; }
say "H6 aac6 driver: h4 $(git -C $H4 log --oneline -1 | cut -c1-40); main $(git -C $MAIN log --oneline -1 | cut -c1-40)"
phase A $H4 "t_mul_20|1500|stdbuf -oL ./tests/t_mul 20" "t_newton_26_host|1200|stdbuf -oL ./tests/t_newton 26" "t_mn_grid_2|900|MNGRID" &
sleep 60
phase B $H4 "t_newton_26_dev|2550|NEWTON_DEVICE=1 stdbuf -oL ./tests/t_newton 26" &
sleep 60
phase C $MAIN "main_t_newton_26_dev|2550|LIMB_BASE=10 NEWTON_DEVICE=1 stdbuf -oL ./tests/t_newton 26" &
wait
np=$(cat $OUT/res_* 2>/dev/null | grep -c '^PASS'); nf=$(cat $OUT/res_* 2>/dev/null | grep -c '^FAIL')
say "done: $np PASS, $nf FAIL"
echo "$([ $nf = 0 ] && [ $np -ge 5 ] && echo SUCCESS || echo FAILED) pass=$np fail=$nf $(date +%s)" > $OUT/H6_DONE

#!/bin/bash
# tools/h7_aac6.sh - H7: the h4 test-cost runs on aac6 CPX nodes (results/H4.md 7.5), fire-and-forget on the login node:
#   setsid nohup bash ~/ntt-h7/tools/h7_aac6.sh > ~/h7/driver.out 2>&1 < /dev/null &
# Four single-node holds (SH5_MI300A_CPX, <= 45 min each), in parallel, each cancelled by this script when its test ends.
# CPX env: RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30.  A: h4 t_mul 20; B: h4 NEWTON_DEVICE=1 t_newton 26;
# C: main (pre-removal) LIMB_BASE=10 NEWTON_DEVICE=1 t_newton 26; D: h4 t_newton 26 (host, control).
# Every log line carries the seconds since the test started ([Ns]).  Output ~/h7/ (summary.txt, log/), marker ~/h7/DONE.
set -u
H4=$HOME/ntt-h7; MAIN=$HOME/ntt-h7m; OUT=$HOME/h7; PART=SH5_MI300A_CPX; PEND_MAX=${PEND_MAX:-3000}
mkdir -p $OUT/log; SUM=$OUT/summary.txt; rm -f $OUT/DONE $OUT/res_*
ENVS="RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30"
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $SUM; }
phase() { local P=$1 C=$2 name=$3 to=$4 cmd=$5 E=$2/ecalc J w=0 st t1 rc v log=$OUT/log/$3.log
  res() { echo "$1" >> $OUT/res_$P; say "$1"; }
  J=$(sbatch -p $PART -N1 --gpus=4 -t 0:45:00 -J h7$P --parsable --wrap "sleep 2700" < /dev/null 2>>$OUT/log/sbatch.err); J=${J%%;*}
  [ -n "$J" ] || { res "FAIL $name: sbatch failed"; return; }
  while :; do st=$(squeue -j $J -h -o %T 2>/dev/null); [ "$st" = RUNNING ] && break
    [ -z "$st" ] && { res "FAIL $name: hold $J vanished"; return; }
    [ $w -ge $PEND_MAX ] && { scancel $J; res "FAIL $name: hold $J pending > ${PEND_MAX}s, cancelled"; return; }
    sleep 20; w=$((w + 20)); done
  t1=$(date +%s); say "phase $P ($name): hold $J running on $(squeue -j $J -h -o %N), clone $(git -C $C log --oneline -1 | cut -c1-50)"
  timeout $to srun --jobid=$J -N1 -n1 --mem=0 --gpus=4 --overlap bash -lc "module load rocm; cd $E; export $ENVS; set -o pipefail; t0=\$(date +%s); env $cmd 2>&1 | while IFS= read -r l; do echo \"[\$(( \$(date +%s) - t0 ))s] \$l\"; done" > $log 2>&1 < /dev/null; rc=$?
  v=$(grep -a 'VERIFY' $log | tail -1 | cut -c1-100); v=${v#*] }
  if [ $rc -eq 0 ] && grep -aq 'VERIFY OK' $log && ! grep -aq 'VERIFY FAILED' $log; then res "PASS $name: $v; $(( $(date +%s) - t1 )) s"
  else res "FAIL $name: rc $rc; ${v:-no VERIFY line}; $(( $(date +%s) - t1 )) s; last: $(tail -1 $log | cut -c1-100)"; fi
  scancel $J; say "hold $J cancelled"; }
say "H7 aac6 CPX driver: h4 $(git -C $H4 log --oneline -1 | cut -c1-40); main $(git -C $MAIN log --oneline -1 | cut -c1-40)"
phase A $H4 t_mul_20 2400 "stdbuf -oL ./tests/t_mul 20" &
phase B $H4 t_newton_26_dev 2400 "NEWTON_DEVICE=1 stdbuf -oL ./tests/t_newton 26" &
phase C $MAIN main_t_newton_26_dev 2400 "LIMB_BASE=10 NEWTON_DEVICE=1 stdbuf -oL ./tests/t_newton 26" &
phase D $H4 t_newton_26_host 2400 "stdbuf -oL ./tests/t_newton 26" &
wait
np=$(cat $OUT/res_* 2>/dev/null | grep -c '^PASS'); nf=$(cat $OUT/res_* 2>/dev/null | grep -c '^FAIL')
say "done: $np PASS, $nf FAIL"
echo "$([ $nf = 0 ] && [ $np -ge 4 ] && echo SUCCESS || echo FAILED) pass=$np fail=$nf $(date +%s)" > $OUT/DONE

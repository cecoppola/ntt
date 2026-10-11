#!/bin/bash
# s46_soak6.sh (S46, results/S46.md) - the aac6 1-node hang soak: from START (a sleep-until timer: no Slurm job pending through
# the 00:00-03:30 EDT blackout) 45-min jobs on one PPAC node, back to back until D, ecalc at the defaults (DBIG_QSEL default),
# one run after another, with the hang watchdog: STALL s without log growth -> capture (last log lines, ps -L of our ecalc,
# rocm-smi), then kill our own srun PID. Counts start-up vs post-init hangs (post-init = a "bs: level <n>" line was printed).
#
#   usage: START=<epoch> D=<epoch> setsid nohup bash s46_soak6.sh > OUT/driver.log 2>&1 < /dev/null &   (on the aac6 login node)
#   env  : START, D (epochs; required), OUT (~/batch1011/T1/aac6), WT (~/ntt-s46), NODE (aac6-spx-a), PART (PPAC_MI300A_SPX),
#          DIG (64410000000), VERB (RNS_VERBOSE=1 ECALC_VERBOSE=2 ECALC_INIT_TL=1: print only), RUN_MAX (240 s budgeted per run),
#          STALL (300), TMO (900), JOB_MIN (45), PEND_MAX (600 s pending before cancel + retry), STUB=1 (dry run: stub runs, no Slurm)
#   files: OUT/runs.tsv (as s46_drive.sh; arm = qdef), OUT/stats.txt, OUT/summary.txt, OUT/log, OUT/cap, OUT/ALERT, OUT/job (current
#          job id), OUT/STOP (touch: stop after the run in flight), marker OUT/S46S_DONE.
#   Safety: scancels only jobs it submitted (checked by name b1011T1 and user); kills only its own srun PID; never pkill/pgrep -f.
WT=${WT:-$HOME/ntt-s46}; OUT=${OUT:-$HOME/batch1011/T1/aac6}; STUB=${STUB:-0}
: ${START:?}; : ${D:?}; NODE=${NODE:-aac6-spx-a}; PART=${PART:-PPAC_MI300A_SPX}; JNAME=b1011T1
DIG=${DIG:-64410000000}; VERB=${VERB:-RNS_VERBOSE=1 ECALC_VERBOSE=2 ECALC_INIT_TL=1}
RUN_MAX=${RUN_MAX:-240}; STALL=${STALL:-300}; TMO=${TMO:-900}; JOB_MIN=${JOB_MIN:-45}; PEND_MAX=${PEND_MAX:-600}; PERIOD=${PERIOD:-5}; POLL=${POLL:-20}
mkdir -p $OUT/log $OUT/cap; rm -f $OUT/S46S_DONE
source $WT/tools/rundriver.sh; rd_init $OUT S46S_DONE
E=$WT/ecalc; cd $E || { RD_VERDICT="FAILED: no $E"; exit 2; }
[ $STUB = 1 ] || [ -x ./ecalc ] || { RD_VERDICT="FAILED: no ecalc build in $E"; exit 2; }
ts() { TZ=America/New_York date -d @$1 '+%H:%M:%S %Z'; }
alert() { echo "$(TZ=America/New_York date) $*" >> $OUT/ALERT; rd_say "ALERT: $*"; }
J=none
release() { [ "$J" = none ] && return; if [ $STUB = 1 ]; then rd_say "STUB scancel $J"; J=none; return; fi
  [ "$(squeue -j $J -h -o '%u %j' 2>/dev/null)" = "<user> $JNAME" ] && { rd_say "scancel $J"; scancel $J; }; J=none; echo none > $OUT/job; }
trap 'release; rd_finish' EXIT
rd_say "S46 aac6 soak: node $NODE, START $(ts $START), D $(ts $D), digits $DIG, '$VERB', stub $STUB"
while [ $(date +%s) -lt $START ]; do [ -e $OUT/STOP ] && { RD_VERDICT="SUCCESS: STOP before START"; exit 0; }; s=$((START - $(date +%s))); sleep $((s > 300 ? 300 : s)); done

CRASH_RE='Segmentation fault|ecalc: SEGV \[|core dumped|srun: error: .*(Exited with exit code [1-9]|Killed)|CANCELLED'
stub_cmd() { local r=$((RANDOM % 100)) p=(${STUB_P:-5 10 5}); echo "stub: start"; sleep 2
  if [ $r -lt ${p[0]} ]; then echo "Segmentation fault"; exit 1; elif [ $r -lt $((p[0] + p[1])) ]; then [ $((r % 2)) = 0 ] && echo "bs: level  1 stub"; exec sleep 100000
  elif [ $r -lt $((p[0] + p[1] + p[2])) ]; then echo "VERIFY FAILED stub"; exit 1; fi; echo "bs: level  1 stub"; echo "total 1.0 s"; echo "VERIFY OK"; exit 0; }
capture() { local lab=$1
  { echo "== $lab hang capture at $(TZ=America/New_York date), job $J, pid $RUN_PID"; echo "-- last 30 log lines"; tail -n 30 $OUT/log/$lab.log
    echo "-- ps -L of our ecalc processes; rocm-smi"
    if [ $STUB = 1 ]; then echo "STUB: srun --jobid=$J -N1 --overlap ... ps -L -o pid,tid,wchan:32,stat,comm"
    else timeout 120 srun --jobid=$J -N1 --overlap bash -c 'hostname; for p in $(pgrep -u $USER -x ecalc); do echo "pid $p"; ps -L -o pid,tid,wchan:32,stat,pcpu,comm -p $p; command -v eu-stack >/dev/null && timeout 100 eu-stack -p $p 2>&1 | head -4000; done; (rocm-smi --showuse --showmemuse 2>&1 || true) | head -40' 2>&1; fi
  } > $OUT/cap/$lab.txt 2>&1 < /dev/null; rd_say "hang capture: $OUT/cap/$lab.txt"; }
killrun() { kill -TERM $RUN_PID 2>/dev/null; local w=0; while kill -0 $RUN_PID 2>/dev/null && [ $w -lt 60 ]; do sleep 2; w=$((w+2)); done
            kill -0 $RUN_PID 2>/dev/null && { rd_say "PID $RUN_PID alive after 60 s: SIGKILL"; kill -KILL $RUN_PID 2>/dev/null; }; }
run_one() { local lab=$1; local log=$OUT/log/$lab.log; : > $log
  if [ $STUB = 1 ]; then ( stub_cmd ) >> $log 2>&1 < /dev/null &
  else srun --jobid=$J -N1 --gpus-per-node=4 --overlap --cpu-bind=none bash -lc "module load rocm >/dev/null 2>&1; cd $E; exec env $VERB ECALC_SEGV_TRACE=1 ./ecalc $DIG" >> $log 2>&1 < /dev/null & fi
  RUN_PID=$!; local t0=$(date +%s) last=-1 still=0 crash=0; RUN_WHY=ok
  while kill -0 $RUN_PID 2>/dev/null; do
    sleep $PERIOD; kill -0 $RUN_PID 2>/dev/null || break
    local now=$(date +%s) sz=$(stat -c %s $log 2>/dev/null || echo 0)
    [ $crash = 0 ] && grep -aqE "$CRASH_RE" $log && crash=$now
    if [ $crash -gt 0 ] && [ $((now - crash)) -ge 30 ]; then RUN_WHY=crash; killrun; break; fi
    [ "$sz" = "$last" ] && still=$((still + PERIOD)) || still=0; last=$sz
    if [ $still -ge $STALL ]; then RUN_WHY=hang; rd_say "$lab: HANG (no log growth for $still s): capture, kill $RUN_PID"; capture $lab; killrun; break; fi
    if [ $((now - t0)) -ge $TMO ]; then RUN_WHY=timeout; rd_say "$lab: hard timeout: capture, kill $RUN_PID"; capture $lab; killrun; break; fi
  done
  wait $RUN_PID 2>/dev/null; RUN_RC=$?; RUN_WALL=$(( $(date +%s) - t0 )); }
submit() { local m=$1
  if [ $STUB = 1 ]; then J=88$((RANDOM % 1000)); rd_say "STUB sbatch -p $PART -N1 -w $NODE --gpus=4 --exclusive -t $m -J $JNAME"; JEND=$(( $(date +%s) + m * 60 )); echo $J > $OUT/job; return 0; fi
  J=$(sbatch -p $PART -N1 -w $NODE --gpus=4 --exclusive -t $m -J $JNAME -o $OUT/job_%j.out --parsable --wrap "sleep $((m * 60))") || { J=none; return 1; }
  echo $J > $OUT/job; local t=0 st
  while :; do st=$(squeue -j $J -h -o %T 2>/dev/null); [ "$st" = RUNNING ] && break
    [ -z "$st" ] && { alert "job $J vanished while pending"; J=none; return 1; }
    [ $t -ge $PEND_MAX ] && { alert "job $J pending > $PEND_MAX s ($(squeue -j $J -h -o %R)): cancelled"; release; return 1; }
    sleep $POLL; t=$((t + POLL)); done
  JEND=$(( $(date +%s) + m * 60 )); rd_say "job $J running on $NODE for $m min (to $(ts $JEND))"; }

[ -s $OUT/runs.tsv ] || printf 'label\tcycle\tarm\trc\twall\tresult\tverify\tsegv\tend\thold\ttotal\tphase\n' > $OUT/runs.tsv
k=$(awk -F'\t' 'NR>1' $OUT/runs.tsv | wc -l); jn=0; bad=0
while :; do
  [ -e $OUT/STOP ] && { rd_say STOP; break; }
  m=$(( (D - $(date +%s)) / 60 - 1 )); [ $m -gt $JOB_MIN ] && m=$JOB_MIN
  [ $((m * 60)) -ge $((RUN_MAX + 30)) ] || { rd_say "less than a run's job before D: done"; break; }
  submit $m || { sleep 60; continue; }
  jn=$((jn + 1))
  while [ ! -e $OUT/STOP ]; do
    now=$(date +%s); lim=$(( JEND - 30 )); [ $D -lt $lim ] && lim=$D
    [ $((now + RUN_MAX)) -le $lim ] || break
    k=$((k + 1)); lab=j${jn}_r$k
    run_one $lab; log=$OUT/log/$lab.log
    v=$(rd_verify $log | head -1); sg=$(grep -a -c 'Segmentation fault\|ecalc: SEGV \[' $log)
    tot=$(grep -a -m1 '^total' $log | awk '{print $2}'); [ -n "$tot" ] || tot=NA
    if [ $RUN_WHY = hang ] || [ $RUN_WHY = timeout ]; then res=$RUN_WHY; elif [ $sg -gt 0 ]; then res=segv
    elif [ $RUN_RC = 0 ] && [ "$v" = "VERIFY OK" ]; then res=ok; else res=fail; fi
    grep -aqE 'bs: level +[0-9]' $log && ph=post-init || ph=startup
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $lab $jn qdef $RUN_RC $RUN_WALL $res "$v" $sg "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" $J $tot $ph >> $OUT/runs.tsv
    rd_say "$lab: $res rc $RUN_RC wall ${RUN_WALL}s phase $ph total $tot"
    python3 $E/s46_stats.py $OUT/runs.tsv > $OUT/stats.txt.tmp 2>&1 && mv $OUT/stats.txt.tmp $OUT/stats.txt
    if [ $res != ok ]; then alert "$lab $res rc $RUN_RC phase $ph (log $log)"; bad=$((bad + 1))
      [ $bad -ge 4 ] && { alert "4 consecutive non-OK runs: STOP"; touch $OUT/STOP; }
      [ $res = hang ] || [ $res = timeout ] && { release; break; }   # a fresh job (node state) after a hang
    else bad=0; fi
  done
  release
done
python3 $E/s46_stats.py $OUT/runs.tsv > $OUT/stats.txt 2>&1
RD_VERDICT="SUCCESS $k runs in $jn jobs; $(head -1 $OUT/stats.txt)"

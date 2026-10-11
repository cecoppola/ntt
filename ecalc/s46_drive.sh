#!/bin/bash
# s46_drive.sh (S46, results/S46.md) - the Job A driver of the crash + hang track: a login-node, fire-and-forget loop that runs
# the fast crash reproducer on 2 aac7 nodes in arms interleaved by rotation (base / ECALC_VMM_BG=0 / ECALC_VMM_BG=2), with a hang
# watchdog, the W handoff (own hold A -> hold A2) and a stats file + done-marker at the deadline D. Uses tools/rundriver.sh
# (rd_init / rd_say / rd_verify / rd_health); its own run function adds the hang capture before the kill.
#
#   usage: HOLD=<jobid> D=<epoch> [W=<epoch>] [S=<epoch>] setsid nohup bash s46_drive.sh > OUT/driver.log 2>&1 < /dev/null &
#   env  : HOLD   the running 2-node hold (ours, -J b1011A); required
#          D      epoch: start no run that cannot finish by D (RUN_MAX); then stats, marker, scancel own hold
#          W      epoch of the 6-node window (0 = no handoff): start no run crossing W; at W scancel HOLD, submit A2
#                 (sbatch -N2 -J $A2_NAME --dependency=$A2_DEP -x $EXCL -t <to S>), poll every POLL_S (>= 60 s) until it runs
#          S      epoch every Slurm job must end by (A2's -t)
#          OUT    output dir (~/batch1011/T1/jobA); WT the worktree with the built ecalc (~/ntt-wt/s46)
#          DIG    total digits (10000000000); REPRO the reproducer env (DB_POOL_VMM_CHUNK_GB=0.0625 ECALC_VMM_REPRO=8)
#          ARMS   arm names in order (base bg0 bg2); arm env: base = none, bg0 = ECALC_VMM_BG=0, bg2 = ECALC_VMM_BG=2
#          RUN_MAX seconds budgeted per run for the W / D checks (240); STALL hang watchdog, s without log growth (300);
#          TMO    hard per-run timeout (900); MAXBAD consecutive non-OK runs before STOP (6)
#          STUB=1 dry run: a stub replaces mnrun/ecalc (random OK / segfault / hang / failure) and every Slurm call is simulated
#   files: OUT/runs.tsv (label cycle arm rc wall result verify segv end hold total phase), OUT/stats.txt (s46_stats.py),
#          OUT/summary.txt, OUT/log/<label>.log, OUT/cap/<label>.txt (hang captures), OUT/ALERT, OUT/hold (current hold id),
#          OUT/STOP (touch to stop after the run in flight), marker OUT/S46A_DONE ("SUCCESS ..." or "FAILED: ...").
#   Safety: kills only the PID it started (TERM, 60 s, KILL); scancels only HOLD/A2 after checking they are ours (name b1011A /
#   $A2_NAME, user <user>); never pkill/pgrep -f; ps capture uses pgrep -x ecalc (exact name) read-only.
WT=${WT:-$HOME/ntt-wt/s46}; OUT=${OUT:-$HOME/batch1011/T1/jobA}; STUB=${STUB:-0}
: ${HOLD:?HOLD not set}; : ${D:?D not set}; W=${W:-0}; S=${S:-$D}
DIG=${DIG:-10000000000}; REPRO=${REPRO:-DB_POOL_VMM_CHUNK_GB=0.0625 ECALC_VMM_REPRO=8}; ARMS=(${ARMS:-base bg0 bg2})
RUN_MAX=${RUN_MAX:-240}; STALL=${STALL:-300}; TMO=${TMO:-900}; MAXBAD=${MAXBAD:-6}; POLL_S=${POLL_S:-60}
A2_NAME=${A2_NAME:-b1011w}; A2_DEP=${A2_DEP:-singleton}; EXCL=${EXCL:-aac7-n01,aac7-n61}; HOLD_NAME=${HOLD_NAME:-b1011A}
PERIOD=${PERIOD:-5}
mkdir -p $OUT/log $OUT/cap; rm -f $OUT/S46A_DONE
source $WT/tools/rundriver.sh; rd_init $OUT S46A_DONE
E=$WT/ecalc; cd $E || { RD_VERDICT="FAILED: no $E"; exit 2; }
if [ $STUB = 1 ]; then LINE=stub; else
  source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || { RD_VERDICT="FAILED: aac7env.sh set no MNRUN_MODULES"; exit 2; }
  LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ e16_headline.sh); [ -n "$LINE" ] || { RD_VERDICT="FAILED: no LINE"; exit 2; }
  [ -x ./ecalc ] || { RD_VERDICT="FAILED: no ecalc build"; exit 2; }
fi
echo $HOLD > $OUT/hold
alert() { echo "$(TZ=America/New_York date) $*" >> $OUT/ALERT; rd_say "ALERT: $*"; }
ts() { TZ=America/New_York date -d @$1 '+%H:%M:%S'; }
rd_say "S46 Job A driver: hold $HOLD, D $(ts $D), W $( [ $W -gt 0 ] && ts $W || echo none), S $(ts $S), digits $DIG, repro '$REPRO', arms ${ARMS[*]}, stub $STUB"

# ---- Slurm calls (simulated under STUB=1)
rm -f $OUT/.stubpolls
sl_state() { if [ $STUB = 1 ]; then [ "$1" = 99999 ] && { echo x >> $OUT/.stubpolls; [ $(wc -l < $OUT/.stubpolls) -ge 2 ] && echo RUNNING || echo PENDING; } || echo RUNNING; return; fi
             squeue -j $1 -h -o %T 2>/dev/null; }
sl_ours() { [ $STUB = 1 ] && return 0; [ "$(squeue -j $1 -h -o '%u %j' 2>/dev/null)" = "<user> $2" ]; }
sl_cancel() { if ! sl_ours $1 $2; then alert "not cancelling $1: not our job named $2"; return 1; fi
              rd_say "scancel $1 ($2)"; [ $STUB = 1 ] && return 0; scancel $1; }
sl_submit_a2() { local m=$(( (S - $(date +%s)) / 60 - 1 )); [ $m -ge 10 ] || { alert "A2: only $m min to S: not submitted"; return 1; }
  if [ $STUB = 1 ]; then rd_say "STUB sbatch -N2 -J $A2_NAME --dependency=$A2_DEP -x $EXCL --exclusive -t $m"; echo 99999; return 0; fi
  sbatch -N2 -J $A2_NAME --dependency=$A2_DEP -x $EXCL --exclusive -t $m -o $OUT/holdA2_%j.out --parsable --wrap "sleep $((m * 60))"; }

# ---- one run: background, watchdog (crash text; STALL s without log growth = hang, captured before the kill; TMO)
CRASH_RE='Segmentation fault|ecalc: SEGV \[|core dumped|srun: error: .*(Exited with exit code [1-9]|Killed)|CANCELLED'
stub_cmd() { local r=$((RANDOM % 100)) p=(${STUB_P:-8 4 2}); echo "stub: start"; sleep 2; echo "stub: init"   # STUB_P: % segv, hang, fail
  if [ $r -lt ${p[0]} ]; then echo "ecalc: SEGV [stub]"; echo "Segmentation fault"; exit 1
  elif [ $r -lt $((p[0] + p[1])) ]; then exec sleep 100000
  elif [ $r -lt $((p[0] + p[1] + p[2])) ]; then echo "VERIFY FAILED stub"; exit 1; fi
  echo "mn: self-test, four meshes over 2 nodes at 2^22 points: ok"; sleep 1; echo "total 1.00 s"; echo "all 2 nodes: VERIFY OK"; exit 0; }
capture() { local lab=$1 cf=$OUT/cap/$1.txt
  { echo "== $lab hang capture at $(TZ=America/New_York date), hold $HOLD, pid $RUN_PID"; echo "-- last 30 log lines"; tail -n 30 $OUT/log/$lab.log
    echo "-- ps -L of our ecalc processes on each node"
    if [ $STUB = 1 ]; then echo "STUB: srun --jobid=$HOLD -N2 --ntasks-per-node=1 --overlap ... ps -L -o pid,tid,wchan:32,stat,comm"
    else timeout 120 srun --jobid=$HOLD -N2 --ntasks-per-node=1 --overlap bash -c 'echo "== $(hostname)"; for p in $(pgrep -u $USER -x ecalc); do echo "pid $p"; ps -L -o pid,tid,wchan:32,stat,comm -p $p; done' 2>&1; fi
  } > $cf 2>&1 < /dev/null; rd_say "hang capture: $cf"; }
killrun() { kill -TERM $RUN_PID 2>/dev/null; local w=0; while kill -0 $RUN_PID 2>/dev/null && [ $w -lt 60 ]; do sleep 2; w=$((w+2)); done
            kill -0 $RUN_PID 2>/dev/null && { rd_say "PID $RUN_PID alive after 60 s: SIGKILL"; kill -KILL $RUN_PID 2>/dev/null; }; }
run_one() { # label, env words...  -> RUN_RC RUN_WALL RUN_WHY (ok|crash|hang|timeout)
  local lab=$1; shift; local log=$OUT/log/$lab.log; : > $log
  if [ $STUB = 1 ]; then ( stub_cmd ) >> $log 2>&1 < /dev/null &
  else ( exec env SLURM_JOB_ID=$HOLD MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 900)) ./mnrun.sh 2 env $LINE ECALC_SEGV_TRACE=1 "$@" ./ecalc $DIG ) >> $log 2>&1 < /dev/null & fi
  RUN_PID=$!; local t0=$(date +%s) last=-1 still=0 crash=0; RUN_WHY=ok
  while kill -0 $RUN_PID 2>/dev/null; do
    sleep $PERIOD; kill -0 $RUN_PID 2>/dev/null || break
    local now=$(date +%s) sz=$(stat -c %s $log 2>/dev/null || echo 0)
    if [ $crash = 0 ] && grep -aqE "$CRASH_RE" $log; then crash=$now; fi
    if [ $crash -gt 0 ] && [ $((now - crash)) -ge 30 ]; then RUN_WHY=crash; rd_say "$lab: crash text, still alive 30 s later: kill $RUN_PID"; killrun; break; fi
    [ "$sz" = "$last" ] && still=$((still + PERIOD)) || still=0; last=$sz
    if [ $still -ge $STALL ]; then RUN_WHY=hang; rd_say "$lab: HANG (no log growth for $still s): capture, then kill $RUN_PID"; capture $lab; killrun; break; fi
    if [ $((now - t0)) -ge $TMO ]; then RUN_WHY=timeout; rd_say "$lab: hard timeout $TMO s: capture, kill $RUN_PID"; capture $lab; killrun; break; fi
  done
  wait $RUN_PID 2>/dev/null; RUN_RC=$?; RUN_WALL=$(( $(date +%s) - t0 )); }
healthy() { [ $STUB = 1 ] && return 0; [ "$HOLD" = none ] && return 1; local nl; nl=$(scontrol show hostnames "$(squeue -j $HOLD -h -o %N)" | tr '\n' ' '); [ -n "$nl" ] && rd_health $HOLD $nl >/dev/null 2>&1; }

# ---- the W handoff and the deadline
HANDED=0
handoff() {
  rd_say "W handoff: no run may cross W ($(ts $W)): releasing hold $HOLD"
  sl_cancel $HOLD $HOLD_NAME; HANDED=1; HOLD=none
  local j; j=$(sl_submit_a2) || { alert "A2 submission failed: $j"; return 1; }
  rd_say "A2 = $j submitted ($A2_NAME, dependency $A2_DEP); polling every $POLL_S s"
  echo "$j (A2, pending)" > $OUT/hold
  while :; do
    local st; st=$(sl_state $j)
    [ "$st" = RUNNING ] && break
    [ -z "$st" ] && { alert "A2 $j vanished (state empty)"; return 1; }
    if [ $(( $(date +%s) + POLL_S + RUN_MAX )) -gt $D ]; then rd_say "A2 $j not running before D - run: cancelling it"; sl_cancel $j $A2_NAME; return 1; fi
    [ -e $OUT/STOP ] && { sl_cancel $j $A2_NAME; return 1; }
    sleep $POLL_S
  done
  HOLD=$j; HOLD_NAME=$A2_NAME; echo $HOLD > $OUT/hold; rd_say "A2 $HOLD running on $([ $STUB = 1 ] && echo stub || squeue -j $HOLD -h -o %N)"
  sleep $([ $STUB = 1 ] && echo 1 || echo 20); healthy || { sleep 60; healthy; } || { alert "A2 nodes unhealthy at start"; return 1; }
}
can_start() { # 0 = start a run now; 1 = stop (D reached / handoff failed)
  local now=$(date +%s)
  [ $((now + RUN_MAX)) -le $D ] || { rd_say "deadline: now + $RUN_MAX s > D"; return 1; }
  if [ $W -gt 0 ] && [ $HANDED = 0 ] && [ $((now + RUN_MAX)) -gt $W ]; then handoff || return 1; can_start; return; fi
  return 0; }

# ---- the arms, interleaved by rotation (cycle c starts with arm (c-1) mod n)
armenv() { case $1 in base) echo "";; bg0) echo "ECALC_VMM_BG=0";; bg2) echo "ECALC_VMM_BG=2";; *) echo "BAD_ARM";; esac; }
[ -s $OUT/runs.tsv ] || printf 'label\tcycle\tarm\trc\twall\tresult\tverify\tsegv\tend\thold\ttotal\tphase\n' > $OUT/runs.tsv
healthy || { sleep 60; healthy; } || { RD_VERDICT="FAILED: hold $HOLD unhealthy at start"; exit 1; }
c=$(awk -F'\t' 'NR>1 && $2+0>m {m=$2+0} END {print m+0}' $OUT/runs.tsv); bad=0; stop=0; n=${#ARMS[@]}
while [ $stop = 0 ]; do
  c=$((c+1))
  for i in $(seq 0 $((n-1))); do
    arm=${ARMS[$(( (c - 1 + i) % n ))]}
    [ -e $OUT/STOP ] && { rd_say "STOP file"; stop=1; break; }
    can_start || { stop=1; break; }
    lab=c${c}_$arm
    run_one $lab $REPRO $(armenv $arm)
    log=$OUT/log/$lab.log; v=$(rd_verify $log | head -1); sg=$(grep -a -c 'Segmentation fault\|ecalc: SEGV \[' $log)
    tot=$(sed -E 's/^ *[0-9]+: //' $log | grep -a -m1 '^total' | awk '{print $2}'); [ -n "$tot" ] || tot=NA
    if [ $RUN_WHY = hang ] || [ $RUN_WHY = timeout ]; then res=$RUN_WHY
    elif [ $sg -gt 0 ]; then res=segv
    elif [ $RUN_RC = 0 ] && [ "$v" = "VERIFY OK" ]; then res=ok
    else res=fail; fi
    grep -aqE 'self-test.*: ok|level 1 group' $log && ph=post-init || ph=startup
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $lab $c $arm $RUN_RC $RUN_WALL $res "$v" $sg "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" $HOLD $tot $ph >> $OUT/runs.tsv
    rd_say "$lab: $res rc $RUN_RC wall ${RUN_WALL}s segv $sg phase $ph total $tot"
    if [ $res != ok ]; then
      alert "$lab ($arm) $res rc $RUN_RC phase $ph (log $log)"$'\n'"$(grep -a -m1 -A20 'ecalc: SEGV \[' $log | cut -c1-200)"
      bad=$((bad+1)); healthy || { sleep 90; healthy; } || { alert "hold $HOLD unhealthy after $lab: STOP"; stop=1; break; }
      [ $bad -ge $MAXBAD ] && { alert "$MAXBAD consecutive non-OK runs: STOP"; stop=1; break; }
    else bad=0; fi
    python3 $E/s46_stats.py $OUT/runs.tsv > $OUT/stats.txt.tmp 2>&1 && mv $OUT/stats.txt.tmp $OUT/stats.txt
  done
done
python3 $E/s46_stats.py $OUT/runs.tsv > $OUT/stats.txt 2>&1
[ $STUB = 1 ] || { squeue -s -j $HOLD -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 | while read l; do rd_say "step still listed: $l"; done; }
[ "$HOLD" != none ] && sl_cancel $HOLD $HOLD_NAME
RD_VERDICT="SUCCESS $(awk -F'\t' 'NR>1' $OUT/runs.tsv | wc -l) runs; $(grep -m1 '^arm base' $OUT/stats.txt)"
rd_say "done: $(cat $OUT/stats.txt | head -5 | tr '\n' ' ')"

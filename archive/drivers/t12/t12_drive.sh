#!/bin/bash
# t12_drive.sh (T12, results/T12.md): unattended soak of FI_MR_CACHE_MONITOR=userfaultfd on a 4-node aac7 hold.  Login-node,
# fire-and-forget: back-to-back real-size 4-node runs (6.441e10 digits/node, main defaults + aac7 line + uffd + xstats), no
# digit writes, page-cache eviction + MemAvailable guard before every run, a hang/crash/timeout watchdog, then stats, marker, scancel own hold.
#   usage: HOLD=<jobid> [DEADLINE=<epoch>] [MAXRUNS=90] [OUT=~/t12/soak] [WT=~/ntt-wt/t12] [STUB=1] setsid nohup bash t12_drive.sh > OUT.log 2>&1 < /dev/null &
#   KEEP_HOLD=1 do not scancel the hold at the end; XTRA="K=V ..." extra env words for every run (the one-run check used FI_LOG_LEVEL=info).
#   stop: MAXRUNS runs, DEADLINE (default now+6.5 h, clipped to hold end - 20 min; no run starts unless now+TMO <= it), MAXBAD (6) consecutive
#         non-OK runs, MAXGUARD (10) consecutive guard failures, or a STOP file in OUT.
#   files: OUT/runs.tsv (label n rc class wall total init bs dm a2a_mean mode start_et phase segv maxstill), OUT/stats.txt, OUT/summary.txt,
#          OUT/log/<label>.log, OUT/cap/<label>.txt (hang captures), OUT/ALERT, marker OUT/T12_DONE, OUT/ecalc_t12 (private binary copy).
#   Safety: kills only PIDs it started (or, after a kill, our own leftover ecalc PIDs one by one); scancels only HOLD, after checking name t12soak and user.
WT=${WT:-$HOME/ntt-wt/t12}; OUT=${OUT:-$HOME/t12/soak}; STUB=${STUB:-0}; : ${HOLD:?HOLD not set}
NN=4; DPN=${DPN:-64410000000}; MAXRUNS=${MAXRUNS:-90}; MAXBAD=${MAXBAD:-6}; MAXGUARD=${MAXGUARD:-10}; STALL=${STALL:-300}; TMO=${TMO:-900}
PERIOD=${PERIOD:-5}; MEMGB=${MEMGB:-428}; HOLD_NAME=${HOLD_NAME:-t12soak}; HOLD_USER=${HOLD_USER:-<user>}; SPAN=${SPAN:-23400}
HD=$(cd "$(dirname "$0")" && pwd)
mkdir -p $OUT/log $OUT/cap; rm -f $OUT/T12_DONE $OUT/ALERT
source $WT/tools/rundriver.sh; rd_init $OUT T12_DONE
E=$WT/ecalc; EC=$OUT/ecalc_t12; cd $E || { RD_VERDICT="FAILED: no $E"; exit 2; }
NOW() { date +%s; }
if [ $STUB = 1 ]; then HEND=$(( $(NOW) + 100000 )); LINE=stub; : > $EC
else
  source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || { RD_VERDICT="FAILED: aac7env.sh set no MNRUN_MODULES"; exit 2; }
  LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ e16_headline.sh); [ -n "$LINE" ] || { RD_VERDICT="FAILED: no LINE"; exit 2; }
  case "$LINE" in *MN_T_CHUNK_MB=2048*COMM_LAYER_INTER2=1*COMM_LAYER_VSLOT_POOL=1*) ;; *) RD_VERDICT="FAILED: unexpected LINE: $LINE"; exit 2;; esac
  cp ./ecalc $EC.tmp && mv $EC.tmp $EC && ldd $EC | grep -q libsma || { RD_VERDICT="FAILED: binary copy / no SHMEM"; exit 2; }
  [ "$(squeue -j $HOLD -h -o %T)" = RUNNING ] || { RD_VERDICT="FAILED: hold $HOLD not running"; exit 2; }
  HEND=$(date -d "$(squeue -j $HOLD -h -o %e)" +%s) || HEND=$(( $(NOW) + 25000 ))
  export SLURM_JOB_ID=$HOLD
fi
DEADLINE=${DEADLINE:-$(( $(NOW) + SPAN ))}; [ $DEADLINE -le $((HEND - 1200)) ] || DEADLINE=$((HEND - 1200))
ET() { TZ=America/New_York date -d @$1 '+%m-%d %H:%M:%S'; }
alert() { echo "$(TZ=America/New_York date) $*" >> $OUT/ALERT; rd_say "ALERT: $*"; }
rd_say "T12 driver: hold $HOLD, stub $STUB, digits/node $DPN x $NN, maxruns $MAXRUNS, deadline $(ET $DEADLINE) ET (hold end $(ET $HEND)), stall ${STALL}s tmo ${TMO}s; LINE | $LINE ; binary md5 $(md5sum $EC | cut -c1-12) rev $(git -C $WT rev-parse --short HEAD 2>/dev/null)"
UFF="FI_MR_CACHE_MONITOR=userfaultfd COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1"
CRASH_RE='Segmentation fault|ecalc: SEGV \[|core dumped'

# ---- node calls (simulated under STUB)
nodecall() { # cmd string, run once per node via srun (one task per node)
  if [ $STUB = 1 ]; then return 0; fi
  timeout 150 srun --jobid=$HOLD -N$NN -n$NN --ntasks-per-node=1 --overlap bash -c "$1" 2>&1 < /dev/null; }
prep() { local t0=$SECONDS out v bad
  [ $STUB = 1 ] && return 0
  while :; do
    out=$(nodecall 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done; echo "$(hostname -s) n=$(pgrep -u $USER -x ecalc | wc -l) availGB=$(awk "/MemAvailable/{printf \"%d\", \$2*1024/1e9}" /proc/meminfo)"')
    rd_say "prep: $(echo "$out" | tr '\n' ';' | cut -c1-300)"
    bad=0; echo "$out" | grep -q "n=[1-9]" && bad=2
    [ $(echo "$out" | grep -c availGB=) = $NN ] || bad=3
    for v in $(echo "$out" | grep -o 'availGB=[0-9]*' | cut -d= -f2); do [ "$v" -ge $MEMGB ] || bad=1; done
    [ $bad = 0 ] && return 0
    if [ $bad = 2 ]; then   # our own leftover ecalc after a kill: kill them one PID at a time (user-owned, exact name)
      rd_say "prep: leftover ecalc on a node: killing our own PIDs one by one"
      nodecall 'for p in $(pgrep -u $USER -x ecalc); do kill -TERM $p; done; sleep 15; for p in $(pgrep -u $USER -x ecalc); do kill -KILL $p; done' >/dev/null; sleep 20
    fi
    [ $((SECONDS - t0)) -ge 300 ] && { rd_say "prep: gave up after 300 s (bad=$bad)"; return 1; }
    sleep 30
  done; }

# ---- one run
stub_cmd() { local r=$((RANDOM % 100)) p=(${STUB_P:-4 3 2}); echo "stub: start"; sleep 2
  echo "bs: arenas ok"; sleep 1
  if [ $r -lt ${p[0]} ]; then echo "ecalc: SEGV [stub]"; echo "Segmentation fault"; exec sleep 100000
  elif [ $r -lt $((p[0] + p[1])) ]; then exec sleep 100000
  elif [ $r -lt $((p[0] + p[1] + p[2])) ]; then echo "VERIFY FAILED stub"; exit 1; fi
  local rate=$(awk -v r=$RANDOM 'BEGIN{printf "%.2f", (r%2) ? 10.9 : 13.8}')
  for pe in 0 1 2 3; do echo "   $pe: comm_shmem: pe $pe: all-to-all 100 exchanges, 1.0 s post to completion, 10 GB received = $rate GB/s per APU thread; x"; done
  echo "  0: total   224.72 s   (bs 82.4 + 10dP 0.0 + dm 92.4 + T1 0.4 + dc 3.0 + T2 0.0 = 178.2; init 46.2; other 0.3); VmHWM 28.8 GB"
  echo "mn: all 4 nodes: VERIFY OK"; exit 0; }
capture() { local lab=$1 cf=$OUT/cap/$1.txt
  { echo "== $lab hang capture at $(TZ=America/New_York date), hold $HOLD, pid $RUN_PID"; echo "-- last 30 log lines"; tail -n 30 $OUT/log/$lab.log
    echo "-- ps -L and eu-stack of our ecalc processes on each node"
    if [ $STUB = 1 ]; then echo "STUB capture"; else
      timeout 240 srun --jobid=$HOLD -N$NN -n$NN --ntasks-per-node=1 --overlap bash -c 'echo "== $(hostname)"; for p in $(pgrep -u $USER -x ecalc); do echo "pid $p"; ps -L -o pid,tid,wchan:32,stat,pcpu,comm -p $p; command -v eu-stack >/dev/null && timeout 100 eu-stack -p $p 2>&1 | head -3000; done' 2>&1; fi
  } > $cf 2>&1 < /dev/null; rd_say "capture: $cf"; }
killrun() { kill -TERM $RUN_PID 2>/dev/null; local w=0; while kill -0 $RUN_PID 2>/dev/null && [ $w -lt 60 ]; do sleep 2; w=$((w+2)); done
            kill -0 $RUN_PID 2>/dev/null && { rd_say "PID $RUN_PID alive after 60 s: SIGKILL"; kill -KILL $RUN_PID 2>/dev/null; }; }
run_one() { local lab=$1 log=$OUT/log/$1.log; : > $log
  if [ $STUB = 1 ]; then ( stub_cmd ) >> $log 2>&1 < /dev/null &
  else ( exec env MNRUN_NODES=$NN MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 900)) ./mnrun.sh $NN env $LINE ECALC_SEGV_TRACE=1 $UFF $XTRA $EC $((NN * DPN)) ) >> $log 2>&1 < /dev/null & fi
  RUN_PID=$!; local t0=$(date +%s) last=-1 still=0 crash=0; RUN_WHY=ok; MAXSTILL=0
  while kill -0 $RUN_PID 2>/dev/null; do
    sleep $PERIOD; kill -0 $RUN_PID 2>/dev/null || break
    local now=$(date +%s) sz=$(stat -c %s $log 2>/dev/null || echo 0)
    if [ $crash = 0 ] && grep -aqE "$CRASH_RE" $log; then crash=$now; fi
    if [ $crash -gt 0 ] && [ $((now - crash)) -ge 30 ]; then RUN_WHY=segv; rd_say "$lab: crash text, still alive 30 s later: kill $RUN_PID"; killrun; break; fi
    [ "$sz" = "$last" ] && still=$((still + PERIOD)) || still=0; last=$sz; [ $still -gt $MAXSTILL ] && MAXSTILL=$still
    if [ $still -ge $STALL ]; then RUN_WHY=hang; rd_say "$lab: HANG (no log growth for $still s): capture, then kill $RUN_PID"; capture $lab; killrun; break; fi
    if [ $((now - t0)) -ge $TMO ]; then RUN_WHY=timeout; rd_say "$lab: hard timeout $TMO s: capture, kill $RUN_PID"; capture $lab; killrun; break; fi
  done
  wait $RUN_PID 2>/dev/null; RUN_RC=$?; RUN_WALL=$(( $(date +%s) - t0 )); }

# ---- parse one log: sets TOT INIT BS DM A2A MODE V SG PH
parse() { local pl=$OUT/log/$1.plain; sed -E 's/^ *[0-9]+: //' $OUT/log/$1.log > $pl
  local tl; tl=$(grep -a -m1 '^total' $pl)
  TOT=$(echo "$tl" | awk '{print ($2 ~ /^[0-9.]+$/) ? $2 : "NA"}'); [ -n "$TOT" ] || TOT=NA
  INIT=$(echo "$tl" | sed -nE 's/.*init ([0-9.]+).*/\1/p'); BS=$(echo "$tl" | sed -nE 's/.*\(bs ([0-9.]+).*/\1/p'); DM=$(echo "$tl" | sed -nE 's/.* dm ([0-9.]+).*/\1/p')
  : ${INIT:=NA} ${BS:=NA} ${DM:=NA}
  A2A=$(grep -a 'comm_shmem: pe [0-9]*: all-to-all' $pl | awk '!s[$3]++ {for(i=1;i<=NF;i++) if($i=="GB/s"){v+=$(i-1); n++}} END {if(n>0) printf "%.2f", v/n; else print "NA"}')
  MODE=NA; [ "$A2A" != NA ] && MODE=$(awk -v a=$A2A 'BEGIN{print (a<11.95) ? "slow" : (a>12.3 ? "fast" : "mid")}')
  V=$(rd_verify $pl | head -1); SG=$(grep -a -c 'Segmentation fault\|ecalc: SEGV \[' $pl); grep -aqE 'self-test.*: ok|dm over' $pl && PH=post-init || PH=startup; }
health_stats() { python3 $HD/t12_stats.py $OUT/runs.tsv > $OUT/stats.txt.tmp 2>&1 && mv $OUT/stats.txt.tmp $OUT/stats.txt; }

# ---- main loop
[ -s $OUT/runs.tsv ] || printf 'label\tn\trc\tclass\twall\ttotal\tinit\tbs\tdm\ta2a\tmode\tstart_et\tphase\tsegv\tmaxstill\n' > $OUT/runs.tsv
n=$(awk 'NR>1' $OUT/runs.tsv | wc -l); bad=0; gbad=0; why="MAXRUNS"
while :; do
  [ -e $OUT/STOP ] && { why="STOP file"; break; }
  [ $n -ge $MAXRUNS ] && { why="$MAXRUNS runs done"; break; }
  [ $(( $(NOW) + TMO )) -le $DEADLINE ] || { why="deadline $(ET $DEADLINE)"; break; }
  [ $STUB = 1 ] || [ "$(squeue -j $HOLD -h -o %T 2>/dev/null)" = RUNNING ] || { why="hold $HOLD not running"; alert "$why"; break; }
  if ! prep; then gbad=$((gbad+1)); alert "guard failed ($gbad in a row)"; [ $gbad -ge $MAXGUARD ] && { why="$MAXGUARD guard failures"; break; }; sleep 60; continue; fi
  gbad=0; n=$((n+1)); lab=r$(printf %03d $n); st=$(TZ=America/New_York date +%m-%d_%H:%M:%S)
  run_one $lab; parse $lab
  if [ $RUN_WHY = segv ] || [ $SG -gt 0 ]; then cls=segv
  elif [ $RUN_WHY = hang ] || [ $RUN_WHY = timeout ]; then cls=$RUN_WHY
  elif [ $RUN_RC = 0 ] && [ "$V" = "VERIFY OK" ]; then cls=ok
  else cls=fail; fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $lab $n $RUN_RC $cls $RUN_WALL $TOT $INIT $BS $DM $A2A $MODE $st $PH $SG $MAXSTILL >> $OUT/runs.tsv
  rd_say "$lab: $cls rc $RUN_RC wall ${RUN_WALL}s total $TOT init $INIT bs $BS dm $DM a2a $A2A ($MODE) phase $PH maxstill ${MAXSTILL}s"
  if [ $cls != ok ]; then alert "$lab $cls rc $RUN_RC phase $PH (log $OUT/log/$lab.log)"$'\n'"$(grep -a -m1 -A20 'ecalc: SEGV \[' $OUT/log/$lab.plain | cut -c1-200)"
    bad=$((bad+1)); [ $bad -ge $MAXBAD ] && { why="$MAXBAD consecutive non-OK runs"; alert "$why: STOP"; break; }
  else bad=0; fi
  health_stats
done
rd_say "loop over: $why"
health_stats; python3 $HD/t12_stats.py $OUT/runs.tsv > $OUT/stats.txt 2>&1
[ $STUB = 1 ] || squeue -s -j $HOLD -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 | while read l; do rd_say "step still listed: $l"; done
RD_VERDICT="SUCCESS ($why): $(grep -m1 '^runs' $OUT/stats.txt)"
if [ "${KEEP_HOLD:-0}" = 1 ]; then rd_say "KEEP_HOLD=1: hold $HOLD kept"
elif [ $STUB = 1 ]; then rd_say "STUB: would scancel $HOLD"
elif [ "$(squeue -j $HOLD -h -o '%u %j' 2>/dev/null)" = "$HOLD_USER $HOLD_NAME" ]; then rd_say "scancel $HOLD"; scancel $HOLD
else alert "not cancelling $HOLD: not our job named $HOLD_NAME"; fi
rd_say "done: $(head -8 $OUT/stats.txt | tr '\n' ' ')"

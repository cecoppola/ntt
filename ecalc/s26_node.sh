#!/bin/bash
# S26: one single-node driver per node of hold 12377 HOLDB; three run in parallel (roles A B C), each on its own node.  NOT armed by its author.
#   Usage (from ~/s25 after copying s26_node.sh s25_lib.sh s25_phases.awk there; the worktree must stay at BUILD_REV):  nohup bash ~/s25/s26_node.sh A > ~/s26/A.nohup 2>&1 &
#   role A  x9000c1s5b0n0  reliability soak (TASKS A1 / 06_EVALUATION B12): one process, four APUs, 1e9 digits, repeated until the hold has 45 min left
#   role B  x9000c1s5b1n0  the same soak at the 10-node per-node share 6.441e10 digits (one-node wall variance too)
#   role C  x9000c1s6b0n0  A37-Q9 one-node by-phase breakdown at 6.441e10 x3 (bs: level lines -> phases/), then the kernel benches (t_ntt bench 31 q4sweep real/NOP
#                          R N N R; t_ntt r3bench 22 29 x2; t_ntt has NO 5*2^k / 7*2^k lengths, only 2^k and 3*2^k), then the 1e9 soak as role A
#   Every run: page cache of the reference files evicted first; a run that outlives its hang timeout is a HANG: eu-stack of every thread, the wchan histogram, rocm-smi and
#   the log's tail are captured (ptrace_scope is 0 here, gdb is not installed), then SIGTERM to ecalc, SIGKILL only if it is still alive after 60 s.  Counted ok / bad (rc or
#   VERIFY) / diff (digits vs the 1e9 reference, every 10th run when the digit compare works) / hang.  The soak never starts a run that would end < 45 min before the hold's end.
# Outputs in ~/s26/<role>/: summary.txt, results.txt (digest, rewritten every 20 runs and at exit), soak.tsv, soak_counts.txt, hang/<n>_capture.txt, log/ (kept for bad/hang runs),
#   res/, phases/, ALERT; the marker is ~/s26/<node>_DONE (SUCCESS: ... | FAILED: ...).  Early clean stop: touch ~/s26/<role>/STOP (between runs).
# Never cancels anything, never touches another job; kills only the PIDs it started / the one ecalc on its own node.
J=12377; BUILD_REV=9d7e9461; ROLE=${1:-}
case $ROLE in A) NODE=x9000c1s5b0n0;; B) NODE=x9000c1s5b1n0;; C) NODE=x9000c1s6b0n0;; *) echo "usage: $0 A|B|C"; exit 2;; esac
HD=$(cd "$(dirname "$0")" && pwd); WT=$HOME/ntt-wt/s25; E=$WT/ecalc; OUT=$HOME/s26/$ROLE
END_MARGIN=2700        # the last run ends >= 45 min before the hold's end
D1=64410000000         # one node's share at the 3.71e13 target
mkdir -p $OUT/log $OUT/res $OUT/phases $OUT/hang
rm -f $OUT/ALERT $OUT/STOP $HOME/s26/${NODE}_DONE
source $WT/tools/rundriver.sh; rd_init $OUT DONE_UNUSED; RD_MARKER=$HOME/s26/${NODE}_DONE; rm -f $OUT/DONE_UNUSED
source $HD/s25_lib.sh

check_binary 'ntt-size-stats ECALC_INIT_TL' "tests/t_ntt tests/t_ntt_nop"
cd $E || fail "cd $E"
source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"
[ "$(squeue -j $J -h -o %T 2>/dev/null)" = RUNNING ] || fail "hold $J not RUNNING"
scontrol show hostnames "$(squeue -j $J -h -o %N)" | grep -qx $NODE || fail "$NODE is not a node of hold $J"
[ "$(left_s $J)" -ge 10800 ] || fail "hold $J has $(left_s $J) s left, need >= 10800 at start"
rd_health $J $NODE || fail "$NODE unhealthy at start (leftover ecalc / t_* process or no meminfo)"
av=$(timeout 60 srun --jobid=$J -N1 -w $NODE --overlap awk '/MemAvailable/{print $2}' /proc/meminfo 2>/dev/null)
[ "${av:-0}" -ge 380000000 ] || fail "$NODE MemAvailable ${av:-?} kB < 380 GB"
rd_say "role $ROLE on $NODE, hold $J, $(left_s $J) s left, MemAvailable $av kB"

PRE="module unload rocm/7.0.3 >/dev/null 2>&1; module load $MNRUN_MODULES; cd $E;"
N() { timeout ${NT:-120} srun --jobid=$J -N1 -w $NODE --overlap bash -c "$*" < /dev/null; }
evict_node() { N 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done' >/dev/null 2>&1; sleep 2; }
# time_ok <run_bound_s>: a run of at most that long still ends >= END_MARGIN before the hold's end, and no STOP file
time_ok() { [ -e $OUT/STOP ] && return 1; [ "$(left_s $J)" -ge $(( $1 + END_MARGIN )) ]; }
# one_run <label> <expected_s> "<env words>" <digits> : a watchdog (rd_run) run; sets RUN_V RUN_T; 0 if rc 0 and VERIFY OK
one_run() { local lab=$1 x=$2 envw=$3 d=$4
  [ "$(left_s $J)" -ge $(( 2 * x + 900 + 600 )) ] || { alert "$lab: hold $J too short for this run"; return 1; }
  evict_node
  rd_run $lab $x srun --jobid=$J -N1 -w $NODE --gpus=4 -c 192 --overlap bash -lc "$PRE env $envw ./ecalc $d"; local rc=$?
  RUN_V=$(rd_verify $RD_LAST_LOG); RUN_T=$(grep -a -m1 '^total' $RD_LAST_LOG | cut -c1-140)
  rd_say "$lab on $NODE: rc $rc wall ${RD_LAST_WALL}s $RUN_V | $RUN_T"
  if [ $rc != 0 ] || [ "$RUN_V" != "VERIFY OK" ]; then alert "$lab on $NODE: rc $rc $RUN_V (log $RD_LAST_LOG)"; return 1; fi
  return 0; }

# ---- the soak ---------------------------------------------------------------------------------------------------------------------
nok=0; nbad=0; nhang=0; ndiff=0; nrun=0; DIGCMP=1; consec_bad=0
echo -e "n\tstatus\trc\twall_s\ttotal_line" > $OUT/soak.tsv
capture() { # capture <n>: the state of the hung ecalc on the node, then SIGTERM, SIGKILL only after 60 s
  local c=$OUT/hang/$1_capture.txt
  { echo "== hang capture $(TZ=America/New_York date) node $NODE run $1"
    NT=150 N 'p=$(pgrep -u $USER -x ecalc); echo "ecalc pids: $p"; for q in $p; do ps -o pid,stat,etime,pcpu,rss,nlwp -p $q; echo "-- wchan histogram:"; for t in /proc/$q/task/*; do cat $t/wchan 2>/dev/null; echo; done | sort | uniq -c | sort -rn | head -20; echo "-- eu-stack (all threads):"; timeout 90 eu-stack -p $q 2>&1 | head -600; done; echo "-- rocm-smi:"; rocm-smi --showuse --showmemuse 2>&1 | grep -v "^=*$" | head -30'
    echo "-- tail of the log:"; tail -40 $OUT/log/soak_cur.log; } > $c 2>&1
  N 'p=$(pgrep -u $USER -x ecalc); [ -n "$p" ] && kill -TERM $p 2>/dev/null; for i in $(seq 60); do pgrep -u $USER -x ecalc >/dev/null || exit 0; sleep 1; done; p=$(pgrep -u $USER -x ecalc); [ -n "$p" ] && kill -KILL $p 2>/dev/null; sleep 2; true' >/dev/null 2>&1; }
# soak_run <digits> <hang_s> "<env words>" <digcmp 0|1>: one run; sets SOAK_ST (ok|bad|diff|hang)
soak_run() { local d=$1 hang=$2 envw=$3 dc=$4 log=$OUT/log/soak_cur.log of="" t0=$SECONDS pid rc c v
  nrun=$((nrun+1)); : > $log
  if [ "$dc" = 1 ] && [ $DIGCMP = 1 ]; then of=/tmp/s26_$ROLE.txt; fi
  evict_node
  t0=$SECONDS
  srun --jobid=$J -N1 -w $NODE --gpus=4 -c 192 --overlap bash -lc "$PRE env $envw ./ecalc $d $of" > $log 2>&1 < /dev/null & pid=$!
  SOAK_ST=ok
  while kill -0 $pid 2>/dev/null; do sleep 2
    if [ $((SECONDS - t0)) -ge $hang ]; then SOAK_ST=hang; rd_say "soak $nrun: HANG after ${hang}s: capturing"; capture $nrun
      for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do kill -0 $pid 2>/dev/null || break; sleep 2; done
      kill -0 $pid 2>/dev/null && { kill -TERM $pid 2>/dev/null; sleep 5; kill -0 $pid 2>/dev/null && kill -KILL $pid 2>/dev/null; }
      break; fi
  done
  wait $pid 2>/dev/null; rc=$?; local wall=$((SECONDS - t0))
  v=$(rd_verify $log)
  if [ $SOAK_ST = ok ]; then
    if [ $rc != 0 ] || [ "$v" != "VERIFY OK" ]; then SOAK_ST=bad
    elif [ -n "$of" ]; then c=$(NT=900 N "$E/digcmp.sh $of $HOME/ref/e_1000000000.txt"); [ "$c" = identical ] || SOAK_ST=diff; fi
  fi
  [ -n "$of" ] && N "rm -rf $of $of.*" >/dev/null 2>&1
  local tl; tl=$(grep -a -m1 '^total' $log | cut -c1-120)
  echo -e "$nrun\t$SOAK_ST\t$rc\t$wall\t$tl" >> $OUT/soak.tsv
  case $SOAK_ST in ok) nok=$((nok+1)); consec_bad=0; rm -f $log;; bad) nbad=$((nbad+1)); consec_bad=$((consec_bad+1));; diff) ndiff=$((ndiff+1)); consec_bad=$((consec_bad+1));; hang) nhang=$((nhang+1));; esac
  if [ $SOAK_ST != ok ]; then cp $log $OUT/log/soak_${nrun}_$SOAK_ST.log 2>/dev/null; alert "soak run $nrun on $NODE: $SOAK_ST rc $rc $v ${c:+digits $c} (log $OUT/log/soak_${nrun}_$SOAK_ST.log)"; rm -f $log; fi
  echo "ok $nok bad $nbad diff $ndiff hang $nhang of $nrun ($(TZ=America/New_York date '+%m-%d %H:%M'))" > $OUT/soak_counts.txt; }
soak_digest() { # wall statistics of the OK runs, counts, the list of non-ok runs
  { echo "S26 role $ROLE node $NODE digest $(TZ=America/New_York date), hold $J $(left_s $J) s left"
    echo "soak: $(cat $OUT/soak_counts.txt 2>/dev/null)"
    echo "OK-run wall (s): $(awk -F'\t' '$2=="ok"{t=$4+0;n++;s+=t;ss+=t*t;if(n==1||t<mn)mn=t;if(t>mx)mx=t} END{if(n){m=s/n;printf "n=%d mean %.1f sd %.2f min %d max %d", n,m,(n>1?sqrt((ss-n*m*m)/(n-1)):0),mn,mx}else print "none"}' $OUT/soak.tsv)"
    echo "OK-run 'total' line value (s): $(awk -F'\t' '$2=="ok"{split($5,a," "); t=a[2]+0; if(t>0){n++;s+=t;ss+=t*t;if(n==1||t<mn)mn=t;if(t>mx)mx=t}} END{if(n){m=s/n;printf "n=%d mean %.2f sd %.2f min %.2f max %.2f", n,m,(n>1?sqrt((ss-n*m*m)/(n-1)):0),mn,mx}else print "none"}' $OUT/soak.tsv)"
    echo "non-ok runs:"; awk -F'\t' 'NR>1 && $2!="ok"{print "  run " $1 " " $2 " rc " $3 " wall " $4}' $OUT/soak.tsv
    [ -d $OUT/hang ] && ls $OUT/hang 2>/dev/null | sed 's/^/  capture: /'
    for f in $OUT/res/*.txt; do [ -f "$f" ] && { echo "== $(basename $f)"; head -30 $f | cut -c1-300; }; done
    for f in $OUT/phases/*.txt; do [ -f "$f" ] && { echo "== phases $(basename $f .txt)"; cat $f | cut -c1-200; }; done; } > $OUT/results.txt; }
trap 'soak_digest; rd_finish' EXIT
soak() { # soak <digits> <hang_s> "<env words>" : until the margin / STOP / 5 consecutive bad runs / a hang that leaves the node unhealthy
  local d=$1 hang=$2 envw=$3 k=0
  while time_ok $(( hang + 200 )); do
    k=$((k+1)); soak_run $d $hang "$envw" $(( d == 1000000000 && k % 10 == 1 ? 1 : 0 ))
    [ $((k % 20)) = 0 ] && soak_digest
    if [ $SOAK_ST = hang ]; then rd_health $J $NODE || { RD_VERDICT="FAILED: $NODE unhealthy after the hang in run $nrun (counts: $(cat $OUT/soak_counts.txt))"; alert "$RD_VERDICT"; exit 0; }; fi
    [ $consec_bad -ge 5 ] && { RD_VERDICT="FAILED: 5 consecutive bad runs (counts: $(cat $OUT/soak_counts.txt))"; alert "$RD_VERDICT"; exit 0; }
    [ "$(squeue -j $J -h -o %T -t R)" = RUNNING ] || { RD_VERDICT="FAILED: hold $J no longer RUNNING"; exit 0; }
  done
  rd_say "soak ended: $(cat $OUT/soak_counts.txt 2>/dev/null)"; }

# ---- gate (every role): 1e9, VERIFY OK, digits vs the reference through /tmp on the node (if that compare does not work, DIGCMP=0 and an ALERT line, not a failure)
rd_say "gate: 1e9 with output to /tmp on $NODE"
soak_run 1000000000 240 "" 1
case $SOAK_ST in ok) rd_say "gate OK";; diff) DIGCMP=0; alert "gate: digits DIFFER or the compare is not usable on $NODE; digit compare off for the soak (see summary)"; nbad=0; ndiff=0; nrun=0; nok=0; : > $OUT/soak.tsv;; *) fail "gate FAILED ($SOAK_ST)";; esac
if [ $DIGCMP = 0 ]; then
  rd_say "gate retry without output"; soak_run 1000000000 240 "" 0; [ $SOAK_ST = ok ] || fail "gate (no output) FAILED ($SOAK_ST)"; nrun=0; nok=0; : > $OUT/soak.tsv; echo "ok 0 bad 0 diff 0 hang 0 of 0" > $OUT/soak_counts.txt
else nrun=0; nok=0; : > $OUT/soak.tsv; echo "ok 0 bad 0 diff 0 hang 0 of 0" > $OUT/soak_counts.txt; fi

case $ROLE in
A) soak 1000000000 240 "";;
B) soak $D1 480 "";;
C) for r in 1 2 3; do one_run q9_r$r 200 "ECALC_VERBOSE=2 ECALC_INIT_TL=1" $D1
     grep -aE '^total|^tl |^bs  |^bs: level|VERIFY' $RD_LAST_LOG | cut -c1-500 > $OUT/res/q9_r$r.txt; awk -f $HD/s25_phases.awk $RD_LAST_LOG > $OUT/phases/q9_r$r.txt; done
   n=0; : > $OUT/res/q4sweep.txt
   for lab in real nop nop real; do bin=tests/t_ntt; [ $lab = nop ] && bin=tests/t_ntt_nop; n=$((n+1)); time_ok 1500 || break; evict_node
     rd_run q4s_${lab}_$n 900 srun --jobid=$J -N1 -w $NODE --gpus=4 -c 192 --overlap bash -lc "$PRE ./$bin bench 31 q4sweep"; rc=$?
     { echo "== q4sweep $lab ($bin) rep $n rc $rc"; grep -a 'Q4' $RD_LAST_LOG; } >> $OUT/res/q4sweep.txt; [ $rc = 0 ] || alert "q4sweep $lab rc $rc (log $RD_LAST_LOG)"; done
   n=0; : > $OUT/res/r3bench.txt
   for lab in real real; do n=$((n+1)); time_ok 1500 || break
     rd_run r3b_${lab}_$n 900 srun --jobid=$J -N1 -w $NODE --gpus=4 -c 192 --overlap bash -lc "$PRE ./tests/t_ntt r3bench 22 29"; rc=$?
     { echo "== r3bench $lab rep $n rc $rc"; grep -aE '^[0-9]+ +[0-9]+ \||^3\*2\^k|r3bench' $RD_LAST_LOG; } >> $OUT/res/r3bench.txt; [ $rc = 0 ] || alert "r3bench rc $rc (log $RD_LAST_LOG)"; done
   soak_digest
   soak 1000000000 240 "";;
esac
RD_VERDICT="SUCCESS: role $ROLE: $(cat $OUT/soak_counts.txt 2>/dev/null); digest $OUT/results.txt"

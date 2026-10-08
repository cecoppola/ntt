#!/bin/bash
# S25: the next 10-node batch on hold 12331 HOLDA2, on the new base line (S22 "B" arm: DM_MN_LEAN=1, DIST_CHUNKS default 8, MN_T_CHUNK_MB=2048).  NOT armed by its author.
#   Run from aac7 home under nohup, from ~/s25 (copy s25_batch.sh s25_lib.sh s25_phases.awk there; the worktree must stay at BUILD_REV).
#   (a) gate     2 nodes, 1e9 digits, new switches on (MN_WAIT_STATS NTT_SIZE_STATS ECALC_INIT_TL MN_COMM_MARK), digcmp against ~/ref/e_1000000000.txt
#   (b) rehearsal  10 nodes x 2 runs at 6.441e10 digits/node (6.441e11 total; the 3.71e13 target share), ECALC_INIT_TL=1 MN_WAIT_STATS=1, no digit writes, VERIFY OK
#   (c) A37-Q9   10 nodes x 2 runs, same share: by-phase batch-tier table from the `bs: level` lines (ECALC_VERBOSE=2, in the line) via s25_phases.awk;
#                plus ECALC_INIT_TL=1 (print only).  No extra counters, so the walls are comparable with (b).
#   (d) ladder   per-node shares 6.441e10 7.0e10 7.64e10 8.1e10, one run each with VERIFY, MEM_REPORT_DEVS=1 (in the line).  Before each: MN_PLAN_ONLY (plan check OK) and
#                BS_LAYOUT_ONLY (room: ... fits 1) and the vslot node_with <= 0.95 x the smallest node MemAvailable, on the login node; a share that fails the check is
#                SKIPPED (not run) and ends the ladder; a run that fails (rc / VERIFY) also ends the ladder.
#   (e) repeat   (b)'s run again and again (timing samples) until the hold has < 30 min + one run left, a STOP file appears, 2 consecutive bad runs, or E_MAX runs.
# Outputs in ~/s25: summary.txt, results.txt (digest, rewritten after every run), res/<label>.txt (extracts), log/<label>.log, phases/<label>.txt, e.tsv, ladder.txt,
#   ALERT (any problem), S25_DONE (SUCCESS: ... | FAILED: ...).  To stop early and cleanly: touch ~/s25/STOP (takes effect between runs).
# Never cancels anything; kills only the PID rd_run started (rundriver.sh watchdog); never touches other jobs.  Fire-and-forget.
J=12331; BUILD_REV=9d7e9461
HD=$(cd "$(dirname "$0")" && pwd); WT=$HOME/ntt-wt/s25; E=$WT/ecalc; OUT=$HOME/s25
MARGIN=1800            # keep >= 30 min of the hold unused after the last run
S=64410000000          # one node's share at the 3.71e13 target
E_MAX=${E_MAX:-400}
mkdir -p $OUT/log $OUT/res $OUT/phases
rm -f $OUT/S25_DONE $OUT/ALERT $OUT/STOP; : > $OUT/ladder.txt
source $WT/tools/rundriver.sh; rd_init $OUT S25_DONE
source $HD/s25_lib.sh
[ -f $HD/s25_phases.awk ] || fail "no $HD/s25_phases.awk"

check_binary 'ntt-size-stats NTT_SIZE_STATS MN_WAIT_STATS ECALC_INIT_TL' ""
cd $E || fail "cd $E"
source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"
[ "$(squeue -j $J -h -o %T 2>/dev/null)" = RUNNING ] || fail "hold $J not RUNNING ($(squeue -j $J -h -o %T 2>&1))"
export SLURM_JOB_ID=$J
NODES=$(scontrol show hostnames "$(squeue -j $J -h -o %N)" | tr '\n' ' '); NN=$(echo $NODES | wc -w)
[ "$NN" = 10 ] || fail "hold $J has $NN nodes, need 10"
[ "$(left_s $J)" -ge 7200 ] || fail "hold $J has $(left_s $J) s left, need >= 7200 at start"
rd_say "hold $J nodes: $NODES ($(left_s $J) s left)"
rd_health $J $NODES || fail "unhealthy at start (leftover ecalc / t_* process or no meminfo)"
rd_disk $HOME 20 || fail "home nearly full"
MINAV=999999999
for nd in $NODES; do av=$(timeout 60 srun --jobid=$J -N1 -w $nd --overlap awk '/MemAvailable/{print $2}' /proc/meminfo 2>/dev/null); [ "${av:-0}" -lt $MINAV ] && MINAV=${av:-0}; done
[ "$MINAV" -ge 400000000 ] || fail "smallest MemAvailable $MINAV kB < 400 GB"
rd_say "smallest MemAvailable over the 10 nodes: $MINAV kB"

total=0; bad=0; consec=0
evict() { timeout 120 srun --jobid=$J -N10 -n10 --overlap bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done; rm -f $HOME/s25/gate.txt*' >/dev/null 2>&1; sleep 5; }
# ensure_time <expected_s>: 0 if the run (expected + 600 s slack) ends with >= MARGIN left, and no STOP file
ensure_time() { [ -e $OUT/STOP ] && { rd_say "STOP file: stopping"; return 1; }
  local l; l=$(left_s $J); [ "$l" -ge $(( $1 + 600 + MARGIN )) ] || { rd_say "hold $J has $l s left (< $1 + 600 + $MARGIN): no more runs"; return 1; }; return 0; }
extract() { # extract <label> <log> <full|short>: the interesting lines of a log (short: without the per-level bs lines)
  if [ "$3" = full ]; then grep -aE '^total|wait-stats|ntt-size-stats|comm-mark|^tl |^bs  |^bs: level|^bs: arenas|^bs: .* spans of|VERIFY' $2 | cut -c1-700 > $OUT/res/$1.txt
  else grep -aE '^total|wait-stats|^tl |^bs  |VERIFY' $2 | cut -c1-700 > $OUT/res/$1.txt; fi; }
# run_one <label> <n> <expected_s> <per-node digits> <full|short> <env words...>: sets RUN_RC RUN_V RUN_T RUN_P RUN_WALL; returns 0 if clean
run_one() { local l=$1 n=$2 x=$3 d=$4 ex=$5; shift 5; total=$((total+1))
  evict
  rd_run $l $x env MNRUN_NODES=$n ./mnrun.sh $n env $BASE "$@" ./ecalc $((n * d)); RUN_RC=$?
  RUN_V=$(rd_verify $RD_LAST_LOG); RUN_T=$(grep -a -m1 '^total' $RD_LAST_LOG | cut -c1-120); RUN_WALL=$RD_LAST_WALL
  RUN_P=$(rd_peaks $RD_LAST_LOG 2>/dev/null | grep -m1 'rank 0' | grep -o 'peak=[0-9.]*GB')
  extract $l $RD_LAST_LOG $ex
  [ "$ex" = full ] && awk -f $HD/s25_phases.awk $RD_LAST_LOG > $OUT/phases/$l.txt 2>/dev/null
  rd_say "$l: n=$n d=$d rc $RUN_RC wall ${RUN_WALL}s $RUN_V | $RUN_T $RUN_P"
  if [ $RUN_RC != 0 ] || [ "$RUN_V" != "VERIFY OK" ]; then
    bad=$((bad+1)); consec=$((consec+1)); alert "$l: rc $RUN_RC $RUN_V (log $RD_LAST_LOG)"
    rd_health $J $NODES || { RD_VERDICT="FAILED: unhealthy after $l ($bad bad runs of $total)"; exit 0; }
    [ $consec -ge 2 ] && { RD_VERDICT="FAILED: 2 consecutive bad runs, last $l ($bad bad of $total)"; exit 0; }
    return 1
  fi
  consec=0; return 0; }

digest() {
  { echo "S25 results digest, $(TZ=America/New_York date), worktree $(git -C $WT rev-parse --short HEAD), hold $J ($(left_s $J) s left), base line: DM_MN_LEAN=1 MN_T_CHUNK_MB=2048 (DIST_CHUNKS default 8)"
    echo "runs $total, bad $bad"
    echo; echo "######## (a) gate"; cat $OUT/res/gate.txt 2>/dev/null | cut -c1-300 | head -12
    for grp in b q9; do echo; echo "######## ($( [ $grp = b ] && echo b rehearsal || echo c A37-Q9 )) 10 nodes x 6.441e10"
      for f in $OUT/res/${grp}_r*.txt; do [ -f "$f" ] || continue; echo "== $(basename $f .txt)"; grep -aE '^total|wait-stats max/min|wait-stats node 0:|VERIFY' $f | cut -c1-500; done; done
    echo; echo "######## A37-Q9 by-phase tables (phases/q9_r*.txt; the batch tier rows and tier totals)"
    for f in $OUT/phases/q9_r*.txt; do [ -f "$f" ] || continue; echo "== $(basename $f .txt)"; cat $f | cut -c1-200; done
    echo; echo "######## (d) memory / max-share ladder (ladder.txt)"; cat $OUT/ladder.txt
    echo; echo "######## (e) repeats (e.tsv: label wall_s verify total_line)"; cut -f1-4 $OUT/e.tsv 2>/dev/null | cut -c1-200
    echo "-- total (s) over the OK repeats AND the (b) runs:"
    { cat $OUT/e.tsv 2>/dev/null | awk -F'\t' '$3=="VERIFY OK"{print $4}'; for f in $OUT/res/b_r*.txt; do [ -f "$f" ] && grep -a -m1 '^total' $f; done; } | awk '{t=$2+0; n++; s+=t; ss+=t*t; if(n==1||t<mn)mn=t; if(t>mx)mx=t} END{ if(n){m=s/n; sd=n>1?sqrt((ss-n*m*m)/(n-1)):0; printf "   n=%d mean %.1f sd %.1f min %.1f max %.1f\n", n, m, sd, mn, mx} else print "   none"}'
  } > $OUT/results.txt; }
trap 'digest; rd_finish' EXIT

# --- (a) gate
G=$OUT/gate.txt
ensure_time 300 || fail "no time/STOP at the gate"; evict
rd_run gate 300 env MNRUN_NODES=2 ./mnrun.sh 2 env $BASE MN_WAIT_STATS=1 NTT_SIZE_STATS=1 ECALC_INIT_TL=1 MN_COMM_MARK=1 ./ecalc 1000000000 $G; grc=$?
gv=$(rd_verify $RD_LAST_LOG); gc=$($E/digcmp.sh $G $HOME/ref/e_1000000000.txt); rm -rf $G $G.*
extract gate $RD_LAST_LOG short
gr=$(grep -a 'wait-stats' $RD_LAST_LOG | grep -ac ' ready '); gn=$(grep -ac 'ntt-size-stats' $RD_LAST_LOG)
rd_say "gate: rc $grc $gv digits $gc | wait-stats lines with 'ready': $gr, ntt-size-stats lines: $gn"
if [ $grc != 0 ] || [ "$gv" != "VERIFY OK" ] || ! grep -aq 'mn: all 2 nodes: VERIFY OK' $RD_LAST_LOG || [ "$gc" != identical ]; then fail "GATE FAILED: rc $grc $gv digits $gc (log $RD_LAST_LOG)"; fi
[ "$gr" -ge 1 ] || fail "gate passed but no 'ready' wait-stats lines (MN_WAIT_STATS not effective): see $RD_LAST_LOG"
rd_health $J $NODES || fail "unhealthy after gate"

# --- (b) rehearsal, (c) Q9
for r in 1 2; do ensure_time 450 || break; run_one b_r$r 10 450 $S full ECALC_INIT_TL=1 MN_WAIT_STATS=1; done
for r in 1 2; do ensure_time 450 || break; run_one q9_r$r 10 450 $S full ECALC_INIT_TL=1; done

# --- (d) ladder: plan / layout check on the login node, then the run
fit_check() { # fit_check <per-node digits>: 0 if the plan and layout say the share fits; prints why not
  local D=$1 T=$((D*10)) out lay vs
  out=$(bash -lc "module unload rocm/7.0.3 >/dev/null 2>&1; module load $MNRUN_MODULES >/dev/null 2>&1; cd $E; env $BASE POOL_LOG=31 MN_PLAN_ONLY=$T:10 ./ecalc $T 2>&1; env $BASE POOL_LOG=31 BS_LAYOUT_ONLY=$D:10 ./ecalc $D 2>&1")
  echo "$out" | grep -a '^plan check' | grep -q ' OK ' || { echo "plan check not OK: $(echo "$out" | grep -a '^plan check' | cut -c1-200)"; return 1; }
  lay=$(echo "$out" | grep -a '^room:' | tail -1)
  echo "$lay" | grep -q 'fits 1' || { echo "layout does not fit: $(echo "$lay" | cut -c1-250)"; return 1; }
  vs=$(echo "$out" | grep -a '^vslot:' | tail -1 | sed -n 's/.* node_with \([0-9]*\).*/\1/p')
  [ -n "$vs" ] && awk -v v="$vs" -v a="$MINAV" 'BEGIN{exit !(v > 0.95 * a * 1000)}' && { echo "vslot node_with $vs B > 0.95 x MemAvailable ${MINAV} kB"; return 1; }
  echo "fits (layout $(echo "$lay" | sed -n 's/.*node \([0-9]*\) budget.*/\1/p') B, with vslot ${vs:-?} B, MemAvailable min $MINAV kB)"; return 0; }
for D in 64410000000 70000000000 76400000000 81000000000; do
  ensure_time 900 || { echo "share $D: not run (time/STOP)" >> $OUT/ladder.txt; break; }
  why=$(fit_check $D); frc=$?
  if [ $frc != 0 ]; then echo "share $D: SKIPPED, $why; ladder ends" >> $OUT/ladder.txt; rd_say "ladder $D: SKIPPED ($why)"; break; fi
  run_one ladder_$D 10 800 $D full MEM_REPORT_DEVS=1; lrc=$?
  echo "share $D: $why | rc $RUN_RC wall ${RUN_WALL}s $RUN_V | $RUN_T $RUN_P" >> $OUT/ladder.txt
  rd_peaks $RD_LAST_LOG 2>/dev/null | head -12 | sed 's/^/    /' >> $OUT/ladder.txt
  if [ $lrc != 0 ]; then echo "share $D: FAILED, ladder stops" >> $OUT/ladder.txt; break; fi
done

# --- (e) repeat (b) until the margin
echo -e "label\twall_s\tverify\ttotal_line\tpeak" > $OUT/e.tsv; ne=0
while [ $ne -lt $E_MAX ]; do
  ensure_time 450 || break
  ne=$((ne+1)); run_one e_r$ne 10 450 $S short ECALC_INIT_TL=1 MN_WAIT_STATS=1
  echo -e "e_r$ne\t$RUN_WALL\t$RUN_V\t$RUN_T\t$RUN_P" >> $OUT/e.tsv; digest
  [ -z "$(squeue -j $J -h -o %T -t R)" ] && fail "hold $J no longer RUNNING"
done
RD_VERDICT="SUCCESS: $bad bad runs of $total ($ne repeats); see $OUT/results.txt, ladder.txt, phases/"

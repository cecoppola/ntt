#!/bin/bash
# S24: NTT3P E0 on ONE node of hold 12377 (results/NTT3P.md section 7-8): the tile bandwidth probe, the 2^13 bottom pass, the 13/9/9 prototype.  NOT armed by its author.
# Usage (from the aac7 home, a COPY of this script outside the worktree, under nohup):  NODE=<node of hold 12377> bash ~/s24/s24_batch.sh   (or: bash s24_batch.sh <node>)
#   The binaries are built beforehand (login node, ~/ntt-wt/s24/ecalc: make -B -j6 SHMEM_CRAY=1 tests/t_ntt tests/t_ntt_nop, then
#   `git log -1 --format=%H -- $REVPATHS > tests/t_ntt.built_rev`); this driver NEVER builds: it checks the built rev against the worktree's sources and stops otherwise.
#   Per repetition (x3, interleaved so a drift shows): (1) t_ntt e0 probe 31 (tile read/write bandwidth, 7/9/10-stage tiles, s_lo 4 12 13 17 21 22 24),
#   (2) t_ntt e0 b13 31 (the 2^13 b1 pass against the 2^12 one + the identity check vs the default plan), (3) t_ntt e0 plan 31 (13/9/9 prototype: identity vs the
#   production plan, per-pass times, real modmul), (4) t_ntt_nop e0 plan 31 (the same with the NOP modmul: data movement only; timing only).
#   Before the first run: waits (5-min steps, up to 6 h) until no ecalc / t_* process of ours runs on the node (hold 12377 may be busy with another batch, S23).
# Outputs in ~/s24: build.log (from the build), summary.txt, results.txt (digest), res/*.txt, log/*.log, ALERT (on any problem), S24_DONE (SUCCESS: ... | FAILED: ...).
# Never cancels anything; kills only the PID rd_run started; never touches other jobs.  GO / NO-GO lines are information: they do not fail the driver.
J=12377; WT=$HOME/ntt-wt/s24; E=$WT/ecalc; OUT=$HOME/s24
NODE=${1:-${NODE:-}}
REVPATHS="ecalc/ntt.c ecalc/ntt.h ecalc/modarith.h ecalc/tests/t_ntt.c ecalc/tests/harness.h ecalc/Makefile ecalc/ntt3.c ecalc/bigint.c ecalc/fatal.c"
THR_TILE=1600; THR_TOTAL=80          # NTT3P section 7 go / no-go: 9-stage tile (s_lo 13 / 22) >= 1.6 TB/s; 3-pass real total <= 80 ms at 2^31
mkdir -p $OUT/log $OUT/res
rm -f $OUT/S24_DONE $OUT/ALERT
alert() { { echo "$(TZ=America/New_York date) $*"; } >> $OUT/ALERT; }
source $WT/tools/rundriver.sh; rd_init $OUT S24_DONE
source $WT/ecalc/aac7env.sh >/dev/null 2>&1       # MNRUN_MODULES (the run-time modules)
fail() { alert "$*"; RD_VERDICT="FAILED: $*"; exit 0; }
left_s() { local t; t=$(squeue -j $1 -h -o %L -t R 2>/dev/null); [ -z "$t" ] && { echo 0; return; }
  echo "$t" | awk -F'[-:]' '{n=NF; s=$n+60*$(n-1); if(n>=3) s+=3600*$(n-2); if(n>=4) s+=86400*$(n-3); print s}'; }

# --- the node
[ -n "$NODE" ] || fail "no node given (argument or NODE=)"
[ "$(squeue -j $J -h -o %T 2>/dev/null)" = RUNNING ] || fail "hold $J not RUNNING ($(squeue -j $J -h -o %T 2>&1))"
scontrol show hostnames "$(squeue -j $J -h -o %N)" | grep -qx "$NODE" || fail "node $NODE is not in hold $J ($(squeue -j $J -h -o %N))"
rd_say "S24 start: node $NODE of hold $J ($(left_s $J) s left), worktree $WT at $(git -C $WT rev-parse --short HEAD)"

# --- the binaries: built at the rev of the sources they were built from, sources unchanged since, binaries newer than the sources.  No rebuild here.
cd $E || fail "no $E"
REV_NOW=$(git -C $WT log -1 --format=%H -- $REVPATHS)
REV_BUILT=$(cat $E/tests/t_ntt.built_rev 2>/dev/null)
[ -n "$REV_NOW" ] && [ "$REV_NOW" = "$REV_BUILT" ] || fail "binary rev mismatch: sources' last commit ${REV_NOW:0:10}, tests/t_ntt.built_rev '${REV_BUILT:0:10}' (build first, see the header)"
[ -z "$(git -C $WT status --porcelain -- $REVPATHS)" ] || fail "uncommitted changes in the sources: $(git -C $WT status --porcelain -- $REVPATHS | tr '\n' ' ')"
for b in tests/t_ntt tests/t_ntt_nop; do
  [ -x $E/$b ] || fail "$b missing (build first)"
  for s in ntt.c tests/t_ntt.c ntt.h modarith.h; do [ $E/$b -nt $E/$s ] || fail "$b is older than $s (rebuild)"; done
done
rd_say "binaries: built rev ${REV_BUILT:0:10} = the sources' last commit; t_ntt $(stat -c %y $E/tests/t_ntt | cut -c1-19), t_ntt_nop $(stat -c %y $E/tests/t_ntt_nop | cut -c1-19)"
rd_disk $HOME 5 || fail "home nearly full"

# --- wait until no process of ours runs on the node (5-min steps, 6 h); the node must be idle twice in a row, 60 s apart
node_procs() { timeout 90 srun --jobid=$J -N1 -w $NODE --overlap bash -c 'pgrep -u "$USER" -f "(^|/)(ecalc|t_[A-Za-z0-9_]+)( |$)" | wc -l' 2>/dev/null | tail -1; }
W=0; idle=0
while :; do
  n=$(node_procs)
  if [ "$n" = 0 ]; then
    idle=$((idle+1)); [ $idle -ge 2 ] && break
    sleep 60; W=$((W+60)); continue
  fi
  idle=0
  [ $W -ge 21600 ] && fail "node $NODE still busy (${n:-?} processes of ours) after 6 h"
  [ "$(squeue -j $J -h -o %T 2>/dev/null)" = RUNNING ] || fail "hold $J no longer RUNNING while waiting"
  [ "$(left_s $J)" -ge 2400 ] || fail "hold $J has $(left_s $J) s left while waiting (need >= 2400)"
  rd_say "node $NODE busy (${n:-?} ecalc / t_* processes of ours); waited ${W}s; next check in 300 s"
  sleep 300; W=$((W+300))
done
[ "$(left_s $J)" -ge 2400 ] || fail "hold $J has $(left_s $J) s left after the wait (need >= 2400)"
rd_say "node $NODE idle after waiting ${W}s ($(left_s $J) s of the hold left)"
rd_health $J $NODE || fail "node $NODE unhealthy at start (leftover ecalc / t_* process, no meminfo)"
av=$(timeout 60 srun --jobid=$J -N1 -w $NODE --overlap awk '/MemAvailable/{print $2}' /proc/meminfo 2>/dev/null)
[ "${av:-0}" -ge 80000000 ] || fail "$NODE MemAvailable ${av:-?} kB < 80 GB (the 2^31 runs need 3 x 16 GB)"

PRE="module unload rocm/7.0.3 >/dev/null 2>&1; module load $MNRUN_MODULES; cd $E;"
# run_e0 <label> <expected_s> <binary> <args...>: rc 0 required; for the runs that check results ("verify" in the label) also "VERIFY OK"
run_e0() { local lab=$1 x=$2 bin=$3; shift 3
  rd_run $lab $x srun --jobid=$J -N1 -w $NODE --gpus=4 -c 192 --overlap bash -lc "$PRE ./$bin $*"; local rc=$?
  local v=-; case $lab in *b13*|*plan_real*) v=$(rd_verify $RD_LAST_LOG);; esac
  rd_say "$lab: rc $rc wall ${RD_LAST_WALL}s verify $v"
  grep -aE '^   E0|^E0|^-- |^== ' $RD_LAST_LOG | cut -c1-260 > $OUT/res/$lab.txt
  if [ $rc != 0 ] || { [ "$v" != - ] && [ "$v" != "VERIFY OK" ]; }; then alert "$lab: rc $rc verify $v (log $RD_LAST_LOG)"; return 1; fi
  case $lab in probe*) grep -aq '^E0 GATE' $RD_LAST_LOG || { alert "$lab: no E0 GATE line"; return 1; };; esac
  return 0; }

bad=0
for rep in 1 2 3; do
  run_e0 probe_r$rep 120 tests/t_ntt e0 probe 31 || bad=$((bad+1))
  run_e0 b13_r$rep 300 tests/t_ntt e0 b13 31 || bad=$((bad+1))
  run_e0 plan_real_r$rep 300 tests/t_ntt e0 plan 31 || bad=$((bad+1))
  run_e0 plan_nop_r$rep 300 tests/t_ntt_nop e0 plan 31 || bad=$((bad+1))
done

# --- digest
{ echo "S24 results digest, $(TZ=America/New_York date), node $NODE of hold $J, worktree rev $(git -C $WT rev-parse --short HEAD), binaries built at ${REV_BUILT:0:10}"
  echo "NTT3P go / no-go: 9-stage 16-column tile (rw) at s_lo 13 and 22 >= $THR_TILE GB/s; 3-pass real total (13/9/9) <= $THR_TOTAL ms at 2^31.  Model (results/NTT3P.md section 4): 9-stage pass 20.0 / 22.5 / 25 ms (opt / central / pess), b1-13 26.5 / 30 / 33 ms, today's b1-12 25.8 ms (m), total 66.5 / 75.0 / 83; today 4 passes 90.4 ms (m)."
  echo; echo "######## GATES (all repetitions)"
  grep -aH '^E0 GATE\|^E0 B13 b1r\|^E0 PLAN' $OUT/log/*.log | sed "s|$OUT/log/||" | cut -c1-300
  for f in $(ls $OUT/res/*.txt | sort -V); do echo; echo "######## $(basename $f .txt)"; cat $f; done; } > $OUT/results.txt
# the gate lines as pass / fail summary on the first real repetitions
rd_say "gate 9-stage tile: $(grep -ah '^E0 GATE 9-stage' $OUT/log/probe_r*.log | cut -c1-140 | tr '\n' ';')"
rd_say "gate 3-pass total: $(grep -ah '^E0 GATE 3-pass' $OUT/log/plan_real_r*.log | cut -c1-120 | tr '\n' ';')"
[ $bad = 0 ] && RD_VERDICT="SUCCESS: 12 runs OK (digest $OUT/results.txt)" || RD_VERDICT="FAILED: $bad of 12 runs not OK (see $OUT/ALERT, $OUT/summary.txt)"

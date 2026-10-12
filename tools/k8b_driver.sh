#!/bin/bash
# k8b_driver.sh (K8b) - aac6 fire-and-forget: gate.sh --build (1 node, BS_SEED_PULL=1) then paired ABBA rounds on/off at
# 6.441e10 (1 node, PPAC_MI300A_SPX), stop rule after each round from round 2: p < 0.05 or CI half-width <= HALF s on the
# primary metric (level-1 start time, ECALC_INIT_TL=1), round cap ROUNDS; total time is the secondary metric.
#   arm:  cd ~ && setsid nohup bash ~/k8b/k8b_driver.sh > ~/k8b/driver.log 2>&1 < /dev/null &
#   env:  WT (~/ntt-k8b), DEADLINE_ET (give up waiting for the hold), TL_MIN (45), ROUNDS (4), HALF (0.5), N (64410000000),
#         SWITCH (BS_SEED_PULL=1), SKIP_GATE=1
# Output: ~/k8b/summary.txt, ~/k8b/pairs.tsv, ~/k8b/log/<run>.log, ~/k8b/gate/; marker ~/k8b/K8B_DONE (SUCCESS / FAILED: reason).
# Cancels only its own hold; kills nothing else.
WT=${WT:-$HOME/ntt-k8b}; O=${O:-$HOME/k8b}; mkdir -p $O/log; E=$WT/ecalc; S=$O/summary.txt; PT=$O/pairs.tsv
DEADLINE_ET=${DEADLINE_ET:-"2026-10-13 06:00"}; TL_MIN=${TL_MIN:-45}; ROUNDS=${ROUNDS:-4}; export HALF=${HALF:-0.5}; N=${N:-64410000000}
SWITCH=${SWITCH:-BS_SEED_PULL=1}; SKIP_GATE=${SKIP_GATE:-0}
NPRE=${NPRE:-0}; RUN_TMO=${RUN_TMO:-600}   # K8c: NPRE switch-on runs (all must pass VERIFY) before the ABBA; RUN_TMO per run (s)
VERDICT="FAILED: driver ended early"; J=
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $S; }
fin() { if [ -n "$J" ]; then scancel $J 2>/dev/null; sleep 2; left=$(squeue -s -j $J -h 2>/dev/null | wc -l); say "hold $J cancelled (own job); steps left $left"; fi
        echo "$VERDICT" > $O/K8B_DONE; say "$VERDICT"; }
trap fin EXIT
rm -f $O/K8B_DONE; : > $S; printf "round\tpair\tA_l1\tB_l1\tA_total\tB_total\n" > $PT
say "commit $(git -C $WT log --oneline -1)"
DL=$(TZ=America/New_York date -d "$DEADLINE_ET" +%s)
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:$TL_MIN:00 -J k8b --parsable -o /dev/null --wrap "sleep $((TL_MIN*60))" < /dev/null) || { VERDICT="FAILED: sbatch"; exit 2; }
J=${J%%;*}; say "hold $J submitted"
while [ "$(squeue -j $J -h -o %T)" != RUNNING ]; do
  [ -z "$(squeue -j $J -h)" ] && { VERDICT="FAILED: hold $J vanished"; J=; exit 1; }
  [ $(date +%s) -gt $DL ] && { VERDICT="FAILED: hold did not start before $DEADLINE_ET"; exit 1; }
  sleep 30
done
T0=$(date +%s); say "hold $J RUNNING on node 0"
left() { echo $(( TL_MIN*60 - ($(date +%s) - T0) )); }
# 1. the gate (build + 1e9 digits with the switch on, identical to the reference)
GATE=skipped
if [ "$SKIP_GATE" != 1 ]; then
  bash -lc "module load rocm >/dev/null 2>&1; cd $WT && tools/gate.sh --build --nodes 1 --hold $J --line none --env '$SWITCH ECALC_SEGV_TRACE=1' --out $O/gate" > $O/log/gate.log 2>&1 < /dev/null
  GATE="rc $? | $(tail -1 $O/gate/gate.txt 2>/dev/null)"; say "gate: $GATE"
  grep -q "gate PASS" $O/gate/gate.txt 2>/dev/null || { VERDICT="FAILED: gate did not pass ($GATE)"; exit 1; }
fi
# 2. ABBA rounds
run() { # tag env...
  local tag=$1; shift
  timeout $RUN_TMO srun --jobid=$J -N1 -n1 --mem=0 --gpus=4 -c 192 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; env ECALC_INIT_TL=1 ECALC_VERBOSE=2 ECALC_SEGV_TRACE=1 $* ./ecalc $N" > $O/log/$tag.log 2>&1 < /dev/null
  local rc=$? l1 tot v
  l1=$(grep -a -m1 'bs: level 1 starts' $O/log/$tag.log | awk '{print $2}'); tot=$(grep -a -m1 '^total' $O/log/$tag.log | awk '{print $2}')
  v=$(grep -a -c 'VERIFY OK' $O/log/$tag.log)
  say "$tag rc $rc level1 ${l1:-NA} total ${tot:-NA} VERIFY-OK $v $(grep -a -m1 'seeds were computed' $O/log/$tag.log | sed 's/.*during init (/(/' | cut -c1-170)"
  [ $rc -eq 0 ] && [ "$v" -ge 1 ] && [ -n "$l1" ] && [ -n "$tot" ] || return 1
  R_L1=$l1; R_TOT=$tot; return 0
}
# 1b. K8c: NPRE switch-on runs first, every one must pass (VERIFY OK); any failure ends the batch
for k in $(seq 1 $NPRE); do
  run pre_$k $SWITCH || { VERDICT="FAILED: switch-on run pre_$k failed"; exit 1; }
done
[ $NPRE -gt 0 ] && say "all $NPRE switch-on runs passed (VERIFY OK)"
NEED=400   # seconds per run, with margin (73-77 s measured in K8 plus srun start)
NBAD=0; NR=0
for r in $(seq 1 $ROUNDS); do
  [ $(left) -lt $((4*NEED/2)) ] && { say "round $r skipped: $(left) s left in the hold"; break; }
  ok=1; declare -A L T
  for step in A1 B1 B2 A2; do
    arm=${step:0:1}; if [ $arm = B ]; then e=$SWITCH; else e=; fi
    if run r${r}_$step $e; then L[$step]=$R_L1; T[$step]=$R_TOT; else ok=0; NBAD=$((NBAD+1)); break; fi
  done
  [ $ok = 1 ] || { say "round $r had a failed run; the round is dropped"; [ $NBAD -ge 2 ] && { VERDICT="FAILED: $NBAD failed runs"; exit 1; }; continue; }
  NR=$((NR+1))
  printf "%d\t1\t%s\t%s\t%s\t%s\n%d\t2\t%s\t%s\t%s\t%s\n" $r "${L[A1]}" "${L[B1]}" "${T[A1]}" "${T[B1]}" $r "${L[A2]}" "${L[B2]}" "${T[A2]}" "${T[B2]}" >> $PT
  ST=$(python3 $WT/tools/k8b_stats.py $PT l1 total); say "after round $r: $(echo "$ST" | tr '\n' ';')"
  [ $NR -ge 2 ] && echo "$ST" | grep -q "^STOP 1" && { say "stop rule met after round $r"; break; }
done
[ $NR -ge 2 ] && VERDICT="SUCCESS: $NR ABBA rounds at $N, gate $GATE; stats in $S, pairs in $PT" || VERDICT="FAILED: only $NR complete round(s) (see $S)"

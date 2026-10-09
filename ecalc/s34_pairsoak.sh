#!/bin/bash
# s34_pairsoak.sh (S34; from s32_pairsoak.sh) - ONE worker of a parallel crash soak.  Arm B = BASE + $SOAK_BENV (default ECALC_VMM_BG=0: synchronous VMM mapping).  Runs ecalc runs back to back on its own node set of a Slurm hold,
#   alternating arm A (BASE) and arm B (BASE + ECALC_VMM_BG=0), until the hold has too little time left, a STOP file exists, or the node set is unhealthy.
#   usage: s32_pairsoak.sh <hold> <outdir> <tag> <nodes,comma> <first-arm A|B> <margin_s> <digits-per-node>
#   env  : BASE (the env words of the run line; required, exported by the caller), SOAK_SAFE (default 2), SOAK_ARMS (default "AB"; "A" = base only, the single-process control),
#          MAXBAD (consecutive bad runs before the worker quits, default 3), RUN_S (expected wall for the watchdog, default 450), PORT_BASE (default 20000).
#   Nodes are disjoint between workers (one network program per node).  1 node: plain srun of ./ecalc (as the single-node soaks); >= 2 nodes: mnrun.sh with MNRUN_NODELIST,
#   MNRUN_LABEL=1, a per-worker COMM_PORT range.  Every run: ECALC_SEGV_TRACE=1.  Output: <outdir>/w_<tag>/{log,summary.txt,ALERT}, row per run appended to <outdir>/pairs.tsv:
#   tag nn arm rc wall_s verify total_line end_time nodes.   A non-clean run appends to <outdir>/ALERT (with the SEGV trace).  Never cancels anything; kills only its own srun PID (rd_run).
HOLD=$1; OUT=$2; TAG=$3; NODELIST=$4; FIRST=$5; MARGIN=$6; DPN=$7
BASE=${BASE:?BASE not set}; SOAK_BENV=${SOAK_BENV:-ECALC_VMM_BG=0}; SOAK_ARMS=${SOAK_ARMS:-AB}; MAXBAD=${MAXBAD:-3}; RUN_S=${RUN_S:-450}
WT=${WT:-$HOME/ntt-wt/s34}; E=$WT/ecalc; W=$OUT/w_$TAG; mkdir -p $W/log
source $WT/tools/rundriver.sh; RD_OUTDIR=$W; RD_SUMMARY=$W/summary.txt; rm -f $W/ALERT $W/STOP
cd $E || exit 2; source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || { echo "no MNRUN_MODULES" > $W/ALERT; exit 2; }
export SLURM_JOB_ID=$HOLD
NN=$(echo "$NODELIST" | tr , '\n' | wc -l)
left_s() { local t; t=$(squeue -j $1 -h -o %L -t R 2>/dev/null); [ -z "$t" ] && { echo 0; return; }
  echo "$t" | awk -F'[-:]' '{n=NF; s=$n+60*$(n-1); if(n>=3) s+=3600*$(n-2); if(n>=4) s+=86400*$(n-3); print s}'; }
alert() { echo "$(TZ=America/New_York date) [$TAG] $*" >> $OUT/ALERT; }
healthy() { rd_health $HOLD $(echo "$NODELIST" | tr , ' ') >/dev/null 2>&1; }
armlist() { if [ ${#SOAK_ARMS} = 1 ]; then echo $SOAK_ARMS $SOAK_ARMS; elif [ $FIRST = B ]; then echo "B A"; else echo "A B"; fi; }
read -r A1 A2 <<< "$(armlist)"
healthy || { sleep 60; healthy; } || { alert "unhealthy nodes $NODELIST at start"; exit 1; }
rd_say "worker $TAG nodes $NODELIST nn $NN arms $SOAK_ARMS first $FIRST margin $MARGIN hold $HOLD ($(left_s $HOLD) s left)"
k=0; consec=0; port=$(( ${PORT_BASE:-20000} + 100 * $(echo "$TAG" | tr -dc 0-9 | sed 's/^0*//; s/^$/0/') ))
while :; do
  [ -e $OUT/STOP ] || [ -e $W/STOP ] && { rd_say "STOP"; break; }
  [ "$(squeue -j $HOLD -h -o %T 2>/dev/null)" = RUNNING ] || { rd_say "hold $HOLD not RUNNING"; break; }
  [ "$(left_s $HOLD)" -ge $(( RUN_S + 600 + MARGIN )) ] || { rd_say "hold $HOLD: $(left_s $HOLD) s left (< run + 600 + margin $MARGIN): done"; break; }
  k=$((k+1)); if [ $((k % 2)) = 1 ]; then arm=$A1; else arm=$A2; fi
  if [ $arm = B ]; then cw="$SOAK_BENV"; else cw=""; fi
  lab=${TAG}_r${k}_$arm
  if [ $NN = 1 ]; then
    PRE="module unload rocm/7.0.3 >/dev/null 2>&1; module load $MNRUN_MODULES; cd $E;"
    rd_run $lab $RUN_S srun --jobid=$HOLD -N1 -w $NODELIST --gpus=4 -c 192 --overlap bash -lc "$PRE env ECALC_SEGV_TRACE=1 $cw ./ecalc $DPN" < /dev/null; rc=$?
    P=$W/log/$lab.plain; cp $W/log/$lab.log $P
  else
    p=$(( port + RANDOM % 90 ))
    rd_run $lab $RUN_S env MNRUN_NODES=$NN MNRUN_NODELIST=$NODELIST MNRUN_LABEL=1 COMM_PORT=$p ./mnrun.sh $NN env $BASE ECALC_SEGV_TRACE=1 $cw ./ecalc $((NN * DPN)) < /dev/null; rc=$?
    P=$W/log/$lab.plain; sed -E 's/^ *[0-9]+: //' $W/log/$lab.log > $P
  fi
  wall=$RD_LAST_WALL; v=$(rd_verify $P); tot=$(grep -a -m1 '^total' $P | cut -c1-140)
  sg=$(grep -a -c 'Segmentation fault\|ecalc: SEGV \[' $W/log/$lab.log)   # the rc139 column bug: srun reports rc 1, so count the text
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $TAG $NN $arm $rc $wall "$v" "$tot" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" $NODELIST $sg >> $OUT/pairs.tsv
  rd_say "$lab: rc $rc wall ${wall}s $v | $tot"
  if [ $rc != 0 ] || [ "$v" != "VERIFY OK" ]; then
    consec=$((consec+1))
    alert "$lab (arm $arm, nodes $NODELIST): rc $rc $v (log $W/log/$lab.log)"$'\n'"$(grep -a -m1 -A28 'ecalc: SEGV \[' $W/log/$lab.log | cut -c1-200)"
    healthy || { sleep 90; healthy; } || { alert "unhealthy nodes after $lab: worker stops"; break; }
    [ $consec -ge $MAXBAD ] && { alert "$MAXBAD consecutive bad runs: worker stops"; break; }
  else consec=0; fi
done
rd_say "worker $TAG ended after $k runs"

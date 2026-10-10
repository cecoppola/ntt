#!/bin/bash
# s35_pairsoak.sh (S35; from s34_pairsoak.sh) - ONE worker of the S35 crash soak, run by s35_batch.sh inside its own 4-node job.
#   usage: s35_pairsoak.sh <outdir> <tag> <node1,node2> <first-arm A|C> <digits-per-node>
#   env  : BASE (required), SLURM_JOB_ID (the batch job), SOAK_DEADLINE (epoch s: no run starts after it), WT, RUN_S (default 450),
#          MAXBAD (consecutive bad runs before the worker quits, default 3), PORT_BASE (default 23000).
#   Arms: run k (1, 2, ...) is B (BASE + ECALC_VMM_BG=0, the cost reference) when k % 6 == 0, otherwise A (BASE) and C
#   (BASE + ECALC_VMM_BG=2) alternate, starting with <first-arm>; consecutive non-B runs form the A/C pairs of the paired difference.
#   Every run ECALC_SEGV_TRACE=1, no digit output.  Row per run appended to <outdir>/pairs.tsv:
#   tag nn arm rc wall_s verify total_line end_time nodes segv k ecalc_total_s
#   Unhealthy nodes after a bad run (also re-checked 90 s later): a line in <outdir>/unhealthy.txt, the worker stops; the second line
#   (either worker) touches <outdir>/STOP.  Never cancels anything; kills only its own srun PID (rd_run's watchdog).
OUT=$1; TAG=$2; NODELIST=$3; FIRST=$4; DPN=$5
BASE=${BASE:?BASE not set}; : ${SLURM_JOB_ID:?}; : ${SOAK_DEADLINE:?}; MAXBAD=${MAXBAD:-3}; RUN_S=${RUN_S:-450}
WT=${WT:-$HOME/ntt-wt/s35}; E=$WT/ecalc; W=$OUT/w_$TAG; mkdir -p $W/log
source $WT/tools/rundriver.sh; RD_OUTDIR=$W; RD_SUMMARY=$W/summary.txt; RD_WATCHDOG_PERIOD=2   # 2 s wall resolution (S34's 20 s quantised the walls)
cd $E || exit 2; source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || { echo "no MNRUN_MODULES" >> $OUT/ALERT; exit 2; }
NN=2; JOB=$SLURM_JOB_ID
left_s() { local t; t=$(squeue -j $1 -h -o %L -t R 2>/dev/null); [ -z "$t" ] && { echo 0; return; }
  echo "$t" | awk -F'[-:]' '{n=NF; s=$n+60*$(n-1); if(n>=3) s+=3600*$(n-2); if(n>=4) s+=86400*$(n-3); print s}'; }
alert() { echo "$(TZ=America/New_York date) [$TAG] $*" >> $OUT/ALERT; }
healthy() { rd_health $JOB $(echo "$NODELIST" | tr , ' ') >/dev/null 2>&1; }
unhealthy_event() { echo "$(TZ=America/New_York date) [$TAG] $NODELIST $*" >> $OUT/unhealthy.txt; alert "unhealthy nodes $NODELIST ($*): worker stops"
  [ "$(wc -l < $OUT/unhealthy.txt)" -ge 2 ] && { alert "2 unhealthy-node events: STOP"; touch $OUT/STOP; }; }
if [ $FIRST = C ]; then O1=C; O2=A; else O1=A; O2=C; fi
healthy || { sleep 60; healthy; } || { unhealthy_event "at start"; exit 1; }
rd_say "worker $TAG nodes $NODELIST first $FIRST job $JOB ($(left_s $JOB) s left), deadline $(TZ=America/New_York date -d @$SOAK_DEADLINE)"
k=0; j=0; consec=0; port=$(( ${PORT_BASE:-23000} + 100 * $(echo "$TAG" | tr -dc 0-9 | sed 's/^0*//; s/^$/0/') ))
while :; do
  [ -e $OUT/STOP ] && { rd_say "STOP"; break; }
  [ $(date +%s) -lt $SOAK_DEADLINE ] || { rd_say "soak deadline reached"; break; }
  [ "$(left_s $JOB)" -ge $(( RUN_S + 600 )) ] || { rd_say "job $JOB: $(left_s $JOB) s left (< run + 600): done"; break; }
  k=$((k+1))
  if [ $((k % 6)) = 0 ]; then arm=B; else j=$((j+1)); if [ $((j % 2)) = 1 ]; then arm=$O1; else arm=$O2; fi; fi
  case $arm in A) cw="";; B) cw="ECALC_VMM_BG=0";; C) cw="ECALC_VMM_BG=2";; esac
  lab=${TAG}_r${k}_$arm; p=$(( port + RANDOM % 90 ))
  rd_run $lab $RUN_S env MNRUN_NODES=$NN MNRUN_NODELIST=$NODELIST MNRUN_LABEL=1 COMM_PORT=$p ./mnrun.sh $NN env $BASE ECALC_SEGV_TRACE=1 $cw ./ecalc $((NN * DPN)) < /dev/null; rc=$?
  P=$W/log/$lab.plain; sed -E 's/^ *[0-9]+: //' $W/log/$lab.log > $P
  wall=$RD_LAST_WALL; v=$(rd_verify $P); tot=$(grep -a -m1 '^total' $P | cut -c1-140); ets=$(echo "$tot" | awk '{print ($2 ~ /^[0-9.]+$/) ? $2 : "NA"}')
  sg=$(grep -a -c 'Segmentation fault\|ecalc: SEGV \[' $W/log/$lab.log)   # srun reports rc 1, not 139: count the text
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $TAG $NN $arm $rc $wall "$v" "$tot" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" $NODELIST $sg $k $ets >> $OUT/pairs.tsv
  rd_say "$lab: rc $rc wall ${wall}s $v segv $sg | $tot"
  if [ $rc != 0 ] || [ "$v" != "VERIFY OK" ]; then
    consec=$((consec+1))
    alert "$lab (arm $arm, nodes $NODELIST): rc $rc segv $sg $v (log $W/log/$lab.log)"$'\n'"$(grep -a -m1 -A28 'ecalc: SEGV \[' $W/log/$lab.log | cut -c1-200)"
    healthy || { sleep 90; healthy; } || { unhealthy_event "after $lab"; break; }
    [ $consec -ge $MAXBAD ] && { alert "$MAXBAD consecutive bad runs: worker stops"; break; }
  else consec=0; fi
done
rd_say "worker $TAG ended after $k runs"

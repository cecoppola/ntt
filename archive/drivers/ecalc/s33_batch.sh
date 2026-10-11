#!/bin/bash
# S33: parallel crash soak on the 3-node holds (12377, successor 12390): one 2-node job (alternating arms A = BASE, B = BASE + ECALC_VMM_SAFE=$SOAK_SAFE) + one single-node job
#   (base, ECALC_SEGV_TRACE=1: the single-process control), both at 6.441e10 digits per node, on the s32 build (~/ntt-wt/s32).  First stops the s26 single-node soaks on 12377
#   (touch ~/s26/{A,B,C}/STOP, waits for ~/s26/<node>_DONE).  Runs until 45 min before each hold's end; goes on to the next hold of HOLDS when it is RUNNING (3 nodes).
#   Outputs ~/s33: summary.txt pairs.tsv soak_stats.txt results.txt (digest) ALERT w_*/ ; marker ~/s33/S33_DONE.  Early stop: touch ~/s33/STOP.  Never cancels anything.
BUILD_REV=383327a
HOLDS=${HOLDS:-"12377 12390"}; MARGIN=2700; S=64410000000; RUN_S=450; SOAK_SAFE=${SOAK_SAFE:-2}
HD=$(cd "$(dirname "$0")" && pwd); WT=$HOME/ntt-wt/s32; E=$WT/ecalc; OUT=$HOME/s33
mkdir -p $OUT/log; rm -f $OUT/S33_DONE $OUT/ALERT $OUT/STOP $OUT/pairs.tsv; : > $OUT/pairs.tsv
source $WT/tools/rundriver.sh; rd_init $OUT S33_DONE
source $HD/s31_lib.sh
cd $E || fail "cd $E"
source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"
check_binary 'ECALC_VMM_SAFE ECALC_SEGV_TRACE' ""
digest() { { echo "S33 digest $(TZ=America/New_York date), worktree $(git -C $WT rev-parse --short HEAD), holds $HOLDS, arms A=BASE B=BASE+ECALC_VMM_SAFE=$SOAK_SAFE (2-node job); 1-node job = control (base)"
  echo; echo "crash counts and wall per arm and node count (rc139 = the known crash; other = any other failure or no VERIFY OK):"; cat $OUT/soak_stats.txt 2>/dev/null
  echo; echo "runs (tag nn arm rc wall verify):"; cut -f1-6 $OUT/pairs.tsv | cut -c1-120; echo; echo "ALERT:"; cat $OUT/ALERT 2>/dev/null | cut -c1-220 | head -60; } > $OUT/results.txt; }
soak_stats() { awk -f $HD/pairstats.awk $OUT/pairs.tsv | sort > $OUT/soak_stats.txt
  awk -F'\t' '$4==139 { c[$2 "n/" $3]++ } END { printf "rc139 by node count / arm:"; for (a in c) printf " %s=%d", a, c[a]; printf "\n" }' $OUT/pairs.tsv >> $OUT/soak_stats.txt; }
trap 'soak_stats; digest; rd_finish' EXIT

# stop the s26 single-node soaks (they have enough data) and wait for their markers
if [ "$(squeue -j 12377 -h -o %T 2>/dev/null)" = RUNNING ]; then
  for r in A B C; do touch $HOME/s26/$r/STOP; done
  rd_say "s26 STOP files set; waiting for ~/s26/{x9000c1s5b0n0,x9000c1s5b1n0,x9000c1s6b0n0}_DONE"
  for i in $(seq 120); do n=0; for nd in x9000c1s5b0n0 x9000c1s5b1n0 x9000c1s6b0n0; do [ -e $HOME/s26/${nd}_DONE ] && n=$((n+1)); done; [ $n = 3 ] && break; sleep 30; done
  [ $n = 3 ] || fail "the s26 soaks did not end within 60 min ($n of 3 markers)"
  rd_say "s26 soaks ended: $(cat $HOME/s26/x9000c1s5b0n0_DONE $HOME/s26/x9000c1s5b1n0_DONE $HOME/s26/x9000c1s6b0n0_DONE | cut -c1-120 | tr '\n' ';')"
fi
export BASE SOAK_SAFE RUN_S MARGIN
for SH in $HOLDS; do
  while :; do
    [ -e $OUT/STOP ] && break 2
    st=$(squeue -j $SH -h -o %T 2>/dev/null)
    [ -z "$st" ] && { rd_say "hold $SH is gone: skipped"; continue 2; }
    [ "$st" = RUNNING ] && break
    rd_say "waiting for hold $SH ($st)"; sleep 300
  done
  NODES=$(scontrol show hostnames "$(squeue -j $SH -h -o %N)" | tr '\n' ' ')
  [ "$(echo $NODES | wc -w)" = 3 ] || { rd_say "hold $SH has not 3 nodes: skipped"; continue; }
  [ "$(left_s $SH)" -ge $(( RUN_S + 600 + MARGIN )) ] || { rd_say "hold $SH: too little time left: skipped"; continue; }
  export SLURM_JOB_ID=$SH
  rd_health $SH $NODES || { sleep 90; rd_health $SH $NODES; } || { alert "hold $SH unhealthy at the soak start: skipped"; continue; }
  rd_say "soak on hold $SH: nodes $NODES, $(left_s $SH) s left"
  set -- $NODES
  nohup bash $HD/s32_pairsoak.sh $SH $OUT p0 $1,$2 A $MARGIN $S > $OUT/w_p0.nohup 2>&1 < /dev/null & PIDS="$!"
  sleep 20
  SOAK_ARMS=A PORT_BASE=21000 nohup bash $HD/s32_pairsoak.sh $SH $OUT s1 $3 A $MARGIN $S > $OUT/w_s1.nohup 2>&1 < /dev/null & PIDS="$PIDS $!"
  while :; do alive=0; for p in $PIDS; do kill -0 $p 2>/dev/null && alive=1; done; [ $alive = 0 ] && break
    sleep 600; soak_stats; digest; done
  soak_stats; digest
done
soak_stats; digest
RD_VERDICT="SUCCESS: parallel soak done: $(tr '\n' ';' < $OUT/soak_stats.txt | cut -c1-400) (digest $OUT/results.txt)"

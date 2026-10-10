#!/bin/bash
# S34: A/B crash soak of the synchronous-VMM-mapping switch ECALC_VMM_BG=0 (results/S34.md: the rc139 segfault is hipMemMap in vmm_bg_map's thread).
#   Arms: A = BASE (the S32/S33 base, ~/s32/summary.txt line 1), B = BASE + ECALC_VMM_BG=0.  All runs ECALC_SEGV_TRACE=1, 6.441e10 digits per node.
#   (0) waits for ~/s32/S32_DONE and ~/s33/S33_DONE (the old soaks, stopped by their STOP files).
#   (a) 10-node hold chain "12331 12389": 5 disjoint 2-node workers p0..p4 (first arm alternating), until 30 min before each hold's end.
#   (b) 3-node hold chain "12377 12390": a 2-node worker p0 and a 1-node worker s1 (control), both alternating A/B, until 45 min before each hold's end.
#   (c) gate (parallel to the soak): 2 nodes x 1e9 digits with ECALC_VMM_BG=0 on the 3-node hold's first two nodes, digcmp vs ~/ref/e_1000000000.txt; a failed gate touches STOP.
#   Outputs ~/s34: results.txt (digest) pairs.tsv (tag nn arm rc wall verify total end nodes segv) soak_stats.txt gate.txt ALERT summary.txt w_*/; marker ~/s34/S34_DONE.
#   Early stop: touch ~/s34/STOP.  Never cancels anything; kills only its own srun.
BUILD_REV=8424ed1
H10=${H10:-"12331 12389"}; H3=${H3:-"12377 12390"}
HD=$(cd "$(dirname "$0")" && pwd); WT=$HOME/ntt-wt/s34; E=$WT/ecalc; OUT=$HOME/s34; export WT
S=64410000000; RUN_S=450; export RUN_S SOAK_BENV=ECALC_VMM_BG=0
mkdir -p $OUT/log; rm -f $OUT/S34_DONE $OUT/ALERT $OUT/STOP $OUT/pairs.tsv; : > $OUT/pairs.tsv
source $WT/tools/rundriver.sh; rd_init $OUT S34_DONE
source $HD/s31_lib.sh
cd $E || fail "cd $E"
source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"
# own binary check (the worktree HEAD is a prefix of BUILD_REV's commit; scripts are not in the aac7 worktree)
git -C $WT rev-parse HEAD | grep -q "^$BUILD_REV" || fail "worktree HEAD is not $BUILD_REV"
[ -x $E/ecalc ] && grep -aqF ECALC_VMM_BG $E/ecalc && grep -aqF ECALC_SEGV_TRACE $E/ecalc || fail "ecalc binary missing or lacks ECALC_VMM_BG"
[ -z "$(find $E -maxdepth 1 \( -name '*.c' -o -name '*.h' -o -name Makefile \) -newer $E/ecalc | head -1)" ] || fail "a source is newer than the ecalc binary"
[ -x $WT/tools/unpack_digits ] && [ -x $E/digcmp.sh ] && [ -x $E/mnrun.sh ] || fail "tools/digcmp/mnrun missing"
BASE=$(head -1 $HOME/s32/summary.txt | sed 's/^BASE used: .* | //'); case "$BASE" in COMM_TRANSPORT=*) ;; *) fail "no BASE in ~/s32/summary.txt";; esac
export BASE; echo "S34 BASE | $BASE ; arm B adds $SOAK_BENV ; build $BUILD_REV" > $OUT/summary.txt
soak_stats() { awk -f $HD/s34_stats.awk $OUT/pairs.tsv | sort > $OUT/soak_stats.txt; }
digest() { soak_stats; { echo "S34 digest $(TZ=America/New_York date), build $BUILD_REV, holds 10n: $H10, 3n: $H3; A=BASE, B=BASE+$SOAK_BENV"
  echo; echo "per node count / arm (segfault = text 'Segmentation fault' / 'ecalc: SEGV [' in the log; rc is 1 under srun, not 139):"; cat $OUT/soak_stats.txt
  echo; echo "gate:"; cat $OUT/gate.txt 2>/dev/null; echo; echo "runs (tag nn arm rc wall verify segv):"; awk -F'\t' '{print $1,$2,$3,$4,$5,$6,$10}' $OUT/pairs.tsv | tail -80
  echo; echo "ALERT:"; cut -c1-200 $OUT/ALERT 2>/dev/null | head -60; } > $OUT/results.txt; }
trap 'digest; rd_finish' EXIT

# (0) wait for the old soaks
for i in $(seq 120); do [ -e $HOME/s32/S32_DONE ] && [ -e $HOME/s33/S33_DONE ] && break; [ -e $OUT/STOP ] && fail "STOP while waiting"; sleep 30; done
[ -e $HOME/s32/S32_DONE ] && [ -e $HOME/s33/S33_DONE ] || fail "S32/S33 did not finish within 60 min"
rd_say "S32/S33 done; starting S34 soaks"

# soak_chain <kind 10|3> <margin_s> <holds...>   (runs in a background subshell; no fail() here)
soak_chain() { local kind=$1 margin=$2 SH NODES PIDS i fa; shift 2
  for SH in "$@"; do
    while :; do
      [ -e $OUT/STOP ] && return
      st=$(squeue -j $SH -h -o %T 2>/dev/null)
      [ -z "$st" ] && { rd_say "hold $SH is gone: skipped"; continue 2; }
      [ "$st" = RUNNING ] && break
      rd_say "waiting for hold $SH ($st)"; sleep 300
    done
    NODES=$(scontrol show hostnames "$(squeue -j $SH -h -o %N)" | tr '\n' ' ')
    [ "$(echo $NODES | wc -w)" = $kind ] || { rd_say "hold $SH has not $kind nodes: skipped"; continue; }
    [ "$(left_s $SH)" -ge $(( RUN_S + 600 + margin )) ] || { rd_say "hold $SH: too little time left: skipped"; continue; }
    export SLURM_JOB_ID=$SH
    rd_health $SH $NODES || { sleep 90; rd_health $SH $NODES; } || { alert "hold $SH unhealthy at the soak start: skipped"; continue; }
    rd_say "soak on hold $SH: nodes $NODES, $(left_s $SH) s left"
    set -- $NODES; PIDS=""; i=0
    if [ $kind = 10 ]; then
      while [ $# -ge 2 ]; do
        if [ $((i % 2)) = 0 ]; then fa=A; else fa=B; fi
        nohup bash $HD/s34_pairsoak.sh $SH $OUT p$i $1,$2 $fa $margin $S > $OUT/w_p$i.nohup 2>&1 < /dev/null &
        PIDS="$PIDS $!"; i=$((i+1)); shift 2; sleep 20
      done
    else
      nohup bash $HD/s34_pairsoak.sh $SH $OUT p0 $1,$2 A $margin $S > $OUT/w_p0.nohup 2>&1 < /dev/null & PIDS="$!"; sleep 20
      PORT_BASE=21000 nohup bash $HD/s34_pairsoak.sh $SH $OUT s1 $3 B $margin $S > $OUT/w_s1.nohup 2>&1 < /dev/null & PIDS="$PIDS $!"
    fi
    while :; do alive=0; for p in $PIDS; do kill -0 $p 2>/dev/null && alive=1; done; [ $alive = 0 ] && break; sleep 600; digest; done
    digest
  done; }
soak_chain 10 1800 $H10 & P10=$!

# (c) gate: 2 nodes x 1e9 digits with ECALC_VMM_BG=0 on the 3-node hold's first two nodes (the 10-node soak is already running on its own nodes), digits vs ~/ref
G3=$(echo $H3 | cut -d' ' -f1); export SLURM_JOB_ID=$G3
GN=$(scontrol show hostnames "$(squeue -j $G3 -h -o %N)" | head -2 | paste -sd,)
if [ "$(squeue -j $G3 -h -o %T)" = RUNNING ] && [ -n "$GN" ]; then
  GF=$OUT/gate_digits.txt; rd_run gate_bg0 600 env MNRUN_NODES=2 MNRUN_NODELIST=$GN MNRUN_LABEL=1 COMM_PORT=22$((RANDOM % 90 + 10))0 ./mnrun.sh 2 env $BASE ECALC_SEGV_TRACE=1 ECALC_VMM_BG=0 ./ecalc 1000000000 $GF; grc=$?
  GP=$OUT/log/gate_bg0.plain; sed -E 's/^ *[0-9]+: //' $RD_LAST_LOG > $GP; gv=$(rd_verify $GP); gc=$($E/digcmp.sh $GF $HOME/ref/e_1000000000.txt); rm -rf $GF $GF.*
  echo "gate_bg0: nodes $GN rc $grc $gv digits $gc | $(grep -a -m1 '^total' $GP | cut -c1-120)" > $OUT/gate.txt; rd_say "$(cat $OUT/gate.txt)"
  if [ $grc != 0 ] || [ "$gv" != "VERIFY OK" ] || [ "$gc" != identical ]; then touch $OUT/STOP; alert "GATE FAILED: $(cat $OUT/gate.txt)"; wait $P10; fail "gate failed"; fi
else echo "gate skipped: hold $G3 not RUNNING" > $OUT/gate.txt; rd_say "$(cat $OUT/gate.txt)"; fi
soak_chain 3 2700 $H3 & P3=$!
wait $P10 $P3
digest
RD_VERDICT="SUCCESS: S34 soak done: $(tr '\n' ';' < $OUT/soak_stats.txt | cut -c1-500) (digest $OUT/results.txt)"

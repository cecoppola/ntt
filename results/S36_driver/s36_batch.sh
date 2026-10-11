#!/bin/bash
# S36: COMM_LAYER_VSLOT_SHARE=1 (idea S-1: one pair of general-map v-slots per device, handed from tree level to tree level).  Run directly by the
#   4-node sbatch job s36_job.sbatch (no hold: the allocation ends when this script exits).  DRY=1 (no Slurm job needed): the logic with stub runs.
#   BASE = first line of ~/s34/summary.txt before " ; arm B"; X2 = COMM_LAYER_INTER2=1 COMM_LAYER_VSLOT_POOL=1 (the aac7 line);
#   every run ECALC_SEGV_TRACE=1 MEM_REPORT_DEVS=1, no digit writes except the gates' (compared with ~/ref/e_1000000000.txt, then removed).
#   (1) gates, 1e9 digits, digits identical + VERIFY OK:  g1 2n X2+SHARE | g2 2n X2+SHARE+DIST_GEN (concurrent, nodes 1-2 / 3-4) |
#       g3 2n SHARE+DIST_GEN, no X2 | g4 2n SHARE, no X2 (concurrent) | g5 4n X2+DIST_GEN (control, SHARE off) | g6 4n X2+DIST_GEN+SHARE | g7 4n DIST_GEN+SHARE, no X2.
#       (DIST_GEN=1 forces the general map at power-of-two group sizes: 4 nodes have no non-power-of-two level, so without it SHARE has nothing to share.)
#   (2) 4-node ABBA, 5e10 digits per node (total 2e11): A = BASE+X2+DIST_GEN=1, B = A + COMM_LAYER_VSLOT_SHARE=1; round r odd = A,B, even = B,A;
#       stop rule (after >= 4 complete rounds): wall CI excludes 0, or CI half-width <= 15 s, or 8 complete rounds.  Per run: wall, ecalc total, per-node peak
#       device memory (the mem[] "driver: X of Y GB" lines summed over the node's 4 APUs).  (3) one 2-node pair (6.441e10 digits per node, A then B).
#   (4) digest ~/s36/results.txt, stats ~/s36/stats.txt, marker ~/s36/S36_DONE.  Never cancels anything; kills only its own srun PIDs (rd_run's watchdog).
BUILD_REV=db9cb7f
HD=$(cd "$(dirname "$0")" && pwd); WT=${WT:-$HOME/ntt-wt/s36}; E=$WT/ecalc; OUT=${OUT:-$HOME/s36}; export WT
DRY=${DRY:-0}
DPN=${DPN:-50000000000}; DPN2=${DPN2:-64410000000}; RUN_S=${RUN_S:-500}; RUN2_S=${RUN2_S:-330}; MAXPAIRS=8; MAXROUNDS=12; MAXBAD=3
X2="COMM_LAYER_INTER2=1 COMM_LAYER_VSLOT_POOL=1"
mkdir -p $OUT/log; rm -f $OUT/S36_DONE $OUT/GATE_DONE $OUT/ALERT $OUT/unhealthy.txt; [ "$DRY" = 1 ] && rm -f $OUT/STOP; : > $OUT/ab.tsv; : > $OUT/gate.txt
source $WT/tools/rundriver.sh; rd_init $OUT S36_DONE; RD_WATCHDOG_PERIOD=2
source $HD/s31_lib.sh
if [ "$DRY" = 1 ]; then J=0; else J=${SLURM_JOB_ID:?not inside a Slurm job}; export SLURM_JOB_ID=$J; fi
cd $E || fail "cd $E"
source aac7env.sh >/dev/null 2>&1; [ "$DRY" = 1 ] || [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"
git -C $WT rev-parse HEAD | grep -q "^$BUILD_REV" || fail "worktree HEAD is not $BUILD_REV"
git -C $WT diff --quiet -- ecalc || fail "tracked files in $WT/ecalc are modified"
[ -x $E/ecalc ] && grep -aqF COMM_LAYER_VSLOT_SHARE $E/ecalc && grep -aqF ECALC_SEGV_TRACE $E/ecalc || fail "ecalc binary missing or lacks COMM_LAYER_VSLOT_SHARE"
[ -z "$(find $E -maxdepth 1 \( -name '*.c' -o -name '*.h' -o -name Makefile \) -newer $E/ecalc | head -1)" ] || fail "a source is newer than the ecalc binary"
[ -x $WT/tools/unpack_digits ] && [ -x $E/digcmp.sh ] && [ -x $E/mnrun.sh ] && [ -r $HOME/ref/e_1000000000.txt ] || fail "tools/digcmp/mnrun/ref missing"
[ -r $HD/s36_stats.py ] && [ -r $HD/s36_devpeak.py ] || fail "s36_stats.py / s36_devpeak.py missing in $HD"
BASE=$(head -1 $HOME/s34/summary.txt | sed 's/^S34 BASE | //; s/ ; arm B adds.*$//'); case "$BASE" in COMM_TRANSPORT=*) ;; *) fail "no BASE in ~/s34/summary.txt";; esac
export BASE
if [ "$DRY" = 1 ]; then NODES="n1 n2 n3 n4"; else NODES=$(scontrol show hostnames "$SLURM_JOB_NODELIST" | tr '\n' ' '); fi
set -- $NODES; [ $# = 4 ] || fail "job $J has $# nodes, not 4: $NODES"
N1=$1,$2; N2=$3,$4; NALL=$1,$2,$3,$4
# the batch step's shape variables would shape every srun of the drivers: drop them (as S35)
unset SLURM_CPUS_PER_TASK SLURM_NTASKS SLURM_NPROCS SLURM_NTASKS_PER_NODE SLURM_TASKS_PER_NODE SLURM_NNODES SLURM_JOB_NUM_NODES SLURM_NODELIST \
      SLURM_JOB_NODELIST SLURM_GPUS_PER_NODE SLURM_TRES_PER_TASK SLURM_MEM_PER_NODE SLURM_MEM_PER_CPU SLURM_JOB_CPUS_PER_NODE SLURM_DISTRIBUTION SLURM_CPU_BIND SLURM_GPUS
rd_say "S36 BASE | $BASE ; X2 = $X2 ; A/B arm B adds COMM_LAYER_VSLOT_SHARE=1 ; A/B both DIST_GEN=1 ; build $BUILD_REV ; job $J nodes $NODES ; DRY=$DRY"
if [ "$DRY" != 1 ]; then rd_health $J $NODES || { sleep 90; rd_health $J $NODES; } || fail "unhealthy nodes at the start"; fi
[ "$DRY" = 1 ] || rd_disk $HOME 15 || fail "less than 15 GB free in \$HOME (the gates write 1e9-digit files)"
digest() { python3 $HD/s36_stats.py $OUT/ab.tsv > $OUT/stats.txt 2>&1
  { echo "S36 digest $(TZ=America/New_York date), build $BUILD_REV, job $J ($NODES); BASE+X2, B = + COMM_LAYER_VSLOT_SHARE=1"; echo
    echo "gates:"; cat $OUT/gate.txt 2>/dev/null; echo; cat $OUT/stats.txt; echo
    echo "runs (tag round arm rc wall verify ecalc_total devmax devmean pool vslot segv nodes end):"; cat $OUT/ab.tsv | cut -c1-200
    echo; echo "unhealthy:"; cat $OUT/unhealthy.txt 2>/dev/null; echo; echo "ALERT:"; cut -c1-200 $OUT/ALERT 2>/dev/null | head -60; } > $OUT/results.txt; }
trap 'digest; rd_finish' EXIT
stop_now() { [ -e $OUT/STOP ] && { rd_say "STOP file"; return 0; }; [ "$DRY" = 1 ] && return 1
  [ "$(left_s $J)" -ge $(( $1 + 600 )) ] || { rd_say "job $J: $(left_s $J) s left (< run + 600)"; return 0; }; return 1; }

# one ecalc run: run_one <label> <nodes-count> <nodelist> <expected_s> <digits-per-node|"G:<file>"> <env words...>   (sets RC WALL V TOT)
run_one() {
  local lab=$1 nn=$2 nl=$3 exp=$4 dpn=$5; shift 5
  local p=24$((RANDOM % 90 + 10))0 targ
  if [ "${dpn:0:2}" = "G:" ]; then targ="1000000000 ${dpn:2}"; else targ="$((nn * dpn))"; fi
  if [ "$DRY" = 1 ]; then dry_run $lab "$nn" "$*" "$targ"; RC=0
  else rd_run $lab $exp env MNRUN_NODES=$nn MNRUN_NODELIST=$nl MNRUN_LABEL=1 COMM_PORT=$p ./mnrun.sh $nn env $BASE ECALC_SEGV_TRACE=1 MEM_REPORT_DEVS=1 "$@" ./ecalc $targ < /dev/null; RC=$?; fi
  PL=$OUT/log/$lab.plain; sed -E 's/^ *[0-9]+: //' $OUT/log/$lab.log > $PL
  WALL=$RD_LAST_WALL; V=$(rd_verify $PL) || true; TOT=$(grep -a -m1 '^total' $PL | awk '{print ($2 ~ /^[0-9.]+$/) ? $2 : "NA"}'); [ -n "$TOT" ] || TOT=NA
  SEGV=$(grep -a -c 'Segmentation fault\|ecalc: SEGV \[' $OUT/log/$lab.log)
}
# DRY: a stub of rd_run for the logic test: a fake log whose numbers follow the arm (no srun, no digits)
dry_run() { local lab=$1 nn=$2 w=$3 l=$OUT/log/$lab.log sh=0; case "$w" in *VSLOT_SHARE=1*) sh=1;; esac
  RD_LAST_WALL=$(( 300 + RANDOM % 21 - 10 + sh * ${DRY_EFFECT:-4} )); RD_LAST_RC=0
  { echo "all $nn nodes: VERIFY OK"; echo "total   $((RD_LAST_WALL - 6)).10 s   (bs 1 + dm 2)"
    for rr in $(seq 0 $((nn - 1))); do for a in 0 1 2 3; do echo "mem[$rr] [dm]   APU$a: in use 80.0 GB: x  (driver: $((85 - sh * 2)).5 of 137.4 GB used) | outside the layout: v-slots layout 0.00 GB, hipMalloc'd now 0.00; comm pool 4.20 GB -> y"; done; echo "mem[$rr] [dm] host 11.0 GB RSS (HWM 20.0): x"; done; } > $l; }

# gate: gate_one <label> <nn> <nodes> <env words...>; row in gate.txt
gate_one() { local lab=$1 nn=$2 nl=$3 gf=$OUT/gate_$1_digits.txt; shift 3
  run_one gate_$lab $nn $nl 600 G:$gf "$@"
  local c=NA; if [ "$DRY" = 1 ]; then c="identical (DRY)"; else c=$($E/digcmp.sh $gf $HOME/ref/e_1000000000.txt 2>&1 | tail -1); fi; rm -rf $gf $gf.*
  echo "gate_$lab: nodes $nl rc $RC $V digits $c segv $SEGV | env: $* | $(grep -a -m1 '^total' $PL | cut -c1-100) | $(python3 $HD/s36_devpeak.py $PL)" > $OUT/gate_$lab.txt; }
gate_ok() { grep -q "^gate_$1: .* rc 0 VERIFY OK digits identical" $OUT/gate_$1.txt 2>/dev/null; }

rd_say "gates (1e9 digits): stage 1, g1 (2n X2+SHARE) and g2 (2n X2+SHARE+DIST_GEN) concurrently"
( RD_SUMMARY=$OUT/gate_g1.say; gate_one g1 2 $N1 $X2 COMM_LAYER_VSLOT_SHARE=1 ) & G1=$!
sleep 15; ( RD_SUMMARY=$OUT/gate_g2.say; gate_one g2 2 $N2 $X2 COMM_LAYER_VSLOT_SHARE=1 DIST_GEN=1 ) & G2=$!
wait $G1 $G2
rd_say "gates: stage 2, g3 (2n SHARE+DIST_GEN, no X2) and g4 (2n SHARE, no X2) concurrently"
( RD_SUMMARY=$OUT/gate_g3.say; gate_one g3 2 $N1 COMM_LAYER_VSLOT_SHARE=1 DIST_GEN=1 ) & G3=$!
sleep 15; ( RD_SUMMARY=$OUT/gate_g4.say; gate_one g4 2 $N2 COMM_LAYER_VSLOT_SHARE=1 ) & G4=$!
wait $G3 $G4
rd_say "gates: stage 3, 4 nodes: g5 (control, SHARE off), g6 (X2+SHARE), g7 (SHARE, no X2), all DIST_GEN=1"
gate_one g5 4 $NALL $X2 DIST_GEN=1
gate_one g6 4 $NALL $X2 DIST_GEN=1 COMM_LAYER_VSLOT_SHARE=1
gate_one g7 4 $NALL DIST_GEN=1 COMM_LAYER_VSLOT_SHARE=1
for g in g1 g2 g3 g4 g5 g6 g7; do cat $OUT/gate_$g.txt >> $OUT/gate.txt 2>/dev/null; done; rd_say "gates: $(cut -c1-110 $OUT/gate.txt | tr '\n' ';')"
if gate_ok g1 && gate_ok g2 && gate_ok g3 && gate_ok g4 && gate_ok g5 && gate_ok g6 && gate_ok g7; then echo "PASSED $(TZ=America/New_York date)" > $OUT/GATE_DONE
else echo "FAILED $(TZ=America/New_York date)" > $OUT/GATE_DONE
  gate_ok g5 || rd_say "NOTE: the control g5 (SHARE off, DIST_GEN=1, 4 nodes) failed: DIST_GEN at 4 nodes is not usable, the A/B cannot run"
  fail "gate failed: $(cut -c1-90 $OUT/gate.txt | tr '\n' ';')"; fi

# (2) the 4-node ABBA
row() { printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$RC" "$WALL" "$V" "$TOT" "$DM" "$DMEAN" "$POOL" "$VSL" "$SEGV" "$4" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" >> $OUT/ab.tsv; }
ab_run() { # tag round arm nn nodes dpn exp
  local tag=$1 r=$2 arm=$3 nn=$4 nl=$5 dpn=$6 exp=$7 w="" lab
  lab=${tag}_r${r}_$arm
  case $tag in ab) w="DIST_GEN=1";; p2) w="";; esac
  [ $arm = B ] && w="$w COMM_LAYER_VSLOT_SHARE=1"
  run_one $lab $nn $nl $exp $dpn $X2 $w
  local dp=$(python3 $HD/s36_devpeak.py $PL); DM=$(echo "$dp" | sed -E 's/.*devmax=([^ ]+).*/\1/'); DMEAN=$(echo "$dp" | sed -E 's/.*devmean=([^ ]+).*/\1/')
  POOL=$(echo "$dp" | sed -E 's/.*pool=([^ ]+).*/\1/'); VSL=$(echo "$dp" | sed -E 's/.*vslot=([^ ]+).*/\1/')
  row $tag $r $arm $nl
  rd_say "$lab: rc $RC wall ${WALL}s $V devmax $DM GB | $(grep -a -m1 '^total' $PL | cut -c1-110)"
  [ $RC = 0 ] && [ "$V" = "VERIFY OK" ]; }
health_or_stop() { [ "$DRY" = 1 ] && return 0; rd_health $J $(echo "$1" | tr , ' ') >/dev/null 2>&1 || { sleep 90; rd_health $J $(echo "$1" | tr , ' ') >/dev/null 2>&1; }; }

r=0; consec=0
rd_say "A/B: 4 nodes $NALL, $DPN digits per node, up to $MAXPAIRS complete rounds"
while [ $r -lt $MAXROUNDS ]; do
  stop_now $(( 2 * RUN_S )) && break
  r=$((r + 1)); if [ $((r % 2)) = 1 ]; then order="A B"; else order="B A"; fi
  for arm in $order; do
    if ab_run ab $r $arm 4 $NALL $DPN $RUN_S; then consec=0
    else consec=$((consec + 1)); alert "ab_r${r}_$arm: rc $RC $V segv $SEGV (log $OUT/log/ab_r${r}_$arm.log)"$'\n'"$(grep -a -m1 -A28 'ecalc: SEGV \[' $OUT/log/ab_r${r}_$arm.log | cut -c1-200)"
      health_or_stop $NALL || { echo "$(TZ=America/New_York date) $NALL after ab_r${r}_$arm" >> $OUT/unhealthy.txt; alert "unhealthy nodes after ab_r${r}_$arm: stop"; touch $OUT/STOP; break; }
      break; fi
  done
  [ $consec -ge $MAXBAD ] && { alert "$MAXBAD consecutive bad runs: A/B stops"; break; }
  [ -e $OUT/STOP ] && break
  out=$(python3 $HD/s36_stats.py $OUT/ab.tsv --rule); src=$?; rd_say "rule after round $r: $out"
  [ $src = 0 ] && break
done

# (3) the 2-node pair (A then B, nodes 1-2)
if ! stop_now $(( 2 * RUN2_S )); then
  rd_say "2-node pair: $N1, $DPN2 digits per node"
  ab_run p2 1 A 2 $N1 $DPN2 $RUN2_S && ab_run p2 1 B 2 $N1 $DPN2 $RUN2_S
fi
digest
RD_VERDICT="SUCCESS: S36 gates passed, A/B done ($(grep -c . $OUT/ab.tsv) runs): $(sed -n '1,4p' $OUT/stats.txt | cut -c1-230 | tr '\n' ';') (digest $OUT/results.txt)"

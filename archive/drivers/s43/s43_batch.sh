#!/bin/bash
# S43: multi-node A/B of the single-node kernel switches on aac7.  Run directly by the 4-node job s43_job.sbatch (afterany:12461).  DRY=1: logic with stub runs.
#   A = DBIG_ADDSUB2=0 (old kernels); B = DBIG_ADDSUB2=1 DBIG_MAXIDX_TOP=1 DBIG_QSEL=1 (all new).  Same binary (copy of ~/ntt-wt/s41/ecalc/ecalc) for both arms.
#   Env of every run = BASE (first line of ~/s34/summary.txt before " ; arm B") + COMM_LAYER_INTER2=1 COMM_LAYER_VSLOT_POOL=1 ECALC_SEGV_TRACE=1.
#   0 wait (<= 45 min) for the s41 binary: exists, newer than every *.c/*.h/Makefile, linked with SHMEM (libsma), no make running; copy to ~/s43/ecalc_s41.
#   1 gates 1e9 digits, arm B, 2 nodes (pair 1 and pair 2 concurrently): VERIFY OK + digits identical to ~/ref/e_1000000000.txt.
#   2 2-node ABBA, 6.441e10 digits/node, two pairs concurrently (nodes 1-2 = p1, 3-4 = p2), 3 rounds (odd A B, even B A);
#     then 4-node ABBA, same size per node, >= 3 rounds, stop when the wall CI excludes 0 or 5 rounds or the job time runs short.
#   3 marker ~/s43/S43_DONE, summary ~/s43/summary.txt, ALERT on failures.  Kills only rd_run's own srun PIDs; never scancel.
HD=$(cd "$(dirname "$0")" && pwd); WT=${WT:-$HOME/ntt-wt/s41}; SRC=$WT/ecalc; OUT=${OUT:-$HOME/s43}; E=$WT/ecalc; EC=$OUT/ecalc_s41; export WT
DRY=${DRY:-0}; DPN=${DPN:-64410000000}; RUN2_S=${RUN2_S:-330}; RUN4_S=${RUN4_S:-480}; WAIT_S=${WAIT_S:-2700}; MAXBAD=3
X2="COMM_LAYER_INTER2=1 COMM_LAYER_VSLOT_POOL=1"
ARM_A="DBIG_ADDSUB2=0"; ARM_B="DBIG_ADDSUB2=1 DBIG_MAXIDX_TOP=1 DBIG_QSEL=1"
mkdir -p $OUT/log; rm -f $OUT/S43_DONE $OUT/ALERT; : > $OUT/ab.tsv; : > $OUT/gate.txt
source $HD/rundriver.sh; rd_init $OUT S43_DONE; RD_WATCHDOG_PERIOD=2
source $HD/s31_lib.sh
if [ "$DRY" = 1 ]; then J=0; else J=${SLURM_JOB_ID:?not inside a Slurm job}; export SLURM_JOB_ID=$J; fi
left_ok() { [ "$DRY" = 1 ] && return 0; [ "$(left_s $J)" -ge $(( $1 + 600 )) ]; }
BASE=$(head -1 $HOME/s34/summary.txt | sed 's/^S34 BASE | //; s/ ; arm B adds.*$//'); case "$BASE" in COMM_TRANSPORT=*) ;; *) fail "no BASE in ~/s34/summary.txt";; esac
export BASE
# --- 0: wait for the binary
bin_ready() { [ -x $SRC/ecalc ] || return 1
  [ -z "$(find $SRC -maxdepth 1 \( -name '*.c' -o -name '*.h' -o -name Makefile \) -newer $SRC/ecalc | head -1)" ] || return 1
  ldd $SRC/ecalc 2>/dev/null | grep -q libsma || return 1
  for s in DBIG_ADDSUB2 DBIG_MAXIDX_TOP DBIG_QSEL; do grep -aqF $s $SRC/ecalc || return 1; done
  ! pgrep -af "make|hipcc|cc1" 2>/dev/null | grep -v pgrep | grep -qF "$SRC" ; }
if [ "$DRY" != 1 ] || [ -n "$DRY_WAIT" ]; then
  t0=$(date +%s); until bin_ready; do
    [ $(( $(date +%s) - t0 )) -ge $WAIT_S ] && fail "s41 binary not ready after ${WAIT_S}s ($(stat -c %y $SRC/ecalc 2>&1 | cut -c1-19))"; sleep 30; done
  sleep 20; bin_ready || fail "s41 binary changed while settling"
  cp $SRC/ecalc $EC.tmp && mv $EC.tmp $EC || fail "copy of the binary"
else cp $SRC/ecalc $EC 2>/dev/null || true; fi
rd_say "S43 binary: $EC (md5 $(md5sum $EC 2>/dev/null | cut -c1-12), s41 $(git -C $WT rev-parse --short HEAD), built $(stat -c %y $SRC/ecalc | cut -c1-19))"
cd $E || fail "cd $E"
source aac7env.sh >/dev/null 2>&1; [ "$DRY" = 1 ] || [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"
[ -x $EC ] && [ -x $WT/tools/unpack_digits ] && [ -x $E/digcmp.sh ] && [ -x $E/mnrun.sh ] && [ -r $HOME/ref/e_1000000000.txt ] || fail "binary/tools/digcmp/mnrun/ref missing"
if [ "$DRY" = 1 ]; then NODES="n1 n2 n3 n4"; else NODES=$(scontrol show hostnames "$SLURM_JOB_NODELIST" | tr '\n' ' '); fi
set -- $NODES; [ $# = 4 ] || fail "job $J has $# nodes, not 4: $NODES"
N1=$1,$2; N2=$3,$4; NALL=$1,$2,$3,$4
unset SLURM_CPUS_PER_TASK SLURM_NTASKS SLURM_NPROCS SLURM_NTASKS_PER_NODE SLURM_TASKS_PER_NODE SLURM_NNODES SLURM_JOB_NUM_NODES SLURM_NODELIST \
      SLURM_JOB_NODELIST SLURM_GPUS_PER_NODE SLURM_TRES_PER_TASK SLURM_MEM_PER_NODE SLURM_MEM_PER_CPU SLURM_JOB_CPUS_PER_NODE SLURM_DISTRIBUTION SLURM_CPU_BIND SLURM_GPUS
rd_say "S43 BASE | $BASE $X2 ECALC_SEGV_TRACE=1 ; A = $ARM_A ; B = $ARM_B ; job $J nodes $NODES ; DRY=$DRY"
if [ "$DRY" != 1 ]; then rd_health $J $NODES || { sleep 90; rd_health $J $NODES; } || fail "unhealthy nodes at the start"; fi
[ "$DRY" = 1 ] || rd_disk $HOME 15 || fail "less than 15 GB free in \$HOME (gates write 1e9-digit files)"
digest() { python3 $HD/s43_stats.py $OUT/ab.tsv > $OUT/stats.txt 2>&1
  { echo "S43 digest $(TZ=America/New_York date), job $J ($NODES); A = $ARM_A ; B = $ARM_B"; echo; echo "gates:"; cat $OUT/gate.txt; echo; cat $OUT/stats.txt; echo
    echo "runs (tag round arm rc wall verify ecalc_total nodes end):"; cut -c1-200 $OUT/ab.tsv; echo; echo "ALERT:"; cut -c1-200 $OUT/ALERT 2>/dev/null | head -60; } > $OUT/results.txt; }
trap 'digest; rd_finish' EXIT
# run_one <label> <nn> <nodelist> <expected_s> <digits-per-node|G:file> <env words...>  (sets RC WALL V TOT SEGV PL)
run_one() { local lab=$1 nn=$2 nl=$3 exp=$4 dpn=$5; shift 5; local p=24$((RANDOM % 90 + 10))0 targ
  if [ "${dpn:0:2}" = "G:" ]; then targ="1000000000 ${dpn:2}"; else targ="$((nn * dpn))"; fi
  if [ "$DRY" = 1 ]; then dry_run $lab "$nn" "$*"; RC=0
  else rd_run $lab $exp env MNRUN_NODES=$nn MNRUN_NODELIST=$nl MNRUN_LABEL=1 COMM_PORT=$p ./mnrun.sh $nn env $BASE $X2 ECALC_SEGV_TRACE=1 "$@" $EC $targ < /dev/null; RC=$?; fi
  PL=$OUT/log/$lab.plain; sed -E 's/^ *[0-9]+: //' $OUT/log/$lab.log > $PL
  WALL=$RD_LAST_WALL; V=$(rd_verify $PL) || true; TOT=$(grep -a -m1 '^total' $PL | awk '{print ($2 ~ /^[0-9.]+$/) ? $2 : "NA"}'); [ -n "$TOT" ] || TOT=NA
  SEGV=$(grep -a -c 'Segmentation fault\|ecalc: SEGV \[' $OUT/log/$lab.log); }
dry_run() { local l=$OUT/log/$1.log b=0; case "$3" in *QSEL=1*) b=1;; esac
  RD_LAST_WALL=$(( 300 + RANDOM % 21 - 10 - b * ${DRY_EFFECT:-12} )); RD_LAST_RC=0
  { echo "all $2 nodes: VERIFY OK"; echo "total   $((RD_LAST_WALL - 6)).10 s   (bs 1 + dm 2)"; } > $l; }
gate_one() { local lab=$1 nn=$2 nl=$3 gf=$OUT/gate_$1_digits.txt; shift 3
  run_one gate_$lab $nn $nl 600 G:$gf "$@"
  local c=identical; [ "$DRY" = 1 ] || c=$($E/digcmp.sh $gf $HOME/ref/e_1000000000.txt 2>&1 | tail -1); rm -rf $gf $gf.*
  echo "gate_$lab: nodes $nl rc $RC $V digits $c segv $SEGV | env: $* | $(grep -a -m1 '^total' $PL | cut -c1-100)" > $OUT/gate_$lab.txt; }
gate_ok() { grep -q "^gate_$1: .* rc 0 VERIFY OK digits identical" $OUT/gate_$1.txt 2>/dev/null; }
rd_say "gates (1e9, arm B, 2 nodes): pair 1 ($N1) and pair 2 ($N2) concurrently"
( RD_SUMMARY=$OUT/gate_g1.say; gate_one g1 2 $N1 $ARM_B ) & G1=$!
sleep 15; ( RD_SUMMARY=$OUT/gate_g2.say; gate_one g2 2 $N2 $ARM_B ) & G2=$!
wait $G1 $G2
cat $OUT/gate_g1.txt $OUT/gate_g2.txt >> $OUT/gate.txt 2>/dev/null; rd_say "gates: $(cut -c1-110 $OUT/gate.txt | tr '\n' ';')"
if gate_ok g1 && gate_ok g2; then echo "PASSED $(TZ=America/New_York date)" > $OUT/GATE_DONE
else echo "FAILED $(TZ=America/New_York date)" > $OUT/GATE_DONE; fail "gate failed: $(cut -c1-90 $OUT/gate.txt | tr '\n' ';')"; fi
row() { printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$RC" "$WALL" "$V" "$TOT" "$4" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" >> $OUT/ab.tsv; }
ab_run() { # tag round arm nn nodes exp
  local tag=$1 r=$2 arm=$3 nn=$4 nl=$5 exp=$6 w lab=${1}_r${2}_$3
  [ $arm = A ] && w="$ARM_A" || w="$ARM_B"
  run_one $lab $nn $nl $exp $DPN $w; row $tag $r $arm $nl
  rd_say "$lab: rc $RC wall ${WALL}s $V | $(grep -a -m1 '^total' $PL | cut -c1-110)"
  [ $RC = 0 ] && [ "$V" = "VERIFY OK" ] || alert "$lab: rc $RC $V segv $SEGV (log $OUT/log/$lab.log)"$'\n'"$(grep -a -m1 -A28 'ecalc: SEGV \[' $OUT/log/$lab.log | cut -c1-200)"
  [ $RC = 0 ] && [ "$V" = "VERIFY OK" ]; }
healthy() { [ "$DRY" = 1 ] && return 0; rd_health $J $(echo "$1" | tr , ' ') >/dev/null 2>&1 || { sleep 90; rd_health $J $(echo "$1" | tr , ' ') >/dev/null 2>&1; }; }
# pair_loop <tag> <nodes>: 3 rounds, odd A B, even B A (runs in a subshell)
pair_loop() { local tag=$1 nl=$2 r order arm bad=0
  for r in 1 2 3; do left_ok $(( 2 * RUN2_S )) || { rd_say "$tag: job time short"; break; }
    [ $((r % 2)) = 1 ] && order="A B" || order="B A"
    for arm in $order; do ab_run $tag $r $arm 2 $nl $RUN2_S && bad=0 || { bad=$((bad + 1)); healthy $nl || { alert "$tag: unhealthy nodes $nl"; return; }; }; done
    [ $bad -ge $MAXBAD ] && { alert "$tag: $MAXBAD consecutive bad runs, stop"; return; }; done; }
rd_say "2-node ABBA: two pairs concurrently, $DPN digits per node"
( RD_SUMMARY=$OUT/p1.say; pair_loop p1 $N1 ) & P1=$!
sleep 20; ( RD_SUMMARY=$OUT/p2.say; pair_loop p2 $N2 ) & P2=$!
wait $P1 $P2; cat $OUT/p1.say $OUT/p2.say >> $OUT/summary.txt 2>/dev/null
rd_say "2-node results: $(python3 $HD/s43_stats.py $OUT/ab.tsv | grep -E 'complete|wall' | head -6 | cut -c1-150 | tr '\n' ';')"
r=0; bad=0
rd_say "4-node ABBA: $NALL, $DPN digits per node"
while [ $r -lt 5 ]; do
  left_ok $(( 2 * RUN4_S )) || { rd_say "job time short: 4-node stops"; break; }
  r=$((r + 1)); [ $((r % 2)) = 1 ] && order="A B" || order="B A"
  for arm in $order; do ab_run ab $r $arm 4 $NALL $RUN4_S && bad=0 || { bad=$((bad + 1)); healthy $NALL || { alert "4n: unhealthy nodes"; break 2; }; }; done
  [ $bad -ge $MAXBAD ] && { alert "$MAXBAD consecutive bad 4-node runs: stop"; break; }
  out=$(python3 $HD/s43_stats.py $OUT/ab.tsv --rule); src=$?; rd_say "rule after 4-node round $r: $out"; [ $src = 0 ] && break
done
digest
RD_VERDICT="SUCCESS: S43 done: $(grep -E 'complete|wall' $OUT/stats.txt | cut -c1-200 | tr '\n' ';') (digest $OUT/results.txt)"

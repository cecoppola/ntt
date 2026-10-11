#!/bin/bash
# S35: gate + crash soak of ECALC_VMM_BG=2 (the background VMM mapper under the process-wide HIP memory lock, hipmm.c), run directly by
#   the 4-node sbatch job s35_job.sbatch (no hold: the job ends, and releases its nodes, when this script exits).
#   Arms: A = BASE (~/s34/summary.txt line 1), C = BASE + ECALC_VMM_BG=2, B = BASE + ECALC_VMM_BG=0 (every 6th run, the cost reference).
#   (1) gate: 2 nodes x 1e9 digits, A on nodes 1-2 and C on nodes 3-4 at once, each digcmp'd against ~/ref/e_1000000000.txt (both identical
#       => C's digits = A's); C must be rc 0 + VERIFY OK.  ~/s35/gate.txt, ~/s35/GATE_DONE; a failed gate ends the job, marker FAILED.
#   (2) soak: workers p0 (nodes 1-2, first A) and p1 (nodes 3-4, first C), s35_pairsoak.sh, 6.441e10 digits per node, until 11 h after
#       the soak start, ~/s35/STOP, or 2 unhealthy-node events (the workers touch STOP on the second).
#   (3) digest ~/s35/results.txt, stats ~/s35/soak_stats.txt, marker ~/s35/S35_DONE.  Never cancels anything; kills only its own srun PIDs.
BUILD_REV=015994a
HD=$(cd "$(dirname "$0")" && pwd); WT=$HOME/ntt-wt/s35; E=$WT/ecalc; OUT=$HOME/s35; export WT
S=64410000000; RUN_S=450; SOAK_H=${SOAK_H:-11}; export RUN_S
mkdir -p $OUT/log; rm -f $OUT/S35_DONE $OUT/GATE_DONE $OUT/ALERT $OUT/STOP $OUT/unhealthy.txt; : > $OUT/pairs.tsv
source $WT/tools/rundriver.sh; rd_init $OUT S35_DONE
source $HD/s31_lib.sh
J=${SLURM_JOB_ID:?not inside a Slurm job}; export SLURM_JOB_ID=$J
cd $E || fail "cd $E"
source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"
git -C $WT rev-parse HEAD | grep -q "^$BUILD_REV" || fail "worktree HEAD is not $BUILD_REV"
git -C $WT diff --quiet -- ecalc || fail "tracked files in $WT/ecalc are modified"
[ -x $E/ecalc ] && grep -aqF ECALC_VMM_BG $E/ecalc && grep -aqF ECALC_SEGV_TRACE $E/ecalc && nm $E/ecalc | grep -q ' T __wrap_hipMemMap' || fail "ecalc binary missing or lacks ECALC_VMM_BG / the hipmm wraps"
[ -z "$(find $E -maxdepth 1 \( -name '*.c' -o -name '*.h' -o -name Makefile \) -newer $E/ecalc | head -1)" ] || fail "a source is newer than the ecalc binary"
[ -x $WT/tools/unpack_digits ] && [ -x $E/digcmp.sh ] && [ -x $E/mnrun.sh ] && [ -r $HOME/ref/e_1000000000.txt ] || fail "tools/digcmp/mnrun/ref missing"
BASE=$(head -1 $HOME/s34/summary.txt | sed 's/^S34 BASE | //; s/ ; arm B adds.*$//'); case "$BASE" in COMM_TRANSPORT=*) ;; *) fail "no BASE in ~/s34/summary.txt";; esac
export BASE
NODES=$(scontrol show hostnames "$SLURM_JOB_NODELIST" | tr '\n' ' '); set -- $NODES; [ $# = 4 ] || fail "job $J has $# nodes, not 4: $NODES"
N1=$1,$2; N2=$3,$4
# the batch step's shape variables would shape every srun of the drivers (S34 ran them from the login node without these): drop them
unset SLURM_CPUS_PER_TASK SLURM_NTASKS SLURM_NPROCS SLURM_NTASKS_PER_NODE SLURM_TASKS_PER_NODE SLURM_NNODES SLURM_JOB_NUM_NODES SLURM_NODELIST \
      SLURM_JOB_NODELIST SLURM_GPUS_PER_NODE SLURM_TRES_PER_TASK SLURM_MEM_PER_NODE SLURM_MEM_PER_CPU SLURM_JOB_CPUS_PER_NODE SLURM_DISTRIBUTION SLURM_CPU_BIND SLURM_GPUS
rd_say "S35 BASE | $BASE ; arm C adds ECALC_VMM_BG=2, arm B ECALC_VMM_BG=0 ; build $BUILD_REV ; job $J nodes $NODES"
rd_health $J $NODES || { sleep 90; rd_health $J $NODES; } || fail "unhealthy nodes at the start"
soak_stats() { awk -f $HD/s35_stats.awk $OUT/pairs.tsv > $OUT/soak_stats.txt; }
digest() { soak_stats; { echo "S35 digest $(TZ=America/New_York date), build $BUILD_REV, job $J ($NODES); A=BASE, C=BASE+ECALC_VMM_BG=2, B=BASE+ECALC_VMM_BG=0"
  echo; echo "per arm (2 nodes x 6.441e10 digits per node; segfault = text 'Segmentation fault' / 'ecalc: SEGV [' in the log; srun rc is 1):"; cat $OUT/soak_stats.txt
  echo; echo "gate:"; cat $OUT/gate.txt 2>/dev/null; echo; echo "unhealthy events:"; cat $OUT/unhealthy.txt 2>/dev/null
  echo; echo "runs (tag nn arm rc wall verify segv k ecalc_total):"; awk -F'\t' '{print $1,$2,$3,$4,$5,$6,$10,$11,$12}' $OUT/pairs.tsv | tail -80
  echo; echo "ALERT:"; cut -c1-200 $OUT/ALERT 2>/dev/null | head -80; } > $OUT/results.txt; }
trap 'digest; rd_finish' EXIT

# (1) gate: A on N1 and C on N2 at once, 1e9 digits each, digits written (the only digit writes of S35) and removed after the compare
gate_one() { local lab=$1 nl=$2 cw=$3 gf=$OUT/gate_$1_digits.txt
  rd_run gate_$lab 600 env MNRUN_NODES=2 MNRUN_NODELIST=$nl MNRUN_LABEL=1 COMM_PORT=24$((RANDOM % 90 + 10))0 ./mnrun.sh 2 env $BASE ECALC_SEGV_TRACE=1 $cw ./ecalc 1000000000 $gf < /dev/null; local rc=$?
  local gp=$OUT/log/gate_$lab.plain; sed -E 's/^ *[0-9]+: //' $OUT/log/gate_$lab.log > $gp
  local v=$(rd_verify $gp) c=$($E/digcmp.sh $gf $HOME/ref/e_1000000000.txt); rm -rf $gf $gf.*
  echo "gate_$lab: nodes $nl rc $rc $v digits $c segv $(grep -a -c 'Segmentation fault\|ecalc: SEGV \[' $OUT/log/gate_$lab.log) | $(grep -a -m1 '^total' $gp | cut -c1-120)" > $OUT/gate_$lab.txt; }
( RD_SUMMARY=$OUT/gate_A.say; gate_one A $N1 "" ) & GA=$!
sleep 15; ( RD_SUMMARY=$OUT/gate_C.say; gate_one C $N2 ECALC_VMM_BG=2 ) & GC=$!
wait $GA $GC
cat $OUT/gate_A.txt $OUT/gate_C.txt > $OUT/gate.txt 2>/dev/null; rd_say "gate: $(tr '\n' ';' < $OUT/gate.txt)"
if grep -q '^gate_C: .* rc 0 VERIFY OK digits identical ' $OUT/gate.txt && grep -q '^gate_A: .* digits identical ' $OUT/gate.txt; then
  echo "PASSED $(TZ=America/New_York date)" > $OUT/GATE_DONE
else echo "FAILED $(TZ=America/New_York date)" > $OUT/GATE_DONE; fail "gate failed: $(tr '\n' ';' < $OUT/gate.txt)"; fi

# (2) soak
export SOAK_DEADLINE=$(( $(date +%s) + SOAK_H * 3600 ))
rd_say "soak: $SOAK_H h, until $(TZ=America/New_York date -d @$SOAK_DEADLINE), job $(left_s $J) s left"
nohup bash $HD/s35_pairsoak.sh $OUT p0 $N1 A $S > $OUT/w_p0.nohup 2>&1 < /dev/null & P0=$!
sleep 20
nohup bash $HD/s35_pairsoak.sh $OUT p1 $N2 C $S > $OUT/w_p1.nohup 2>&1 < /dev/null & P1=$!
while kill -0 $P0 2>/dev/null || kill -0 $P1 2>/dev/null; do sleep 300; digest; done
wait $P0 $P1
digest
RD_VERDICT="SUCCESS: S35 gate passed, soak done ($([ -e $OUT/STOP ] && echo 'STOP file' || echo 'deadline/job end')): $(tr '\n' ';' < $OUT/soak_stats.txt | cut -c1-600) (digest $OUT/results.txt)"

#!/bin/bash
# C3 node batch (aac6 login node):  setsid nohup bash ~/C3_batch.sh > ~/C314/batch.out 2>&1 &
# One job (<= 45 min, self-cancelling): A3 forced failures (size 1, pool guard at size 1 and 4), E12 budget check at size 4
# (low budget, one rank over, normal budget + digits), then mnaccept unit,e9,mn,full.
set -u
D=~/C314; mkdir -p $D; S=$D/summary.txt
E=~/ntt-C314/ecalc; cd $E || exit 1
log() { echo "$(date +%T) $*" | tee -a $S; }
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J C314 --parsable --wrap "sleep 2700")
log "job $J submitted"
trap "scancel $J; log 'job $J cancelled'" EXIT
GIVEUP=$(( $(date +%s) + 4 * 3600 ))
until [ "$(squeue -j $J -h -o %T)" = RUNNING ]; do [ $(date +%s) -gt $GIVEUP ] && { log "GIVE UP"; exit 1; }; sleep 20; done
T0=$(date +%s); NODE=$(squeue -j $J -h -o %N); log "job $J running on $NODE"
REF=~/ntt/ecalc/ref
TMP=/tmp/c314_$J
N() { srun --jobid=$J -N1 --overlap bash -c "$*"; }
N "mkdir -p $TMP; rm -rf $TMP/*"
# one process on the node (the four APUs)
one() { local tag=$1 to=$2; shift 2
  local t1=$(date +%s)
  timeout $to srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; $*" > $D/$tag.log 2>&1; local rc=$?
  log "$tag: rc $rc, $(( $(date +%s) - t1 )) s | FATAL lines $(grep -a -c 'ecalc: FATAL' $D/$tag.log) | Segmentation/corrupted $(grep -a -c -i 'segmentation\|corrupted\|core dumped' $D/$tag.log) | $(grep -a 'ecalc: FATAL\|BUDGET' $D/$tag.log | head -1 | cut -c1-220)"; }
# <p> node-processes; each task's rc echoed by a wrapper
many() { local tag=$1 to=$2 p=$3; shift 3
  local t1=$(date +%s)
  SLURM_JOB_ID=$J timeout $to ./mnrun.sh $p bash -c "$* ; echo \"C3RC rank \$COMM_RANK rc \$?\"" > $D/$tag.log 2>&1; local rc=$?
  log "$tag: srun rc $rc, $(( $(date +%s) - t1 )) s | $(grep -a '^C3RC' $D/$tag.log | sort | tr '\n' ' ') | FATAL lines $(grep -a -c 'ecalc: FATAL' $D/$tag.log), BUDGET lines $(grep -a -c 'BUDGET CHECK FAILED' $D/$tag.log) | Segmentation/corrupted $(grep -a -c -i 'segmentation\|corrupted\|core dumped' $D/$tag.log)"
  grep -a 'ecalc: FATAL\|BUDGET CHECK FAILED\|budget: ' $D/$tag.log | cut -c1-260 | head -8 | sed 's/^/        /' | tee -a $S; }
cmpp() { N "f=$1; cat \$f.part* > \$f.all 2>/dev/null || cp \$f \$f.all; cmp -s \$f.all $2 && echo identical || echo DIFFERS; rm -rf \$f \$f.*"; }

# ---- A3 ----
one a3_forced_s1 300 ECALC_FATAL_TEST=1 ./ecalc 100000000 $TMP/f1.txt
one a3_guard_s1_e9 400 POOL_LOG=27 ./ecalc 1000000000 $TMP/g1.txt
many a3_guard_s4_e9 400 4 POOL_LOG=27 ./ecalc 1000000000 $TMP/g4.txt
many a3_guard_s4_e9b 400 4 POOL_LOG=27 ./ecalc 1000000000 $TMP/g4b.txt
many a3_guard_s4_920e7 400 4 POOL_LOG=27 ./ecalc 9200000000 $TMP/g4c.txt
many a3_forced_s4 400 4 ECALC_FATAL_TEST=1 POOL_LOG=29 ./ecalc 1000000000 $TMP/f4.txt
# ---- E12 ----
many e12_low_s4 400 4 ECALC_BUDGET_CHECK=1 ECALC_NODE_GB=10 ECALC_VERBOSE=2 POOL_LOG=29 ./ecalc 1000000000 $TMP/b1.txt
many e12_rank2_s4 400 4 ECALC_BUDGET_CHECK=1 ECALC_BUDGET_RANK_GB=2:1 POOL_LOG=29 ./ecalc 1000000000 $TMP/b2.txt
many e12_ok_s4 900 4 ECALC_BUDGET_CHECK=1 ECALC_VERBOSE=2 POOL_LOG=29 ./ecalc 1000000000 $TMP/b3.txt
log "e12_ok_s4 digits: $(cmpp $TMP/b3.txt $REF/e_1000000000.txt), all 4 VERIFY OK: $(grep -a -c 'mn: all 4 nodes: VERIFY OK' $D/e12_ok_s4.log)"
one e12_ok_s1 600 ECALC_BUDGET_CHECK=1 ECALC_VERBOSE=2 ./ecalc 1000000000 $TMP/b4.txt
log "e12_ok_s1 digits: $(cmpp $TMP/b4.txt $REF/e_1000000000.txt), VERIFY OK: $(grep -a -c '^VERIFY OK' $D/e12_ok_s1.log) | $(grep -a 'budget:' $D/e12_ok_s1.log | head -2 | cut -c1-200 | tr '\n' ' ')"
N "rm -rf $TMP"
log "tests done, elapsed $(( $(date +%s) - T0 )) s"
# ---- gates ----
./mnaccept.sh $J --only unit,e9,mn,full > $D/mnaccept.out 2>&1
log "mnaccept: $(grep -a '^PASS\|^FAIL' $D/mnaccept.out | wc -l) steps, $(grep -a -c '^FAIL' $D/mnaccept.out) FAIL"
grep -a '^PASS\|^FAIL' $D/mnaccept.out | cut -c1-200 | sed 's/^/        /' | tee -a $S
log "TOTAL batch done, elapsed $(( $(date +%s) - T0 )) s"

#!/bin/bash -l
# PC15 batch j1 (Phase 15 Batch 3 PC, results/PC15.md): tests/cx_grid -- CX's target-like grid shapes (scaled to cap 2^25 at 2 procs, 2^25 at 4)
# with RNS_DIST_CACHE_PARTIAL=1: forced k = 1, 2 prime slots, full slots, and the pool rule; 18-digit (MN_P24=0) and 24-digit (MN_P24=2);
# the switch-off reference first.  Every product's share hashed (identical over the slot counts; the configurations compared after).
#   usage: setsid nohup bash -l ~/ntt-PC15/ecalc/tests/pc_j1.sh <tag> <procs> <pool_log> <shapes> <cfg,...> > ~/PC15/<tag>.out 2>&1 &
#   cfg: off (the switch off, slots 0,2), k1 / k2 (PARTIAL, K = 1 / 2, forced planes, slots 0,1), k4 (PARTIAL, forced 8 planes: full slots 0,1,2),
#        pool (PARTIAL, the pool rule, slots 0,1,2); suffix 24 = MN_P24=2 (e.g. k1_24)
module load rocm >/dev/null 2>&1
TAG=$1 P=$2 PL=$3 SH=$4 CFGS=$5 REPS=${REPS:-1}
L=~/PC15/$TAG; mkdir -p $L
cd ~/ntt-PC15/ecalc || exit 1
echo "== $TAG: $(git log --oneline -1) $(date); $P procs, pool_log $PL, $SH, $CFGS"
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J PC --parsable --wrap "sleep 2700")
echo "job $J queued $(date)"
trap 'scancel $J 2>/dev/null' EXIT
while :; do s=$(squeue -j $J -h -o %T 2>/dev/null); [ "$s" = RUNNING ] && break; [ -z "$s" ] && { echo "job $J gone"; exit 1; }; sleep 20; done
echo "job $J running on $(squeue -j $J -h -o %N) $(date)"; T0=$(date +%s)
export SLURM_JOB_ID=$J
left() { echo $(( 2600 - ($(date +%s) - T0) )); }
E="LIMB_BASE=10 ECALC_NP=4 RNS_POOL_GROW=1 RNS_DIST_CACHE_TRACE=1 ECALC_VERBOSE=1"
for c in ${CFGS//,/ }; do
  base=${c%_24}; x=""; [ "$base" != "$c" ] && x="MN_P24=2"
  case $base in
    off)  ce="RNS_DIST_CACHE_FIT=1"; sl=0,2;;
    k1)   ce="RNS_DIST_CACHE_PARTIAL=1 RNS_DIST_CACHE_PARTIAL_K=1 RNS_DIST_CACHE_PARTIAL_FORCE=1"; sl=0,1;;
    k2)   ce="RNS_DIST_CACHE_PARTIAL=1 RNS_DIST_CACHE_PARTIAL_K=2 RNS_DIST_CACHE_PARTIAL_FORCE=2"; sl=0,1;;
    k3)   ce="RNS_DIST_CACHE_PARTIAL=1 RNS_DIST_CACHE_PARTIAL_K=3 RNS_DIST_CACHE_PARTIAL_FORCE=3"; sl=0,1;;
    k4)   ce="RNS_DIST_CACHE_PARTIAL=1 RNS_DIST_CACHE_PARTIAL_FORCE=8"; sl=0,1,2;;
    pool) ce="RNS_DIST_CACHE_PARTIAL=1"; sl=0,1,2;;
  esac
  [ $(left) -lt 240 ] && { echo "SKIP $c (time: $(left) s left)"; continue; }
  t=$(date +%s)
  timeout $(( $(left) - 40 )) ./mnrun.sh $P env $E $x $ce ./tests/cx_grid $PL $REPS $sl tests/$SH > $L/$c.log 2>&1; rc=$?
  echo "$c rc $rc $(( $(date +%s) - t )) s | $(grep -a -c 'cx_grid node 0: rep [1-9]' $L/$c.log) timed products on node 0 | $(grep -a -c DIFFERS $L/$c.log) DIFFERS | $(grep -a -E 'VERIFY (OK|FAILED)' $L/$c.log | wc -l) VERIFY lines, $(grep -a -c 'VERIFY OK' $L/$c.log) OK | grew: $(grep -a -c 'pool GREW' $L/$c.log) | $(grep -a 'RNS_DIST_CACHE_PARTIAL) node 0' $L/$c.log | head -1 | cut -c1-150)"
done
echo "== $TAG done $(date), $(( $(date +%s) - T0 )) s on the node"

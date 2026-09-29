#!/bin/bash -l
# PC15 batch j5 (Phase 15 Batch 3 PC, results/PC15.md): t_mn_grid (GMP-checked grid products, cuts, X, the tree's layout) at 2 and 3 node-processes
# with RNS_DIST_CACHE_PARTIAL=1 forced to k = 1, 2 prime slots and full slots, 18- and 24-digit; then tests/cx_grid at 4 node-processes (cap 2^25).
#   usage: setsid nohup bash -l ~/ntt-PC15/ecalc/tests/pc_j5.sh <tag> > ~/PC15/<tag>.out 2>&1 &
module load rocm >/dev/null 2>&1
TAG=$1
L=~/PC15/$TAG; mkdir -p $L
cd ~/ntt-PC15/ecalc || exit 1
echo "== $TAG: $(git log --oneline -1) $(date)"
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J PC --parsable --wrap "sleep 2700")
echo "job $J queued $(date)"
trap 'scancel $J 2>/dev/null' EXIT
while :; do s=$(squeue -j $J -h -o %T 2>/dev/null); [ "$s" = RUNNING ] && break; [ -z "$s" ] && { echo "job $J gone"; exit 1; }; sleep 20; done
echo "job $J running on $(squeue -j $J -h -o %N) $(date)"; T0=$(date +%s)
export SLURM_JOB_ID=$J
left() { echo $(( 2600 - ($(date +%s) - T0) )); }
tmg() {   # tmg <tag> <procs> VAR=val ...
  local tag=$1 p=$2; shift 2
  [ $(left) -lt 200 ] && { echo "SKIP $tag (time: $(left) s left)"; return; }
  local t=$(date +%s)
  timeout $(( $(left) - 60 )) ./mnrun.sh $p env ECALC_VERBOSE=1 "$@" ./tests/t_mn_grid > $L/$tag.log 2>&1; local rc=$?
  echo "$tag rc $rc $(( $(date +%s) - t )) s | $(grep -a -c 'VERIFY OK' $L/$tag.log) VERIFY OK, $(grep -a -c 'VERIFY FAILED' $L/$tag.log) FAILED | $(grep -a -c 'RNS_DIST_CACHE_PARTIAL) node' $L/$tag.log) slot takes, grew $(grep -a -c 'pool GREW' $L/$tag.log) | hits: $(grep -a -o '[0-9]* hits' $L/$tag.log | awk '{s+=$1} END {print s}')"
}
K1="RNS_DIST_CACHE_PARTIAL=1 RNS_DIST_CACHE_PARTIAL_K=1 RNS_DIST_CACHE_PARTIAL_FORCE=1"
K2="RNS_DIST_CACHE_PARTIAL=1 RNS_DIST_CACHE_PARTIAL_K=2 RNS_DIST_CACHE_PARTIAL_FORCE=2"
K4="RNS_DIST_CACHE_PARTIAL=1 RNS_DIST_CACHE_PARTIAL_FORCE=8"
tmg tmg2_k1 2 $K1; tmg tmg2_k2 2 $K2; tmg tmg2_k4 2 $K4
tmg tmg2_k1_24 2 MN_P24=2 $K1; tmg tmg2_k2_24 2 MN_P24=2 $K2; tmg tmg2_k4_24 2 MN_P24=2 $K4
tmg tmg3_k2 3 $K2; tmg tmg3_k2_24 3 MN_P24=2 $K2; tmg tmg4_k1_24 4 MN_P24=2 $K1
echo "t_mn_grid done $(date), $(( $(date +%s) - T0 )) s"
E="LIMB_BASE=10 ECALC_NP=4 RNS_POOL_GROW=1 RNS_DIST_CACHE_TRACE=1 ECALC_VERBOSE=1"
for c in off k1 k2 off_24 k1_24 k2_24; do
  base=${c%_24}; x=""; [ "$base" != "$c" ] && x="MN_P24=2"
  case $base in off) ce="RNS_DIST_CACHE_FIT=1"; sl=0,2;; k1) ce="$K1"; sl=0,1;; k2) ce="$K2"; sl=0,1;; esac
  [ $(left) -lt 240 ] && { echo "SKIP cx4_$c (time: $(left) s left)"; continue; }
  t=$(date +%s)
  timeout $(( $(left) - 40 )) ./mnrun.sh 4 env $E $x $ce ./tests/cx_grid 23 1 $sl tests/cx_shapes_c25.txt > $L/cx4_$c.log 2>&1; rc=$?
  echo "cx4_$c rc $rc $(( $(date +%s) - t )) s | $(grep -a -c 'cx_grid node 0: rep [1-9]' $L/cx4_$c.log) timed products on node 0 | $(grep -a -c DIFFERS $L/cx4_$c.log) DIFFERS | $(grep -a -c 'VERIFY OK' $L/cx4_$c.log) VERIFY OK"
done
echo "== $TAG done $(date), $(( $(date +%s) - T0 )) s on the node"

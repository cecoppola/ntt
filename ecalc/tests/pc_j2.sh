#!/bin/bash -l
# PC15 batch j2 (Phase 15 Batch 3 PC, results/PC15.md): ecalc end to end with target-like mn grids (POOL_LOG lowered, as CX15 j2/j3), the
# configurations interleaved; the wall is mnrun alone, the digits compared after it (digcmp.sh against the reference; not in the wall).
#   usage: setsid nohup bash -l ~/ntt-PC15/ecalc/tests/pc_j2.sh <tag> <procs> <digits> <pool_log> <reps> <cfg,...> > ~/PC15/<tag>.out 2>&1 &
#   cfg: c0 (the cache off: RNS_DIST_CACHE_MN=0), pc (RNS_DIST_CACHE_PARTIAL=1, the pool rule), pk1 / pk2 (PARTIAL, K = 1 / 2 forced planes),
#        f1 (RNS_DIST_CACHE_FIT=1, 1 held slot, reserve 0: CX's f1); suffix _pd = MN_P24=2 NEWTON_DKM=1 (e.g. pc_pd)
module load rocm >/dev/null 2>&1
TAG=$1 P=$2 D=$3 PL=$4 REPS=$5 CFGS=$6
L=~/PC15/$TAG; mkdir -p $L
cd ~/ntt-PC15/ecalc || exit 1
echo "== $TAG: $(git log --oneline -1) $(date); $P procs, $D digits, POOL_LOG=$PL, $REPS reps of $CFGS"
case $D in 1000000000) REF=$HOME/ntt/ecalc/results/e_1000000000.out;; 10000000000) REF=$HOME/ntt/ecalc/results/e_1e10.out;; *) echo "no reference for $D"; exit 1;; esac
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J PC --parsable --wrap "sleep 2700")
echo "job $J queued $(date)"
trap 'scancel $J 2>/dev/null' EXIT
while :; do s=$(squeue -j $J -h -o %T 2>/dev/null); [ "$s" = RUNNING ] && break; [ -z "$s" ] && { echo "job $J gone"; exit 1; }; sleep 20; done
echo "job $J running on $(squeue -j $J -h -o %N) $(date)"; T0=$(date +%s)
export SLURM_JOB_ID=$J
N() { srun --jobid=$J -N1 --overlap bash -c "$*"; }
N "mkdir -p /tmp/pc15; rm -rf /tmp/pc15/*"
left() { echo $(( 2600 - ($(date +%s) - T0) )); }
E="ECALC_NP=4 POOL_LOG=$PL RNS_POOL_GROW=1 RNS_DIST_CACHE_TRACE=1 ECALC_VERBOSE=1 MEM_REPORT_DEVS=1"
cfgenv() { local b=${1%_pd} x=""; [ "$b" != "$1" ] && x="MN_P24=2 NEWTON_DKM=1"
  case $b in c0) echo $x RNS_DIST_CACHE_MN=0;; pc) echo $x RNS_DIST_CACHE_PARTIAL=1;;
             pk1) echo $x RNS_DIST_CACHE_PARTIAL=1 RNS_DIST_CACHE_PARTIAL_K=1 RNS_DIST_CACHE_PARTIAL_FORCE=1;;
             pk2) echo $x RNS_DIST_CACHE_PARTIAL=1 RNS_DIST_CACHE_PARTIAL_K=2 RNS_DIST_CACHE_PARTIAL_FORCE=2;;
             f1) echo $x RNS_DIST_CACHE_MN=1 RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_FIT_RESERVE_GB=0;; esac; }
last=150
for r in $(seq 1 $REPS); do for c in ${CFGS//,/ }; do
  tag=${c}_r$r; [ $(left) -lt $(( last + 90 )) ] && { echo "SKIP $tag (time: $(left) s left)"; continue; }
  f=/tmp/pc15/$tag.txt log=$L/$tag.log
  t=$(date +%s.%N)
  timeout $(( $(left) - 30 )) ./mnrun.sh $P env $E $(cfgenv $c) ./ecalc $D $f > $log 2>&1; rc=$?
  w=$(echo "$(date +%s.%N) - $t" | bc); last=${w%.*}
  cmpr=$(N "$PWD/digcmp.sh $f $REF; rm -rf $f $f.*")
  echo "$tag rc $rc $cmpr wall $w s | $(grep -a '^total' $log | tail -1 | sed 's/  */ /g' | cut -c1-90) | $(grep -ac 'VERIFY OK' $log) VERIFY OK | $(grep -a 'RNS_DIST_CACHE_PARTIAL): ' $log | head -1 | sed 's/^ *//' | cut -c1-200) | grew $(grep -ac 'pool GREW' $log)"
done; done
echo "== $TAG done $(date), $(( $(date +%s) - T0 )) s on the node"

#!/bin/bash
# sx15_batch.sh <expected short sha> - agent SX (Phase 15 task 0a, results/SX15.md): one unattended 1-node batch, no code change.
#   1. 10^11 on the defaults (B2 + DKM), verbose (NEWTON_VERBOSE=1 RNS_VERBOSE=1): the division's two low products timed, the grids'
#      piece counts (the A mu high products' skipped pieces = the m of the error bound), the reciprocal's last rounds, the corrections
#   2. 10^11 with NEWTON_DKM=0, the same verbosity: today's single low product on B2 (the no-DKM skip option)
#   3. a sweep of sizes near 10^9 on the device division (BS_MDEV_LOGL=24), DKM on, grids forced (DIST_LOGN_TEST=22 / 24) so the A mu
#      high products skip pieces: the step-1 / step-2 correction counts (the measured X0 - X) against the bound
# One job (-J SX, 45 min), cancelled at the end. Logs in ~/sx15tmp/.
SHA=$1
cd ~/ntt-SX15/ecalc || exit 1
L=~/sx15tmp; mkdir -p "$L"; exec > "$L/batch.log" 2>&1
[ "$(git rev-parse --short=7 HEAD)" = "${SHA:0:7}" ] || { echo "WRONG COMMIT $(git log --oneline -1)"; exit 1; }
echo "== SX15 batch $(date) $(git log --oneline -1)"
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J SX --parsable --wrap "sleep 2700") || exit 1
trap 'scancel $J' EXIT
echo "$J" > ~/sx15tmp/job
until [ "$(squeue -j "$J" -h -o %T)" = RUNNING ]; do [ -z "$(squeue -j "$J" -h -o %T)" ] && { echo "job $J gone"; exit 1; }; sleep 20; done
NODE=$(squeue -j "$J" -h -o %N); echo "job $J on $NODE $(date)"
R() { srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd ~/ntt-SX15/ecalc; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
T=/tmp/sx15_$J
N "mkdir -p $T"
E11=~/ntt/ecalc/results/e_1e11.out
evict() { N "python3 -c \"import os,sys
for p in sys.argv[1:]:
    fd=os.open(p,os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED); os.close(fd)\" $*"; }
run11() { # tag env...
    local tag=$1; shift; local log=$L/$tag.log f=$T/e11.txt
    N "rm -rf $f $f.*"; evict $E11
    local t0; t0=$(date +%s.%N)
    R "env $* ECALC_VERBOSE=1 RNS_VERBOSE=1 NEWTON_VERBOSE=1 ./ecalc 100000000000 $f" > "$log" 2>&1; local rc=$?
    local wall; wall=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b - a}')
    N "rm -rf $f $f.*"
    echo "RUN11 $tag: rc $rc wall ${wall} s; $(grep -a '^total' "$log" | awk '{print "total", $2}'); $(grep -a '^recip ' "$log" | awk '{print "recip", $2}'); $(grep -a '^dm ' "$log" | awk '{print "dm", $2}'); $(grep -a '^dm ' "$log" | grep -o 'corrections [0-9/]*'); $(grep -ac 'VERIFY OK' "$log") VERIFY OK"
    grep -a 'divmod(dev' "$log" | head -1 | cut -c1-600
}
run11 dkm_1
run11 nodkm_1 NEWTON_DKM=0
# the sweep: sizes spread over [4e8, 1.2e9] (Q's top limb varies with the size); no file
for lg in 22 24; do
  for d in 400000007 455555555 511461828 577777777 633333333 700000001 766666666 820719000 888888888 944444444 1000000000 1111111111 1200000000; do
    log=$L/sw_${lg}_$d.log
    R "env BS_MDEV_LOGL=24 DIST_LOGN_TEST=$lg ECALC_VERBOSE=1 RNS_VERBOSE=1 NEWTON_VERBOSE=1 ./ecalc $d" > "$log" 2>&1; rc=$?
    echo "SW lg $lg d $d: rc $rc; $(grep -ac 'VERIFY OK' "$log") VERIFY OK; $(grep -a 'divmod(dev, DKM)' "$log" | head -1 | grep -o 'k [0-9]* = [0-9]* + [0-9]* (s), h [0-9]*\|X_hi Q + corrections [0-9.]* ([-0-9]*)\|corrections [0-9.]* ([-0-9]*)' | tr '\n' ' '); skipped(low cut) $(grep -a 'dist_db .*(low cut)' "$log" | grep -o '[0-9]* skipped' | tr '\n' ' ')"
  done
done
N "rm -rf $T"
echo "== SX15 batch done $(date)"

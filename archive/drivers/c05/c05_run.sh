#!/bin/bash
# c05 (M5): the 60-bit packed output on one aac6 SH5 CPX node.  Fire and forget: flock the node, one hold, steps via srun --overlap,
# summary + done-marker, cancels only its own hold.  usage: c05_run.sh <node> ; output ~/sess1011c/c05/
NODE=${1:-aac6-sh5cpx-a}; W=~/sess1011c/c05; R=~/ntt-c05; mkdir -p $W/out; cd $W
exec 9>~/sess1011c/$NODE.lock; flock 9
rm -f $W/DONE $W/ALERT; : > $W/summary.txt
S() { echo "$(date +%H:%M:%S) $*" | tee -a $W/summary.txt; }
J=$(sbatch -p SH5_MI300A_CPX -w $NODE -N1 --gpus=4 -t 0:44:00 -J c05 --parsable --wrap "sleep 2640" 2>&1); S "hold $J on $NODE"
for i in $(seq 60); do [ "$(squeue -h -j $J -o %T)" = RUNNING ] && break; sleep 5; done
[ "$(squeue -h -j $J -o %T)" = RUNNING ] || { S "ALERT hold not running"; scancel $J; echo "hold not running" > $W/ALERT; touch $W/DONE; exit 1; }
trap 'scancel $J; touch $W/DONE' EXIT
ENVS="RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30"
run() {   # run <tag> <digits> <fmt> [extra env...] -> out/<tag>.txt|bin, log/<tag>.log
  local tag=$1 d=$2 fmt=$3; shift 3
  srun --jobid=$J --overlap bash -lc "module load rocm; cd $R/ecalc; export $ENVS; env ECALC_OUT_PACKED=$fmt $* timeout 1500 ./ecalc $d $W/out/$tag.bin" > $W/$tag.log 2>&1 < /dev/null
  local rc=$?; S "run $tag d=$d fmt=$fmt rc=$rc: $(grep -c 'VERIFY OK' $W/$tag.log) VERIFY OK; $(grep -h 'wrote ' $W/$tag.log | tail -1 | cut -c1-200); $(grep -h '^total\|total  ' $W/$tag.log | tail -1 | cut -c1-80)"
  return $rc
}
cmpf() {  # cmpf <tag> <ref ASCII>
  local f=$W/out/$1.bin; local t=$(date +%s.%N)
  local r=$($R/ecalc/digcmp.sh $f $2); S "digcmp $1 vs $(basename $2): $r  ($(du -sb $f* | awk '{s+=$1} END{print s}') bytes on disk)"
}
# 1. 1e8 and 1e9 against the repo references
R8=$R/ecalc/results/e_100000000.txt; R9=$R/ecalc/results/e_1000000000.txt; [ -f $R9 ] || R9=~/ntt/ecalc/results/e_1000000000.out; [ -f $R8 ] || R8=$R/ecalc/ref/e_100000000.txt
S "refs: $R8 $R9"
for f in 0 1 2; do run p8_f$f 100000000 $f; cmpf p8_f$f $R8; done
for f in 1 2 2 1 0; do run p9_f${f}_$RANDOM 1000000000 $f; done
for t in $(ls $W/out | sed -n 's/^\(p9_f[0-9]_[0-9]*\)\.bin.*/\1/p' | sort -u); do cmpf $t $R9; done
rm -f $W/out/p9_f*
# 2. the K patch path (forced corrections) and RECHECK on the 60-bit form
run k_f0 511461828 0 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=14; run k_f1 511461828 1 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=14; run k_f2 511461828 2 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=14
for f in 1 2; do cmpf k_f$f $W/out/k_f0.bin; done
run rc_f2 1000000000 2 ECALC_RECHECK=1; grep -h "recheck" $W/rc_f2.log | head -5 | tee -a $W/summary.txt
rm -f $W/out/k_f* $W/out/rc_f*
# 3. 4e9: the ASCII file is the reference (VERIFY OK from the run); then the packed forms ABBA
run g4_f0 4000000000 0; head -c 1000000002 $W/out/g4_f0.bin | cmp - <(head -c 1000000002 $R9) && S "4e9 ASCII prefix 1e9 == reference e_1e9" ; 
for f in 1 2 2 1; do run g4_f${f}_$RANDOM 4000000000 $f; done
for t in $(ls $W/out | sed -n 's/^\(g4_f[12]_[0-9]*\)\.bin.*/\1/p' | sort -u); do cmpf $t $W/out/g4_f0.bin; done
rm -f $W/out/g4_*
S "all steps done"; echo SUCCESS > $W/DONE

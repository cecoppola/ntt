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
# second pass: refs, odd chunk sizes (the carry of an odd last limb), RECHECK on the 60-bit form, more 4e9 ABBA rounds
R8=~/ntt/ecalc/results/e_100000000.txt; R9=~/ntt/ecalc/results/e_1000000000.out
for f in 0 1 2; do run q8_f$f 100000000 $f; cmpf q8_f$f $R8; done
for mb in 1 5 7; do run o9_mb${mb} 1000000000 2 MN_OUT_CHUNK_MB=$mb; cmpf o9_mb$mb $R9; done
for f in 1 2; do run rc_f$f 1000000000 $f ECALC_CKPT_TOP=1; srun --jobid=$J --overlap bash -lc "module load rocm; cd $R/ecalc; export $ENVS; ECALC_RECHECK=1 ECALC_OUT_PACKED=$f ./ecalc 1000000000 $W/out/rc_f$f.bin" > $W/rcheck_f$f.log 2>&1 < /dev/null; S "RECHECK fmt=$f rc=$?: $(grep -h -i 'recheck' $W/rcheck_f$f.log | tail -2 | tr '\n' '|' | cut -c1-250)"; done
rm -f $W/out/q8_* $W/out/o9_* $W/out/rc_f*
run g4_f0 4000000000 0
for r in 1 2 3 4; do for f in 1 2 2 1; do run h${r}_f${f}_$RANDOM 4000000000 $f; done; done
cmp_all() { for t in $(ls $W/out | sed -n 's/^\(h[0-9]_f[12]_[0-9]*\)\.bin$/\1/p'); do cmpf $t $W/out/g4_f0.bin; done; }
cmp_all
rm -f $W/out/h* $W/out/g4_*
S "all steps done"; echo SUCCESS > $W/DONE

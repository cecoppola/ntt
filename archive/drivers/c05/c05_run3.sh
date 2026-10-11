#!/bin/bash
NODE=aac6-sh5cpx-a; W=~/sess1011c/c05; R=~/ntt-c05; cd $W
exec 9>~/sess1011c/$NODE.lock; flock 9
rm -f $W/DONE3; : > $W/summary3.txt
J=$(sbatch -p SH5_MI300A_CPX -w $NODE -N1 --gpus=4 -t 0:15:00 -J c05 --parsable --wrap "sleep 800")
trap 'scancel $J; touch $W/DONE3' EXIT
for i in $(seq 60); do [ "$(squeue -h -j $J -o %T)" = RUNNING ] && break; sleep 5; done
(head -c 100000002 ~/ntt/ecalc/results/e_1000000000.out; echo) > $W/out/ref8.txt
for f in 0 1 2; do echo "1e8 f$f: $($R/ecalc/digcmp.sh $W/out/p8_f$f.bin $W/out/ref8.txt)" >> $W/summary3.txt; done
for f in 0 1 2; do
  srun --jobid=$J --overlap bash -lc "module load rocm; cd $R/ecalc; export RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30; ECALC_RECHECK=1 ./ecalc 100000000 $W/out/p8_f$f.bin" > $W/rcheck8_f$f.log 2>&1 < /dev/null
  echo "RECHECK fmt=$f rc=$?: $(grep -i 'recheck' $W/rcheck8_f$f.log | tail -3 | tr '\n' '|' | cut -c1-300)" >> $W/summary3.txt
done

#!/bin/bash
# V3 (Phase 14): one unattended node batch (<= 30 min).  Run on aac6 as: setsid nohup bash ~/ntt-V314/tests/v3_node_batch.sh > ~/V314/batch.out 2>&1 &
# 1. the seed-span bench on the node's CPU (the pipeline's span() against the widened Barrett step and the double-estimate form)
# 2. 10^11 on the defaults (ECALC_VERBOSE=2: the seed thread's line), and with BS_SEED_TERMS=168 (the batch tier's first products
#    at 3 2^6 = 192 points instead of 384)
# 3. 4 x 10^10: defaults, BS_SEED_TERMS=236 (2^8) and 176 (3 2^6)
# (bench.log also has tests/v3_r3_bench: the radix-3 pass inside a 3 2^k transform)
set -u
OUT=~/V314/node; mkdir -p $OUT; cd ~/ntt-V314/ecalc
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:30:00 -J V3 --begin=17:32 --parsable --wrap "sleep 1800")
echo "job $J submitted $(date)"
while [ "$(squeue -h -j $J -o %T)" != "RUNNING" ]; do sleep 10; done
echo "job $J running on $(squeue -h -j $J -o %N) $(date)"
t0=$(date +%s)
R() { srun --jobid=$J -N1 -n1 --gpus=4 "$@"; }
evict() { R python3 -c "
import os
for f in ['results/e_4e10.out']:
    try:
        fd = os.open(f, os.O_RDONLY); os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED); os.close(fd)
    except OSError: pass
" ; }
{
  R lscpu | grep -E "Model name|^CPU\(s\)"
  for k in 4000000000 10000000000 3500000000000; do R env OMP_NUM_THREADS=96 OMP_PROC_BIND=spread ~/V314/v3sb $k 400000; done
  for k in 4000000000 3500000000000; do R env OMP_NUM_THREADS=1 ~/V314/v3sb $k 4000; done
  R ~/ntt-V314/tests/v3_r3_bench 10 12 17 22 26 28 29
} > $OUT/bench.log 2>&1
echo "bench done $(( $(date +%s) - t0 )) s"
run() { local tag=$1; shift; evict; ( time R env ECALC_VERBOSE=2 ECALC_BUDGET_CHECK=0 "$@" ) > $OUT/$tag.log 2>&1; echo "$tag rc $? $(( $(date +%s) - t0 )) s: $(grep -E 'VERIFY' $OUT/$tag.log | head -1)"; }
run e11_def   ./ecalc 100000000000
run e11_s168  env BS_SEED_TERMS=168 ./ecalc 100000000000
run e4_def    ./ecalc 40000000000
run e4_s236   env BS_SEED_TERMS=236 ./ecalc 40000000000
run e4_s176   env BS_SEED_TERMS=176 ./ecalc 40000000000
if [ $(( $(date +%s) - t0 )) -lt 1300 ]; then run e11_def2 ./ecalc 100000000000; fi
scancel $J; echo "done $(date), job $J cancelled"

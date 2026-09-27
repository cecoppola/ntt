#!/bin/bash
# p15_job.sh <batch> - Phase 15 agent P: the node tests, unattended (login node: setsid nohup bash p15_job.sh 1 > log 2>&1 &).
#   batch 1 (B=1): t_roots, t_dist DIST_BIG=33 and 34 (primes 0 and 2: identity + sparse square on four APUs), mnaccept unit,e9,mn
#   batch 2 (B=2): 10^11 at size 1 (the reference evicted), its sha1 (dd iflag=direct) against ~/V214/e_1e11.sha1 (V2: the sha1 of results/e_1e11.out)
# B=21: batch 2, then batch 1 in the same job.  One 1-node job (-J P, <= 45 min), cancelled at the end.
B=${1:-1}
cd "$(dirname "$0")" || exit 2
OUT=../results/P15; mkdir -p "$OUT"
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J P --parsable --wrap "sleep 2700") || exit 2
echo "p15_job batch $B: job $J submitted $(date)"
trap 'scancel $J' EXIT
while [ "$(squeue -j "$J" -h -o %T)" != "RUNNING" ]; do sleep 20; [ -z "$(squeue -j "$J" -h -o %T)" ] && { echo "job $J gone"; exit 2; }; done
NODE=$(squeue -j "$J" -h -o %N); echo "job $J running on $NODE $(date)"
R() { srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $PWD; $*"; }
b1() {
    R "./tests/t_roots" > "$OUT/t_roots_node.log" 2>&1; echo "t_roots rc $? $(tail -1 "$OUT/t_roots_node.log")"
    R "DIST_BIG=33 DIST_BIG_PRIMES=0,2 OMP_NUM_THREADS=96 timeout 900 ./tests/t_dist" > "$OUT/big33.log" 2>&1; echo "big33 rc $? $(tail -1 "$OUT/big33.log")"
    R "DIST_BIG=34 DIST_BIG_PRIMES=0,2 OMP_NUM_THREADS=96 timeout 1200 ./tests/t_dist" > "$OUT/big34.log" 2>&1; echo "big34 rc $? $(tail -1 "$OUT/big34.log")"
    ./mnaccept.sh "$J" --only unit,e9,mn > "$OUT/mnaccept_b1.log" 2>&1; echo "mnaccept rc $?"; grep -E "^(PASS|FAIL)" "$OUT/mnaccept_b1.log"
}
b2() {
    F=/tmp/p15_e1e11.out
    R "python3 -c \"import os; fd=os.open('$HOME/ntt/ecalc/results/e_1e11.out', os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\"; rm -f $F*; ./ecalc 100000000000 $F" > "$OUT/e1e11.log" 2>&1; echo "e1e11 rc $? $(grep -a '^total' "$OUT/e1e11.log" | tail -1)"
    grep -a "VERIFY" "$OUT/e1e11.log" | tail -3
    R "ls -la $F*; S=\$(dd if=$F iflag=direct bs=64M status=none | sha1sum | cut -d' ' -f1); R=\$(cut -d' ' -f1 ~/V214/e_1e11.sha1); echo \"sha1 \$S ref \$R\"; [ \"\$S\" = \"\$R\" ] && echo SHA1 IDENTICAL || echo SHA1 DIFFERS; rm -rf $F*" > "$OUT/e1e11_hash.log" 2>&1; cat "$OUT/e1e11_hash.log"
}
b3() {   # the other primes at 2^34 (the first run's driver stopped after prime 0: strtok, fixed)
    R "DIST_BIG=34 DIST_BIG_PRIMES=1,2,3 OMP_NUM_THREADS=96 timeout 1200 ./tests/t_dist" > "$OUT/big34b.log" 2>&1; echo "big34b rc $? $(tail -1 "$OUT/big34b.log")"
}
for b in$(echo "$B" | grep -o .); do b$b; done
echo "p15_job batch $B done $(date)"

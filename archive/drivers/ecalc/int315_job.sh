#!/bin/bash
# int315_job.sh <batches> [node] - Phase 15 Batch 3 agent INT3 (results/INT315.md): the node tests of P24 + MPB + DKM composed, unattended
# (login node: setsid nohup bash int315_job.sh 12345 > ../results/INT315/job_<tag>.log 2>&1 &).  Each batch is its own 1-node job
# (-J INT3, <= 45 min), cancelled at its end; everything inside a batch runs one after the other (nothing maps memory while kernels run).
#   1: the defaults (every switch off): mnaccept --only unit,e9,mn,recheck,corr; t_p24; t_crt (decimal, binary)
#   2: the four switches on (S4 = ECALC_NP=auto ECALC_NP_AUTO_MIN=1 MN_P24=1 NEWTON_DKM=1): t_newton (NEWTON_DEVICE=1 DIST_LOGN_TEST=20),
#      t_crt, t_patch; mnaccept --only e9,mn,corr,recheck under S4
#   3: the forced mixed tests (ECALC_NP_AUTO_TERMS lowered) with MPB + P24 (+ DKM): t_mn_grid at 2, 3, 4 (RNS_VERBOSE: the 18-digit lines
#      against MPB's rule by tests/mpb15_lines.py, the P24 lines counted, no 18-digit four-prime mn line); MN_P24=2 at 2; 10^9 / 10^8 at
#      sizes 1-4 with lowered bounds under S4; mnaccept --only e9,mn,corr under S4 with ECALC_NP_AUTO_TERMS=2000000
#   4: 4 x 10^10 at size 1, the defaults and S4 (digcmp against results/e_4e10.out, the reference evicted first); t_mn_grid defaults at 3, 4
#   5: 10^11 at size 1, the defaults and S4 (digcmp against results/e_1e11.out, the reference evicted first)
cd "$(dirname "$0")" || exit 2
OUT=../results/INT315; mkdir -p "$OUT"
REF=$HOME/int315tmp/ref                  # e_1000000000.txt (a link to ~/ntt/ecalc/ref) and e_100000000.txt (its first 10^8 digits + a newline)
SHA=$(git log --oneline -1 | cut -c1-8)
NODEW=${2:+-w $2}
S4="ECALC_NP=auto ECALC_NP_AUTO_MIN=1 MN_P24=1 NEWTON_DKM=1"
echo "int315_job $1 at $SHA $(date)"
job() {
    local B=$1
    J=$(sbatch -p PPAC_MI300A_SPX -N1 $NODEW --gpus=4 -t 0:45:00 -J INT3 --parsable --wrap "sleep 2700") || exit 2
    echo "batch $B: job $J submitted $(date)"
    trap 'scancel $J' EXIT
    while [ "$(squeue -j "$J" -h -o %T)" != "RUNNING" ]; do sleep 20; [ -z "$(squeue -j "$J" -h -o %T)" ] && { echo "job $J gone"; exit 2; }; done
    NODE=$(squeue -j "$J" -h -o %N); echo "batch $B: job $J running on $NODE $(date)"
    b$B
    [ "$(squeue -j "$J" -h -o %T)" = "RUNNING" ] || echo "batch $B: JOB $J ENDED BEFORE THE BATCH DID (cancelled from outside?): the batch's results are void $(sacct -j "$J" -X -n -o State,End)"
    scancel "$J"; trap - EXIT
    echo "batch $B done $(date)"
}
R() { srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $PWD; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
M() { local to=$1 p=$2; shift 2; SLURM_JOB_ID=$J timeout "$to" ./mnrun.sh "$p" env "$@"; }
vline() { grep -a 'VERIFY' "$1" | tail -1; }
vcnt() { echo "$(grep -ac 'VERIFY OK' "$1") VERIFY OK, $(grep -ac 'VERIFY FAILED' "$1") FAILED"; }
# the mn lines: the 18-digit ones against MPB's rule, the P24 pieces counted, the 18-digit four-prime pieces counted (0 expected under MN_P24=1)
lines() { local k=$1 f=$2; echo "$(python3 ../tests/mpb15_lines.py "$k" "$f" | tail -1); P24 piece lines $(grep -a 'dist_mn node' "$f" | grep -ac '4 primes, P24 (rows'), 18-digit four-prime piece lines $(grep -a 'dist_mn node' "$f" | grep -ac ', 4 primes (rows')"; }
fadv() { echo "python3 -c \"import os; fd=os.open('$1', os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\""; }
erun() { local tag=$1 p=$2 d=$3 pl=$4 k=$5; shift 5; local log=$OUT/$tag.log f=/tmp/int315_$tag.txt rc c
    if [ "$p" = 1 ]; then R "env POOL_LOG=$pl $S4 ECALC_NP_AUTO_TERMS=$k $* timeout 900 ./ecalc $d $f" > "$log" 2>&1; rc=$?
    else M 900 "$p" POOL_LOG=$pl $S4 ECALC_NP_AUTO_TERMS=$k "$@" ./ecalc "$d" "$f" > "$log" 2>&1; rc=$?; fi
    c=$(N "$PWD/digcmp.sh $f $REF/e_$d.txt; rm -rf $f $f.*")
    echo "$tag: size $p d $d k $k $S4 $*: rc $rc, $c, $(vcnt "$log"); $(lines "$k" "$log"); $(grep -a '^total' "$log" | tail -1 | sed 's/  */ /g' | cut -c1-60)"
}
big() { local tag=$1 d=$2 ref=$3; shift 3; local log=$OUT/$tag.log f=/tmp/int315_$tag.out t0 rc
    t0=$(date +%s); R "$(fadv "$ref"); rm -rf $f*; env $* ./ecalc $d $f" > "$log" 2>&1; rc=$?
    echo "$tag: $* ./ecalc $d: rc $rc, $(vline "$log"), $(grep -a '^total' "$log" | tail -1 | sed 's/  */ /g' | cut -c1-80) (wall $(( $(date +%s) - t0 )) s)"
    t0=$(date +%s); echo "$tag: against $(basename "$ref"): $(N "$PWD/digcmp.sh $f $ref; rm -rf $f $f.*") ($(( $(date +%s) - t0 )) s)"
}
b1() {
    ./mnaccept.sh "$J" --only unit,e9,mn,recheck,corr > "$OUT/mnaccept_off.log" 2>&1; echo "mnaccept unit,e9,mn,recheck,corr (defaults) rc $?"; grep -aE "^(PASS|FAIL)" "$OUT/mnaccept_off.log"
    R "timeout 600 ./tests/t_p24" > "$OUT/t_p24.log" 2>&1; echo "t_p24 rc $? $(vline "$OUT/t_p24.log")"
    R "LIMB_BASE=10 ./tests/t_crt 24" > "$OUT/t_crt_off.log" 2>&1; echo "t_crt 24 decimal (defaults) rc $? $(vline "$OUT/t_crt_off.log")"
    R "./tests/t_crt 24" > "$OUT/t_crt_off_bin.log" 2>&1; echo "t_crt 24 binary (defaults) rc $? $(vline "$OUT/t_crt_off_bin.log")"
}
b2() {
    R "env $S4 NEWTON_DEVICE=1 DIST_LOGN_TEST=20 timeout 1500 ./tests/t_newton 20" > "$OUT/t_newton_s4.log" 2>&1; echo "t_newton 20 (S4, NEWTON_DEVICE=1 DIST_LOGN_TEST=20) rc $? $(vline "$OUT/t_newton_s4.log")"
    R "env $S4 LIMB_BASE=10 ./tests/t_crt 24" > "$OUT/t_crt_s4.log" 2>&1; echo "t_crt 24 decimal (S4) rc $? $(vline "$OUT/t_crt_s4.log")"
    R "env $S4 timeout 900 ./tests/t_patch" > "$OUT/t_patch_s4.log" 2>&1; echo "t_patch (S4) rc $? $(vline "$OUT/t_patch_s4.log")"
    env $S4 ./mnaccept.sh "$J" --only e9,mn,corr,recheck > "$OUT/mnaccept_s4.log" 2>&1; echo "mnaccept e9,mn,corr,recheck ($S4) rc $?"; grep -aE "^(PASS|FAIL)" "$OUT/mnaccept_s4.log"
}
b3() {
    for pk in "2 11000000" "3 11000000" "4 20000000"; do set -- $pk; local p=$1 k=$2 log=$OUT/t_mn_grid_mix_$1.log
        M 1200 "$p" LIMB_BASE=10 $S4 ECALC_NP_AUTO_TERMS=$k RNS_VERBOSE=1 ./tests/t_mn_grid 1 28 > "$log" 2>&1; rc=$?
        echo "t_mn_grid size $p decimal, $S4, bound $k: rc $rc: $(vcnt "$log"); $(lines $k "$log")"
    done
    log=$OUT/t_mn_grid_mix_2_p24_2.log
    M 1200 2 LIMB_BASE=10 $S4 MN_P24=2 ECALC_NP_AUTO_TERMS=11000000 RNS_VERBOSE=1 ./tests/t_mn_grid 1 28 > "$log" 2>&1; rc=$?
    echo "t_mn_grid size 2 decimal, $S4 MN_P24=2 (the last wins), bound 11000000: rc $rc: $(vcnt "$log"); P24 piece lines $(grep -a 'dist_mn node' "$log" | grep -ac '4 primes, P24 (rows'), 18-digit piece lines $(grep -a 'dist_mn node' "$log" | grep -ac 'primes (rows')"
    for p in 1 2 4; do erun e9_k8e6_$p $p 1000000000 $([ $p = 1 ] && echo 31 || echo 29) 8000000 RNS_VERBOSE=1; done
    for p in 2 3 4; do erun e8_k2e6_$p $p 100000000 27 2000000 RNS_VERBOSE=1; done
    env $S4 ECALC_NP_AUTO_TERMS=2000000 ./mnaccept.sh "$J" --only e9,mn,corr > "$OUT/mnaccept_s4_k.log" 2>&1; echo "mnaccept e9,mn,corr ($S4 ECALC_NP_AUTO_TERMS=2000000) rc $?"; grep -aE "^(PASS|FAIL)" "$OUT/mnaccept_s4_k.log"
}
b4() {
    big e4e10_off 40000000000 "$HOME/ntt/ecalc/results/e_4e10.out"
    big e4e10_s4 40000000000 "$HOME/ntt/ecalc/results/e_4e10.out" $S4
    for p in 3 4; do log=$OUT/t_mn_grid_off_$p.log; M 1200 $p ./tests/t_mn_grid 1 28 > "$log" 2>&1; echo "t_mn_grid size $p (defaults) rc $?: $(vcnt "$log")"; done
}
b5() {   # the S4 run's files cmp'd against the defaults' (the packed header has no run-dependent field), then the defaults' against the reference
    local f0=/tmp/int315_e1e11_off.out f1=/tmp/int315_e1e11_s4.out ref=$HOME/ntt/ecalc/results/e_1e11.out t0 rc
    for x in off s4; do local f=/tmp/int315_e1e11_$x.out log=$OUT/e1e11_$x.log e=""; [ $x = s4 ] && e=$S4
        t0=$(date +%s); R "$(fadv "$ref"); rm -rf $f*; env $e ./ecalc 100000000000 $f" > "$log" 2>&1; rc=$?
        echo "e1e11_$x: $e ./ecalc 1e11: rc $rc, $(vline "$log"), $(grep -a '^total' "$log" | tail -1 | sed 's/  */ /g' | cut -c1-80) (wall $(( $(date +%s) - t0 )) s)"
    done
    t0=$(date +%s); echo "e1e11_s4 against e1e11_off (cmp of every part file): $(N "cd /tmp; n=0; m=0; for a in int315_e1e11_off.out int315_e1e11_off.out.part*; do [ -f \$a ] || continue; m=\$((m+1)); b=\${a/_off/_s4}; cmp -s \$a \$b || n=\$((n+1)); done; echo \"\$m files, \$n differ\"") ($(( $(date +%s) - t0 )) s)"
    N "rm -rf $f1 $f1.*"
    t0=$(date +%s); echo "e1e11_off against e_1e11.out: $(N "$PWD/digcmp.sh $f0 $ref; rm -rf $f0 $f0.*") ($(( $(date +%s) - t0 )) s)"
}
for b in $(echo "$1" | grep -o .); do job $b; done
echo "int315_job $1 done $(date)"

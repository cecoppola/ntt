#!/bin/bash
# mpb15_job.sh <batches> [node] - Phase 15 agent MPB: the node tests of ECALC_NP_AUTO_MIN=1 (with ECALC_NP=auto), unattended
# (login node: setsid nohup bash mpb15_job.sh 12 ppac-pl1-s24-16 > log 2>&1 &).  Each batch is its own 1-node job (-J MPB, <= 45 min),
# cancelled at its end.  Every run has the switch on; tests/mpb15_lines.py checks every product line against the rule (four primes
# iff min(na, nb) > the switch-over) and counts the products the switch moved to three (min <= k < na + nb).
#   1: the forced tests: t_crt (real bound, 1e6, switch off, binary), t_dbig / t_newton (decimal, lowered bound), t_mn_grid at 2, 3, 4
#      node-processes (decimal, 0.7 x the cap), 10^9 at sizes 1, 2, 4 (bound 8e6), 10^8 at sizes 1-4 (bound 2e6), 10^9 at sizes 3, 4
#      (bound 2e7: a tree level all / half moved to three), every digit file against the reference
#   2: mnaccept --only unit,e9,mn with the switch on (the real bound), then --only e9,mn with the bound lowered to 2e6
cd "$(dirname "$0")" || exit 2
OUT=../results/MPB15; mkdir -p "$OUT"
REF=$HOME/ntt/ecalc/ref
SHA=$(git log --oneline -1 | cut -c1-8)
NODEW=${2:+-w $2}
AM="ECALC_NP=auto ECALC_NP_AUTO_MIN=1"
echo "mpb15_job $1 at $SHA $(date)"
job() {
    local B=$1
    J=$(sbatch -p PPAC_MI300A_SPX -N1 $NODEW --gpus=4 -t 0:45:00 -J MPB --parsable --wrap "sleep 2700") || exit 2
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
chk() { python3 ../tests/mpb15_lines.py "$1" "$2"; }
erun() { local tag=$1 p=$2 d=$3 pl=$4 k=$5; shift 5; local log=$OUT/$tag.log f=/tmp/mpb15_$tag.txt rc c
    if [ "$p" = 1 ]; then R "env POOL_LOG=$pl $AM ECALC_NP_AUTO_TERMS=$k $* timeout 900 ./ecalc $d $f" > "$log" 2>&1; rc=$?
    else M 900 "$p" POOL_LOG=$pl $AM ECALC_NP_AUTO_TERMS=$k "$@" ./ecalc "$d" "$f" > "$log" 2>&1; rc=$?; fi
    c=$(N "$PWD/digcmp.sh $f $REF/e_$d.txt; rm -rf $f $f.*")
    echo "$tag: size $p d $d k $k $*: rc $rc, $c, $(grep -ac 'VERIFY OK' "$log") VERIFY OK; $(chk "$k" "$log"); $(grep -a '^total' "$log" | tail -1 | sed 's/  */ /g' | cut -c1-60)"
}
b1() {
    R "LIMB_BASE=10 $AM ./tests/t_crt 24" > "$OUT/t_crt_min.log" 2>&1; echo "t_crt (min, real bound) rc $? $(vline "$OUT/t_crt_min.log")"
    R "LIMB_BASE=10 $AM ECALC_NP_AUTO_TERMS=1000000 ./tests/t_crt 24" > "$OUT/t_crt_min_k.log" 2>&1; echo "t_crt (min, bound 1e6) rc $? $(vline "$OUT/t_crt_min_k.log")"
    R "LIMB_BASE=10 ECALC_NP=auto ./tests/t_crt 24" > "$OUT/t_crt_nomin.log" 2>&1; echo "t_crt (switch off) rc $? $(vline "$OUT/t_crt_nomin.log")"
    R "$AM ./tests/t_crt 24" > "$OUT/t_crt_min_bin.log" 2>&1; echo "t_crt (min, binary: four everywhere) rc $? $(vline "$OUT/t_crt_min_bin.log")"
    R "LIMB_BASE=10 $AM ECALC_NP_AUTO_TERMS=4000000 RNS_VERBOSE=1 ./tests/t_dbig 0 x" > "$OUT/t_dbig_k.log" 2>&1; echo "t_dbig 0 x (decimal, bound 4e6) rc $? $(vline "$OUT/t_dbig_k.log"); $(chk 4000000 "$OUT/t_dbig_k.log")"
    R "LIMB_BASE=10 $AM ECALC_NP_AUTO_TERMS=300000 RNS_VERBOSE=1 ./tests/t_newton 20" > "$OUT/t_newton_k.log" 2>&1; echo "t_newton 20 (decimal, bound 3e5) rc $? $(vline "$OUT/t_newton_k.log"); $(chk 300000 "$OUT/t_newton_k.log")"
    for p in 2 3 4; do
        k=$(( p >= 4 ? 46976204 : 23488102 ))
        log=$OUT/t_mn_grid_${p}.log
        M 1200 "$p" LIMB_BASE=10 $AM ECALC_NP_AUTO_TERMS=$k RNS_VERBOSE=1 ./tests/t_mn_grid 1 28 > "$log" 2>&1; rc=$?
        echo "t_mn_grid size $p decimal (bound $k) rc $rc: $(grep -ac 'VERIFY OK' "$log") VERIFY OK, $(grep -ac 'VERIFY FAILED' "$log") FAILED; $(chk $k "$log")"
    done
    for p in 1 2 4; do erun e9_k8e6_$p $p 1000000000 $([ $p = 1 ] && echo 31 || echo 29) 8000000 RNS_VERBOSE=1; done
    for p in 1 2 3 4; do erun e8_k2e6_$p $p 100000000 $([ $p = 1 ] && echo 31 || echo 27) 2000000 RNS_VERBOSE=1; done
    for p in 3 4; do erun e9_k2e7_$p $p 1000000000 29 20000000 RNS_VERBOSE=1; done
}
b2() {
    env $AM ./mnaccept.sh "$J" --only unit,e9,mn > "$OUT/mnaccept_min.log" 2>&1; echo "mnaccept unit,e9,mn ($AM) rc $?"; grep -aE "^(PASS|FAIL)" "$OUT/mnaccept_min.log"
    env $AM ECALC_NP_AUTO_TERMS=2000000 ./mnaccept.sh "$J" --only e9,mn > "$OUT/mnaccept_min_k.log" 2>&1; echo "mnaccept e9,mn ($AM ECALC_NP_AUTO_TERMS=2000000) rc $?"; grep -aE "^(PASS|FAIL)" "$OUT/mnaccept_min_k.log"
}
for b in $(echo "$1" | grep -o .); do job $b; done
echo "mpb15_job $1 done $(date)"

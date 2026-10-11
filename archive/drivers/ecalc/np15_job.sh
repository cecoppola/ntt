#!/bin/bash
# np15_job.sh <batches> - Phase 15 agent NP (PLAN 37 row 1): the node tests of ECALC_NP=auto, unattended
# (login node: setsid nohup bash np15_job.sh 123 > log 2>&1 &).  Each batch is its own 1-node job (-J NP, <= 45 min), cancelled at its end.
#   1: mnaccept --only unit,e9,mn with ECALC_NP=auto (decimal e9 / mn: every product within the bound = three primes; the binary
#      unit tests and e9 binary: auto = four everywhere)
#   2: the forced tests -- ECALC_NP_AUTO_TERMS lowers the switch-over so three and four primes occur in one run: t_crt part 0,
#      t_dbig, t_newton (decimal), t_mn_grid at 2, 3, 4 node-processes (decimal and binary), 10^9 at sizes 1, 2, 4 (bound 8e6)
#      and 10^8 at sizes 1, 2, 3, 4 (bound 2e6), 10^9 at size 4 (bound 5e7: a tree level mixed), every digit file against the reference
#   3: 10^11 at size 1, ECALC_NP=auto and then the defaults (ECALC_NP=3), both hashed against ~/V214/e_1e11.sha1 (the unpacked stream)
#   4: the three-vs-four cost per product: 10^10 at sizes 1 and 2, RNS_STRATEGY=C, RNS_VERBOSE=1, ECALC_NP=auto with the real bound
#      (all three) and with ECALC_NP_AUTO_TERMS=1 (every distributed product at four), the product lines paired by order
cd "$(dirname "$0")" || exit 2
OUT=../results/NP15; mkdir -p "$OUT"
REF=$HOME/ntt/ecalc/ref
SHA=$(git log --oneline -1 | cut -c1-8)
echo "np15_job $1 at $SHA $(date)"
job() {   # job <batch>: one 1-node job for the batch
    local B=$1
    J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J NP --parsable --wrap "sleep 2700") || exit 2
    echo "batch $B: job $J submitted $(date)"
    trap 'scancel $J' EXIT
    while [ "$(squeue -j "$J" -h -o %T)" != "RUNNING" ]; do sleep 20; [ -z "$(squeue -j "$J" -h -o %T)" ] && { echo "job $J gone"; exit 2; }; done
    NODE=$(squeue -j "$J" -h -o %N); echo "batch $B: job $J running on $NODE $(date)"
    b$B
    scancel "$J"; trap - EXIT
    echo "batch $B done $(date)"
}
R() { srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $PWD; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
M() { local to=$1 p=$2; shift 2; SLURM_JOB_ID=$J timeout "$to" ./mnrun.sh "$p" env "$@"; }
vline() { grep -a 'VERIFY' "$1" | tail -1; }
b1() {
    ECALC_NP=auto ./mnaccept.sh "$J" --only unit,e9,mn > "$OUT/mnaccept_auto.log" 2>&1; echo "mnaccept (ECALC_NP=auto) rc $?"; grep -E "^(PASS|FAIL)" "$OUT/mnaccept_auto.log"
}
# ecalc at size p (1: srun) with env, the digits against the reference: one line
erun() { local tag=$1 p=$2 d=$3 pl=$4; shift 4; local log=$OUT/$tag.log f=/tmp/np15_$tag.txt rc c
    if [ "$p" = 1 ]; then R "env POOL_LOG=$pl $* timeout 900 ./ecalc $d $f" > "$log" 2>&1; rc=$?
    else M 900 "$p" POOL_LOG=$pl "$@" ./ecalc "$d" "$f" > "$log" 2>&1; rc=$?; fi
    if [ -s "$REF/e_$d.txt" ]; then c=$(N "$PWD/digcmp.sh $f $REF/e_$d.txt; rm -rf $f $f.*")
    else c="sha1 $(N "../tools/unpack_digits -q \$(ls $f.part* 2>/dev/null | sort || true) \$([ -e $f.part0000 ] || echo $f) | sha1sum | cut -c1-40; rm -rf $f $f.*")"; fi   # (no reference file: the runs' hashes are compared with each other)
    local n4 n3; n4=$(grep -ac ', 4 primes' "$log"); n3=$(grep -ac ', 3 primes' "$log")
    echo "$tag: size $p d $d $*: rc $rc, $c, $(grep -ac 'VERIFY OK' "$log") VERIFY OK, product lines at 4 primes $n4, at 3 $n3; $(grep -a '^total' "$log" | tail -1 | sed 's/  */ /g' | cut -c1-60)"
}
b2() {
    R "LIMB_BASE=10 ECALC_NP=auto ./tests/t_crt 24" > "$OUT/t_crt_auto.log" 2>&1; echo "t_crt auto (real bound) rc $? $(vline "$OUT/t_crt_auto.log")"
    R "LIMB_BASE=10 ECALC_NP=auto ECALC_NP_AUTO_TERMS=1000000 ./tests/t_crt 24" > "$OUT/t_crt_auto_k.log" 2>&1; echo "t_crt auto (bound 1e6) rc $? $(vline "$OUT/t_crt_auto_k.log")"
    R "ECALC_NP=auto ./tests/t_crt 24" > "$OUT/t_crt_auto_bin.log" 2>&1; echo "t_crt auto binary rc $? $(vline "$OUT/t_crt_auto_bin.log")"
    R "LIMB_BASE=10 ECALC_NP=auto ECALC_NP_AUTO_TERMS=4000000 RNS_VERBOSE=1 ./tests/t_dbig 0 x" > "$OUT/t_dbig_k.log" 2>&1; echo "t_dbig 0 x (decimal, bound 4e6) rc $? $(vline "$OUT/t_dbig_k.log"); lines at 4 primes $(grep -ac ', 4 primes' "$OUT/t_dbig_k.log"), at 3 $(grep -ac ', 3 primes' "$OUT/t_dbig_k.log")"
    R "LIMB_BASE=10 ECALC_NP=auto ECALC_NP_AUTO_TERMS=300000 RNS_VERBOSE=1 ./tests/t_newton 20" > "$OUT/t_newton_k.log" 2>&1; echo "t_newton 20 (decimal, bound 3e5) rc $? $(vline "$OUT/t_newton_k.log"); lines at 4 primes $(grep -ac ', 4 primes' "$OUT/t_newton_k.log"), at 3 $(grep -ac ', 3 primes' "$OUT/t_newton_k.log")"
    for p in 2 3 4; do
        k=$(( p >= 4 ? 46976204 : 23488102 ))                   # 0.7 x the cap 2^(24 + floor(log2 p)) (DIST_LOGN_TEST=24 in the test)
        for base in 10 2; do
            log=$OUT/t_mn_grid_${p}_b$base.log
            M 1200 "$p" LIMB_BASE=$base ECALC_NP=auto ECALC_NP_AUTO_TERMS=$k RNS_VERBOSE=1 ./tests/t_mn_grid 1 28 > "$log" 2>&1; rc=$?
            echo "t_mn_grid size $p base $base (bound $k) rc $rc: $(grep -ac 'VERIFY OK' "$log") VERIFY OK, $(grep -ac 'VERIFY FAILED' "$log") FAILED; dist_mn lines at 4 primes $(grep -ac ', 4 primes' "$log"), at 3 $(grep -ac ', 3 primes' "$log")"
        done
    done
    for p in 1 2 4; do erun e9_k8e6_$p $p 1000000000 $([ $p = 1 ] && echo 31 || echo 29) ECALC_NP=auto ECALC_NP_AUTO_TERMS=8000000 RNS_VERBOSE=1; done
    for p in 1 2 3 4; do erun e8_k2e6_$p $p 100000000 $([ $p = 1 ] && echo 31 || echo 27) ECALC_NP=auto ECALC_NP_AUTO_TERMS=2000000 RNS_VERBOSE=1; done
    erun e9_k5e7_4 4 1000000000 29 ECALC_NP=auto ECALC_NP_AUTO_TERMS=50000000 RNS_VERBOSE=1
}
e11() { local tag=$1; shift; local F=/tmp/np15_e1e11.out
    R "python3 -c \"import os; fd=os.open('$HOME/ntt/ecalc/results/e_1e11.out', os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\"; rm -rf $F*; env $* ./ecalc 100000000000 $F" > "$OUT/$tag.log" 2>&1
    echo "$tag ($*): rc $? $(grep -a 'VERIFY' "$OUT/$tag.log" | tail -1 | cut -c1-80) | $(grep -a '^total' "$OUT/$tag.log" | tail -1)"
    grep -a "rns_init: primes\|rns_init: three" "$OUT/$tag.log" | head -2
    R "S=\$(../tools/unpack_digits -q $F | sha1sum | cut -d' ' -f1); R=\$(cut -d' ' -f1 ~/V214/e_1e11.sha1); echo \"sha1 \$S ref \$R\"; [ \"\$S\" = \"\$R\" ] && echo SHA1 IDENTICAL || echo SHA1 DIFFERS; rm -rf $F*" > "$OUT/$tag.hash" 2>&1; cat "$OUT/$tag.hash"
}
b3() { e11 e1e11_auto ECALC_NP=auto; e11 e1e11_def; }
b6() { e11 e1e11_def_r; e11 e1e11_auto_r ECALC_NP=auto; }   # the pair in the reverse order (the init: an order effect or a cost)
b4() {
    for p in 1 2; do for k in real 1; do
        tag=e10_s${p}_k$k; ex=""; [ $k = 1 ] && ex="ECALC_NP_AUTO_TERMS=1"
        erun $tag $p 10000000000 $([ $p = 1 ] && echo 31 || echo 29) ECALC_NP=auto $ex RNS_STRATEGY=C RNS_VERBOSE=1
    done; done
}
b5() {   # size 1 again with the B form's lines tagged (59371a3): the forced unit tests and runs, the B form and C
    R "LIMB_BASE=10 ECALC_NP=auto ECALC_NP_AUTO_TERMS=4000000 RNS_VERBOSE=1 ./tests/t_dbig 0 x" > "$OUT/t_dbig_k5.log" 2>&1; echo "t_dbig 0 x (decimal, bound 4e6) rc $? $(vline "$OUT/t_dbig_k5.log"); lines at 4 primes $(grep -ac ', 4 primes' "$OUT/t_dbig_k5.log"), at 3 $(grep -ac ', 3 primes' "$OUT/t_dbig_k5.log")"
    R "LIMB_BASE=10 ECALC_NP=auto ECALC_NP_AUTO_TERMS=300000 RNS_VERBOSE=1 ./tests/t_newton 20" > "$OUT/t_newton_k5.log" 2>&1; echo "t_newton 20 (decimal, bound 3e5) rc $? $(vline "$OUT/t_newton_k5.log"); lines at 4 primes $(grep -ac ', 4 primes' "$OUT/t_newton_k5.log"), at 3 $(grep -ac ', 3 primes' "$OUT/t_newton_k5.log")"
    R "LIMB_BASE=10 ECALC_NP=auto ECALC_NP_AUTO_TERMS=300000 RNS_VERBOSE=1 RNS_STRATEGY=C ./tests/t_newton 20" > "$OUT/t_newton_k5c.log" 2>&1; echo "t_newton 20 (decimal, bound 3e5, RNS_STRATEGY=C) rc $? $(vline "$OUT/t_newton_k5c.log"); lines at 4 primes $(grep -ac ', 4 primes' "$OUT/t_newton_k5c.log"), at 3 $(grep -ac ', 3 primes' "$OUT/t_newton_k5c.log")"
    erun e9_k8e6_1b 1 1000000000 31 ECALC_NP=auto ECALC_NP_AUTO_TERMS=8000000 RNS_VERBOSE=1
    erun e9_k8e6_1c 1 1000000000 31 ECALC_NP=auto ECALC_NP_AUTO_TERMS=8000000 RNS_VERBOSE=1 RNS_STRATEGY=C
    erun e8_k2e6_1b 1 100000000 31 ECALC_NP=auto ECALC_NP_AUTO_TERMS=2000000 RNS_VERBOSE=1
}
for b in $(echo "$1" | grep -o .); do job $b; done
echo "np15_job $1 done $(date)"

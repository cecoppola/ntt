#!/bin/bash
# ew15_batch.sh <stage> <expected short sha> - agent EW (Phase 15 Batch 3, results/EW15.md): one unattended node batch.
#   A1 MN_OUT_DKM_HI=1 through the regression: t_newton 20 (NEWTON_DEVICE=1 DIST_LOGN_TEST=20: sections 5-7), mnaccept unit,e9,mn,recheck,corr
#      with the switch, then e9,recheck,corr again with BS_MDEV_LOGL=24 (at 1e9 size 1 takes the device flow only so: the switch's size-1 path)
#   A2 forced corrections with the switch: size 1 (1e9 through the device flow) ECALC_TEST_CORR +-7, NEWTON_DKM_TEST_HI 7 / -9 (+5),
#      MN_OUT_DKM_HI_TEST_CARRY=1 (the carry path) with -7 and with c511's +14 (digits inside the file); sizes 2 and 4 (1e8, POOL_LOG=27):
#      c108u +36, c108d -39, NEWTON_DKM_TEST_HI -9, the carry path with +36; 4e10 identical with the switch
#   C  10^11 timing: on / off interleaved, 3 each, the packed file, the reference evicted before each run; on_1 against the reference,
#      every other file cmp'd against on_1's (the packed header has no run-dependent field)
# One 1-node job (-J EW, 45 min), cancelled at the end.  Logs in ~/ew15tmp/<stage>/.  STAGES="A1 A2" runs them in turn (a job each).
ST=$1 SHA=$2
cd ~/ntt-EW15/ecalc || exit 1
L=~/ew15tmp/$ST; mkdir -p "$L"; exec > "$L/batch.log" 2>&1
[ "$(git rev-parse --short=7 HEAD)" = "${SHA:0:7}" ] || { echo "WRONG COMMIT $(git log --oneline -1)"; exit 1; }
echo "== EW15 batch $ST $(date) $(git log --oneline -1)"
X=; [ -n "${NODE_X:-}" ] && X="-x $NODE_X"
J=$(sbatch -p PPAC_MI300A_SPX -N1 $X --gpus=4 -t 0:45:00 -J EW --parsable --wrap "sleep 2700") || exit 1
trap 'scancel $J' EXIT
echo "$J" > ~/ew15tmp/"$ST".job
until [ "$(squeue -j "$J" -h -o %T)" = RUNNING ]; do [ -z "$(squeue -j "$J" -h -o %T)" ] && { echo "job $J gone"; exit 1; }; sleep 20; done
NODE=$(squeue -j "$J" -h -o %N); echo "job $J on $NODE $(date)"
R() { srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd ~/ntt-EW15/ecalc; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
REFD=~/ntt/ecalc/ref T=/tmp/ew15_$J
N "mkdir -p $T"
cmpref() { N "$PWD/digcmp.sh $1 $2; rm -rf $1 $1.*"; }
evict() { N "python3 -c \"import os,sys
for p in sys.argv[1:]:
    fd=os.open(p,os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED); os.close(fd)\" $*"; }
one() { # tag procs d env...  (the digits against ref/e_<d>.txt, or the first d digits of e_1000000000.txt)
    local tag=$1 p=$2 d=$3; shift 3; local f=$T/$tag.txt log=$L/$tag.log rc c ref=$REFD/e_$d.txt
    if [ "$p" = 1 ]; then R "env $* ECALC_VERBOSE=2 RNS_VERBOSE=1 ./ecalc $d $f" > "$log" 2>&1; rc=$?
    else SLURM_JOB_ID=$J timeout 900 ./mnrun.sh "$p" env "$@" ./ecalc "$d" "$f" > "$log" 2>&1; rc=$?; fi
    local np; np=$(N "ls $f.part* 2>/dev/null | wc -l")
    if [ -s "$ref" ]; then c=$(cmpref "$f" "$ref")
    else N "head -c \$(( $d + 2 )) $REFD/e_1000000000.txt > $T/ref_$d.txt; printf '\n' >> $T/ref_$d.txt"; c=$(cmpref "$f" "$T/ref_$d.txt"); N "rm -f $T/ref_$d.txt"; fi
    local hi; hi=$(grep -ac 'MN_OUT_DKM_HI: the writer started\|MN_OUT_DKM_HI: the X_hi part files' "$log")
    echo "RUN $tag: rc $rc; $c; $np part files; X_hi writer lines $hi; $(grep -ac 'VERIFY OK' "$log") VERIFY OK, $(grep -ac 'VERIFY FAILED' "$log") FAILED; $(grep -a 'X_hi writer)' "$log" | head -1 | grep -o 'corrections [0-9.]* ([-0-9]*, [a-z ]*)\|step 1 ([^)]*)\|X_hi Q + corrections [0-9.]* ([-0-9]*)\|X_lo0 [<>=]* B^s' | tr '\n' ' '); $(grep -a 'patch: X' "$log" | head -1 | sed 's/^ *//' | cut -c1-110); $(grep -a '^total' "$log" | awk '{print "total", $2}')"
}
case $ST in
A1)
    R "NEWTON_DEVICE=1 DIST_LOGN_TEST=20 ./tests/t_newton 20" > "$L/t_newton.log" 2>&1; echo "t_newton 20 NEWTON_DEVICE=1 DIST_LOGN_TEST=20: rc $? $(grep -a VERIFY "$L/t_newton.log" | tail -1)"
    grep -a 'FAILED\|^   nq .*X mod B' "$L/t_newton.log" | head -40
    MN_OUT_DKM_HI=1 ./mnaccept.sh "$J" --only unit,e9,mn,recheck,corr 2>&1 | grep -a '^PASS\|^FAIL\|passed\|==' ; echo "mnaccept dir: results/mnaccept/$J"
    grep -lac 'MN_OUT_DKM_HI: the writer started\|MN_OUT_DKM_HI: the X_hi part files' results/mnaccept/"$J"/*.log | sed 's/^/  X_hi writer taken: /'
    mkdir -p results/mnaccept/"$J"_a; mv results/mnaccept/"$J"/* results/mnaccept/"$J"_a/ 2>/dev/null
    MN_OUT_DKM_HI=1 BS_MDEV_LOGL=24 ./mnaccept.sh "$J" --only e9,recheck,corr 2>&1 | grep -a '^PASS\|^FAIL\|passed\|=='
    grep -lac 'MN_OUT_DKM_HI: the writer started\|MN_OUT_DKM_HI: the X_hi part files' results/mnaccept/"$J"/*.log | sed 's/^/  X_hi writer taken: /'
    ;;
A2)
    D=BS_MDEV_LOGL=24
    one s1_p7 1 1000000000 $D MN_OUT_DKM_HI=1 ECALC_TEST_CORR=7
    one s1_m7 1 1000000000 $D MN_OUT_DKM_HI=1 ECALC_TEST_CORR=-7
    one s1_hi7 1 1000000000 $D MN_OUT_DKM_HI=1 NEWTON_DKM_TEST_HI=7
    one s1_hi9_p5 1 1000000000 $D MN_OUT_DKM_HI=1 NEWTON_DKM_TEST_HI=-9 ECALC_TEST_CORR=5
    one s1_carry_m7 1 1000000000 $D MN_OUT_DKM_HI=1 MN_OUT_DKM_HI_TEST_CARRY=1 ECALC_TEST_CORR=-7
    one s1_carry_c511 1 511461828 $D MN_OUT_DKM_HI=1 MN_OUT_DKM_HI_TEST_CARRY=1 ECALC_TEST_CORR=14
    one s1_c511 1 511461828 $D MN_OUT_DKM_HI=1 ECALC_TEST_CORR=14
    one s1_c820 1 820719000 $D MN_OUT_DKM_HI=1 ECALC_TEST_CORR=-44
    for p in 2 4; do
        one c108u_$p $p 108388422 POOL_LOG=27 MN_OUT_DKM_HI=1 ECALC_TEST_CORR=36
        one c108d_$p $p 108072396 POOL_LOG=27 MN_OUT_DKM_HI=1 ECALC_TEST_CORR=-39
        one hi9_$p $p 100000000 POOL_LOG=27 MN_OUT_DKM_HI=1 NEWTON_DKM_TEST_HI=-9
        one carry_c108u_$p $p 108388422 POOL_LOG=27 MN_OUT_DKM_HI=1 MN_OUT_DKM_HI_TEST_CARRY=1 ECALC_TEST_CORR=36
    done
    one e9_3 3 1000000000 POOL_LOG=27 MN_OUT_DKM_HI=1 NEWTON_DKM_TEST_HI=9 ECALC_TEST_CORR=-5
    E4=~/ntt/ecalc/results/e_4e10.out
    evict $E4
    R "env MN_OUT_DKM_HI=1 ECALC_VERBOSE=1 RNS_VERBOSE=1 ./ecalc 40000000000 $T/e4.txt" > "$L/e4_on.log" 2>&1; rc=$?
    echo "RUN e4_on: rc $rc; $(cmpref $T/e4.txt $E4); $(grep -a '^total\|^dm ' "$L/e4_on.log" | tr -s ' ' | tr '\n' ' ' | cut -c1-200)"
    grep -a 'X_hi writer)\|MN_OUT_DKM_HI\|^wrote' "$L/e4_on.log" | head -4 | cut -c1-400
    ;;
C)
    E11=~/ntt/ecalc/results/e_1e11.out; K=$T/keep.txt
    run11() { # tag env...
        local tag=$1; shift; local log=$L/$tag.log f=$T/e11.txt c
        N "rm -rf $f $f.*"; evict $E11
        local t0; t0=$(date +%s.%N)
        R "env $* ECALC_VERBOSE=1 RNS_VERBOSE=1 ./ecalc 100000000000 $f" > "$log" 2>&1; local rc=$?
        local wall; wall=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b - a}')
        if N "test -e $K"; then c=$(N "cmp -s $f $K && echo 'same bytes as on_1' || echo 'DIFFERS from on_1'; rm -rf $f $f.*"); evict $K
        else N "mv $f $K; rm -rf $f.*"; c="on_1: $(N "$PWD/digcmp.sh $K $E11") against the reference"; evict $K $E11; fi
        echo "RUN11 $tag: rc $rc wall ${wall} s; $(grep -a '^total' "$log" | awk '{print "total", $2}'); $(grep -a '^dm ' "$log" | awk '{print "dm", $2}'); $(grep -a '^dm ' "$log" | grep -o 'corrections [0-9/]*'); $(grep -a '^wrote' "$log" | grep -o '[0-9.]* s after the checks'); $(grep -ac 'VERIFY OK' "$log") VERIFY OK; $c"
        grep -a 'divmod(dev' "$log" | head -1 | cut -c1-420
    }
    run11 on_1 MN_OUT_DKM_HI=1; run11 off_1; run11 on_2 MN_OUT_DKM_HI=1; run11 off_2; run11 on_3 MN_OUT_DKM_HI=1; run11 off_3
    ;;
esac
N "rm -rf $T"
echo "== EW15 batch $ST done $(date)"

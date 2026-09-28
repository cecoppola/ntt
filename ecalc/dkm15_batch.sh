#!/bin/bash
# dkm15_batch.sh <stage> <expected short sha> - agent DKM (Phase 15 Batch 3, results/DKM15.md): one unattended node batch.
#   A  NEWTON_DKM=1 correctness: t_newton 20 with NEWTON_DEVICE=1 (section 6); e9 both bases; e9 forced corrections (ECALC_TEST_CORR +-7
#      under ECALC_CORR_PATCH=1 and 2, NEWTON_DKM_TEST_HI 7); 4e10 identical; mnaccept mn (sizes 2-4); forced at sizes 2-4 (mnaccept
#      mn,recheck with ECALC_CORR_PATCH=2 ECALC_TEST_CORR=7; K15's long chains c108u / c108d; NEWTON_DKM_TEST_HI)
#   B  t_newton 20 with NEWTON_DEVICE=1 DIST_LOGN_TEST=20 (sections 5, 6 on grids); the gates with the switch off: mnaccept unit,e9; 4e10 identical
#   C  10^11: off / on interleaved, 3 each, with the packed file (wall and `total`), the reference evicted first; every file compared
# One 1-node job (-J DKM, 45 min, not on s24-16 unless NODE_OK=any), cancelled at the end. Logs in ~/dkm15tmp/<stage>/.
ST=$1 SHA=$2
cd ~/ntt-DKM15/ecalc || exit 1
L=~/dkm15tmp/$ST; mkdir -p "$L"; exec > "$L/batch.log" 2>&1
[ "$(git rev-parse --short=7 HEAD)" = "${SHA:0:7}" ] || { echo "WRONG COMMIT $(git log --oneline -1)"; exit 1; }
echo "== DKM15 batch $ST $(date) $(git log --oneline -1)"
X=; [ "${NODE_OK:-}" = any ] || X="-x ppac-pl1-s24-16"
J=$(sbatch -p PPAC_MI300A_SPX -N1 $X --gpus=4 -t 0:45:00 -J DKM --parsable --wrap "sleep 2700") || exit 1
trap 'scancel $J' EXIT
echo "$J" > ~/dkm15tmp/"$ST".job
until [ "$(squeue -j "$J" -h -o %T)" = RUNNING ]; do [ -z "$(squeue -j "$J" -h -o %T)" ] && { echo "job $J gone"; exit 1; }; sleep 20; done
NODE=$(squeue -j "$J" -h -o %N); echo "job $J on $NODE $(date)"
R() { srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd ~/ntt-DKM15/ecalc; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
REFD=~/ntt/ecalc/ref T=/tmp/dkm15_$J
N "mkdir -p $T"
cmpref() { N "$PWD/digcmp.sh $1 $2; rm -rf $1 $1.*"; }
evict() { N "python3 -c \"import os,sys
for p in sys.argv[1:]:
    fd=os.open(p,os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED); os.close(fd)\" $*"; }
one() { # tag procs d_out env...  (the digits against ref/e_<d>.txt, or the first d_out digits of e_1000000000.txt for the chain sizes)
    local tag=$1 p=$2 d=$3; shift 3; local f=$T/$tag.txt log=$L/$tag.log rc c ref=$REFD/e_$d.txt
    if [ "$p" = 1 ]; then R "env $* ECALC_VERBOSE=2 RNS_VERBOSE=1 ./ecalc $d $f" > "$log" 2>&1; rc=$?
    else SLURM_JOB_ID=$J timeout 900 ./mnrun.sh "$p" env "$@" ./ecalc "$d" "$f" > "$log" 2>&1; rc=$?; fi
    if [ -s "$ref" ]; then c=$(cmpref "$f" "$ref")
    else N "head -c \$(( $d + 2 )) $REFD/e_1000000000.txt > $T/ref_$d.txt; printf '\n' >> $T/ref_$d.txt"; c=$(cmpref "$f" "$T/ref_$d.txt"); N "rm -f $T/ref_$d.txt"; fi
    echo "RUN $tag: rc $rc; $c; $(grep -ac 'VERIFY OK' "$log") VERIFY OK, $(grep -ac 'VERIFY FAILED' "$log") FAILED; $(grep -a '^dm ' "$log" | grep -o 'corrections [0-9/]*' | head -1); $(grep -a 'divmod(dev, DKM)\|divmod(mn, DKM)' "$log" | head -1 | grep -o 'k [0-9]* = [0-9]* + [0-9]* (s), h [0-9]*\|X_hi Q + corrections [0-9.]* ([-0-9]*)\|step 1 ([^)]*)\|corrections [0-9.]* ([-0-9]*)' | tr '\n' ' '); $(grep -a '^total' "$log" | awk '{print "total", $2}')"
}
case $ST in
A)
    R "NEWTON_DEVICE=1 ./tests/t_newton 20" > "$L/t_newton_dev.log" 2>&1; echo "t_newton 20 NEWTON_DEVICE=1: rc $? $(grep -a VERIFY "$L/t_newton_dev.log" | tail -1)"
    grep -a '^   nq .*shape' "$L/t_newton_dev.log" | head -40
    for b in 10 2; do one e9_on_b$b 1 1000000000 NEWTON_DKM=1 LIMB_BASE=$b; done
    one e9_on_p7_c1 1 1000000000 NEWTON_DKM=1 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=7
    one e9_on_m7_c2 1 1000000000 NEWTON_DKM=1 ECALC_TEST_CORR=-7
    one e9_on_p7_c0 1 1000000000 NEWTON_DKM=1 ECALC_CORR_PATCH=0 ECALC_TEST_CORR=7
    one e9_on_hi7 1 1000000000 NEWTON_DKM=1 NEWTON_DKM_TEST_HI=7
    one c511_on 1 511461828 NEWTON_DKM=1 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=14
    one c820_on 1 820719000 NEWTON_DKM=1 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=-44
    E4=~/ntt/ecalc/results/e_4e10.out
    R "env NEWTON_DKM=1 ECALC_VERBOSE=1 RNS_VERBOSE=1 ./ecalc 40000000000 $T/e4.txt" > "$L/e4_on.log" 2>&1; rc=$?
    echo "RUN e4_on: rc $rc; $(cmpref $T/e4.txt $E4); $(grep -a '^total\|^dm \|^recip ' "$L/e4_on.log" | tr -s ' ' | tr '\n' ' ' | cut -c1-300)"
    grep -a 'divmod(dev, DKM)' "$L/e4_on.log" | head -1
    NEWTON_DKM=1 ./mnaccept.sh "$J" --only mn 2>&1 | grep -a '^PASS\|^FAIL\|passed'
    NEWTON_DKM=1 ECALC_CORR_PATCH=2 ECALC_TEST_CORR=7 ./mnaccept.sh "$J" --only mn,recheck 2>&1 | grep -a '^PASS\|^FAIL\|passed'
    for p in 2 4; do
        one c108u_on_$p $p 108388422 NEWTON_DKM=1 POOL_LOG=27 ECALC_CORR_PATCH=2 ECALC_TEST_CORR=36
        one c108d_on_$p $p 108072396 NEWTON_DKM=1 POOL_LOG=27 ECALC_CORR_PATCH=2 ECALC_TEST_CORR=-39
        one c108d_on0_$p $p 108072396 NEWTON_DKM=1 POOL_LOG=27 ECALC_CORR_PATCH=0 ECALC_TEST_CORR=-39
        one hi_on_$p $p 100000000 NEWTON_DKM=1 POOL_LOG=27 NEWTON_DKM_TEST_HI=-9
    done
    one e8_on_3 3 100000000 NEWTON_DKM=1 POOL_LOG=27 NEWTON_DKM_TEST_HI=9 ECALC_TEST_CORR=-5
    ;;
B)
    R "NEWTON_DEVICE=1 DIST_LOGN_TEST=20 ./tests/t_newton 20" > "$L/t_newton_dev20.log" 2>&1; echo "t_newton 20 NEWTON_DEVICE=1 DIST_LOGN_TEST=20: rc $? $(grep -a VERIFY "$L/t_newton_dev20.log" | tail -1)"
    grep -a 'FAILED' "$L/t_newton_dev20.log" | head
    # size 1 takes newton_db_divmod_shifted only when the top levels ran on the device tier (> 2^30 limbs: 4e10 and up); BS_MDEV_LOGL=24 at 1e9
    for sw in 0 1; do one e9_dev_$sw 1 1000000000 BS_MDEV_LOGL=24 NEWTON_DKM=$sw; done
    one e9_dev_p7_c1 1 1000000000 BS_MDEV_LOGL=24 NEWTON_DKM=1 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=7
    one e9_dev_m7_c2 1 1000000000 BS_MDEV_LOGL=24 NEWTON_DKM=1 ECALC_TEST_CORR=-7
    one e9_dev_p7_c0 1 1000000000 BS_MDEV_LOGL=24 NEWTON_DKM=1 ECALC_CORR_PATCH=0 ECALC_TEST_CORR=7
    one e9_dev_hi7 1 1000000000 BS_MDEV_LOGL=24 NEWTON_DKM=1 NEWTON_DKM_TEST_HI=7
    one c511_dev 1 511461828 BS_MDEV_LOGL=24 NEWTON_DKM=1 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=14
    one c820_dev 1 820719000 BS_MDEV_LOGL=24 NEWTON_DKM=1 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=-44
    one c511_base 1 511461828 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=14                       # control: the switch off, the host flow (stage A's c511_on failed in the patch)
    one c511_base_ascii 1 511461828 ECALC_OUT_PACKED=0 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=14
    ./mnaccept.sh "$J" --only unit,e9 2>&1 | grep -a '^PASS\|^FAIL\|passed'
    E4=~/ntt/ecalc/results/e_4e10.out
    R "env ECALC_VERBOSE=1 ./ecalc 40000000000 $T/e4.txt" > "$L/e4_off.log" 2>&1; rc=$?
    echo "RUN e4_off: rc $rc; $(cmpref $T/e4.txt $E4); $(grep -a '^total' "$L/e4_off.log" | tr -s ' ')"
    ;;
C)
    E11=~/ntt/ecalc/results/e_1e11.out
    run11() { # tag env...
        local tag=$1; shift; local log=$L/$tag.log f=$T/e11.txt
        N "rm -rf $f $f.*"; evict $E11
        local t0; t0=$(date +%s.%N)
        R "env $* ECALC_VERBOSE=1 RNS_VERBOSE=1 ./ecalc 100000000000 $f" > "$log" 2>&1; local rc=$?
        local wall; wall=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b - a}')
        local c; c=$(cmpref "$f" "$E11")
        echo "RUN11 $tag: rc $rc wall ${wall} s; $(grep -a '^total' "$log" | awk '{print "total", $2}'); $(grep -a '^recip ' "$log" | awk '{print "recip", $2}'); $(grep -a '^dm ' "$log" | awk '{print "dm", $2}'); $(grep -a '^dm ' "$log" | grep -o 'corrections [0-9/]*'); $(grep -ac 'VERIFY OK' "$log") VERIFY OK; $c"
        grep -a 'divmod(dev' "$log" | head -1 | cut -c1-400
    }
    for i in 1 2 3; do run11 off_$i; run11 on_$i NEWTON_DKM=1; done
    ;;
esac
N "rm -rf $T"
echo "== DKM15 batch $ST done $(date)"

#!/bin/bash
# k15_batch.sh <stage> <expected short sha> - agent K (Phase 15, results/K15.md): one unattended node batch.
#   A  gates: t_patch (O_DIRECT and buffered), t_newton 20, t_mul 20; mnaccept unit,e9,mn with the switch off
#   B  forced corrections: mnaccept e9,mn,recheck with ECALC_CORR_PATCH=2 ECALC_TEST_CORR=7, e9 with k = -7 (on) and
#      k = 7 (off); the longest chains of e's first 10^9 digits (d = 0 mod 18): size 1 on / off, size 2 and 4 on / off
#   C  10^11 timing: off / on / off / on with the digit file (sha1 against e_1e11.out's); K15_NOFILE=1: the same without a file
# Waits for Stage 0 (~/reg15b.done: 3 lines, polled every 10 min), one 1-node job (-J K, 45 min), cancels it at the end.
ST=$1 SHA=$2
cd ~/ntt-K15/ecalc || exit 1
L=~/k15tmp/$ST; mkdir -p "$L"; exec > "$L/batch.log" 2>&1
[ "$(git rev-parse --short=7 HEAD)" = "${SHA:0:7}" ] || { echo "WRONG COMMIT $(git log --oneline -1)"; exit 1; }
until [ "$(wc -l < ~/reg15b.done 2>/dev/null)" -ge 3 ]; do sleep 600; done
echo "== K15 batch $ST $(date) $(git log --oneline -1)"
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J K --parsable --wrap "sleep 2700") || exit 1
trap 'scancel $J' EXIT
echo "$J" > ~/k15tmp/"$ST".job
until [ "$(squeue -j "$J" -h -o %T)" = RUNNING ]; do [ -z "$(squeue -j "$J" -h -o %T)" ] && { echo "job $J gone"; exit 1; }; sleep 20; done
NODE=$(squeue -j "$J" -h -o %N); echo "job $J on $NODE $(date)"
R() { srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd ~/ntt-K15/ecalc; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
REF=~/ntt/ecalc/ref/e_1000000000.txt T=/tmp/k15_$J
N "mkdir -p $T"
# pref <file or part prefix> <d_out>: the output against the reference's first d_out digits (read with O_DIRECT)
pref() { N "P=\$(ls $1.part* 2>/dev/null | sort); [ -n \"\$P\" ] || P=$1; cmp -s <(for p in \$P; do dd if=\$p iflag=direct bs=64M status=none; done) <(head -c \$(( $2 + 2 )) $REF; printf '\n') && echo identical || echo DIFFERS; rm -rf $1 $1.*"; }
one() { # tag procs d_out env...
    local tag=$1 p=$2 d=$3; shift 3; local f=$T/$tag.txt log=$L/$tag.log
    if [ "$p" = 1 ]; then R "env $* ECALC_VERBOSE=2 ./ecalc $d $f" > "$log" 2>&1; else SLURM_JOB_ID=$J timeout 900 ./mnrun.sh "$p" env "$@" ./ecalc "$d" "$f" > "$log" 2>&1; fi
    local rc=$? c; c=$(pref "$f" "$d")
    echo "RUN $tag: rc $rc; $c; $(grep -ac 'VERIFY OK' "$log") VERIFY OK, $(grep -ac 'VERIFY FAILED' "$log") FAILED; $(grep -a '^dm ' "$log" | grep -o 'corrections [0-9/]*' | head -1); $(grep -a 'patch: X\|patch: digits\|redoing' "$log" | head -2 | tr -s ' ' | tr '\n' ' ')"
}
case $ST in
A)
    for od in 1 0; do R "mkdir -p $T/p$od; ECALC_ODIRECT=$od T_PATCH_DIR=$T/p$od ./tests/t_patch" > "$L/t_patch_od$od.log" 2>&1; echo "t_patch ECALC_ODIRECT=$od: rc $? $(grep -a 'checks,\|VERIFY' "$L/t_patch_od$od.log" | tr '\n' ' ')"; done
    for t in "t_newton 20" "t_mul 20"; do R "./tests/$t" > "$L/${t// /_}.log" 2>&1; echo "$t: rc $? $(grep -a VERIFY "$L/${t// /_}.log" | tail -1)"; done
    ./mnaccept.sh "$J" --only unit,e9,mn 2>&1 | grep -a '^PASS\|^FAIL\|^==\|summary\|passed'
    ;;
B)
    ECALC_CORR_PATCH=2 ECALC_TEST_CORR=7 ./mnaccept.sh "$J" --only e9,mn,recheck 2>&1 | grep -a '^PASS\|^FAIL\|^==\|passed'
    one e9_on_m7 1 1000000000 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=-7
    one e9_off_p7 1 1000000000 ECALC_TEST_CORR=7
    one c511_on 1 511461828 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=14
    one c511_on_do 1 511461823 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=14
    one c511_off 1 511461828 ECALC_TEST_CORR=14
    one c820_on 1 820719000 ECALC_CORR_PATCH=1 ECALC_TEST_CORR=-44
    one c820_off 1 820719000 ECALC_TEST_CORR=-44
    for p in 2 4; do
        one c108u_on_$p $p 108388422 POOL_LOG=27 ECALC_CORR_PATCH=2 ECALC_TEST_CORR=36
        one c108d_on_$p $p 108072396 POOL_LOG=27 ECALC_CORR_PATCH=2 ECALC_TEST_CORR=-39
        one c108d_off_$p $p 108072396 POOL_LOG=27 ECALC_TEST_CORR=-39
    done
    ;;
C)
    E11=~/ntt/ecalc/results/e_1e11.out WANT=$(cut -c1-40 ~/V214/e_1e11.sha1)
    run11() { # tag file(0/1) env...
        local tag=$1 wf=$2; shift 2; local log=$L/$tag.log f=; [ "$wf" = 1 ] && f=$T/e11.txt
        N "rm -f $T/e11.txt*"
        local t0; t0=$(date +%s.%N)
        R "env $* ECALC_VERBOSE=1 RNS_VERBOSE=1 ./ecalc 100000000000 $f" > "$log" 2>&1; local rc=$?
        local wall; wall=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b - a}')
        local h=; [ "$wf" = 1 ] && h=$(N "dd if=$T/e11.txt iflag=direct bs=64M status=none | sha1sum | cut -c1-40")
        local same=; [ "$wf" = 1 ] && { [ "$h" = "$WANT" ] && same=identical || same="DIFFERS ($h)"; }
        echo "RUN11 $tag: rc $rc wall ${wall} s; $(grep -a '^total' "$log" | awk '{print "total", $2}'); $(grep -a '^T1 ' "$log" | awk '{print "T1", $2}'); $(grep -a '^dc ' "$log" | awk '{print "dc", $3}'); $(grep -a '^dm ' "$log" | grep -o 'corrections [0-9/]*'); $(grep -a 'patch: X' "$log" | tr -s ' ' | cut -c1-120); $(grep -ac 'VERIFY OK' "$log") VERIFY OK; $same"
        N "rm -f $T/e11.txt*"
    }
    if [ "${K15_NOFILE:-0}" = 1 ]; then run11 off_nf1 0; run11 on_nf1 0 ECALC_CORR_PATCH=1; run11 off_nf2 0; run11 on_nf2 0 ECALC_CORR_PATCH=1
    else run11 off_w1 1; run11 on_w1 1 ECALC_CORR_PATCH=1; run11 off_w2 1; run11 on_w2 1 ECALC_CORR_PATCH=1; fi
    ;;
esac
N "rm -rf $T"
echo "== K15 batch $ST done $(date)"

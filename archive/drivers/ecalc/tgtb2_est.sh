#!/bin/bash
# TGTBENCH2 (2026-10-06): the 576-node estimates of results/TGTBENCH2.md §3-§4 -- old (TGT17's flag set: the target fabric, aac7's
# MAP_RATE 0.070) vs new (--fabric target-m: MAP_RATE 0.010 measured on the target), and the candidate sizes of §4.
# Local python only; modelled.  usage: ./tgtb2_est.sh [old-new|sizes]
cd "$(dirname "$0")"
export DM_MN_LEAN=1 MN_OUT_DKM_HI=1 COMM_OFI=1
FLAGS="--g 576 --np-mn auto --lat 8.5e-6 --hide-pow2 0.72 --t-round 0.015"
row() {   # $1 label, then estimate.py args
    local lab="$1"; shift
    python3 estimate.py "$@" --verbose 2>&1 | grep -E "per-node wall|^576|target-m:" | sed -E \
        -e 's/.*per-node wall ([0-9.]+) s.*with the part file, ([0-9.]+) s.*: init ([0-9.]+), seed wait ([0-9.]+), batch ([0-9.]+), top levels ([0-9.]+), distributed levels ([0-9.]+), reciprocal ([0-9.]+), division ([0-9.]+),.*/W \2 \1 init \3 seed \4 levels \7 recip \8 div \9/' \
        -e 's/^576 +[^|]+\| +[0-9.]+ +[0-9.]+ +[0-9.]+ \| +([0-9.]+) .*/expo \1/' \
        -e 's/.*modelled device ([0-9.]+) GB.*/dev_mod \1/' | tr '\n' ' ' | sed "s/^/$lab | /"; echo
}
case "${1:-old-new}" in
old-new)
    for lf in 1.0 1.22; do for bw in 11 47 100; do
        MN_MODEL_MAP_RATE=0.070 row "old lf $lf bw $bw" --fabric target $FLAGS --D 70833333333.33 --bw $bw --local-factor $lf
        row "new lf $lf bw $bw" --fabric target-m $FLAGS --D 70833333333.33 --bw $bw --local-factor $lf
    done; done ;;
sizes)
    for T in 37100000000000 37600000000000 37690000000000 37700000000000 38200000000000 38900000000000 39900000000000 40800000000000; do
        D=$(python3 -c "print(repr($T/576))")
        for bw in 11 47 100; do row "T $T bw $bw" --fabric target-m $FLAGS --D $D --bw $bw; done
    done ;;
esac

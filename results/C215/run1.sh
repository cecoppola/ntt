#!/bin/bash -l
# C215: one ecalc run on the node (inside srun): run1.sh <digits> <outfile|-> [VAR=val ...]; prints "C2WALL <s> rc <rc>" (the
# process wall: ecalc's start to its exit, i.e. with the digit file's write and the top set when there is an outfile)
module load rocm 2>/dev/null; cd $HOME/ntt-C215/ecalc || exit 9
DG=$1; OUT=$2; shift 2; [ "$OUT" = - ] && OUT=
hostname; git -C $HOME/ntt-C215 log --oneline -1
t0=$(date +%s.%N); env "$@" ECALC_VERBOSE=2 RNS_VERBOSE=1 ./ecalc $DG $OUT; rc=$?; t1=$(date +%s.%N)
echo "C2WALL $(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.2f", b - a}') rc $rc"

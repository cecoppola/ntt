#!/bin/bash
# t10_d5.sh <jobid> [n] - D5 diagnosis (results/T.md): 10^10 at size 4, POOL_LOG 29, pool 1 forced to 3 GiB so the batch-local
# tier grows it at its first level (A-mem's failing configuration).  Series a: n runs that exit right after the tree level 1
# set (on disk: the leaf level 21 set and tree level 1 per node); series b: n runs that exit after tree level 2 (tree levels
# 1 and 2).  Every set is compared across the runs of its series: a differing set names the first wrong level and node.
J=$1; n=${2:-3}; [ -n "$J" ] || { echo "usage: $0 <jobid> [runs]"; exit 1; }
cd "$(dirname "$0")" || exit 1
E=$PWD; OUT=results/t10/$J; mkdir -p $OUT; SUM=$OUT/summary_d5.txt; TMP=/tmp/t10d5_$J
note() { echo "$*" | tee -a $SUM; }
N() { srun --jobid=$J -N1 --overlap bash -c "$*"; }
note "== D5 diagnosis, job $J, $(date -Is), $(git rev-parse --short HEAD) =="
N "mkdir -p $TMP"
for s in a b; do
  [ $s = a ] && AB=1 || AB=2
  for i in $(seq 1 $n); do
    tag=$s$i; CK=$TMP/ck_$tag
    SLURM_JOB_ID=$J timeout 900 ./mnrun.sh 4 env ECALC_VERBOSE=2 POOL_LOG=29 RNS_POOL1_GB=3.2213 RNS_VERBOSE=1 \
        BS_CKPT_DIR=$CK BS_CKPT_EVERY=1 BS_CKPT_MIN_LEVEL=21 BS_CKPT_TREE_EVERY=1 BS_CKPT_ABORT_TREE=$AB ./ecalc 10000000000 > $OUT/d5$tag.log 2>&1
    sleep 2
    note "$tag: $(grep -ac 'BS_CKPT_ABORT_TREE' $OUT/d5$tag.log) nodes exited after tree level $AB; bs $(grep -a '^bs  *[0-9]' $OUT/d5$tag.log | head -1 | awk '{print $2}') s; sets: $(N "ls $CK | tr '\n' ' ' | cut -c1-160")"
  done
  for f in $(N "ls $TMP/ck_${s}1 | grep -v hdr"); do
    line="$f:"
    for i in $(seq 2 $n); do line="$line run$i $(N "cmp -s $TMP/ck_${s}1/$f $TMP/ck_$s$i/$f && echo same || echo DIFF")"; done
    note "  $line"
  done
done
note "done $(date -Is)"

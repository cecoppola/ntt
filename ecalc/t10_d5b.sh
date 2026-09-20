#!/bin/bash
# t10_d5b.sh <jobid> [n] - D5, second series (results/T.md): n full runs of 10^10 at size 4, POOL_LOG 29 with pool 1 forced
# to 3 GiB (the batch tier grows it inside the phase); the digits kept and, for a failing run, the first differing byte
# against results/e_1e10.out and the T1 detail printed -- is the number wrong (a local difference) or the whole tail?
J=$1; n=${2:-8}; [ -n "$J" ] || { echo "usage: $0 <jobid> [runs]"; exit 1; }
cd "$(dirname "$0")" || exit 1
E=$PWD; OUT=results/t10/$J; mkdir -p $OUT; SUM=$OUT/summary_d5b.txt; TMP=/tmp/t10d5b_$J
R10=$HOME/ntt/ecalc/results/e_1e10.out
note() { echo "$*" | tee -a $SUM; }
N() { srun --jobid=$J -N1 --overlap bash -c "$*"; }
note "== D5 series b, job $J, $(date -Is), $(git rev-parse --short HEAD) =="
N "mkdir -p $TMP"
for i in $(seq 1 $n); do
  tag=r$i
  SLURM_JOB_ID=$J timeout 900 ./mnrun.sh 4 env ECALC_VERBOSE=2 POOL_LOG=29 RNS_POOL1_GB=3.2213 RNS_VERBOSE=1 ./ecalc 10000000000 $TMP/$tag.txt > $OUT/d5$tag.log 2>&1
  sleep 2
  N "cd $TMP; cat $tag.txt.part* > $tag.all; rm -f $tag.txt.part*"
  c=$(N "cmp $TMP/$tag.all $R10 2>&1 | head -1"); [ -z "$c" ] && c=identical
  note "$tag: $(grep -ac 'VERIFY OK' $OUT/d5$tag.log) VERIFY OK; $(grep -a '^total' $OUT/d5$tag.log | head -1 | sed 's/  */ /g' | cut -c1-30); vs ref: $c"
  if [ "$c" != identical ]; then
    note "  T1: $(grep -a 'T1 q' $OUT/d5$tag.log | head -8 | sed 's/.*q\([0-9]\)=[0-9]*: /q\1 /' | tr '\n' ';')"
    note "  parts: $(grep -a 'wrote' $OUT/d5$tag.log | cut -c1-80 | tr '\n' ';')"
    note "  sizes: $(N "ls -l $TMP/$tag.all $R10 | awk '{print \$5}' | tr '\n' ' '")"
    note "  differing bytes in the first 1e6 / last 1e6: $(N "cmp -l $TMP/$tag.all $R10 2>/dev/null | awk '\$1 <= 1000000 {a++} \$1 > 9999000000 {b++} END {print a+0, b+0}'"); total differing: $(N "cmp -l $TMP/$tag.all $R10 2>/dev/null | wc -l")"
    N "cp $TMP/$tag.all $HOME/t10_d5_$tag.bad"
  fi
  N "rm -f $TMP/$tag.all"
done
note "done $(date -Is)"

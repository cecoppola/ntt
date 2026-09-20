#!/bin/bash
# v11_d5.sh <jobid> [runs] [shard|host|alt] [digits] [size] - Phase 11 V, D5 (results/V.md): runs of 10^10 at size 4 with the
# plane-pool growth forced (POOL_LOG 29, pool 1 at 3 GiB: the batch tier grows it inside the phase -- T's recipe) and
# ECALC_RES_LOG=1 (every residue printed and cross-checked: RES lines).  shard = the sharded division (default flow),
# host = MN_DM=host (A-mem's flow: gather to node 0, the division there, the stand-in scatter), alt = alternate.
# The digits are cmp'd against results/e_1e10.out; a failing run's log and digits are kept (~/v11_d5_<tag>.bad).
J=$1; n=${2:-8}; mode=${3:-shard}; D=${4:-10000000000}; SZ=${5:-4}; [ -n "$J" ] || { echo "usage: $0 <jobid> [runs] [shard|host|alt] [digits] [size]"; exit 1; }
cd "$(dirname "$0")" || exit 1
OUT=results/v11/$J; mkdir -p $OUT; SUM=$OUT/summary_d5.txt; TMP=/tmp/v11d5_$J
case $D in 10000000000) R=$HOME/ntt/ecalc/results/e_1e10.out;; *) R=$HOME/ntt/ecalc/ref/e_$D.txt;; esac
note() { echo "$*" | tee -a $SUM; }
N() { srun --jobid=$J -N1 --overlap bash -c "$*"; }
note "== D5 (V), job $J, $(date -Is), $(git rev-parse --short HEAD), mode $mode, $D digits at size $SZ =="
N "mkdir -p $TMP"
for i in $(seq 1 $n); do
  m=$mode; [ "$mode" = alt ] && { [ $((i % 2)) -eq 1 ] && m=shard || m=host; }
  tag=${TAG:-}${m}$i; env="$EXTRA"; [ "$m" = host ] && env="$EXTRA MN_DM=host"   # EXTRA: more env (e.g. MEM_DPOOL_FILL=1); TAG: a run-name prefix
  t0=$(date +%s)
  N "mkdir -p $TMP/leaf_$tag"
  SLURM_JOB_ID=$J timeout 900 ./mnrun.sh $SZ env ECALC_RES_LOG=1 ECALC_LEAF_DUMP=$TMP/leaf_$tag ECALC_VERBOSE=2 POOL_LOG=29 RNS_POOL1_GB=3.2213 RNS_VERBOSE=1 $env ./ecalc $D $TMP/$tag.txt > $OUT/$tag.log 2>&1
  t1=$(date +%s); sleep 2
  N "cd $TMP; cat $tag.txt.part* > $tag.all; rm -f $tag.txt.part*"
  c=$(N "cmp $TMP/$tag.all $R 2>&1 | head -1"); [ -z "$c" ] && c=identical
  ok=$(grep -ac 'VERIFY OK' $OUT/$tag.log); mm=$(grep -ac 'MISMATCH' $OUT/$tag.log)
  note "$tag: $ok VERIFY OK, $mm MISMATCH lines; $(grep -a '^total' $OUT/$tag.log | head -1 | sed 's/  */ /g' | cut -c1-40); wall $((t1 - t0)) s; vs ref: $c"
  if [ "$c" != identical ] || [ "$ok" != $((SZ + 1)) ] || [ "$mm" != 0 ]; then
    note "  T1: $(grep -a 'T1 q' $OUT/$tag.log | grep -a BAD | head -8 | sed 's/.*q\([0-9]\)=[0-9]*: /q\1 /' | tr '\n' ';')"
    note "  RES: $(grep -a 'MISMATCH\|!=' $OUT/$tag.log | head -6 | cut -c1-200 | tr '\n' ';')"
    if [ "$c" != identical ]; then
      note "  sizes: $(N "ls -l $TMP/$tag.all $R | awk '{print \$5}' | tr '\n' ' '")"
      note "  differing bytes in the first 1e6 / last 1e6: $(N "cmp -l -n 1000000 $TMP/$tag.all $R 2>/dev/null | wc -l") / $(N "cmp -l -i $((D - 1000000)) $TMP/$tag.all $R 2>/dev/null | wc -l")"
      N "cp $TMP/$tag.all $HOME/v11_d5_$tag.bad"
    fi
  fi
  if [ "$c" = identical ] && [ "$ok" = $((SZ + 1)) ]; then N "test -d $TMP/leaf_ref || mv $TMP/leaf_$tag $TMP/leaf_ref; rm -rf $TMP/leaf_$tag"   # the first good run's leaves are the reference
  else for f in $(N "ls $TMP/leaf_$tag"); do note "  leaf $f vs ref: $(N "cmp -l $TMP/leaf_$tag/$f $TMP/leaf_ref/$f 2>/dev/null | awk 'NR == 1 {a = \$1} {b = \$1} END {printf \"%d differing bytes, first at limb %d, last at limb %d\", NR, a / 8, b / 8}'")"; done; N "rm -rf $TMP/leaf_$tag"; fi
  N "rm -f $TMP/$tag.all"
done
note "done $(date -Is)"

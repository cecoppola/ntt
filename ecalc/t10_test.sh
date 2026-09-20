#!/bin/bash
# t10_test.sh <jobid> [which...] - agent T's Phase 10 runs (results/T.md): D2 the restart at 10^10 from tree-level sets at
# size 4, D4 the binary path at 10^10, D5 the plane-pool growth inside the batch tier, C6 the deferred tree barrier and
# BS_CKPT_TREE_EVERY at 10^8.  Run on the login node from the clone's ecalc/ against a one-node allocation; digits and
# checkpoints on the node's /tmp, logs in results/t10/<jobid>/, one digest line per run in summary.txt.
#   which: d2 d4 d5 c6 (default all)
J=$1; shift; [ -n "$J" ] || { echo "usage: $0 <jobid> [which...]"; exit 1; }
cd "$(dirname "$0")" || exit 1
E=$PWD; OUT=results/t10/$J; mkdir -p $OUT; SUM=$OUT/summary.txt; TMP=/tmp/t10_$J
REF=$HOME/ntt/ecalc/ref; R10=$HOME/ntt/ecalc/results/e_1e10.out
echo "== T phase 10 runs, job $J, $(date -Is), $(git rev-parse --short HEAD) ==" | tee -a $SUM
note() { echo "$*" | tee -a $SUM; }
N() { srun --jobid=$J -N1 --overlap bash -c "$*"; }
run1() { local tag=$1 d=$2; shift 2   # single process
  local t0=$(date +%s); srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $E; env ECALC_VERBOSE=2 $* ./ecalc $d $TMP/$tag.txt" > $OUT/$tag.log 2>&1; echo $(( $(date +%s) - t0 )) > $OUT/$tag.wall; }
runm() { local tag=$1 p=$2 d=$3 to=$4; shift 4   # p node-processes on the node, killed after $to s
  local t0=$(date +%s); SLURM_JOB_ID=$J timeout $to ./mnrun.sh $p env ECALC_VERBOSE=2 "$@" ./ecalc $d $TMP/$tag.txt > $OUT/$tag.log 2>&1; echo $(( $(date +%s) - t0 )) > $OUT/$tag.wall; sleep 2; }
# join the part files (or the single file) into <tag>.all on the node, cmp against a reference file
join() { N "cd $TMP; cat $1.part* > $1.all 2>/dev/null || cp $1.txt $1.all; rm -f $1.part* $1.txt"; }
cmpf() { N "cmp -s $TMP/$1.all $2 && echo identical || echo DIFFERS"; }
chk() { local tag=$1 ref=$2; join $tag
  note "$tag: $(grep -ac 'VERIFY OK' $OUT/$tag.log) VERIFY OK; vs ref: $(cmpf $tag $ref); $(grep -a '^total' $OUT/$tag.log | head -1 | sed 's/  */ /g' | cut -c1-40); wall $(cat $OUT/$tag.wall) s; $(grep -aE 'restart|tree checkpoint sets' $OUT/$tag.log | head -2 | sed 's/  */ /g' | cut -c1-90 | tr '\n' ';')"; }
same() { note "  $1 vs $2: $(N "cmp -s $TMP/$1.all $TMP/$2.all && echo identical || echo DIFFERS")"; }
sets() { N "ls -l $1 2>/dev/null | awk '{print \$5, \$9}' | tr '\n' ' ' | cut -c1-400"; }
N "mkdir -p $TMP"
for w in ${*:-d2 d4 d5 c6}; do case $w in
d2)   # 10^10 at size 4, POOL_LOG 29: tree sets at levels 1 and 2 (default: every level); abort after tree level 1, restart
  CK=$TMP/ck10; N "rm -rf $CK"; ENV="POOL_LOG=29 BS_CKPT_DIR=$CK"
  runm d2a 4 10000000000 900 $ENV;                       chk d2a $R10; note "  sets after a: $(sets $CK)"
  runm d2c 4 10000000000 900 $ENV BS_CKPT_ABORT_TREE=1;  note "d2c: $(grep -ac 'BS_CKPT_ABORT_TREE' $OUT/d2c.log) nodes exited after tree level 1; wall $(cat $OUT/d2c.wall) s; sets: $(sets $CK)"
  runm d2d 4 10000000000 900 $ENV BS_RESTART=1;          chk d2d $R10; same d2a d2d; note "  sets after d: $(sets $CK)"
  runm d2b 4 10000000000 900 $ENV BS_RESTART=1;          chk d2b $R10; same d2a d2b   # from the top level's set: straight to the division
  N "rm -rf $CK";;
d4)   # the binary path at 10^10, size 1
  run1 d4 10000000000 LIMB_BASE=2;                       chk d4 $R10;;
d5)   # pool 1 forced to 3 q (A-mem's failing configuration) so the batch tier grows it inside the phase
  runm d5a 4 10000000000 900 POOL_LOG=29 RNS_POOL1_GB=3.2213 RNS_VERBOSE=1;  chk d5a $R10; note "  growth: $(grep -a 'grow\|pool' $OUT/d5a.log | grep -av 'block pool\|pool_log\|POOL' | head -3 | cut -c1-100 | tr '\n' ';')"
  runm d5b 4 10000000000 900 POOL_LOG=29 RNS_POOL1_GB=3.2213 RNS_VERBOSE=1;  chk d5b $R10
  runm d5c 4 1000000000 600 POOL_LOG=27 RNS_POOL1_GB=0.8054 RNS_VERBOSE=1;   chk d5c $REF/e_1000000000.txt
  run1 d5d 1000000000 POOL_LOG=29 RNS_POOL1_GB=3.2213 RNS_VERBOSE=1;         chk d5d $REF/e_1000000000.txt;;
c6)   # the deferred barrier and BS_CKPT_TREE_EVERY at 10^8, sizes 4 and 3 (POOL_LOG 27)
  for p in 4 3; do CK=$TMP/ck8_$p; N "rm -rf $CK"; ENV="POOL_LOG=27 BS_CKPT_EVERY=2 BS_CKPT_MIN_LEVEL=2 BS_CKPT_DIR=$CK"; T=c6.$p
    runm ${T}a $p 100000000 600 $ENV;                              chk ${T}a $REF/e_100000000.txt; note "  sets after a: $(sets $CK)"
    runm ${T}b $p 100000000 600 $ENV BS_RESTART=1;                 chk ${T}b $REF/e_100000000.txt; same ${T}a ${T}b
    runm ${T}e $p 100000000 600 $ENV BS_CKPT_ABORT_TREE=1;         note "${T}e: $(grep -ac 'BS_CKPT_ABORT_TREE' $OUT/${T}e.log) nodes exited; sets: $(sets $CK)"
    runm ${T}f $p 100000000 600 $ENV BS_RESTART=1;                 chk ${T}f $REF/e_100000000.txt; same ${T}a ${T}f; note "  sets after f: $(sets $CK)"
    runm ${T}g $p 100000000 60 $ENV BS_CKPT_ABORT_TREE=1 BS_CKPT_ABORT_NODE=1; note "${T}g: node 1 exited after tree level 1, the rest killed; sets: $(sets $CK)"
    runm ${T}h $p 100000000 600 $ENV BS_RESTART=1;                 chk ${T}h $REF/e_100000000.txt; same ${T}a ${T}h
    runm ${T}t $p 100000000 6 $ENV;                                note "${T}t: killed after 6 s at: $(grep -aE 'checkpoint|bs: level' $OUT/${T}t.log | tail -1 | cut -c1-90); sets: $(sets $CK)"
    runm ${T}u $p 100000000 600 $ENV BS_RESTART=1;                 chk ${T}u $REF/e_100000000.txt; same ${T}a ${T}u
    # BS_CKPT_TREE_EVERY=2: at size 4 only the top level (2) is written; abort there and restart
    runm ${T}v $p 100000000 600 $ENV BS_CKPT_TREE_EVERY=2 BS_CKPT_ABORT_TREE=2; note "${T}v: $(grep -ac 'checkpoint tree level' $OUT/${T}v.log) tree sets written ($(grep -a 'checkpoint tree level' $OUT/${T}v.log | awk '{print $7}' | sort -u | tr '\n' ' ')), $(grep -ac 'BS_CKPT_ABORT_TREE' $OUT/${T}v.log) nodes exited; sets: $(sets $CK)"
    runm ${T}w $p 100000000 600 $ENV BS_CKPT_TREE_EVERY=2 BS_RESTART=1; chk ${T}w $REF/e_100000000.txt; same ${T}a ${T}w
    N "rm -rf $CK"
  done;;
esac; done
N "rm -rf $TMP"
note "done $(date -Is)"

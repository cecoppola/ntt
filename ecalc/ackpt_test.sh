#!/bin/bash
# ackpt_test.sh <jobid> [which...] - the M6 checkpoint/restart gate (results/A-ckpt.md), run on the login node
# against a one-node allocation.  Checkpoints go to the node's local /tmp; digits to ~/ntt-A-ckpt/out and are
# cmp'd against ref/e_<digits>.txt.  which: s1 s1v1 s1dev m8 m9 mdev (default all).
J=$1; shift; [ -n "$J" ] || { echo "usage: $0 <jobid> [which...]"; exit 1; }
cd ~/ntt-A-ckpt/ecalc || exit 1
OUT=~/ntt-A-ckpt/out; mkdir -p $OUT; CK=/tmp/ackpt_$J
SUM=$OUT/summary_$J.txt; echo "== A-ckpt gate, job $J, $(date -Is), $(git rev-parse --short HEAD) ==" | tee -a $SUM
CKE="BS_CKPT_EVERY=2 BS_CKPT_MIN_LEVEL=2"
run1() { local tag=$1 d=$2; shift 2   # single process
  srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm; cd ~/ntt-A-ckpt/ecalc; env ECALC_VERBOSE=2 $* ./ecalc $d $OUT/$tag.txt" > $OUT/$tag.log 2>&1; }
runmain() { local tag=$1 d=$2; shift 2   # main's ecalc (~/ntt): a v1 set
  srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm; cd ~/ntt/ecalc; env ECALC_VERBOSE=2 $* ./ecalc $d $OUT/$tag.txt" > $OUT/$tag.log 2>&1; }
runm() { local tag=$1 p=$2 d=$3 to=$4; shift 4   # p node-processes on the node, killed after $to s
  rm -f $OUT/$tag.txt
  SLURM_JOB_ID=$J timeout $to ./mnrun.sh $p env ECALC_VERBOSE=2 $* ./ecalc $d $OUT/$tag.txt > $OUT/$tag.log 2>&1; sleep 2; }
chk() { local tag=$1 ref=$2 r
  if [ -s $OUT/$tag.txt ] && cmp -s $OUT/$tag.txt ~/ntt/ecalc/ref/e_$ref.txt; then r="identical to ref"; else r="DIFFERS / missing"; fi
  echo "$tag: $(grep -c 'VERIFY OK' $OUT/$tag.log) VERIFY OK; $r; $(grep -E '^bs +[0-9]' $OUT/$tag.log | sed 's/  */ /g' | cut -c1-60); $(grep -E 'restart|bs checkpoints' $OUT/$tag.log | head -3 | tr '\n' ';')" | tee -a $SUM; }
sets() { srun --jobid=$J -N1 --overlap bash -c "ls -l $1 2>/dev/null | awk '{print \$5, \$9}' | tr '\n' ' '" 2>/dev/null; }
note() { echo "$*" | tee -a $SUM; }
mdev_level() { grep -E '^bs: level +[0-9]+ mdev' $OUT/$1.log | head -1 | awk '{print $3}'; }
WHICH=${*:-"s1 s1v1 s1dev m8 m9 mdev"}
for w in $WHICH; do case $w in
s1)   # single node 10^9, WP7's four runs
  D=1000000000; E="POOL_LOG=29 $CKE BS_CKPT_DIR=${CK}_s1"
  run1 s1a $D $E;                       chk s1a $D; note "  sets after s1a: $(sets ${CK}_s1)"
  run1 s1b $D $E BS_RESTART=1;          chk s1b $D
  run1 s1c $D $E BS_CKPT_ABORT=8;       note "s1c: $(grep -E 'ABORT|checkpoint level' $OUT/s1c.log | tail -2 | tr '\n' ';')"; note "  sets after s1c: $(sets ${CK}_s1)"
  run1 s1d $D $E BS_RESTART=1;          chk s1d $D
  cmp -s $OUT/s1a.txt $OUT/s1b.txt && cmp -s $OUT/s1a.txt $OUT/s1d.txt && note "s1: a = b = d" || note "s1: a, b, d DIFFER";;
s1v1) # a set written by main's ecalc (the v1 header and names), restarted by this branch
  D=1000000000; E="POOL_LOG=29 $CKE BS_CKPT_DIR=${CK}_v1"
  runmain v1c $D $E BS_CKPT_ABORT=8;    note "v1c (main): $(grep -E 'ABORT' $OUT/v1c.log | tail -1)"; note "  sets: $(sets ${CK}_v1)"
  run1 v1d $D $E BS_RESTART=1;          chk v1d $D;;
s1dev) # single node 10^9 with device top levels in the leaf (BS_MDEV_LOGL=25): a set at a device-number level
  D=1000000000; E="POOL_LOG=29 BS_MDEV_LOGL=23 BS_CKPT_EVERY=1 BS_CKPT_MIN_LEVEL=2 BS_CKPT_DIR=${CK}_s1dev"   # (23: levels 18 and 19 are mdev; 25 makes only the top one mdev, which is never snapshotted)
  run1 s1deva $D $E;                    chk s1deva $D; L=$(mdev_level s1deva); note "  first mdev level $L; sets: $(sets ${CK}_s1dev)"
  run1 s1devc $D $E BS_CKPT_ABORT=$L;   note "s1devc: $(grep -E 'ABORT|checkpoint level' $OUT/s1devc.log | tail -2 | tr '\n' ';')"; note "  sets: $(sets ${CK}_s1dev)"
  run1 s1devd $D $E BS_RESTART=1;       chk s1devd $D;;
m8|m9) # sizes 2 and 4 on one node
  if [ $w = m8 ]; then D=100000000; PL=27; else D=1000000000; PL=29; fi
  for p in 2 4; do
    T=$w.$p; E="POOL_LOG=$PL $CKE BS_CKPT_DIR=${CK}_$T"
    runm ${T}a $p $D 900 $E;                              chk ${T}a $D; note "  sets after a: $(sets ${CK}_$T)"
    runm ${T}b $p $D 900 $E BS_RESTART=1;                 chk ${T}b $D   # from the top tree level's sets: straight to the gather
    runm ${T}c $p $D 900 $E BS_CKPT_ABORT=6;              note "${T}c: $(grep -c 'BS_CKPT_ABORT' $OUT/${T}c.log) nodes exited; sets: $(sets ${CK}_$T)"
    runm ${T}d $p $D 900 $E BS_RESTART=1;                 chk ${T}d $D   # from the leaf sets
    runm ${T}e $p $D 900 $E BS_CKPT_ABORT_TREE=1;         note "${T}e: $(grep -c 'BS_CKPT_ABORT_TREE' $OUT/${T}e.log) nodes exited; sets: $(sets ${CK}_$T)"
    runm ${T}f $p $D 900 $E BS_RESTART=1;                 chk ${T}f $D   # from tree level 1 (leaf sets skipped)
    runm ${T}g $p $D 60 $E BS_CKPT_ABORT=6 BS_CKPT_ABORT_NODE=1; note "${T}g: node 1 exited at leaf level 6, the rest killed by timeout; sets: $(sets ${CK}_$T)"
    runm ${T}h $p $D 900 $E BS_RESTART=1;                 chk ${T}h $D   # mixed: the agreed level is 0 -> every node from its leaf set
    if [ $w = m9 ]; then KT=9; else KT=6; fi
    runm ${T}t $p $D $KT $E;                              note "${T}t: killed after $KT s at: $(grep -E 'checkpoint|bs: level' $OUT/${T}t.log | tail -1); sets: $(sets ${CK}_$T)"
    runm ${T}u $p $D 900 $E BS_RESTART=1;                 chk ${T}u $D
    for x in b d f h u; do cmp -s $OUT/${T}a.txt $OUT/${T}$x.txt || note "$T: a and $x DIFFER"; done
  done;;
mdev) # 10^9 at sizes 2 and 4 with the leaf's device top levels (BS_MDEV_LOGL=22: the leaf's last two or three levels): sets at a device-number level of the leaf
  D=1000000000
  for p in 2 4; do
    T=mdev.$p; E="POOL_LOG=29 BS_MDEV_LOGL=22 BS_CKPT_EVERY=1 BS_CKPT_MIN_LEVEL=2 BS_CKPT_DIR=${CK}_$T"
    runm ${T}a $p $D 900 $E;                              chk ${T}a $D; L=$(mdev_level ${T}a); note "  first mdev level $L; sets: $(sets ${CK}_$T)"
    runm ${T}c $p $D 900 $E BS_CKPT_ABORT=$L;             note "${T}c: $(grep -c 'BS_CKPT_ABORT' $OUT/${T}c.log) nodes exited; sets: $(sets ${CK}_$T)"
    runm ${T}d $p $D 900 $E BS_RESTART=1;                 chk ${T}d $D
  done;;
esac; done
note "done $(date -Is)"

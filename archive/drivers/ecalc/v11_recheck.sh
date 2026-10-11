#!/bin/bash
# v11_recheck.sh <jobid> - Phase 11 V: the recheck mode (ECALC_RECHECK=1) against the in-run checks (results/V.md):
# 10^8 at sizes 1, 2 and 10^9 at sizes 1, 2 -- a run with BS_CKPT_DIR (size 1: ECALC_CKPT_TOP=1 for the top-level set), then
# the recheck of its digit file(s) and sidecar; the digits cmp'd against ref/; every step PASS/FAIL.
J=$1; [ -n "$J" ] || { echo "usage: $0 <jobid>"; exit 1; }
cd "$(dirname "$0")" || exit 1
OUT=results/v11/$J; mkdir -p $OUT; SUM=$OUT/summary_recheck.txt; TMP=/tmp/v11rc_$J
note() { echo "$*" | tee -a $SUM; }
N() { srun --jobid=$J -N1 --overlap bash -c "$*"; }
note "== recheck (V), job $J, $(date -Is), $(git rev-parse --short HEAD) =="
N "mkdir -p $TMP"
for cfg in "100000000 1 27" "100000000 2 27" "1000000000 1 29" "1000000000 2 29"; do
  set -- $cfg; D=$1; SZ=$2; PL=$3; tag=rc_${D}_$SZ; CK=$TMP/ck_$tag
  N "rm -rf $CK; mkdir -p $CK"
  if [ $SZ = 1 ]; then
    srun --jobid=$J -N1 --overlap bash -lc "module load rocm; cd $PWD; POOL_LOG=$PL BS_CKPT_DIR=$CK ECALC_CKPT_TOP=1 ./ecalc $D $TMP/$tag.txt" > $OUT/$tag.log 2>&1
    N "cp $TMP/$tag.txt $TMP/$tag.all"
  else
    SLURM_JOB_ID=$J ./mnrun.sh $SZ env POOL_LOG=$PL BS_CKPT_DIR=$CK ./ecalc $D $TMP/$tag.txt > $OUT/$tag.log 2>&1
    N "cd $TMP; cat $tag.txt.part* > $tag.all"
  fi
  c=$(N "cmp $TMP/$tag.all $HOME/ntt/ecalc/ref/e_$D.txt 2>&1 | head -1"); [ -z "$c" ] && c=identical
  note "$tag run: $(grep -ac 'VERIFY OK' $OUT/$tag.log) VERIFY OK, vs ref: $c; sidecar: $(N "cat $TMP/$tag.txt.t1 | tr '\n' ' ' | cut -c1-120")"
  # the recheck (same size, same checkpoint dir, same file names)
  if [ $SZ = 1 ]; then
    srun --jobid=$J -N1 --overlap bash -lc "module load rocm; cd $PWD; ECALC_RECHECK=1 ECALC_RES_LOG=1 BS_CKPT_DIR=$CK ./ecalc $D $TMP/$tag.txt" > $OUT/${tag}_recheck.log 2>&1
  else
    SLURM_JOB_ID=$J ./mnrun.sh $SZ env ECALC_RECHECK=1 ECALC_RES_LOG=1 BS_CKPT_DIR=$CK ./ecalc $D $TMP/$tag.txt > $OUT/${tag}_recheck.log 2>&1
  fi
  note "$tag recheck: $(grep -ac 'RECHECK OK' $OUT/${tag}_recheck.log) RECHECK OK, $(grep -ac 'RECHECK FAILED' $OUT/${tag}_recheck.log) FAILED; $(grep -a 'recheck: .*digits read' $OUT/${tag}_recheck.log | head -1 | cut -c1-260)"
  # a corrupted digit must be caught: flip one digit in the middle of the (last) part file and recheck again
  f=$(N "ls $TMP/$tag.txt.part* 2>/dev/null | tail -1"); [ -z "$f" ] && f=$TMP/$tag.txt
  N "cp $f $f.orig; python3 -c \"import sys; p=sys.argv[1]; f=open(p,'r+b'); f.seek(1000); c=f.read(1); f.seek(1000); f.write(b'0' if c != b'0' else b'1'); f.close()\" $f"
  if [ $SZ = 1 ]; then
    srun --jobid=$J -N1 --overlap bash -lc "module load rocm; cd $PWD; ECALC_RECHECK=1 BS_CKPT_DIR=$CK ./ecalc $D $TMP/$tag.txt" > $OUT/${tag}_recheck_bad.log 2>&1
  else
    SLURM_JOB_ID=$J ./mnrun.sh $SZ env ECALC_RECHECK=1 BS_CKPT_DIR=$CK ./ecalc $D $TMP/$tag.txt > $OUT/${tag}_recheck_bad.log 2>&1
  fi
  note "$tag recheck of a corrupted file: $(grep -ac 'RECHECK FAILED' $OUT/${tag}_recheck_bad.log) RECHECK FAILED (expected >= 1); $(grep -a 'digits == X BAD\|digits -> X mod q DIFFER' $OUT/${tag}_recheck_bad.log | head -1 | cut -c1-100)"
  N "mv $f.orig $f; rm -rf $CK $TMP/$tag.all"
done
note "done $(date -Is)"

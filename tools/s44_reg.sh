#!/bin/bash
# s44_reg.sh <jobid> <tag>: S44 regression on a CPX node (unit tests as mnaccept's list, 1e9 gates per switch with digits vs the 1e9 reference), then the ABBAs (s44_node.sh).
J=$1; T=$2; O=$HOME/s44out; W=$HOME/ntt-s44; REF=$HOME/ntt/ecalc/ref/e_1000000000.txt; S=$O/${T}_reg.txt; : > $S
R() { local to=$1; shift; timeout $to srun --jobid=$J --overlap bash -lc "module load rocm; cd $W/ecalc; $*"; }
for t in "t_ntt 24" "t_mul 20" "t_bs" "t_dbig 0" "t_newton 20" "t_verify" "t_out" "t_patch"; do
  n=${t// /_}; s=$(date +%s); R 900 "./tests/$t" > $O/${T}_unit_$n.log 2>&1 < /dev/null; rc=$?
  v=$(grep -a VERIFY $O/${T}_unit_$n.log | tail -1)
  if [ $rc = 0 ] && ! grep -aq 'VERIFY FAILED' $O/${T}_unit_$n.log; then r=PASS; else r=FAIL; fi
  echo "unit $t: $r rc=$rc $(( $(date +%s)-s ))s [$v]" >> $S
done
i=0
for cfg in "X=1" "DBIG_MAXIDX_TOP=1" "DBIG_QSEL=1" "DBIG_MAXIDX_TOP=1 DBIG_QSEL=1" "DBIG_ADDSUB2=0" "DBIG_MAXIDX_TOP=1 DBIG_QSEL=1 LIMB_BASE=2" "LIMB_BASE=2"; do
  i=$((i+1)); f=$HOME/s44tmp/g$i.txt
  R 900 "env POOL_LOG=29 $cfg ./ecalc 1000000000 $f" > $O/${T}_gate$i.log 2>&1 < /dev/null; rc=$?
  c=$($W/ecalc/digcmp.sh $f $REF); rm -rf $f $f.*
  echo "e9 [$cfg POOL_LOG=29]: rc=$rc $(grep -a -m1 '^VERIFY' $O/${T}_gate$i.log) digits $c $(grep -a -m1 '^total' $O/${T}_gate$i.log | cut -c1-14)" >> $S
done
echo REGDONE >> $S
$W/tools/s44_node.sh $J $T

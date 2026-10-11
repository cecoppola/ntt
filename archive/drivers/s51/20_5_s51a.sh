#!/bin/bash
# OWNER=T5
# EST_MIN=25
# S51 (T5b) queue item a: micro-probe of K2 (DBIG_ADDSUB3) and K3 (DBIG_MODQ_MONT), t_dbig with all switches, 1-node gates (1e9) per kernel and combined, then the
# ARM_B file for the ABBA items.  No rebuild (build: ~/ntt-wt/s51/ecalc).  STUB=1: logic only, no Slurm.
H=${SLURM_JOB_ID:-0}; WT=${WT:-$HOME/ntt-wt/s51}; O=${O:-$HOME/batch1011/T5/s51}; mkdir -p $O/probe $O/gates; cd $WT/ecalc || exit 2
R() { if [ "${STUB:-0}" = 1 ]; then echo "STUB srun: $*"; else srun --jobid=$H -N1 --gpus=4 -c 192 --overlap bash -lc "module unload rocm/7.0.3 >/dev/null 2>&1; module load $MNRUN_MODULES; cd $WT/ecalc; $*" < /dev/null; fi; }
[ "${STUB:-0}" = 1 ] || source aac7env.sh >/dev/null 2>&1
for k in 0 1; do for rep in 1 2; do
  R "./tests/t_s51_probe 28 7 $k" > $O/probe/probe_base_${rep}${k}.log 2>&1
  R "DBIG_ADDSUB3=1 ./tests/t_s51_probe 28 7 $k" > $O/probe/probe_k2_${rep}${k}.log 2>&1
  R "DBIG_MODQ_MONT=1 ./tests/t_s51_probe 28 7 $k" > $O/probe/probe_k3_${rep}${k}.log 2>&1
done; [ $k = 0 ] || true; done
R "DBIG_ADDSUB3=1 DBIG_MODQ_MONT=1 ./tests/t_dbig 24" > $O/t_dbig_on.log 2>&1; echo "t_dbig rc $?" >> $O/t_dbig_on.log
for g in "k2:DBIG_ADDSUB3=1" "k3:DBIG_MODQ_MONT=1 ECALC_RES_LOG=1" "k4:RNS_CRT_SUB=5" "all:DBIG_ADDSUB3=1 DBIG_MODQ_MONT=1 RNS_CRT_SUB=5"; do
  n=${g%%:*}; e=${g#*:}
  if [ "${STUB:-0}" = 1 ]; then echo "STUB gate $n $e"; echo "gate PASS | STUB" > $O/gates/gate_$n.txt; continue; fi
  bash $WT/tools/gate.sh --nodes 1 --hold $H --out $O/gates/g_$n --env "DBIG_S51_STATS=1 $e" < /dev/null > $O/gates/gate_$n.log 2>&1
  cp $O/gates/g_$n/gate.txt $O/gates/gate_$n.txt 2>/dev/null || echo "gate FAIL | no summary" > $O/gates/gate_$n.txt
done
grep -q "rc 0" $O/t_dbig_on.log && grep -q "gate PASS" $O/gates/gate_all.txt 2>/dev/null || echo "S51: t_dbig or combined gate not clean" >> $O/decide.txt
python3 $WT/archive/drivers/s51/s51_decide.py $O/probe $O/gates > $O/ARM_B 2>> $O/decide.txt
cat $O/gates/gate_*.txt >> $O/gates.txt; cat $O/ARM_B $O/decide.txt
squeue -s -j $H -h | grep -c . > $O/steps_left.txt
exit 0

#!/bin/bash
# OWNER=o5
# NODES=2
# EST_MIN=20
# APPROVED=coordinator-2026-10-11
# o5 (K7-a, results/O5.md): the B5 form (RNS_STRATEGY_FORM=B5, off by default) on aac7: SHMEM build of ~/ntt-wt/o5 (login-node
#  build by gate.sh --build), gate 2 nodes 1e9 under B5, gate 1 node 1e9 under B5, tests/t_strategy 28 1 V (C/B/B4/B5/auto compared),
#  one 1e10 run with RNS_STRATEGY_CHECK=1 under B5, then a paired ABBA at 1e10 B vs B5 (1 node) in the remaining time (budget 16 min
#  from the item's start for its last round).  Outputs only ~/sess1011c/o5/G/.  Exits by itself.  STUB=1: dry run (no srun).
WT=$HOME/ntt-wt/o5; E=$WT/ecalc; O=$HOME/sess1011c/o5/G; mkdir -p $O; : > $O/summary.txt
say() { echo "$(date '+%m-%d %H:%M:%S') $*" | tee -a $O/summary.txt; }
for f in $E/rns_dist.c $WT/tools/gate.sh $WT/tools/o5_abba.sh $E/mnrun.sh; do [ -e $f ] || { echo "missing $f"; exit 2; }; done
grep -q STRAT_B5 $E/rns_dist.c || { echo "worktree without B5"; exit 2; }
if [ "${STUB:-0}" = 1 ]; then echo "STUB ok: would gate.sh --build 2 nodes + 1 node, t_strategy, CHECK 1e10, ABBA 1e10 on \$SLURM_JOB_ID; outputs $O"; bash -n $0 && echo syntax ok; exit 0; fi
: ${SLURM_JOB_ID:?need the runner allocation}
J=$SLURM_JOB_ID; T0=$(date +%s); export O5_E=$E
cd $E; source aac7env.sh >/dev/null 2>&1
. $WT/tools/o5_abba.sh
bash $WT/tools/gate.sh --build --nodes 2 --hold $J --out $O/gate2_b5 --timeout 600 --env "RNS_STRATEGY_FORM=B5" < /dev/null > $O/gate2_b5.out 2>&1
say "gate 2 nodes B5 1e9: $(cat $O/gate2_b5/gate.txt 2>/dev/null | cut -c1-240)"
[ -x $E/ecalc ] && [ -x $E/tests/t_strategy ] || { make -s -j16 SHMEM_CRAY=1 GMP_HOME=$HOME/gmp tests/t_strategy > $O/build_t.log 2>&1; }
bash $WT/tools/gate.sh --nodes 1 --hold $J --out $O/gate1_b5 --timeout 600 --env "RNS_STRATEGY_FORM=B5" < /dev/null > $O/gate1_b5.out 2>&1
say "gate 1 node B5 1e9: $(cat $O/gate1_b5/gate.txt 2>/dev/null | cut -c1-240)"
srun --jobid=$J -N1 -n1 --overlap --mem=0 --gpus-per-node=4 timeout 600 ./tests/t_strategy 28 1 V < /dev/null > $O/tstrat.log 2>&1; rc=$?
say "t_strategy 28 1 V: rc $rc, $(grep -a -c 'VERIFY OK' $O/tstrat.log) x VERIFY OK, $(grep -a -c -i 'FAIL\|differs' $O/tstrat.log) x FAIL/differs; $(grep -a 'STRAT logn' $O/tstrat.log | sed -E 's/ reps.*//' | tr '\n' ';' | cut -c1-400)"
export O5_J=$J
res=$(o5_run_one $O/check_b5.log 1e10 RNS_STRATEGY_FORM=B5 RNS_STRATEGY_CHECK=1 RNS_VERBOSE=1)
say "CHECK 1e10 B5 (rc total bform verify): $res; $(grep -a -c 'RNS_STRATEGY_CHECK:' $O/check_b5.log) check lines, $(grep -a -m1 'differs from C' $O/check_b5.log | cut -c1-160)"
budget=$(( 16 * 60 - ($(date +%s) - T0) ))
if [ $budget -gt 240 ]; then
    o5_abba $O/abba "$J" 1e10 4 $budget "RNS_STRATEGY_FORM=B" "RNS_STRATEGY_FORM=B5" > $O/abba_driver.log 2>&1
    say "ABBA 1e10 B vs B5: $(grep -a -E '^(total|bform)\(' $O/abba/abba.txt | tail -2 | tr '\n' ' ')"
else say "ABBA skipped (budget ${budget}s)"; fi
echo "DONE $(date)" >> $O/summary.txt

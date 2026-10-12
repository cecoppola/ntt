#!/bin/bash
# tools/o5_aac6.sh - O5 (K7-a, results/O5.md): the aac6 fire-and-forget driver for the B5 form, single node, <= 45 min job.
#   1. waits until O5_START (Eastern, default 2026-10-12 03:40: after the CI window and after the H4 driver took its node);
#   2. builds the clone (~/ntt-o5, branch o5) on the login node (module load rocm, make -s -j16);
#   3. submits one 45-min hold on PPAC_MI300A_SPX excluding the node of any running h4* job of ours; waits <= 25 min for it;
#   4. gate.sh --nodes 1 with RNS_STRATEGY_FORM=B5 (1e9, VERIFY OK + digits identical to the reference);
#   5. tests/t_strategy 28 1 V (C, B, B4, B5, auto compared limb for limb);
#   6. one 1e10 run with RNS_STRATEGY_CHECK=1 under B5 (every B-form product re-formed by C and compared);
#   7. paired ABBA at 1e10, B (default form) vs B5, stop rule, within the job's remaining time;
#   8. scancels only its own hold; summary.txt + DONE marker.  STUB=1: dry run (no sbatch/srun/make).
# Start: setsid nohup bash ~/ntt-o5/tools/o5_aac6.sh > ~/o5/driver.log 2>&1 < /dev/null &      STOP file: ~/o5/STOP
set -u
O=$HOME/o5; mkdir -p $O; R=$HOME/ntt-o5; E=$R/ecalc; export O5_E=$E
say() { echo "$(date '+%m-%d %H:%M:%S') $*" | tee -a $O/summary.txt; }
fin() { echo "$1" > $O/DONE; say "DONE: $1"; exit 0; }
J=
trap '[ -n "$J" ] && [ "${STUB:-0}" != 1 ] && scancel $J 2>/dev/null; echo "trap exit" >> $O/summary.txt' EXIT
START=$(TZ=America/New_York date -d "${O5_START:-2026-10-12 03:40}" +%s)
: > $O/summary.txt; say "driver pid $$, start at $(TZ=America/New_York date -d @$START) Eastern ($(date -d @$START) local)"
while [ $(date +%s) -lt $START ]; do [ -e $O/STOP ] && fin "STOP before start"; sleep 60; done
. $R/tools/o5_abba.sh
# 2. build
if [ "${STUB:-0}" != 1 ]; then
    bash -lc "module load rocm; cd $E && make -s -j16" > $O/build.log 2>&1 || fin "FAILED: build (see build.log)"
    [ -x $E/ecalc ] && [ -x $E/tests/t_strategy ] && [ -x $R/tools/unpack_digits ] || fin "FAILED: binaries missing after build"
    say "build ok: $(git -C $R log --oneline -1)"
    # 3. the hold
    X=$(squeue -u $USER -h -o "%j %N %T" | awk '$1 ~ /^h4/ && $3 == "RUNNING" {print $2}' | head -1)
    xo=; [ -n "$X" ] && xo="-x $X"
    J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 $xo -t 0:45:00 -J o5 --parsable --wrap "sleep 2700" < /dev/null 2>>$O/sbatch.err); J=${J%%;*}
    [ -n "$J" ] || fin "FAILED: sbatch ($(tail -1 $O/sbatch.err))"
    say "hold $J submitted (excluding '${X:-nothing}')"
    w=1500; while [ $w -gt 0 ]; do st=$(squeue -j $J -h -o %T 2>/dev/null); [ "$st" = RUNNING ] && break; [ -z "$st" ] && fin "FAILED: hold $J vanished"; sleep 20; w=$((w - 20)); done
    [ "$st" = RUNNING ] || { scancel $J; J=; fin "FAILED: hold not running after 25 min"; }
    say "hold $J RUNNING on $(squeue -j $J -h -o %N)"
else
    J=stub; say "STUB: no build, no hold"
fi
T0=$(date +%s); left() { echo $(( 2700 - ($(date +%s) - T0) )); }
# 4. gate 1e9 under B5
if [ "${STUB:-0}" != 1 ]; then
    bash $R/tools/gate.sh --nodes 1 --hold $J --line none --out $O/gate_b5 --timeout 600 --env "RNS_STRATEGY_FORM=B5" < /dev/null > $O/gate_b5.out 2>&1
    say "gate B5 1e9: $(cat $O/gate_b5/gate.txt 2>/dev/null | cut -c1-200)"
else say "STUB gate"; fi
# 5. t_strategy V
if [ "${STUB:-0}" != 1 ]; then
    ( cd $E && srun --jobid=$J -N1 -n1 --overlap --mem=0 --gpus=4 timeout 600 ./tests/t_strategy 28 1 V ) < /dev/null > $O/tstrat.log 2>&1; rc=$?
    say "t_strategy 28 1 V: rc $rc, $(grep -a -c 'VERIFY OK' $O/tstrat.log) x VERIFY OK, $(grep -a -c -i 'FAIL\|differs' $O/tstrat.log) x FAIL/differs; $(grep -a 'STRAT logn' $O/tstrat.log | sed -E 's/ reps.*//' | tr '\n' ';' | cut -c1-400)"
else say "STUB t_strategy"; fi
# 6. CHECK run 1e10
[ -e $O/STOP ] && fin "STOP"
res=$(o5_run_one $O/check_b5.log 1e10 RNS_STRATEGY_FORM=B5 RNS_STRATEGY_CHECK=1 RNS_VERBOSE=1)
say "CHECK 1e10 B5 (rc total bform verify): $res; $(grep -a -c 'RNS_STRATEGY_CHECK:' $O/check_b5.log) check lines, $(grep -a -m1 'differs from C' $O/check_b5.log | cut -c1-160)"
# 7. ABBA 1e10 B vs B5
[ -e $O/STOP ] && fin "STOP"
budget=$(( $(left) - 240 )); [ $budget -gt 300 ] || fin "no time for the ABBA (left $(left) s)"
o5_abba $O/abba "$J" 1e10 6 $budget "RNS_STRATEGY_FORM=B" "RNS_STRATEGY_FORM=B5" > $O/abba_driver.log 2>&1
say "ABBA done; $(grep -a -E '^(total|bform)\(' $O/abba/abba.txt | tail -2 | tr '\n' ' ')"
# 8. release
if [ "${STUB:-0}" != 1 ]; then scancel $J; sleep 5; say "hold $J cancelled; squeue: $(squeue -j $J -h -o '%T' 2>/dev/null || echo gone)"; J=; fi
fin "SUCCESS"

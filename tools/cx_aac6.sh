#!/bin/bash
# tools/cx_aac6.sh - CX (results/CX.md): fire-and-forget aac6 correctness sweep of branches o2 o5 k6 k1b k8b o3 g6 on the CPX nodes.
# Needs ~/cx/cx.bundle (a git bundle with a head per branch). Builds each branch in ~/cx/<branch>, then up to 3 slots, each with one
# 45-min hold on a CPX node (partitions SH5_MI300A_CPX,PPAC_MI300A_CPX), run the branches from a queue: per branch, arm off then on
# (e9 via mnaccept, ecalc 4e9 with the digit files cmp'd off vs on, unit via mnaccept; o5 extras; g6 sched), summary ~/cx/summary.txt
# (branch | arm | test | PASS/FAIL | s | note), marker ~/cx/DONE.  Cancels only its own holds.  STOP file: ~/cx/STOP.
# Start: setsid nohup bash ~/cx/driver.sh > ~/cx/driver.log 2>&1 < /dev/null &
set -u
CX=$HOME/cx; SUMM=$CX/summary.txt; BRANCHES=${CX_BRANCHES:-"o5 k6 k1b o2 k8b o3 g6"}; NSLOT=${CX_SLOTS:-3}
CPX="RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30"
mkdir -p $CX; rm -f $CX/DONE; : > $SUMM; printf '%s\n' $BRANCHES > $CX/queue; : > $CX/holds
row() { ( flock 8; echo "$1 | $2 | $3 | $4 | $5 | ${6:-}" >> $SUMM ) 8>$CX/sum.lock; }
claim() { ( flock 8; b=$(head -1 $CX/queue); [ -n "$b" ] && sed -i 1d $CX/queue; echo "$b" ) 8>$CX/q.lock; }
unclaim() { ( flock 8; sed -i "1i $1" $CX/queue ) 8>$CX/q.lock; }
cleanup() { for j in $(cat $CX/holds 2>/dev/null); do scancel $j 2>/dev/null; done; touch $CX/DONE; }
trap cleanup EXIT
# ---- clones and builds (3 at a time) ----
build1() { b=$1; rm -rf $CX/$b; git clone -q -b $b $CX/cx.bundle $CX/$b > $CX/$b.build.log 2>&1 &&
  bash -lc "module load rocm; cd $CX/$b/ecalc && make -s -j12" >> $CX/$b.build.log 2>&1
  if [ -x $CX/$b/ecalc/ecalc ] && [ -x $CX/$b/ecalc/tests/t_mul ] && [ -x $CX/$b/tools/unpack_digits ]; then
    row $b build build PASS - "$(git -C $CX/$b log --oneline -1 | cut -c1-70)"; else row $b build build FAIL - "see build log"; touch $CX/$b.nobuild; fi; }
rm -f $CX/*.nobuild
for b in $BRANCHES; do build1 $b & while [ $(jobs -rp | wc -l) -ge 3 ]; do sleep 5; done; done; wait
# ---- one branch on a hold ----
run_branch() { local b=$1 J=$2 E=$CX/$b/ecalc D=$CX/$b/out T=/tmp/cx_${2}_$1 SW="" PRE="" arms
  mkdir -p $D
  case $b in o2) SW=DBIG_SMALL_BOUNCE=1;; o5) SW=RNS_STRATEGY_FORM=B5;; k6) PRE=RNS_STRATEGY=C; SW=RNS_DIST_OVERLAP=1;;
    k1b) PRE=RNS_STRATEGY=C; SW=DIST_GATHER_RAW=1;; k8b) SW=BS_SEED_PULL=1;; esac
  if [ -n "$SW" ]; then arms="off on"; else arms=std; fi
  S() { local to=$1; shift; timeout $to srun --jobid=$J -N1 -n1 --mem=0 --gpus=4 --overlap bash -lc "module load rocm; cd $E; $*"; }
  N() { srun --jobid=$J -N1 -n1 --mem=0 --overlap bash -c "$*"; }
  envof() { if [ $1 = on ]; then echo "$CPX $PRE $SW"; else echo "$CPX $PRE"; fi; }
  # mnaccept_step <arm> <steps> <timeout> [extra env]: one row per PASS/FAIL line of mnaccept's summary plus the total
  mna() { local arm=$1 st=$2 to=$3 ev="$(envof $1) ${4:-}" t0=$(date +%s) rc
    ( cd $E; env $ev timeout $to ./mnaccept.sh $J --only $st --skip-grid ) > $D/${arm}_$st.log 2>&1; rc=$?
    cp $E/results/mnaccept/$J/summary.txt $D/${arm}_${st}_summary.txt 2>/dev/null
    grep -a -E '^(PASS|FAIL) ' $D/${arm}_${st}_summary.txt 2>/dev/null | while IFS= read -r l; do
      row $b $arm "mnaccept: $(echo "${l#* }" | sed 's/:.*//')" "${l%% *}" - "$(echo "${l#*: }" | cut -c1-70)"; done
    row $b $arm "mnaccept --only $st (rc=$rc)" "$([ $rc = 0 ] && echo PASS || echo FAIL)" $(( $(date +%s) - t0 )) "failures=$rc"; }
  # ecalc_run <arm> <digits> <file> <label> [extra env]: PASS = rc 0 and VERIFY OK
  er() { local arm=$1 dg=$2 f=$3 lab=$4 ev="$(envof $1) ${5:-}" t0=$(date +%s) rc lg=$D/${1}_${4// /_}.log
    S 1000 "env $ev ./ecalc $dg $f" > $lg 2>&1; rc=$?
    local ok=$(grep -a -c 'VERIFY OK' $lg) bad=$(grep -a -c -E 'VERIFY FAILED|differs from C' $lg)
    row $b $arm "$lab" "$([ $rc = 0 ] && [ $ok -ge 1 ] && [ $bad = 0 ] && echo PASS || echo FAIL)" $(( $(date +%s) - t0 )) "rc=$rc ok=$ok bad=$bad $(grep -a '^total' $lg | tail -1 | sed 's/  */ /g' | cut -c1-30)"; }
  N "rm -rf $T; mkdir -p $T"
  for arm in $arms; do   # the key tests first: 1e9 (both bases, cmp to the reference), then 4e9 (digits kept for the off/on cmp)
    mna $arm e9 1500
    f=$T/${arm/std/off}.txt; er $arm 4000000000 $f "ecalc 4e9"
  done
  if [ -n "$SW" ]; then
    t0=$(date +%s); r=$(N "cd $T; n=0; bad=0; for f in off.txt*; do g=on.txt\${f#off.txt}; n=\$((n+1)); cmp -s \$f \$g || bad=\$((bad+1)); done; echo \$n \$bad")
    row $b on/off "ecalc 4e9 digits cmp (files, differing)" "$([ "${r% *}" -ge 1 ] 2>/dev/null && [ "${r#* }" = 0 ] && echo PASS || echo FAIL)" $(( $(date +%s) - t0 )) "$r"
  fi
  N "rm -rf $T"
  if [ $b = o5 ]; then
    for ln in 24 26; do t0=$(date +%s); S 900 "env LIMB_BASE=10 ECALC_NP=3 $CPX ./tests/t_strategy $ln 1 V" > $D/tstrat_$ln.log 2>&1; rc=$?
      row $b on "t_strategy $ln 1 V (LIMB_BASE=10 ECALC_NP=3)" "$([ $rc = 0 ] && [ $(grep -a -c 'VERIFY OK' $D/tstrat_$ln.log) -ge 1 ] && ! grep -aq 'VERIFY FAILED\|differs' $D/tstrat_$ln.log && echo PASS || echo FAIL)" $(( $(date +%s) - t0 )) "rc=$rc $(grep -a -c 'VERIFY OK' $D/tstrat_$ln.log) VERIFY OK"; done
    for dg in 1000000000 4000000000; do
      er on $dg /tmp/cx_${J}_chk.txt "ecalc_$dg STRATEGY_CHECK=1" "RNS_STRATEGY_CHECK=1 RNS_VERBOSE=1"
      row $b on "check lines at $dg" INFO - "$(grep -a -c 'RNS_STRATEGY_CHECK:' $D/on_ecalc_${dg}_STRATEGY_CHECK=1.log 2>/dev/null) lines"; N "rm -rf /tmp/cx_${J}_chk.txt*"
    done
  fi
  for arm in $arms; do mna $arm unit 1800; done
  if [ $b = g6 ]; then mna std sched 1200 "MNACCEPT_SCHED_PROCS=4"; fi
  N "rm -rf $T /tmp/mnaccept_$J"
}
# ---- slots ----
slot() { local n=$1 J= t0=0 b
  while :; do [ -e $CX/STOP ] && break
    b=$(claim); [ -n "$b" ] || break
    if [ -e $CX/$b.nobuild ]; then row $b - - SKIP - "build failed"; continue; fi
    if [ -n "$J" ] && [ $(( 2700 - ($(date +%s) - t0) )) -lt 1900 ]; then scancel $J; sleep 3; J=; fi
    if [ -z "$J" ]; then
      J=$(sbatch -p SH5_MI300A_CPX,PPAC_MI300A_CPX -N1 --gpus=4 -t 0:45:00 -J cx$n --parsable --wrap "sleep 2700" < /dev/null 2>$CX/sbatch$n.err); J=${J%%;*}
      [ -n "$J" ] || { row $b - hold FAIL - "sbatch: $(tail -1 $CX/sbatch$n.err | cut -c1-80)"; unclaim $b; return; }
      echo $J >> $CX/holds; t0=$(date +%s)
      for i in $(seq 240); do [ "$(squeue -h -j $J -o %T)" = RUNNING ] && break; sleep 5; done
      [ "$(squeue -h -j $J -o %T)" = RUNNING ] || { scancel $J; row $b - hold FAIL - "not running in 20 min"; unclaim $b; return; }
      t0=$(date +%s); echo "$(date +%H:%M:%S) slot $n hold $J partition $(squeue -h -j $J -o %P)" >> $CX/driver.txt
    fi
    echo "$(date +%H:%M:%S) slot $n branch $b start" >> $CX/driver.txt
    run_branch $b $J
    echo "$(date +%H:%M:%S) slot $n branch $b end" >> $CX/driver.txt
  done
  [ -n "$J" ] && { scancel $J; sleep 3; echo "slot $n: squeue steps of hold: $(squeue -s -j $J -h 2>/dev/null | wc -l)" >> $CX/driver.txt; }
}
for n in $(seq $NSLOT); do slot $n & sleep 2; done; wait
echo "all branches done $(date +%H:%M:%S)" >> $CX/driver.txt

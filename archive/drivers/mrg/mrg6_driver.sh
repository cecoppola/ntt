#!/bin/bash
# mrg6_driver.sh (task MRG, 2026-10-12) - aac6 fire-and-forget gate of branch mrg with the NEW defaults (DBIG_SMALL_BOUNCE=1,
#   DIST_GATHER_RAW=2, BS_SEED_PULL=1 built in; nothing set on the command line):
#   SPX: clone ~/ntt-mrg from the full bundle ~/mrg6/mrg.bundle; one <= 45-min PPAC_MI300A_SPX hold: tools/gate.sh --build --nodes 1
#        --line none (1e9 identical to the reference), then mnaccept.sh --only unit,e9 (timeout = the hold's remainder); if mnaccept
#        ran out of time (rc 124), a second hold for mnaccept alone.
#   CPX (information only, parallel, one SH5/PPAC CPX hold): ecalc 1e9 and 4e9 with the defaults (CX saw a BS_SEED_PULL=1 pull-kernel
#        timeout on CPX, probably before the hang fix b1ff573): VERIFY OK, rc, the seed-pull line.
# Start: setsid nohup bash ~/mrg6/mrg6_driver.sh > ~/mrg6/driver.log 2>&1 < /dev/null &
# Output ~/mrg6/: summary.txt, log/, gate/; marker ~/mrg6/DONE ("SUCCESS|FAILED pass=N fail=M"; CPX lines are info, not counted).
set -u
O=$HOME/mrg6; C=$HOME/ntt-mrg; E=$C/ecalc; S=$O/summary.txt; PEND_MAX=${PEND_MAX:-10800}
J=; JC=; NPASS=0; NFAIL=0; T0=
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $S; }
pass() { NPASS=$((NPASS + 1)); say "PASS $1: $2"; }
fail() { NFAIL=$((NFAIL + 1)); say "FAIL $1: $2"; }
fin() { [ -n "$J" ] && { scancel $J; say "hold $J cancelled (own)"; }; [ -n "$JC" ] && { scancel $JC; say "CPX hold $JC cancelled (own)"; }
        echo "$([ $NFAIL = 0 ] && [ $NPASS -ge 2 ] && echo SUCCESS || echo FAILED) pass=$NPASS fail=$NFAIL $(date +%s)" > $O/DONE; say "done: $NPASS PASS, $NFAIL FAIL"; }
trap fin EXIT
mkdir -p $O/log; rm -f $O/DONE; : > $S
module load rocm >/dev/null 2>&1
rm -rf $C; git clone -q -b mrg $O/mrg.bundle $C || { fail clone "git clone"; exit 1; }
say "clone $(git -C $C log --oneline -1 | cut -c1-70)"
grep -q 'atoi(getenv("BS_SEED_PULL")) : 1' $E/dbig.c && grep -q 'atoi(getenv("DBIG_SMALL_BOUNCE")) : 1' $E/dbig.c && grep -q 'v = e ? atoi(e) : 2; }' $E/rns_dist.c \
  && say "tree has the three new defaults" || { fail defaults "tree without the new defaults"; exit 1; }
hold() { # partition name -> echo job id (RUNNING) or empty
  local p=$1 n=$2 j w=0 st; j=$(sbatch -p $p -N1 --gpus=4 -t 0:45:00 -J $n --parsable -o /dev/null --wrap "sleep 2700" < /dev/null 2>>$O/log/sbatch.err); j=${j%%;*}
  [ -n "$j" ] || return 1; echo $j > $O/log/hold_$n
  while :; do st=$(squeue -j $j -h -o %T 2>/dev/null); [ "$st" = RUNNING ] && break; [ -z "$st" ] && return 1
    [ $w -ge $PEND_MAX ] && { scancel $j; return 1; }; sleep 20; w=$((w + 20)); done; echo $j; }
# ---- CPX info job in the background (own hold, cancelled at its end)
( JC=$(hold SH5_MI300A_CPX,PPAC_MI300A_CPX mrgcpx) || { say "INFO cpx: no hold"; exit 0; }
  say "INFO cpx: hold $JC on $(squeue -j $JC -h -o %P)"
  for d in 1000000000 4000000000; do log=$O/log/cpx_$d.log; t1=$(date +%s)
    timeout 900 srun --jobid=$JC -N1 -n1 --mem=0 --gpus=4 --overlap bash -lc "module load rocm; cd $E; env RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30 ECALC_INIT_TL=1 ECALC_SEGV_TRACE=1 ./ecalc $d /tmp/mrgcpx_$d.txt; rm -f /tmp/mrgcpx_$d.txt*" > $log 2>&1 < /dev/null; rc=$?
    say "INFO cpx ecalc $d: rc $rc, $(grep -a -m1 'VERIFY' $log | cut -c1-60), total $(grep -a -m1 '^total' $log | awk '{print $2}'), $(( $(date +%s) - t1 )) s; pull: $(grep -a -m1 'seed pull kernels' $log | cut -c1-80); $(grep -a -m1 'FATAL\|db_pull_wait' $log | cut -c1-120)"; done
  scancel $JC; say "INFO cpx: hold $JC cancelled (own)"; rm -f $O/log/hold_mrgcpx ) &
CPXPID=$!
# ---- SPX: gate + mnaccept unit,e9
J=$(hold PPAC_MI300A_SPX mrg6a) || { J=; fail hold "SPX hold not running"; wait $CPXPID; exit 1; }
T0=$(date +%s); say "SPX hold $J running on node 0"
bash -lc "module load rocm >/dev/null 2>&1; cd $C && tools/gate.sh --build --nodes 1 --hold $J --line none --env 'ECALC_INIT_TL=1' --out $O/gate --timeout 900" > $O/log/gate.out 2>&1 < /dev/null; rc=$?
g=$(cat $O/gate/gate.txt 2>/dev/null | cut -c1-250)
if [ $rc = 0 ] && grep -q "gate PASS" $O/gate/gate.txt 2>/dev/null; then pass "gate 1 node (1e9, new defaults)" "$g"; else fail "gate 1 node" "rc $rc: $g"; fi
say "gate run evidence: $(grep -a -m1 'seed pull kernels' $O/gate/run.log 2>/dev/null | cut -c1-90)"
mna() { local to=$1 tag=$2 log=$O/log/mnaccept_$2.log t1=$(date +%s)
  ( cd $E && ECALC_REF=$HOME/ntt/ecalc/ref timeout $to ./mnaccept.sh $J --only unit,e9 ) > $log 2>&1 < /dev/null; MRC=$?
  cp $E/results/mnaccept/$J/summary.txt $O/mnaccept_${tag}_summary.txt 2>/dev/null
  local np=$(grep -ac '^PASS' $log) nf=$(grep -ac '^FAIL' $log)
  say "mnaccept $tag: rc $MRC, $np PASS, $nf FAIL, $(( $(date +%s) - t1 )) s"
  if [ $MRC = 0 ] && [ $nf = 0 ] && [ $np -ge 2 ]; then pass "mnaccept unit,e9" "$np PASS; $(( $(date +%s) - t1 )) s"; return 0; fi
  [ $MRC = 124 ] && return 2; fail "mnaccept unit,e9" "rc $MRC: $(grep -a '^FAIL' $log | head -3 | cut -c1-160 | tr '\n' '|')"; return 1; }
REM=$(( 2580 - ($(date +%s) - T0) )); mna $REM a; r=$?
scancel $J; say "hold $J cancelled (own)"; J=
if [ $r = 2 ]; then say "mnaccept ran out of the first hold; second hold"
  J=$(hold PPAC_MI300A_SPX mrg6b) || { J=; fail hold "second SPX hold not running"; wait $CPXPID; exit 1; }
  T0=$(date +%s); mna 2580 b || true; [ $MRC = 124 ] && fail "mnaccept unit,e9" "timeout in the second hold too"
  scancel $J; say "hold $J cancelled (own)"; J=; fi
wait $CPXPID

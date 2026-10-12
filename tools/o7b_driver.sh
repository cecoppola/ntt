#!/bin/bash
# o7b_driver.sh (O7b) - aac6 fire-and-forget: per-kernel GPU profile of the default code, 1 node (4 APUs), PPAC_MI300A_SPX.
#   arm:  cd ~ && setsid nohup bash ~/ntt-o7b/tools/o7b_driver.sh > ~/o7b/driver.log 2>&1 < /dev/null &
#   env:  BEGIN_ET ("2026-10-12 04:15" Eastern; the hold may not start earlier), DEADLINE_ET ("2026-10-12 09:00": give up waiting),
#         SIZES ("10000000000 64410000000"), TL_MIN (40, the hold's time limit; steps are skipped if too little is left)
# Steps per size: profiled run (rocprofv3 --kernel-trace, csv) then an unprofiled run; no digit writes. Output ~/o7b/summary.txt,
# marker ~/o7b/O7B_DONE (SUCCESS / FAILED: reason). Cancels only its own hold; kills nothing else.
WT=${WT:-$HOME/ntt-o7b}; O=$HOME/o7b; mkdir -p $O/log; E=$WT/ecalc; S=$O/summary.txt
BEGIN_ET=${BEGIN_ET:-"2026-10-12 04:15"}; DEADLINE_ET=${DEADLINE_ET:-"2026-10-12 09:00"}; TL_MIN=${TL_MIN:-40}
SIZES=${SIZES:-"10000000000 64410000000"}
VERDICT="FAILED: driver ended early"; J=
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $S; }
fin() { if [ -n "$J" ]; then scancel $J 2>/dev/null; left=$(squeue -s -j $J -h 2>/dev/null | wc -l); say "hold $J cancelled (own job); steps left $left"; fi
        echo "$VERDICT" > $O/O7B_DONE; say "$VERDICT"; }
trap fin EXIT
rm -f $O/O7B_DONE; : > $S
[ -x $E/ecalc ] || { VERDICT="FAILED: $E/ecalc not built"; exit 2; }
say "commit $(git -C $WT log --oneline -1)"
BEG=$(TZ=America/New_York date -d "$BEGIN_ET" +%s); DL=$(TZ=America/New_York date -d "$DEADLINE_ET" +%s)
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:$TL_MIN:00 -J o7b --begin=$(date -d @$BEG +%Y-%m-%dT%H:%M:%S) --parsable -o /dev/null --wrap "sleep $((TL_MIN*60))" < /dev/null) || { VERDICT="FAILED: sbatch"; exit 2; }
J=${J%%;*}; say "hold $J submitted, begin >= $BEGIN_ET"
while [ "$(squeue -j $J -h -o %T)" != RUNNING ]; do
  [ -z "$(squeue -j $J -h)" ] && { VERDICT="FAILED: hold $J vanished"; J=; exit 1; }
  [ $(date +%s) -gt $DL ] && { VERDICT="FAILED: hold did not start before $DEADLINE_ET"; exit 1; }
  sleep 30
done
T0=$(date +%s); say "hold $J RUNNING on $(squeue -j $J -h -o %N)"
run() { # tag need_s cmd...   (srun inside the hold, no -w; own timeout)
  local tag=$1 need=$2; shift 2
  local left=$(( TL_MIN*60 - ($(date +%s) - T0) ))
  [ $left -lt $need ] && { say "SKIP $tag (left ${left}s < ${need}s)"; return 1; }
  timeout 1200 srun --jobid=$J -N1 -n1 --mem=0 --gpus=4 -c 192 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; $*" > $O/log/$tag.log 2>&1 < /dev/null
  local rc=$?; local tot=$(grep -a -m1 '^total' $O/log/$tag.log | awk '{print $2}')
  say "$tag rc $rc total ${tot:-NA}s $(grep -a -c 'VERIFY OK' $O/log/$tag.log) VERIFY-OK-lines $(grep -a -c 'VERIFY FAILED' $O/log/$tag.log) FAILED-lines"
  return $rc
}
NOK=0
for N in $SIZES; do
  rm -rf $O/rp_$N
  run P_$N 600 env ECALC_VERBOSE=2 ECALC_SEGV_TRACE=1 rocprofv3 --kernel-trace --output-format csv -d $O/rp_$N -o rp -- ./ecalc $N && NOK=$((NOK+1))
  run U_$N 400 env ECALC_VERBOSE=2 ECALC_SEGV_TRACE=1 ./ecalc $N
  Pt=$(grep -a -m1 '^total' $O/log/P_$N.log 2>/dev/null | awk '{print $2}'); Ut=$(grep -a -m1 '^total' $O/log/U_$N.log 2>/dev/null | awk '{print $2}')
  { echo; echo "=== $N digits: profiled total ${Pt:-NA} s, unprofiled total ${Ut:-NA} s (overhead = difference; one pair, not a result) ==="
    [ -n "$Pt" ] && python3 $WT/tools/o7b_table.py $O/rp_$N "$Pt" 25 2>&1; } >> $S
done
VERDICT="SUCCESS: $NOK profiled run(s) tabulated in $S"; [ $NOK -gt 0 ] || VERDICT="FAILED: no profiled run finished (see $S, $O/log)"

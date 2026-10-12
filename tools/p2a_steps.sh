#!/bin/bash
# p2a_steps.sh <hold_job> : P2 part A, run on the login node against an existing hold. Profile of branch cand with the three
# switches on, 1 node, 6.441e10 digits: rocprofv3 --kernel-trace run, then an unprofiled run (method of tools/o7b_driver.sh).
# Output ~/p2a/ (summary.txt, log/, rp_<N>/); the caller writes the marker.  Cancels nothing.
J=$1; N=64410000000; O=$HOME/p2a; mkdir -p $O/log; S=$O/summary.txt; T=$HOME/p2tools
E=$HOME/ntt-cand/ecalc; SW="DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1"
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $S; }
run() { local tag=$1; shift
  timeout 1200 srun --jobid=$J -N1 -n1 --mem=0 --gpus=4 -c 192 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; $*" > $O/log/$tag.log 2>&1 < /dev/null
  rc=$?; say "$tag rc $rc total $(grep -a -m1 '^total' $O/log/$tag.log | awk '{print $2}')s VERIFY-OK-lines $(grep -a -c 'VERIFY OK' $O/log/$tag.log) FAILED-lines $(grep -a -c 'VERIFY FAILED' $O/log/$tag.log)"; }
rm -rf $O/rp_$N; : > $S; say "cand commit $(git -C $HOME/ntt-cand log --oneline -1 | cut -c1-60); switches: $SW"
run P_$N env ECALC_VERBOSE=2 ECALC_SEGV_TRACE=1 $SW rocprofv3 --kernel-trace --output-format csv -d $O/rp_$N -o rp -- ./ecalc $N
run U_$N env ECALC_VERBOSE=2 ECALC_SEGV_TRACE=1 $SW ./ecalc $N
Pt=$(grep -a -m1 '^total' $O/log/P_$N.log | awk '{print $2}'); Ut=$(grep -a -m1 '^total' $O/log/U_$N.log | awk '{print $2}')
{ echo; echo "=== $N digits, cand + switches: profiled total ${Pt:-NA} s, unprofiled total ${Ut:-NA} s (one pair, indicative) ==="
  echo "--- unprofiled phase / seed / init lines (ECALC_VERBOSE=2) ---"
  grep -a -E "^total|init |seeds|waits|bs |mdev|recip|dm " $O/log/U_$N.log | head -40
  if [ -n "$Pt" ]; then echo "--- top-25 kernel table (o7b_table.py) ---"; python3 $T/o7b_table.py $O/rp_$N "$Pt" 25 2>&1
    echo "--- 5 s busy timeline (o7b_timeline.py), profiled run ---"
    f=$(find $O/rp_$N -name '*kernel_trace.csv' | head -1); python3 $T/o7b_timeline.py "$f" "$Pt" 5 2>&1; fi; } >> $S

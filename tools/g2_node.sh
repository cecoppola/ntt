#!/bin/bash
# g2_node.sh <outdir> <tag> <hold_end_epoch> <stop_epoch>   (runs ON the compute node, inside the hold, via srun)
# Soak loop for the G2 hang bound: 4e9-digit ecalc runs back to back, each under timeout = 3 x median wall of the OK runs so far
# (fixed FIRST_TO s until MINOK OK runs exist).  On a timeout: stacks (gdb all threads, /proc/<pid>/task/*/{wchan,syscall}) + last 50 log lines,
# then kill that single PID.  Class per run: OK / HANG / SEGV / VFAIL / OTHER.  Appends <outdir>/<tag>/runs.tsv.
O=$1; T=$2; HE=$3; STOP=$4; D=$O/$T; mkdir -p $D/log $D/hang
E=${G2_ECALC:-$HOME/ntt-g2/ecalc}; DIG=${G2_DIGITS:-4000000000}; FIRST_TO=${G2_FIRST_TO:-300}; MINOK=3
module load rocm >/dev/null 2>&1
export RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30
cd $E || exit 1
median() { awk -F'\t' '$6=="OK"{print $5}' $D/runs.tsv 2>/dev/null | sort -n | awk '{a[NR]=$1} END{if(NR) print (NR%2?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2)}'; }
nok() { awk -F'\t' '$6=="OK"{n++} END{print n+0}' $D/runs.tsv 2>/dev/null; }
n=$(wc -l < $D/runs.tsv 2>/dev/null || echo 0)
while :; do
  now=$(date +%s)
  TO=$FIRST_TO; [ "$(nok)" -ge $MINOK ] && TO=$(awk -v m="$(median)" 'BEGIN{t=int(3*m+0.999); if(t<60)t=60; print t}')
  [ $now -ge $STOP ] && { echo "stop time reached"; break; }
  [ $((now + TO + 90)) -ge $HE ] && { echo "hold nearly over"; break; }
  n=$((n+1)); L=$D/log/run$n.log; t0=$(date +%s)
  # PR_SET_PTRACER_ANY (0x59616d61, -1) so that gdb -p may attach under yama ptrace_scope=1; it survives the exec
  python3 -c 'import ctypes,os,sys; ctypes.CDLL(None).prctl(0x59616d61,-1,0,0,0); os.execv("./ecalc",["./ecalc",sys.argv[1]])' $DIG > $L 2>&1 < /dev/null &
  pid=$!; cls=""
  while kill -0 $pid 2>/dev/null; do
    if [ $(( $(date +%s) - t0 )) -ge $TO ]; then
      cls=HANG; H=$D/hang/run$n; mkdir -p $H
      { date; echo "pid $pid timeout ${TO}s"; tail -n 50 $L; } > $H/log_tail.txt 2>&1
      for tk in /proc/$pid/task/*; do echo "== $tk $(cat $tk/comm 2>/dev/null) state=$(awk '/^State/{print $2}' $tk/status 2>/dev/null)"; echo -n 'wchan='; cat $tk/wchan 2>/dev/null; echo; echo -n 'syscall='; cat $tk/syscall 2>/dev/null; echo; done > $H/proc_tasks.txt 2>&1
      timeout 120 gdb -batch -p $pid -ex "set pagination off" -ex "thread apply all bt 25" > $H/gdb.txt 2>&1
      kill $pid; sleep 20; kill -0 $pid 2>/dev/null && kill -9 $pid
      break
    fi
    sleep 2
  done
  wait $pid 2>/dev/null; rc=$?; t1=$(date +%s); w=$((t1-t0))
  v=$(grep -a -m1 '^VERIFY' $L)
  if [ -z "$cls" ]; then
    if grep -aqE 'Segmentation fault|SIGSEGV|SIGBUS|segv' $L || [ $rc -eq 139 ] || [ $rc -eq 135 ]; then cls=SEGV
    elif [ "$v" = "VERIFY OK" ] && [ $rc -eq 0 ]; then cls=OK
    elif grep -aq '^VERIFY FAILED' $L; then cls=VFAIL
    else cls=OTHER; fi
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $n "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" $rc "$v" $w $cls $TO >> $D/runs.tsv
  [ $cls = OK ] && rm -f $L
done

#!/bin/bash
# G13d: the hang after init at 7.70e10 on the defaults (a_S4_lo_770e8).  Runs ecalc under gdb (ptrace_scope 1 forbids attaching,
# but gdb may launch its child); when a run is still going after <wd> s it records rocm-smi and the log's tail, then sends
# SIGINT to ecalc so gdb stops it and prints every thread's stack, then kills it.
#   g13d_hang.sh <batch> <runlist>   (runlist lines as g13d_run.sh: <tag> <est_s> 1 <digits> - [ENV=VAL ...])
set -u
B=$1; L=$2; D=~/g13d; S=$D/summary.txt
cd ~/ntt-G13d/ecalc || exit 1
log() { echo "$(date +%T) [$B] $*" | tee -a $S; }
J=$(sbatch -p PPAC_MI300A_SPX -N1 -w ppac-pl1-s24-16 --gpus=4 -t 0:45:00 -J G --parsable --wrap "sleep 2700")
log "job $J submitted"
trap "scancel $J; log 'job $J cancelled'" EXIT
until [ "$(squeue -j $J -h -o %T)" = RUNNING ]; do sleep 15; done
T0=$(date +%s); log "job $J running on $(squeue -j $J -h -o %N)"
STOP=$(date -d "22:38" +%s); anyhung=0
N() { srun --jobid=$J -N1 --overlap bash -lc "module load rocm >/dev/null 2>&1; $*" < /dev/null; }
while read -r -u 3 tag est np dg ref envs; do
  [ -z "$tag" ] || [ "${tag:0:1}" = "#" ] && continue
  now=$(date +%s)
  if [ $((now - T0 + est)) -gt 2640 ] || [ $((now + est)) -gt $STOP ]; then log "SKIP $tag (est $est s, elapsed $((now - T0)) s)"; continue; fi
  case " $envs " in *" IFHUNG=1 "*) [ "$anyhung" = 0 ] && { log "$tag: not run (no hang so far)"; continue; };; esac
  wd=$((2 * est + 60))
  G="gdb -q -batch -ex 'set pagination off' -ex 'set confirm off' -ex 'handle SIGUSR1 SIGUSR2 SIGPIPE nostop noprint pass' -ex run -ex 'echo ==GDB STOPPED==\\n' -ex 'info threads' -ex 'thread apply all bt 25' -ex kill --args"
  case " $envs " in *" NOGDB=1 "*) G="";; esac
  base="$envs ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 RNS_VERBOSE=1"
  t1=$(date +%s)
  srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd ~/ntt-G13d/ecalc; env $base $G ./ecalc $dg" > $D/$tag.log 2>&1 < /dev/null &
  sp=$!; hung=0
  while kill -0 $sp 2>/dev/null; do
    sleep 5
    if [ $(( $(date +%s) - t1 )) -gt $wd ] && [ $hung = 0 ]; then
      hung=1; anyhung=1; log "$tag: still running after $wd s: capturing"
      N "rocm-smi --showuse --showmemuse; ps -eo pid,stat,etime,pcpu,rss,comm | grep -w ecalc; for p in \$(pgrep -x ecalc); do cd /proc/\$p/task; for t in *; do echo \$(cat \$t/wchan); done | sort | uniq -c; done" > $D/${tag}_capture.txt 2>&1
      tail -30 $D/$tag.log >> $D/${tag}_capture.txt
      N "kill -INT \$(pgrep -x ecalc)"
      sleep 90; N "kill -KILL \$(pgrep -x ecalc) 2>/dev/null";
    fi
  done
  wait $sp; rc=$?; t2=$(date +%s)
  v=$(grep -a -c '^VERIFY OK' $D/$tag.log)
  tot=$(grep -a '^total' $D/$tag.log | tail -1 | cut -c1-110)
  pcs=$(grep -a -o '[0-9]* x [0-9]* pieces' $D/$tag.log | sort | uniq -c | sort -k2n | awk '{printf "%s*%sx%s ", $1, $2, $4}')
  log "$tag np=$np D=$dg [$envs]: rc $rc, VERIFY OK x$v, $([ $hung = 1 ] && echo HUNG) wall $((t2 - t1)) s | $tot | pieces: $pcs"
done 3< $L
log "batch done, elapsed $(( $(date +%s) - T0 )) s"

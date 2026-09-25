#!/bin/bash
# a14_soak.sh - Phase 14 A1: an unattended, self-cancelling soak batch on one PPAC node (<= 45 min), counting failures.
#   a14_soak.sh <tag> <spec> [<spec> ...]
#   spec  mn:<procs>:<digits>:<count>:<timeout_s>[:ENV=V,ENV=V]   node-processes through mnrun.sh (POOL_LOG as mnaccept: 27 at 1e8, 29 at 1e9)
#         s1:<digits>:<count>:<timeout_s>[:ENV=V,...]             one process, four APUs
#         g1:<digits>:<count>:<timeout_s>[:ENV=V,...]             the same under gdb (launch mode: a hang gets SIGINT and every thread's stack)
#         x1:<command>:<count>:<timeout_s>                        a command in ecalc/ on the node (a test; no ':' in it); passes on rc 0 + VERIFY OK
# A size without a reference file runs without an output file and passes on VERIFY OK alone.
# Every run's digits are compared with ~/ntt/ecalc/ref/e_<digits>.txt (or ~/ntt/ecalc/results/e_4e10.out).  A run that outlives its
# timeout is a HANG: the wchan histogram of its threads, rocm-smi and the log's tail are captured before it is killed.
# Logs: ~/A114/<tag>/<run>.log; the running count in ~/A114/<tag>.txt.  The job is cancelled when the script exits.
# Runs from the clone that holds it (cd to its ecalc/).  NODE=<host> pins the job; BUDGET (s, default 2640) bounds the batch.
set -u
TAG=$1; shift
cd "$(dirname "$0")" || exit 1; E=$PWD
D=~/A114/$TAG; mkdir -p "$D"; S=~/A114/$TAG.txt
BUDGET=${BUDGET:-2640}
log() { echo "$(date +%T) [$TAG] $*" | tee -a "$S"; }
W=""; [ -n "${NODE:-}" ] && W="-w $NODE"
J=$(sbatch -p PPAC_MI300A_SPX -N1 $W --gpus=4 -t 0:45:00 -J A1 --parsable --wrap "sleep 2700")
log "job $J submitted ($(git log --oneline -1 | cut -c1-60))"
trap 'scancel $J; log "job $J cancelled (exit)"' EXIT
while [ "$(squeue -j "$J" -h -o %T)" != RUNNING ]; do sleep 15; [ -z "$(squeue -j "$J" -h -o %T)" ] && { log "job $J gone"; exit 1; }; done
T0=$(date +%s); NODEN=$(squeue -j "$J" -h -o %N); log "job $J running on $NODEN"
N() { srun --jobid="$J" -N1 --overlap bash -c "$*" < /dev/null; }
ref_of() { local d=$1; if [ "$d" = 40000000000 ]; then echo ~/ntt/ecalc/results/e_4e10.out; else echo ~/ntt/ecalc/ref/e_$d.txt; fi; }
capture() {   # <name>: the state of a hung run on the node, then SIGINT (gdb prints the stacks) and, 90 s later, SIGKILL
  local name=$1
  N "rocm-smi --showuse --showmemuse 2>/dev/null | grep -v '^=*$'; ps -eo pid,stat,etime,pcpu,rss,nlwp,comm | grep -w ecalc; for p in \$(pgrep -x ecalc); do echo \"== pid \$p wchan:\"; cd /proc/\$p/task && for t in *; do cat \$t/wchan 2>/dev/null; echo; done | sort | uniq -c | sort -rn; done" > "$D/${name}_capture.txt" 2>&1
  tail -40 "$D/$name.log" >> "$D/${name}_capture.txt"
  N "kill -INT \$(pgrep -x ecalc) 2>/dev/null"; sleep 90
  N "kill -KILL \$(pgrep -x ecalc) 2>/dev/null; sleep 2; pkill -KILL -x gdb 2>/dev/null; true"
}
GDB="gdb -q -batch -ex 'set pagination off' -ex 'set confirm off' -ex 'handle SIGUSR1 SIGUSR2 SIGPIPE nostop noprint pass' -ex run -ex 'echo ==GDB STOPPED==\\n' -ex 'info threads' -ex 'thread apply all bt 25' -ex kill --args"
nok=0; nbad=0; nhang=0; ndiff=0; nrun=0
one() {   # <kind> <procs> <digits> <timeout> <env> <name>
  local kind=$1 p=$2 d=$3 to=$4 envs=$5 name=$6; local f=/tmp/A114_${TAG}_$name t1 rc c v
  local ref; ref=$(ref_of "$d"); local pl=27; [ ${#d} -ge 10 ] && pl=29; [ ${#d} -ge 11 ] && pl=31
  [ "$kind" != x1 ] && [ ! -s "$ref" ] && { ref=""; f=""; }   # no reference at this size: no output file, VERIFY OK is the check
  t1=$(date +%s)
  case $kind in
    mn) SLURM_JOB_ID=$J timeout "$to" ./mnrun.sh "$p" env POOL_LOG=$pl ${envs//,/ } ./ecalc "$d" "$f" > "$D/$name.log" 2>&1; rc=$? ;;
    s1) timeout "$to" srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; env ${envs//,/ } ./ecalc $d $f" > "$D/$name.log" 2>&1 < /dev/null; rc=$? ;;
    g1) timeout "$to" srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; env ${envs//,/ } $GDB ./ecalc $d $f" > "$D/$name.log" 2>&1 < /dev/null; rc=$? ;;
    x1) timeout "$to" srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; $d" > "$D/$name.log" 2>&1 < /dev/null; rc=$?
        nrun=$((nrun + 1)); if [ $rc -eq 0 ] && grep -aq 'VERIFY OK' "$D/$name.log"; then nok=$((nok + 1)); log "$name: ok ($(grep -a 'VERIFY OK' "$D/$name.log" | tail -1 | cut -c1-80))"; else nbad=$((nbad + 1)); log "$name: FAIL rc $rc: $(grep -a 'VERIFY\|error\|FAIL' "$D/$name.log" | tail -1 | cut -c1-160)"; fi; return ;;
  esac
  nrun=$((nrun + 1))
  if [ $rc -eq 124 ]; then nhang=$((nhang + 1)); log "$name: HANG after $to s (rc 124): capturing"; capture "$name"; [ -n "$f" ] && N "rm -rf $f $f.*"; return; fi
  if [ -n "$ref" ]; then c=$(N "cat $f.part* > $f.all 2>/dev/null || cp $f $f.all 2>/dev/null; cmp -s $f.all $ref && echo identical || echo DIFFERS; rm -rf $f $f.*"); else c=identical; fi
  v=$(grep -ac 'VERIFY OK' "$D/$name.log")
  local want=1; [ "$kind" = mn ] && want=$((p + 1))
  if [ $rc -eq 0 ] && [ "$c" = identical ] && [ "$v" -ge "$want" ] && ! grep -aq 'VERIFY FAILED' "$D/$name.log"; then nok=$((nok + 1)); echo "$(date +%T) $name: ok $(( $(date +%s) - t1 )) s" >> "$S"
  else nbad=$((nbad + 1)); [ "$c" = DIFFERS ] && ndiff=$((ndiff + 1))
       log "$name: FAIL rc $rc, $c, $v VERIFY OK; $(grep -a 'VERIFY FAILED\|leaf P\|exceeds\|abort\|error\|maxidx\|Killed\|MISMATCH' "$D/$name.log" | head -2 | tr '\n' ';' | cut -c1-200)"; fi
}
for spec in "$@"; do
  IFS=: read -r kind a b c d e <<< "$spec"
  case $kind in
    mn) p=$a; dg=$b; cnt=$c; to=$d; envs=${e:-};;
    s1|g1|x1) p=1; dg=$a; cnt=$b; to=$c; envs=${d:-};;
    *) log "bad spec $spec"; continue;;
  esac
  for ((i = 1; i <= cnt; i++)); do
    now=$(date +%s); [ $((now - T0 + to + 120)) -gt "$BUDGET" ] && { log "budget: stopping before ${kind}_${p}_${dg} run $i"; break 2; }
    one "$kind" "$p" "$dg" "$to" "$envs" "${kind}_p${p}_e$(( ${#dg} - 1 ))_$i"
  done
  log "spec $spec done: ok $nok, failed $nbad (DIFFERS $ndiff), hung $nhang, of $nrun so far ($(( $(date +%s) - T0 )) s)"
done
log "TOTAL ok $nok, failed $nbad (DIFFERS $ndiff), hung $nhang, of $nrun runs in $(( $(date +%s) - T0 )) s"

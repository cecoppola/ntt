#!/bin/bash
# s14_batch.sh - Phase 14 S1's node batch (PLAN §34 W1, ~04:00 EDT): runs unattended on the login node from ecalc/.
#   setsid nohup bash s14_batch.sh [start-cutoff-epoch] [end-epoch] > results/S114_batch/driver.log 2>&1 &
# Submits ONE 30-min job, waits for it to start (cancelled if it has not by the cutoff), then on the node:
#   1. tests/t_spill /tmp 1 8 32          (the spill primitive's round trips and rates; Cached before/after)
#   2. 4e10 with ECALC_ODIRECT=1 ECALC_CKPT_TOP=1 (O_DIRECT checkpoint writer + output), digits vs e_4e10.out
#   3. 4e10 with ECALC_CKPT_TOP=1 only (the buffered baseline), digits vs e_4e10.out
#      (the O_DIRECT run first: the buffered one leaves dirty page cache that would slow whatever follows; its files
#      are removed right after its comparison)
#   4. ECALC_ODIRECT=1 ./mnaccept.sh $J --only unit,e9,recheck   (if the time remains)
# Every step is skipped when it cannot finish before the end time; the job is cancelled at the end.
cd "$(dirname "$0")" || exit 1
CUTOFF=${1:-0}; END=${2:-0}
STEPS=${STEPS:-spill,4e10,accept}          # a subset: e.g. STEPS=accept (the second batch, 01:35 EDT)
step() { [[ ",$STEPS," == *",$1,"* ]]; }
OUT=results/S114_batch; mkdir -p $OUT
REF4=${ECALC_REF_4E10:-$HOME/ntt/ecalc/results/e_4e10.out}
log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a $OUT/summary.txt; }
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:30:00 -J S1 --parsable --wrap "sleep 1800") || { log "sbatch failed"; exit 1; }
log "job $J submitted; start cutoff $(date -d @$CUTOFF '+%H:%M' 2>/dev/null), end $(date -d @$END '+%H:%M' 2>/dev/null)"
trap 'scancel $J 2>/dev/null' EXIT
while :; do
  st=$(squeue -j $J -h -o %T 2>/dev/null)
  [ "$st" = RUNNING ] && break
  [ -z "$st" ] && { log "job $J vanished"; exit 1; }
  if [ $CUTOFF -gt 0 ] && [ $(date +%s) -gt $CUTOFF ]; then log "job $J not started by the cutoff: cancelled"; exit 0; fi
  sleep 20
done
T0=$(date +%s); JEND=$((T0 + 28 * 60)); [ $END -gt 0 ] && [ $END -lt $JEND ] && JEND=$END
NODE=$(squeue -j $J -h -o %N); log "job $J running on $NODE; steps must end by $(date -d @$JEND '+%H:%M:%S')"
left() { echo $((JEND - $(date +%s))); }
R() { local to=$1; shift; timeout "$to" srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $PWD; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
dcmp() { N "cmp -s <(dd if=$1 iflag=direct bs=64M status=none) <(dd if=$REF4 iflag=direct bs=64M status=none) && echo identical || echo DIFFERS"; }
meminfo() { N "grep -E '^(MemAvailable|Cached|Dirty):' /proc/meminfo | tr -s ' ' | tr '\n' ' '"; }
N "rm -rf /tmp/s14_*"

# 1. the spill primitive
if step spill && [ $(left) -gt 360 ]; then
  log "t_spill: $(meminfo)"
  R 420 "./tests/t_spill /tmp 1 8 32" > $OUT/t_spill.log 2>&1; rc=$?
  log "t_spill rc $rc: $(grep -a 'VERIFY' $OUT/t_spill.log | tail -1)"
  grep -a 'GiB  [ABC]\|sp_file mode' $OUT/t_spill.log | tee -a $OUT/summary.txt
  log "after t_spill: $(meminfo)"
fi

# 2, 3. 4e10 with the top set, O_DIRECT then buffered
run4e10() {   # <tag> <env...>
  local tag=$1; shift; local f=/tmp/s14_$tag.txt
  N "python3 -c \"import os,sys; fd=os.open(sys.argv[1],os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\" $REF4 2>/dev/null; rm -rf /tmp/s14_*; true"
  log "4e10 $tag: before $(meminfo)"
  local t1=$(date +%s)
  R 700 "env ECALC_VERBOSE=2 ECALC_CKPT_TOP=1 ECALC_MEM_SAMPLE=2 ECALC_MEM_SAMPLE_FILE=$PWD/$OUT/mem_$tag.txt $* ./ecalc 40000000000 $f" > $OUT/e4e10_$tag.log 2>&1; local rc=$?
  local el=$(( $(date +%s) - t1 ))
  log "4e10 $tag: rc $rc, ${el} s elapsed; after the run $(meminfo)"
  local c=$(dcmp $f)
  log "4e10 $tag: digits $c; $(grep -a '^total' $OUT/e4e10_$tag.log | tail -1 | tr -s ' ')"
  grep -a 'checkpoint: the top-level\|^T1\|^out\|output\|VERIFY\|VmHWM' $OUT/e4e10_$tag.log | tr -s ' ' | cut -c1-400 | tee -a $OUT/summary.txt
  tail -1 $OUT/mem_$tag.txt 2>/dev/null | tee -a $OUT/summary.txt
  N "ls -la $f $f.top 2>/dev/null | head; rm -rf /tmp/s14_*"
  log "4e10 $tag: files removed: $(meminfo)"
}
step 4e10 && [ $(left) -gt 480 ] && run4e10 odirect ECALC_ODIRECT=1
step 4e10 && [ $(left) -gt 480 ] && run4e10 buffered

# 4. the regression steps with the O_DIRECT paths
if step accept && [ $(left) -gt 420 ]; then
  log "mnaccept unit,e9,recheck with ECALC_ODIRECT=1 ($(left) s left)"
  ECALC_ODIRECT=1 timeout $(( $(left) - 30 )) ./mnaccept.sh $J --only unit,e9,recheck > $OUT/mnaccept.log 2>&1
  grep -a '^PASS\|^FAIL\|^==' $OUT/mnaccept.log | tee -a $OUT/summary.txt
fi
N "rm -rf /tmp/s14_* /tmp/mnaccept_$J"
log "done; job $J cancelled"

#!/bin/bash
# wm15_job.sh <A|B> [base_ecalc_dir] - Phase 15 WM (results/WM15.md): the node batches of the weak-memory audit, run unattended
# from the login node (setsid nohup).  Each batch takes its own one-node job (-J WM, 45 min), waits for it, runs, cancels it.
#   A: mnaccept.sh --only unit,e9,mn,stress --stress (this clone, the fix)
#   B: 4 x 10^10 identical (digcmp.sh), then 10^11 paired base / fix / base / fix (the reference evicted before each; `total`
#      and the elapsed wall per run), the fix runs' digits compared to e_1e11.out
# NODES: sbatch -w list (default: any node of the partition).  Logs: results/wm15/<batch>_<job>/.
B=$1; BASE=${2:-$HOME/ntt-WM15-base/ecalc}
cd "$(dirname "$0")" || exit 2
HERE=$PWD
W=${NODES:+-w $NODES}
J=$(sbatch -p PPAC_MI300A_SPX -N1 $W --gpus=4 -t 0:45:00 -J WM --parsable --wrap "sleep 2700") || exit 2
OUT=results/wm15/${B}_$J; mkdir -p "$OUT"; LOG=$OUT/driver.log
echo "$(date -Is) batch $B job $J submitted ($(git log --oneline -1))" >> "$LOG"
trap 'scancel $J 2>/dev/null' EXIT
while :; do st=$(squeue -j "$J" -h -o %T 2>/dev/null); [ "$st" = RUNNING ] && break; [ -z "$st" ] && { echo "$(date -Is) job $J gone" >> "$LOG"; exit 3; }; sleep 20; done
NODE=$(squeue -j "$J" -h -o %N); echo "$(date -Is) running on $NODE" >> "$LOG"
R() { local to=$1; shift; timeout "$to" srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
evict() { N "python3 -c \"import os,sys
for f in sys.argv[1:]:
    fd=os.open(f,os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED); os.close(fd)\" $*"; }
REF4=$HOME/ntt/ecalc/results/e_4e10.out; REF11=$HOME/ntt/ecalc/results/e_1e11.out; T=/tmp/wm15_$J
if [ "$B" = A ]; then
  ./mnaccept.sh "$J" --stress --only unit,e9,mn,stress > "$OUT/mnaccept.out" 2>&1
  echo "$(date -Is) mnaccept rc $?" >> "$LOG"
elif [ "$B" = B ]; then
  N "mkdir -p $T"
  evict "$REF4"
  R 900 "cd $HERE; env ECALC_VERBOSE=2 ./ecalc 40000000000 $T/e4e10.txt" > "$OUT/fix_4e10.log" 2>&1
  c=$(N "cd $HERE; ./digcmp.sh $T/e4e10.txt $REF4"); echo "$(date -Is) 4e10 fix: $c; $(grep -a '^total' "$OUT/fix_4e10.log" | cut -c1-60)" >> "$LOG"
  N "rm -f $T/e4e10.txt*"
  for run in base1 fix1 base2 fix2; do
    case $run in base*) D=$BASE;; *) D=$HERE;; esac
    evict "$REF11" "$REF4"
    t0=$(date +%s.%N)
    R 1200 "cd $D; env ECALC_VERBOSE=2 ./ecalc 100000000000 $T/e1e11.txt" > "$OUT/${run}_1e11.log" 2>&1; rc=$?
    el=$(awk "BEGIN{printf \"%.1f\", $(date +%s.%N) - $t0}")
    c=""; [ "$run" = fix1 ] && c=$(N "cd $HERE; ./digcmp.sh $T/e1e11.txt $REF11")
    echo "$(date -Is) 1e11 $run rc $rc elapsed $el s ${c:+digits $c}; $(grep -a '^total' "$OUT/${run}_1e11.log" | cut -c1-70); $(grep -a 'VERIFY' "$OUT/${run}_1e11.log" | head -1 | cut -c1-40)" >> "$LOG"
    N "rm -f $T/e1e11.txt*"
  done
  N "rm -rf $T"
fi
echo "$(date -Is) batch $B done" >> "$LOG"

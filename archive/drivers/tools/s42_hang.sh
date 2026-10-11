#!/bin/bash
# S42 hang.sh: repeat the hung config (TOP=1 + QSEL=1 = arm B) with QSEL=0 control runs (arm A), per node sequential, nodes concurrent; one -N2 45-min job
OUT=$HOME/s42/h; mkdir -p $OUT/log $OUT/cap; E=$HOME/ntt-s42c/ecalc; DPN=64410000000
say() { echo "$(TZ=America/New_York date +%H:%M:%S) $*" >> $OUT/summary.txt; }
rm -f $OUT/DONE
J=$(sbatch -p PPAC_MI300A_SPX -N2 --exclusive -w ppac-pl1-s24-16,ppac-pl1-s24-26 --gpus-per-node=4 -t 0:45:00 -J S42h --parsable --wrap "sleep 2700")
echo $J > $OUT/jobid; say "job $J submitted"
trap 'scancel $J; echo "done $(date)" > $OUT/DONE' EXIT
for i in $(seq 1 400); do st=$(squeue -j $J -h -o %T); [ "$st" = RUNNING ] && break; [ -z "$st" ] && exit 1; sleep 15; done
[ "$st" = RUNNING ] || exit 1
T0=$(date +%s); say "running on $(squeue -j $J -h -o %N)"
node() {
  local idx=$1 n=0 pat="B B B A"
  while [ $(( $(date +%s) - T0 )) -lt $((2700-420)) ]; do
    for arm in $pat; do
      [ $(( $(date +%s) - T0 )) -lt $((2700-420)) ] || break 2
      n=$((n+1)); local lab=h${idx}_$n$arm env="DBIG_MAXIDX_TOP=1 DBIG_QSEL=1"; [ $arm = A ] && env="DBIG_MAXIDX_TOP=1 DBIG_QSEL=0"
      srun --jobid=$J -N1 -r $idx --gpus-per-node=4 --overlap --cpu-bind=none bash -lc "module load rocm >/dev/null 2>&1; cd $E; env RNS_VERBOSE=1 ECALC_VERBOSE=2 $env ./ecalc $DPN" < /dev/null > $OUT/log/$lab.log 2>&1 &
      local sp=$! t=0 cap=0
      while kill -0 $sp 2>/dev/null; do
        sleep 5; t=$((t+5))
        if [ $t -ge 240 ] && [ $cap = 0 ]; then cap=1
          { echo "== $lab at ${t}s"; tail -n 8 $OUT/log/$lab.log
            timeout 60 srun --jobid=$J -N1 -r $idx --overlap --cpu-bind=none bash -c 'hostname; for p in $(pgrep -x ecalc); do echo "pid $p"; ps -L -o pid,tid,stat,wchan:32,comm -p $p; done; (rocm-smi --showuse --showmemuse 2>&1 || true) | head -40'
          } > $OUT/cap/$lab.txt 2>&1 < /dev/null
        fi
        [ $t -ge 300 ] && { kill $sp; sleep 3; kill -9 $sp 2>/dev/null; break; }
      done
      wait $sp 2>/dev/null; local rc=$?
      local v=$(grep -a -o 'VERIFY [A-Z]*' $OUT/log/$lab.log | tail -1) tot=$(grep -a -m1 '^total' $OUT/log/$lab.log | awk '{print $2}')
      [ $t -ge 300 ] && rc=124
      printf '%s\t%s\t%s\t%s\t%s\t%s\n' $lab $arm $rc "$v" "$tot" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" >> $OUT/pairs$idx.tsv
      say "$lab rc $rc $v total $tot t=$t"
    done
  done
}
node 0 & P0=$!; node 1 & P1=$!; wait $P0; wait $P1; say FINAL

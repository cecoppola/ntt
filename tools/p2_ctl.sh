#!/bin/bash
# p2_ctl.sh <partition> <stop_HH:MM Eastern> <end_HH:MM Eastern> : P2 fire-and-forget controller on ONE SPX node (login-node script).
# Successive holds (sbatch, no -w, <=45 min, never past <end>); in the first hold part A (p2a_steps.sh), then part B soak
# (p2b_node.sh) until the hold nearly ends; no new run after <stop>.  Cancels only its own holds.  Markers ~/p2a/DONE, ~/p2b/DONE.
PART=$1; STOP=$(TZ=America/New_York date -d "$2" +%s); END=$(TZ=America/New_York date -d "$3" +%s)
O=$HOME/p2b; mkdir -p $O $HOME/p2a; D=$O; T=$HOME/p2tools
S() { echo "$(TZ=America/New_York date +%H:%M:%S) ET $*" >> $D/driver.txt; }
rm -f $HOME/p2a/DONE $O/DONE; AD=0
trap '[ $AD = 1 ] || echo "FAILED: controller ended before part A finished" > $HOME/p2a/DONE; echo "ended $(TZ=America/New_York date +%H:%M) ET; runs $(wc -l < $O/b/runs.tsv 2>/dev/null)" > $O/DONE' EXIT
while [ $(date +%s) -lt $((STOP - 300)) ]; do
  [ -e $O/STOP ] && { S "STOP file"; break; }
  TL=$(( (END - $(date +%s)) / 60 )); [ $TL -gt 45 ] && TL=45; [ $TL -lt 15 ] && break
  J=$(sbatch -p $PART -N1 --gpus=4 -t 0:$TL:00 -J p2 --parsable --wrap "sleep $((TL*60))" < /dev/null 2>$D/sbatch.err); J=${J%%;*}
  [ -n "$J" ] || { S "sbatch failed: $(tail -1 $D/sbatch.err)"; sleep 60; continue; }
  for i in $(seq 90); do [ "$(squeue -h -j $J -o %T)" = RUNNING ] && break; sleep 10; done
  if [ "$(squeue -h -j $J -o %T)" != RUNNING ]; then S "hold $J not running after 15 min, cancelled"; scancel $J; continue; fi
  HS=$(date +%s); HE=$((HS + TL*60 - 120)); S "hold $J running on $(squeue -h -j $J -o %N), ${TL} min"
  if [ $AD = 0 ]; then bash $T/p2a_steps.sh $J < /dev/null >> $D/a_steps.out 2>&1
    echo "SUCCESS: $(grep -a -c 'VERIFY OK' $HOME/p2a/log/*.log | tr '\n' ' ') (see summary.txt)" > $HOME/p2a/DONE; AD=1; S "part A done"; fi
  timeout $((TL*60 - 60)) srun --jobid=$J -N1 -n1 --mem=0 --gpus=4 -c 192 --overlap bash $T/p2b_node.sh $O b $HE $STOP < /dev/null >> $D/node.out 2>&1
  S "hold $J srun rc=$? ; runs so far $(wc -l < $O/b/runs.tsv 2>/dev/null)"
  scancel $J; sleep 5; squeue -s -j $J -h 2>/dev/null | head -3 >> $D/driver.txt
done
S "controller ends"

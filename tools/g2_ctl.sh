#!/bin/bash
# g2_ctl.sh <partition> <nodename> <tag> <stop_HH:MM Eastern, today> : fire-and-forget controller for ONE soak node (G2).
# Successive <=45-min holds on that node (sbatch ... -w node), g2_node.sh run inside via srun --overlap; no new run after the stop time.
# Never touches other jobs: scancel only its own hold id.  Output ~/g2/<tag>/ (driver.txt, runs.tsv, hang/, log/), marker ~/g2/<tag>/CTL_DONE.
PART=$1; NODE=$2; TAG=$3; STOPHM=$4; O=$HOME/g2; D=$O/$TAG; mkdir -p $D
STOP=$(TZ=America/New_York date -d "$STOPHM" +%s)
S() { echo "$(TZ=America/New_York date +%H:%M:%S) ET $*" >> $D/driver.txt; }
rm -f $D/CTL_DONE; trap 'touch $D/CTL_DONE' EXIT
while [ $(date +%s) -lt $((STOP - 300)) ]; do
  [ -e $O/STOP ] && { S "STOP file"; break; }
  J=$(sbatch -p $PART -w $NODE -N1 --gpus=4 -t 0:45:00 -J g2$TAG --parsable --wrap "sleep 2700" < /dev/null 2>$D/sbatch.err); J=${J%%;*}
  [ -n "$J" ] || { S "sbatch failed: $(tail -1 $D/sbatch.err)"; sleep 60; continue; }
  for i in $(seq 120); do [ "$(squeue -h -j $J -o %T)" = RUNNING ] && break; sleep 10; done
  if [ "$(squeue -h -j $J -o %T)" != RUNNING ]; then S "hold $J not running after 20 min, cancelled"; scancel $J; continue; fi
  HS=$(date +%s); S "hold $J running"
  timeout 2650 srun --jobid=$J --overlap bash $HOME/ntt-g2/tools/g2_node.sh $O $TAG $((HS + 2600)) $STOP < /dev/null >> $D/node.out 2>&1
  S "hold $J srun rc=$? ; runs so far $(wc -l < $D/runs.tsv 2>/dev/null)"
  scancel $J; sleep 5; squeue -s -j $J -h 2>/dev/null | head -3 >> $D/driver.txt
done
S "controller ends"

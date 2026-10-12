#!/bin/bash
# O3 aac6 driver (fire-and-forget): wait until NOT_BEFORE (Eastern HH:MM, default 05:35, after the H4 driver), then one 1-node hold
# (<= 45 min), gate A = --build, no profile; gate B = --profile aac6. Output ~/o3/ ; summary.txt and O3_DONE at the end; the hold is
# released at the end (only our own job id is cancelled).
OUT=$HOME/o3; mkdir -p $OUT; rm -f $OUT/O3_DONE; cd $HOME/ntt-o3 || exit 2
NB=${NOT_BEFORE:-05:35}
while [ "$(TZ=America/New_York date +%H%M)" \< "$(echo $NB | tr -d :)" ] || [ "$(TZ=America/New_York date +%H%M)" \> 0900 ]; do
  [ -e $OUT/STOP ] && exit 0; sleep 120; done
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J o3 --parsable --wrap "sleep 2700" < /dev/null); J=${J%%;*}
echo "hold $J" > $OUT/summary.txt
for i in $(seq 1 90); do [ "$(squeue -j $J -h -o %T)" = RUNNING ] && break; sleep 20; done
if [ "$(squeue -j $J -h -o %T)" = RUNNING ]; then
  tools/gate.sh --build --nodes 1 --hold $J --line none --out $OUT/gateA --timeout 900 > $OUT/gateA.log 2>&1; echo "A rc=$?" >> $OUT/summary.txt
  tools/gate.sh --nodes 1 --hold $J --line none --profile aac6 --out $OUT/gateB --timeout 900 > $OUT/gateB.log 2>&1; echo "B rc=$?" >> $OUT/summary.txt
  cat $OUT/gateA/gate.txt $OUT/gateB/gate.txt $OUT/gateB/config_src.txt >> $OUT/summary.txt 2>&1
  squeue -s -j $J >> $OUT/summary.txt 2>&1
else echo "hold never ran" >> $OUT/summary.txt; fi
scancel $J; touch $OUT/O3_DONE

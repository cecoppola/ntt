#!/bin/bash
# t11_run.sh (T11): aac6 fire-and-forget. One SH5 CPX hold on aac6-sh5cpx-a (<= 45 min), ABBA at 4e9 digits via t11_abba.sh, release, marker.
# run: setsid nohup bash ~/sess1011b/t11/t11_run.sh > ~/sess1011b/t11/driver.out 2>&1 < /dev/null &
O=$HOME/sess1011b/t11; WT=$HOME/ntt-t11; mkdir -p $O
say() { echo "$(TZ=America/New_York date '+%F %T %Z') $1" >> $O/driver.log; }
J=$(sbatch -p SH5_MI300A_CPX -w aac6-sh5cpx-a -N1 --gpus=4 -t 0:45:00 -J t11 --parsable --wrap "sleep 2640" < /dev/null 2>$O/sbatch.err); J=${J%%;*}
[ -n "$J" ] || { echo "FAILED: no hold submitted ($(cat $O/sbatch.err))" > $O/T11_DONE; exit 0; }
for i in $(seq 1 120); do st=$(squeue -j $J -h -o %T); [ "$st" = RUNNING ] && break; [ -z "$st" ] && break; sleep 5; done
if [ "$(squeue -j $J -h -o %T)" != RUNNING ]; then scancel $J; echo "FAILED: hold $J not running" > $O/T11_DONE; say "no hold"; exit 0; fi
END=$(( $(date +%s) + 45 * 60 )); STOP=$(( END - 300 )); say "hold $J running on aac6-sh5cpx-a; stop epoch $STOP"
DRY=0 PLAT=aac6 WT=$WT MAXR=6 MINR=3 HALF_S=0.15 MARK=T11_ABBA_DONE STOP_EPOCH=$STOP RUN_S=60 \
  ARM_A="DBIG_S51_STATS=1 RNS_INIT_POOL_LOG=29" ARM_B="DBIG_S51_STATS=1 DBIG_ADDSUB3=1 DBIG_MODQ_MONT=1 RNS_INIT_POOL_LOG=29" \
  bash $O/t11_abba.sh $J $O/abba 1 4000000000 < /dev/null > $O/abba.out 2>&1; st=$?
say "abba rc $st: $(head -1 $O/abba/T11_ABBA_DONE 2>/dev/null)"
scancel $J; sleep 3
[ "$(squeue -s -j $J -h 2>/dev/null | wc -l)" = 0 ] || say "WARNING: steps left in $J"
v=$(head -1 $O/abba/T11_ABBA_DONE 2>/dev/null); case "$v" in SUCCESS*) echo "$v" > $O/T11_DONE;; *) echo "FAILED: ${v:-no marker (rc $st)}" > $O/T11_DONE;; esac
say "done: $(cat $O/T11_DONE)"

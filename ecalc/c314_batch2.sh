#!/bin/bash
# C3 node batch 2 (aac6 login node): the 4e10 gate (mnaccept's full step needs --full as well as --only full).
#   setsid nohup bash ~/C3_batch2.sh > ~/C314/batch2.out 2>&1 &
set -u
D=~/C314; mkdir -p $D; S=$D/summary2.txt
E=~/ntt-C314/ecalc; cd $E || exit 1
log() { echo "$(date +%T) $*" | tee -a $S; }
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:20:00 -J C314b --parsable --wrap "sleep 1200")
log "job $J submitted"
trap "scancel $J; log 'job $J cancelled'" EXIT
GIVEUP=$(( $(date +%s) + 4 * 3600 ))
until [ "$(squeue -j $J -h -o %T)" = RUNNING ]; do [ $(date +%s) -gt $GIVEUP ] && { log "GIVE UP"; exit 1; }; sleep 20; done
T0=$(date +%s); log "job $J running on $(squeue -j $J -h -o %N)"
./mnaccept.sh $J --full --only full > $D/mnaccept_full.out 2>&1
grep -a '^PASS\|^FAIL' $D/mnaccept_full.out | cut -c1-220 | sed 's/^/        /' | tee -a $S
log "TOTAL batch 2 done, elapsed $(( $(date +%s) - T0 )) s"

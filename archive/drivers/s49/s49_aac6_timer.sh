#!/bin/bash
# T4 (S49) aac6 timer (runs on the aac6 login node, setsid nohup): at 03:30 EDT (epoch 1791703826) submit one <=12-min
# nodechk job on each of PPAC s24-16 and s24-26, cancel leftovers at 03:46 EDT, done by 03:50 EDT (1791705026).
# Nothing is pending before 03:30.  Marker: ~/batch1011/T4/DONE_aac6.   DRY=1: print only.   START=<epoch> overrides 03:30.
START=${START:-1791703826}; HARD=$((START+1200)); CANCEL=$((START+960)); T4=$HOME/batch1011/T4; mkdir -p "$T4/out"
log() { echo "$(date +%F_%T) $*"; }
log "armed; start epoch $START ($(date -d @$START +%F_%T) aac6 local)"
while [ "$(date +%s)" -lt "$START" ]; do [ -e "$T4/STOP6" ] && { log STOP; exit 0; }; sleep 20; done
declare -A J
for n in 16 26; do
  h=aac6-ppac-$n
  if [ "${DRY:-0}" = 1 ]; then log "DRY sbatch -p PPAC_MI300A_SPX -N1 -w $h --gpus=4 -t 0:12:00 aac6_job.sh aac6_s24-$n"; continue; fi
  J[$n]=$(sbatch --parsable -p PPAC_MI300A_SPX -N1 -w $h --gpus=4 -t 0:12:00 --deadline=$(date -d @$HARD +%FT%T) -J b1011t4 -o "$T4/job6.%j.out" --wrap "bash $T4/aac6_job.sh aac6_s24-$n")
  log "submitted ${J[$n]} on $h"
done
while :; do
  left=0; for n in "${!J[@]}"; do squeue -h -j "${J[$n]}" 2>/dev/null | grep -q . && left=1; done
  [ $left = 0 ] && break
  if [ "$(date +%s)" -ge "$CANCEL" ]; then for n in "${!J[@]}"; do squeue -h -j "${J[$n]}" 2>/dev/null | grep -q . && { scancel "${J[$n]}"; log "late: scancel ${J[$n]}"; }; done; break; fi
  sleep 10
done
{ echo "T4 aac6 ended $(date +%F_%T)"; ls "$T4"/out | grep aac6; } > "$T4/DONE_aac6"; log end

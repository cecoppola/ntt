#!/bin/bash
# T4 (S49) aac7 login-node driver #2 (post-deadline coverage).  Sleeps to D+60 s, waits until <=1 of our nodes run
# (max until D+720 s), then submits 1-node 10-min nodechk jobs (nc_job.sh) on every partition host except Job A's 2 and
# Job B's 4 (those are covered by the kit/queue item), at most 5 at once, nothing that could run past S.
# Start:  setsid nohup bash adhoc2_driver.sh > driver2.log 2>&1 < /dev/null &   DRY=1 + D=/S= overrides for a dry run.
# Marker: ~/batch1011/T4/DONE_adhoc2 (covered/MISSING).  STOP file ~/batch1011/T4/STOP2 ends it.  Times are epochs.
D=${D:-1791723386}; S=${S:-1791725186}; MAXRUN=${MAXRUN:-5}; DRY=${DRY:-0}
T4=$HOME/batch1011/T4; OUT=$T4/out; EXCL=" aac7-n30 aac7-n31 aac7-n10 aac7-n11 aac7-n20 aac7-n21 "
log() { echo "$(date +%F_%T) $*"; }
done_host() { cat "$OUT"/*/logs/06_nodechk.log 2>/dev/null | grep -a '^NODECHK ' | grep -aq "host=$1 .*apu0_s=[0-9]"; }
allhosts() { sinfo -N -h -o '%N %t' | sort -u | awk '$2 !~ /^(down|drain|fail|maint|unk)/ {print $1}' | sort -u; }
mynodes() { squeue -u "$USER" -h -t R -o '%D' | awk '{s+=$1} END {print s+0}'; }
MYJ=(); declare -A tries jobhost jobt0
mkdir -p "$OUT"; log "start D=$D S=$S DRY=$DRY"
while [ "$(date +%s)" -lt $((D+60)) ]; do [ -e "$T4/STOP2" ] && break; sleep 20; done
while [ "$DRY" != 1 ] && [ "$(date +%s)" -lt $((D+720)) ] && [ "$(mynodes)" -gt 1 ] && [ ! -e "$T4/STOP2" ]; do sleep 15; done
log "go: our running nodes=$(mynodes)"
todo=(); for h in $(allhosts); do case "$EXCL" in *" $h "*) continue;; esac; done_host "$h" || todo+=("$h"); done
ord=(); for h in "${todo[@]}"; do [ "$h" = aac7-n01 ] && ord=("$h" "${ord[@]}") || ord+=("$h"); done
log "hosts: ${ord[*]}"
alljobs=""; queue=("${ord[@]}"); running=()
while { [ ${#queue[@]} -gt 0 ] || [ ${#running[@]} -gt 0 ]; } && [ ! -e "$T4/STOP2" ]; do
  now=$(date +%s)
  nr=(); for J in "${running[@]}"; do   # reap finished, cancel jobs pending > 5 min
    if squeue -h -j "$J" 2>/dev/null | grep -q .; then
      if squeue -h -j "$J" -o '%T' | grep -q PENDING && [ $((now-${jobt0[$J]})) -gt 300 ]; then scancel "$J"; log "pending>5min scancel $J (${jobhost[$J]})"; else nr+=("$J"); fi
    else done_host "${jobhost[$J]}" && log "OK ${jobhost[$J]}" || log "no data ${jobhost[$J]} (job $J)"; fi
  done; running=("${nr[@]}")
  nq=(); for h in "${queue[@]}"; do
    done_host "$h" && continue
    if [ ${#running[@]} -lt "$MAXRUN" ] && [ $((now+660)) -le "$S" ] && { [ "$DRY" = 1 ] || [ "$(mynodes)" -lt 6 ]; }; then
      lab=adhoc2_${h}_$now
      if [ "$DRY" = 1 ]; then log "DRY would sbatch -N1 --exclusive --gpus-per-node=4 -w $h -t 0:10:00 nc_job.sh $lab"; tries[$h]=9; continue; fi
      J=$(sbatch --parsable -N1 --exclusive --gpus-per-node=4 -w "$h" -t 0:10:00 --deadline="$(date -d @$S +%FT%T)" -J b1011t4 -o "$T4/job.%j.out" --wrap "bash $T4/nc_job.sh $lab") || { log "sbatch failed $h"; nq+=("$h"); continue; }
      tries[$h]=$(( ${tries[$h]:-0}+1 )); jobhost[$J]=$h; jobt0[$J]=$now; running+=("$J"); alljobs="$alljobs $J"; log "submitted $J on $h ($lab)"
    else nq+=("$h"); fi
  done; queue=("${nq[@]}")
  [ "$DRY" = 1 ] && [ ${#running[@]} -eq 0 ] && break
  [ $(date +%s) -gt $((S-60)) ] && break
  sleep 10
done
for J in $alljobs; do squeue -h -j "$J" 2>/dev/null | grep -q . && { scancel "$J"; log "final scancel $J"; }; done
log "driver2 end"
{ echo "T4 adhoc2 driver ended $(date +%F_%T) jobs:$alljobs"; for h in $(allhosts); do case "$EXCL" in *" $h "*) echo "skipped(A/B) $h"; continue;; esac; done_host "$h" && echo "covered $h" || echo "MISSING $h"; done; } > "$T4/DONE_adhoc2"

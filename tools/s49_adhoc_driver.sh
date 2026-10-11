#!/bin/bash
# T4 (S49) aac7 login-node driver: one 1-node `target_kit.sh --only nodechk` job per host (sbatch -w <host>), under the
# BATCH_SLOTS ad-hoc rules.  Start:  setsid nohup bash adhoc_driver.sh > driver.log 2>&1 < /dev/null &
# DRY=1 : print decisions only (no sbatch).  STOP file ~/batch1011/T4/STOP ends it.  Marker: ~/batch1011/T4/DONE_adhoc.
# Rules enforced: our running node total <= 5 before submit (so <= 6 after); never submit in [W-60min, W+75min];
# job -t 10 min (rule: <= 20); scancel own job if pending > 10 min; nothing started that cannot finish by D;
# a host is submitted only when Slurm shows it idle (never queue behind A/B/J6 holds); a host already measured
# (a NODECHK line with host=<h> in any out/*/logs/06_nodechk.log, e.g. the Job B queue item) is skipped.
W=1791712586; D=1791723386; T4=$HOME/batch1011/T4; OUT=$T4/out; DRY=${DRY:-0}; RUNLIM=${RUNLIM:-5}
mkdir -p "$OUT"; log() { echo "$(date +%F_%T) $*"; }
done_host() { cat "$OUT"/*/logs/06_nodechk.log 2>/dev/null | grep -a '^NODECHK ' | grep -aq "host=$1 .*apu0_s=[0-9]"; }
allhosts() { sinfo -N -h -o '%N %t' | sort -u | awk '$2 !~ /^(down|drain|fail|maint|unk)/ {print $1}' | sort -u; }
down_hosts() { sinfo -N -h -o '%N %t' | sort -u | awk '$2 ~ /^(down|drain|fail|maint|unk)/ {print $1"("$2")"}'; }
hoststate() { sinfo -N -h -n "$1" -o '%t' | head -1; }
mynodes() { squeue -u "$USER" -h -t R -o '%D' | awk '{s+=$1} END {print s+0}'; }
inwindow() { local t=$1; [ "$t" -ge $((W-3600)) ] && [ "$t" -lt $((W+4500)) ]; }
declare -A tries
[ "$DRY" = 1 ] && log "DRY RUN"
log "down/unlisted: $(down_hosts | tr '\n' ' ')  start; D=$D"
while :; do
  now=$(date +%s)
  [ -e "$T4/STOP" ] && { log STOP; break; }
  [ "$now" -gt $((D-1200)) ] && { log "past D-20min"; break; }
  todo=(); for h in $(allhosts); do done_host "$h" || { [ "${tries[$h]:-0}" -lt 3 ] && todo+=("$h"); }; done
  [ ${#todo[@]} -eq 0 ] && { log "all hosts covered or out of tries"; break; }
  ord=(); for h in "${todo[@]}"; do [ "$h" = aac7-n01 ] && ord=("$h" "${ord[@]}") || ord+=("$h"); done
  for h in "${ord[@]}"; do
    now=$(date +%s)
    if inwindow "$now" || inwindow $((now+1200)) || [ $((now+1200)) -gt "$D" ]; then continue; fi
    [ "$(mynodes)" -gt "$RUNLIM" ] && continue
    st=$(hoststate "$h"); [ "$st" = idle ] || continue
    if [ "$DRY" = 1 ]; then log "DRY would: sbatch -N1 -w $h -t 0:10:00 nc_job.sh adhoc_$h (our nodes running=$(mynodes), state=$st)"; tries[$h]=3; continue; fi
    lab=adhoc_${h}_$now
    J=$(sbatch --parsable -N1 -w "$h" -t 0:10:00 -J b1011t4 -o "$T4/job.%j.out" --wrap "bash $T4/nc_job.sh $lab") || { log "sbatch failed $h"; continue; }
    tries[$h]=$(( ${tries[$h]:-0} + 1 )); log "submitted $J on $h ($lab)"
    t0=$(date +%s)
    while squeue -h -j "$J" 2>/dev/null | grep -q .; do
      if squeue -h -j "$J" -o '%T' | grep -q PENDING && [ $(( $(date +%s) - t0 )) -gt 600 ]; then scancel "$J"; log "pending >10min: scancel $J ($h)"; break; fi
      sleep 10
    done
    done_host "$h" && log "OK $h" || log "no data from $h (rc=$(cat $OUT/$lab/rc 2>/dev/null))"
  done
  sleep 60
done
log "driver end"
{ echo "T4 adhoc driver ended $(date +%F_%T)"; for h in $(allhosts); do done_host "$h" && echo "covered $h" || echo "MISSING $h"; done; } > "$T4/DONE_adhoc"

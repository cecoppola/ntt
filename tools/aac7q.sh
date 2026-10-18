#!/bin/bash
# aac7q.sh - standing work-queue runner for the aac7 login node (keeps the per-user node allowance busy with approved work).
# Budget per cycle = the account's per-user node limit read from Slurm (sacctmgr assoc maxtres/grptres and qos maxtrespu/grptres,
#   smallest node=N; re-read every BUDGET_TTL s, last value kept on failure, start 6; BUDGET=<n> in the environment caps it; items
#   run concurrently while they fit); free nodes never exceed the partition's idle count (sinfo). Never bypasses the Slurm limits.
#   aac7q.sh start|run|stop|status      (start = setsid nohup of "run"; stop = touch STOP: no new items, exit when running ones end)
# Queue dir $AQ/queue/*.sh (AQ=~/aac7q).  Item headers (comment lines): OWNER, NODES (1..budget), EST_MIN, APPROVED (who/date; items without
# it never run), optional NOT_BEFORE / NOT_AFTER (epoch), EXCLUSIVE=1 (only when none of our jobs run, and nothing else starts meanwhile),
# ALLOW_SLOW=1 (do not exclude aac7-n01), GROUP=<name> (members start only when all fit at once, e.g. concurrent A/B arms).  See tools/aac7q_item_template.sh.
# Scheduling: largest-first; items that do not fit are skipped and later ones that fit start (backfill), until a skipped item has waited
#   AQ_STARVE_MIN (45) min: then nothing starts behind it until it does.
# Per item: own sbatch hold, item run with SLURM_JOB_ID=<hold> (steps must use --overlap --mem=0), hold scancelled afterwards, <item>.rc/.log.
# STUB=1: no Slurm; holds are files under $AQ/stub (a file stub/ext_* of N nodes simulates a foreign job of ours).
AQ=${AQ:-$HOME/aac7q}; Q=$AQ/queue; ST=$AQ/state; STUB=${STUB:-0}; BUDGET_CAP=${BUDGET:-}; BUDGET=6; BUDGET_T=0; BUDGET_TTL=${BUDGET_TTL:-600}
POLL=${POLL:-20}; EST_UNIT=${EST_UNIT:-60}          # seconds per EST_MIN (stub tests shrink it)
PEND_MAX=${PEND_MAX:-600}; RETRY_DELAY=${RETRY_DELAY:-300}; MAX_RETRY=${MAX_RETRY:-5}; EXCL_PATIENCE=${EXCL_PATIENCE:-3600}
AQ_STARVE_MIN=${AQ_STARVE_MIN:-45}; STARVE_S=${STARVE_S:-$((AQ_STARVE_MIN * 60))}   # a non-fitting item older than this blocks backfill
PART=${AAC7_PARTITION:-192C4G1H_MI300A_RHEL9_A1}; USR=${USER:-<user>}
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S') $*" >> $AQ/log; }
now() { date +%s; }
hdr() { grep -m1 "^# $2=" "$1" 2>/dev/null | cut -d= -f2- | sed 's/^ *//;s/ *$//'; }
mkdir -p $Q $ST $AQ/stub $AQ/work
cmd=${1:-status}
case $cmd in
  start) setsid nohup bash "$(readlink -f "$0")" run >> $AQ/daemon.out 2>&1 < /dev/null & echo "started (pid $!)"; exit 0;;
  stop) touch $AQ/STOP; echo "STOP set: no new items; daemon exits when running items end (rm $AQ/STOP to cancel)"; exit 0;;
  status) echo "heartbeat: $(cat $AQ/heartbeat 2>/dev/null) (now $(now))"; [ -e $AQ/STOP ] && echo STOP set; [ -e $AQ/IDLE ] && { echo IDLE:; cat $AQ/IDLE; }
    echo "state:"; for f in $ST/*; do [ -e "$f" ] && echo "  $(basename $f): $(cat $f)"; done; tail -15 $AQ/log 2>/dev/null; exit 0;;
  run) ;;
  *) echo "usage: $0 start|run|stop|status"; exit 2;;
esac
exec 9> $AQ/lock; flock -n 9 || { echo "another aac7q daemon holds $AQ/lock"; exit 1; }
rm -f $AQ/STOP
# ---- slurm layer (real or stub)
used_nodes() { if [ "$STUB" = 1 ]; then local s=0 f; for f in $AQ/stub/*; do [ -e "$f" ] && s=$((s + $(head -1 $f | awk '{print $1}'))); done; echo $s
  else squeue -u $USR -h -o %D 2>/dev/null | awk '{s+=$1} END{print s+0}'; fi; }
hold_state() { if [ "$STUB" = 1 ]; then [ -e $AQ/stub/hold_$1 ] || { echo GONE; return; }; [ $(now) -ge $(awk '{print $2}' $AQ/stub/hold_$1) ] && echo RUNNING || echo PENDING
  else local s; s=$(squeue -j $1 -h -o %T 2>/dev/null); echo ${s:-GONE}; fi; }
hold_new() { # nodes minutes name -> id
  if [ "$STUB" = 1 ]; then local id=9$RANDOM; echo "$1 $(( $(now) + ${STUB_PEND_S:-1} ))" > $AQ/stub/hold_$id; echo $id
  else sbatch -p $PART -N $1 --exclusive --gpus-per-node=4 -x $EXCL -t $2 -J aq_$3 --parsable --wrap "sleep $(( $2 * 60 ))" 2>>$AQ/log; fi; }
# per-user node limit from Slurm: smallest node=N over the user's associations (maxtres, grptres) and their QOS (maxtrespu, grptres)
slurm_limit() { local l q n best= src= a
  a=$(sacctmgr -n -P show assoc user=$USR format=maxtres,grptres,qos 2>/dev/null) || return 1; [ -n "$a" ] || return 1
  while IFS='|' read -r mt gt qs; do
    for n in $(echo "$mt,$gt" | tr ',' '\n' | sed -n 's/^node=\([0-9]\+\)$/\1/p'); do [ -z "$best" ] || [ "$n" -lt "$best" ] && { best=$n; src="assoc node=$n"; }; done
    for q in $(echo "$qs" | tr ',' ' '); do l=$(sacctmgr -n -P show qos "$q" format=maxtrespu,grptres 2>/dev/null) || continue
      for n in $(echo "$l" | tr '|,' '\n\n' | sed -n 's/^node=\([0-9]\+\)$/\1/p'); do [ -z "$best" ] || [ "$n" -lt "$best" ] && { best=$n; src="qos $q node=$n"; }; done; done
  done <<< "$a"
  [ -n "$best" ] && echo "$best $src"; }
budget_refresh() { local r n src; [ $(( $(now) - BUDGET_T )) -ge $BUDGET_TTL ] || return 0; BUDGET_T=$(now)
  r=$(slurm_limit) && [ -n "$r" ] || { say "budget: Slurm limit query failed, keeping $BUDGET"; return 0; }
  n=${r%% *}; src=${r#* }; [ -n "$BUDGET_CAP" ] && [ "$BUDGET_CAP" -lt "$n" ] && { n=$BUDGET_CAP; src="$src, capped by BUDGET=$BUDGET_CAP"; }
  [ "$n" = "$BUDGET" ] || { say "budget $BUDGET -> $n ($src)"; BUDGET=$n; }; }
part_idle() { local o; [ "$STUB" = 1 ] && { echo 999; return; }; o=$(sinfo -p $PART -h -o '%T %D' 2>/dev/null)   # idle nodes; unknown (query failed) = no cap
  [ -n "$o" ] && echo "$o" | awk '$1 ~ /^idle/ {s+=$2} END{print s+0}' || echo 999; }
hold_kill() { if [ "$STUB" = 1 ]; then rm -f $AQ/stub/hold_$1; else scancel $1 2>/dev/null; fi; }
kill_tree() { local p=$1 c; for c in $(pgrep -P $p 2>/dev/null); do kill_tree $c; done; kill -TERM $p 2>/dev/null; }
# ---- one item, run in a background subshell: hold -> wait RUNNING -> run -> scancel
run_item() { local f=$1 b n est own nodes ex slow J EXCL_L t0 w pid rc to hmin
  b=$(basename $f .sh); own=$(hdr $f OWNER); nodes=$(hdr $f NODES); est=$(hdr $f EST_MIN); slow=$(hdr $f ALLOW_SLOW)
  EXCL=aac7-n61; [ "$slow" = 1 ] || EXCL=$EXCL,aac7-n01
  hmin=$(( est * 3 / 2 + 5 )); to=$(( est * EST_UNIT * 3 / 2 + 5 * EST_UNIT ))
  J=$(hold_new $nodes $hmin $b); [ -n "$J" ] || { say "$b: sbatch failed"; echo "retry $(( $(now) + RETRY_DELAY ))" > $ST/$b.retry; rm -f $ST/$b; return; }
  echo "hold $J submitted $(now) nodes $nodes owner $own" > $ST/$b; say "$b: hold $J ($nodes nodes, -t $hmin min, owner $own)"
  w=0; while [ "$(hold_state $J)" != RUNNING ]; do
    [ "$(hold_state $J)" = GONE ] && { say "$b: hold $J vanished"; echo "retry $(( $(now) + RETRY_DELAY ))" > $ST/$b.retry; rm -f $ST/$b; return; }
    [ $w -ge $PEND_MAX ] && { say "$b: hold $J pending > ${PEND_MAX}s: cancelled, retry later"; hold_kill $J
      local c=$(cut -d' ' -f3 $ST/$b.retry 2>/dev/null); echo "retry $(( $(now) + RETRY_DELAY )) $(( ${c:-0} + 1 ))" > $ST/$b.retry; rm -f $ST/$b; return; }
    sleep ${HOLD_POLL:-5}; w=$((w + ${HOLD_POLL:-5})); done
  t0=$(now); echo "running hold $J since $t0 nodes $nodes owner $own timeout ${to}s" > $ST/$b; say "$b: hold $J running, item started (timeout ${to}s)"
  ( cd $AQ/work && env -u OUT -u HOLD SLURM_JOB_ID=$J AQ_HOLD=$J AQ_NODES=$nodes AQ_ITEM=$b AQ_STUB=$STUB bash $f > ${f%.sh}.log 2>&1 < /dev/null ) & pid=$!
  w=0; while kill -0 $pid 2>/dev/null; do sleep ${ITEM_POLL:-2}; w=$((w + ${ITEM_POLL:-2})); [ $w -ge $to ] && break; done
  if kill -0 $pid 2>/dev/null; then say "$b: TIMEOUT ${to}s, killing PID tree of $pid"; kill_tree $pid; sleep 3; kill -KILL $pid 2>/dev/null
    echo "[aac7q] timeout ${to}s" >> ${f%.sh}.log; wait $pid 2>/dev/null; rc=124; else wait $pid; rc=$?; fi
  hold_kill $J; echo $rc > ${f%.sh}.rc
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$b" "$own" "$nodes" "$est" "$(TZ=America/New_York date -d @$t0 +%m-%d_%H:%M:%S)" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" "$rc" >> $AQ/done.tsv
  say "$b: rc $rc after $(( $(now) - t0 )) s, hold $J cancelled"; echo "done rc $rc" > $ST/$b.done; rm -f $ST/$b; }
RUNPIDS=""
cleanup() { say "daemon exit: killing runners"; for p in $RUNPIDS; do kill_tree $p; done; for f in $ST/*; do [ -e "$f" ] || continue
    case $f in *.retry|*.done|*.since|*.warn|*.wait|*.starved) ;; *) h=$(awk '{print $2}' $f); [ -n "$h" ] && hold_kill $h; rm -f $f;; esac; done; exit 0; }
trap cleanup TERM INT
# stale state from a crashed daemon: cancel recorded holds, mark the items rc 125
for f in $ST/*; do [ -e "$f" ] || continue; case $f in *.retry|*.done|*.since|*.warn|*.wait|*.starved) continue;; esac
  b=$(basename $f); h=$(awk '{print $2}' $f); [ -n "$h" ] && hold_kill $h; echo 125 > $Q/$b.rc; echo "[aac7q] daemon restarted while item was active" >> $Q/$b.log; say "$b: stale state, hold $h cancelled, rc 125"; rm -f $f; done
budget_refresh; say "daemon start pid $$ STUB=$STUB budget $BUDGET${BUDGET_CAP:+ (cap BUDGET=$BUDGET_CAP)}"
while :; do
  date +%s > $AQ/heartbeat
  RUNPIDS=$(for p in $RUNPIDS; do kill -0 $p 2>/dev/null && echo $p; done | tr '\n' ' ')
  if [ -e $AQ/STOP ]; then [ -z "$RUNPIDS" ] && { say "STOP: no running items, exit"; break; }; sleep $POLL; continue; fi
  budget_refresh; used=$(used_nodes); free=$((BUDGET - used)); idle=$(part_idle); [ "$free" -gt "$idle" ] && free=$idle; t=$(now); cand=""; waiting=0; started=0
  for f in $Q/*.sh; do [ -e "$f" ] || continue; b=$(basename $f .sh)
    [ -e $Q/$b.rc ] || [ -e $ST/$b ] && continue
    ap=$(hdr $f APPROVED); [ -n "$ap" ] || { [ -e $ST/$b.warn ] || { say "$b: no APPROVED header, skipped"; : > $ST/$b.warn; }; continue; }
    n=$(hdr $f NODES); est=$(hdr $f EST_MIN)
    case $n in ''|*[!0-9]*|0) n=;; *) [ "$n" -le "$BUDGET" ] || n=;; esac; case $est in ''|*[!0-9]*) est=;; esac
    [ -n "$n" ] && [ -n "$est" ] || { [ -e $ST/$b.warn ] || { say "$b: bad NODES/EST_MIN, skipped"; : > $ST/$b.warn; }; continue; }
    nb=$(hdr $f NOT_BEFORE); na=$(hdr $f NOT_AFTER); [ -n "$nb" ] && [ "$t" -lt "$nb" ] && continue
    rt=$(cut -d' ' -f2 $ST/$b.retry 2>/dev/null); [ -n "$rt" ] && [ "$t" -lt "$rt" ] && continue
    rc_n=$(cut -d' ' -f3 $ST/$b.retry 2>/dev/null); [ "${rc_n:-0}" -ge $MAX_RETRY ] && { echo 126 > $Q/$b.rc; say "$b: gave up after $MAX_RETRY pending-timeouts, rc 126"; continue; }
    [ -n "$na" ] && [ $((t + est * 60)) -gt "$na" ] && { [ -e $ST/$b.warn ] || { say "$b: would end after NOT_AFTER, skipped"; : > $ST/$b.warn; }; continue; }
    cand="$cand $n:$b"; waiting=$((waiting + 1)); done
  blk=0; for f in $ST/*; do [ -e "$f" ] || continue; case $f in *.retry|*.done|*.since|*.warn|*.wait|*.starved) continue;; esac
    [ "$(hdr $Q/$(basename $f).sh EXCLUSIVE)" = 1 ] && blk=1; done      # an EXCLUSIVE item is active: nothing else starts
  [ $blk = 1 ] && cand=""
  # largest-first (ties by name); an item that does not fit is skipped and later ones that fit start (backfill) until a skipped item
  # has waited AQ_STARVE_MIN: then nothing starts behind it until it does.  GROUP=<g> items start only when all members fit at once.
  sorted=$(echo $cand | tr ' ' '\n' | sort -t: -k1,1nr -k2,2)
  for c in $sorted; do n=${c%%:*}; b=${c#*:}; f=$Q/$b.sh; ex=$(hdr $f EXCLUSIVE); g=$(hdr $f GROUP)
    [ -e $ST/$b ] && continue                                        # started earlier in this cycle (as a group member)
    if [ "$ex" = 1 ]; then
      if [ "$used" = 0 ] && [ $started = 0 ] && [ -z "$RUNPIDS" ]; then : ; else
        [ -e $ST/$b.since ] || echo $t > $ST/$b.since
        if [ $((t - $(cat $ST/$b.since))) -ge $EXCL_PATIENCE ]; then DRAIN=$b; fi; continue; fi
    fi
    [ -n "${DRAIN:-}" ] && [ "${DRAIN:-}" != "$b" ] && continue      # an EXCLUSIVE item waited past patience: drain
    need=$n; members="$n:$b"
    if [ -n "$g" ]; then need=0; members=""; for c2 in $sorted; do b2=${c2#*:}; [ -e $ST/$b2 ] && continue
        [ "$(hdr $Q/$b2.sh GROUP)" = "$g" ] && { need=$((need + ${c2%%:*})); members="$members $c2"; }; done; fi
    if [ "$need" -gt "$free" ]; then
      [ -e $ST/$b.wait ] || echo $t > $ST/$b.wait
      if [ $((t - $(cat $ST/$b.wait))) -ge $STARVE_S ]; then
        [ -e $ST/$b.starved ] || { say "$b: waited $(( (t - $(cat $ST/$b.wait)) / 60 )) min for $need nodes: backfill blocked until it starts"; : > $ST/$b.starved; }
        break; fi
      continue; fi
    for c2 in $members; do n2=${c2%%:*}; b2=${c2#*:}
      run_item $Q/$b2.sh & RUNPIDS="$RUNPIDS $!"; echo "starting $(now)" > $ST/$b2; free=$((free - n2)); used=$((used + n2)); started=$((started + 1))
      rm -f $ST/$b2.since $ST/$b2.wait $ST/$b2.starved; done
    [ -n "$g" ] && say "group $g: started $(echo $members | sed "s/[0-9]*://g")"
    [ "$ex" = 1 ] && break; done
  DRAIN=
  # idle report: nodes free, nothing started
  if [ $free -gt 0 ] && [ $started = 0 ]; then
    [ -e $AQ/IDLE ] && since=$(grep -m1 '^since_epoch=' $AQ/IDLE | cut -d= -f2) || since=$t
    printf 'since=%s\nsince_epoch=%s\nlast=%s\nfree_nodes=%s\nused_nodes=%s\nrunnable_waiting=%s\nnote=%s\n' "$(TZ=America/New_York date -d @$since '+%m-%d %H:%M:%S ET')" "$since" "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET')" "$free" "$used" "$waiting" \
      "$([ $waiting = 0 ] && echo 'queue has nothing startable (empty, unapproved, or windows closed)' || echo 'items wait (do not fit / EXCLUSIVE / patience)')" > $AQ/IDLE
  else rm -f $AQ/IDLE; fi
  sleep $POLL
done

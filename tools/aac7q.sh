#!/bin/bash
# aac7q.sh - standing work-queue runner for the aac7 login node (keeps the 6-node QOS allowance busy with approved work).
#   aac7q.sh start|run|stop|status      (start = setsid nohup of "run"; stop = touch STOP: no new items, exit when running ones end)
# Queue dir $AQ/queue/*.sh (AQ=~/aac7q).  Item headers (comment lines): OWNER, NODES (1-6), EST_MIN, APPROVED (who/date; items without
# it never run), optional NOT_BEFORE / NOT_AFTER (epoch), EXCLUSIVE=1 (only when none of our jobs run, and nothing else starts meanwhile),
# ALLOW_SLOW=1 (do not exclude aac7-n01).  See tools/aac7q_item_template.sh.
# Per item: own sbatch hold, item run with SLURM_JOB_ID=<hold> (steps must use --overlap --mem=0), hold scancelled afterwards, <item>.rc/.log.
# STUB=1: no Slurm; holds are files under $AQ/stub (a file stub/ext_* of N nodes simulates a foreign job of ours).
AQ=${AQ:-$HOME/aac7q}; Q=$AQ/queue; ST=$AQ/state; STUB=${STUB:-0}; BUDGET=${BUDGET:-6}
POLL=${POLL:-20}; EST_UNIT=${EST_UNIT:-60}          # seconds per EST_MIN (stub tests shrink it)
PEND_MAX=${PEND_MAX:-600}; RETRY_DELAY=${RETRY_DELAY:-300}; MAX_RETRY=${MAX_RETRY:-5}; EXCL_PATIENCE=${EXCL_PATIENCE:-3600}
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
    case $f in *.retry|*.done|*.since|*.warn) ;; *) h=$(awk '{print $2}' $f); [ -n "$h" ] && hold_kill $h; rm -f $f;; esac; done; exit 0; }
trap cleanup TERM INT
# stale state from a crashed daemon: cancel recorded holds, mark the items rc 125
for f in $ST/*; do [ -e "$f" ] || continue; case $f in *.retry|*.done|*.since|*.warn) continue;; esac
  b=$(basename $f); h=$(awk '{print $2}' $f); [ -n "$h" ] && hold_kill $h; echo 125 > $Q/$b.rc; echo "[aac7q] daemon restarted while item was active" >> $Q/$b.log; say "$b: stale state, hold $h cancelled, rc 125"; rm -f $f; done
say "daemon start pid $$ STUB=$STUB budget $BUDGET"
while :; do
  date +%s > $AQ/heartbeat
  RUNPIDS=$(for p in $RUNPIDS; do kill -0 $p 2>/dev/null && echo $p; done | tr '\n' ' ')
  if [ -e $AQ/STOP ]; then [ -z "$RUNPIDS" ] && { say "STOP: no running items, exit"; break; }; sleep $POLL; continue; fi
  used=$(used_nodes); free=$((BUDGET - used)); t=$(now); cand=""; waiting=0; started=0
  for f in $Q/*.sh; do [ -e "$f" ] || continue; b=$(basename $f .sh)
    [ -e $Q/$b.rc ] || [ -e $ST/$b ] && continue
    ap=$(hdr $f APPROVED); [ -n "$ap" ] || { [ -e $ST/$b.warn ] || { say "$b: no APPROVED header, skipped"; : > $ST/$b.warn; }; continue; }
    n=$(hdr $f NODES); est=$(hdr $f EST_MIN)
    case $n in [1-6]) ;; *) n=; esac; case $est in ''|*[!0-9]*) est=;; esac
    [ -n "$n" ] && [ -n "$est" ] || { [ -e $ST/$b.warn ] || { say "$b: bad NODES/EST_MIN, skipped"; : > $ST/$b.warn; }; continue; }
    nb=$(hdr $f NOT_BEFORE); na=$(hdr $f NOT_AFTER); [ -n "$nb" ] && [ "$t" -lt "$nb" ] && continue
    rt=$(cut -d' ' -f2 $ST/$b.retry 2>/dev/null); [ -n "$rt" ] && [ "$t" -lt "$rt" ] && continue
    rc_n=$(cut -d' ' -f3 $ST/$b.retry 2>/dev/null); [ "${rc_n:-0}" -ge $MAX_RETRY ] && { echo 126 > $Q/$b.rc; say "$b: gave up after $MAX_RETRY pending-timeouts, rc 126"; continue; }
    [ -n "$na" ] && [ $((t + est * 60)) -gt "$na" ] && { [ -e $ST/$b.warn ] || { say "$b: would end after NOT_AFTER, skipped"; : > $ST/$b.warn; }; continue; }
    cand="$cand $n:$b"; waiting=$((waiting + 1)); done
  blk=0; for f in $ST/*; do [ -e "$f" ] || continue; case $f in *.retry|*.done|*.since|*.warn) continue;; esac
    [ "$(hdr $Q/$(basename $f).sh EXCLUSIVE)" = 1 ] && blk=1; done      # an EXCLUSIVE item is active: nothing else starts
  [ $blk = 1 ] && cand=""
  # largest-first (ties by name)
  for c in $(echo $cand | tr ' ' '\n' | sort -t: -k1,1nr -k2,2); do n=${c%%:*}; b=${c#*:}; f=$Q/$b.sh; ex=$(hdr $f EXCLUSIVE)
    if [ "$ex" = 1 ]; then
      if [ "$used" = 0 ] && [ $started = 0 ] && [ -z "$RUNPIDS" ]; then : ; else
        [ -e $ST/$b.since ] || echo $t > $ST/$b.since
        if [ $((t - $(cat $ST/$b.since))) -ge $EXCL_PATIENCE ]; then DRAIN=$b; fi; continue; fi
    fi
    [ -n "${DRAIN:-}" ] && [ "${DRAIN:-}" != "$b" ] && continue      # an EXCLUSIVE item waited past patience: drain
    [ "$n" -le "$free" ] || continue
    run_item $f & RUNPIDS="$RUNPIDS $!"; echo "starting $(now)" > $ST/$b; free=$((free - n)); used=$((used + n)); started=$((started + 1)); rm -f $ST/$b.since
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

#!/bin/bash
# S47 (batch 2026-10-11, track T2): Job B driver on the aac7 login node.  Phase 1: paired ABBA on the 4-node hold, 6.441e10 digits per node,
#   (i) MN_T_CHUNK_MB A=1024 / B=2048, (ii) X2 A=off / B=on, interleaved round by round (round r odd: A then B, even: B then A),
#   >= 6 rounds each, stop rule per comparison (s47_stats.py --rule), time budget ABBA_BUDGET_S.  No digit writes.  The reference files are evicted from the
#   page cache and MemAvailable is checked on all 4 nodes before every run.
# Phase 2: the shared queue ~/batch1011/jobB/queue/*.sh (the private batch slot rules): serial, filename order, EST_MIN/OWNER headers, skip if now+EST crosses W (before the
#   handoff) or D, timeout 1.5*EST+5 min, <item>.rc / <item>.log next to the item.  At W: scancel the hold, submit B2 (-J b1011w --dependency=singleton), wait, go on until D.
# End: stats + marker $OUT/S47_DONE, scancel own holds (by id).  DRY=1: stub runs and a stub queue, no Slurm.
#   HOLD=<jobid> [OUT=..] [DRY=1] bash s47_driver.sh   (started with setsid nohup bash -lc ... < /dev/null)
HD=$(cd "$(dirname "$0")" && pwd); WT=${WT:-$HOME/ntt-wt/s47}; E=$WT/ecalc; OUT=${OUT:-$HOME/s47}; EC=$OUT/ecalc_s47
DRY=${DRY:-0}; DPN=${DPN:-64410000000}; NN=4; RUN_S=${RUN_S:-300}
W=${W:-1791712586}; D=${D:-1791723386}; S=${S:-1791725186}
Q=${Q:-$HOME/batch1011/jobB/queue}; ABBA_BUDGET_S=${ABBA_BUDGET_S:-9000}; MEMGB=${MEMGB:-428}; MAXBAD=3
EXCL=aac7-n01,aac7-n61; AAC7_PARTITION=${AAC7_PARTITION:-192C4G1H_MI300A_RHEL9_A1}
mkdir -p $OUT/log; rm -f $OUT/S47_DONE $OUT/S47_ABBA_DONE $OUT/ALERT; : > $OUT/ab.tsv; : > $OUT/queue.tsv
source $WT/tools/rundriver.sh; rd_init $OUT S47_DONE; RD_WATCHDOG_PERIOD=${RD_WATCHDOG_PERIOD:-20}
alert() { echo "$(TZ=America/New_York date) $*" >> $OUT/ALERT; }
HOLDS=""   # every hold this driver owns (ids), scancelled at the end
cleanup() { digest; if [ "$DRY" != 1 ]; then for h in $HOLDS; do scancel $h 2>/dev/null; done; else rd_say "DRY: would scancel $HOLDS"; fi; rd_finish; }
fail() { alert "$*"; RD_VERDICT="FAILED: $*"; exit 0; }
et() { TZ=America/New_York date -d @$1 '+%m-%d %H:%M'; }
now() { date +%s; }
J=${HOLD:?set HOLD=<4-node hold id>}; HOLDS=$J; [ "$DRY" = 1 ] || export SLURM_JOB_ID=$J
cd $E || fail "no $E"
if [ "$DRY" != 1 ]; then source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"; fi
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ $E/e16_headline.sh); [ -n "$LINE" ] || fail "no LINE in e16_headline.sh"
case "$LINE" in *MN_T_CHUNK_MB=2048*COMM_LAYER_INTER2=1*COMM_LAYER_VSLOT_POOL=1*) ;; *) fail "unexpected LINE: $LINE";; esac
if [ "$DRY" != 1 ]; then cp $E/ecalc $EC.tmp && mv $EC.tmp $EC || fail "copy of the binary"; ldd $EC | grep -q libsma || fail "binary without SHMEM"; else : > $EC; fi
[ "$DRY" = 1 ] || [ -x $WT/tools/unpack_digits ] || true
export LINE
declare -A AW=( [i_A]="MN_T_CHUNK_MB=1024" [i_B]="MN_T_CHUNK_MB=2048" [ii_A]="COMM_LAYER_INTER2=0 COMM_LAYER_VSLOT_POOL=0" [ii_B]="COMM_LAYER_INTER2=1 COMM_LAYER_VSLOT_POOL=1" )
rd_say "S47 driver: hold $J DRY=$DRY W=$(et $W) D=$(et $D) S=$(et $S) Eastern; LINE | $LINE ; arms: $(for k in i_A i_B ii_A ii_B; do echo -n "$k=[${AW[$k]}] "; done); binary $EC md5 $(md5sum $EC 2>/dev/null | cut -c1-12) rev $(git -C $WT rev-parse --short HEAD)"
digest() { python3 $HD/s47_stats.py $OUT/ab.tsv > $OUT/stats.txt 2>&1
  { echo "S47 digest $(TZ=America/New_York date), hold $J"; echo; cat $OUT/stats.txt; echo; echo "queue (item owner est_min start end rc):"; cat $OUT/queue.tsv; echo
    echo "runs (tag round arm rc wall verify ecalc_total end):"; cut -c1-200 $OUT/ab.tsv; echo; echo "ALERT:"; cut -c1-200 $OUT/ALERT 2>/dev/null | head -60; } > $OUT/results.txt; }
trap cleanup EXIT
hold_ok() { [ "$DRY" = 1 ] && return 0; [ "$(squeue -j $1 -h -o %T 2>/dev/null)" = RUNNING ]; }
# prep <hold>: evict the reference files from every node's page cache, report MemAvailable; wait (<= 300 s) until each node has >= MEMGB GB. 0 = ok
prep() { [ "$DRY" = 1 ] && return 0; local t0=$SECONDS out v bad
  while :; do
    out=$(timeout 150 srun --jobid=$J -N$NN -n$NN --ntasks-per-node=1 --overlap bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done; echo "$(hostname -s) n=$(pgrep -u $USER -x ecalc | wc -l) availGB=$(awk "/MemAvailable/{printf \"%d\", \$2*1024/1e9}" /proc/meminfo)"' 2>&1 < /dev/null)
    rd_say "prep: $(echo "$out" | tr '\n' ';' | cut -c1-300)"
    bad=0; echo "$out" | grep -q "n=[1-9]" && bad=2
    [ $(echo "$out" | grep -c availGB=) = $NN ] || bad=3
    for v in $(echo "$out" | grep -o 'availGB=[0-9]*' | cut -d= -f2); do [ "$v" -ge $MEMGB ] || bad=1; done
    [ $bad = 0 ] && return 0
    [ $bad = 2 ] && { alert "prep: leftover ecalc process on a node of $J"; return 2; }
    [ $((SECONDS - t0)) -ge 300 ] && { rd_say "prep: giving up the wait (bad=$bad), going on"; return 0; }
    sleep 30
  done; }
# run_one <label> <expected_s> <arm words>  (sets RC WALL V TOT PL)
run_one() { local lab=$1 exp=$2 aw=$3 p=24$((RANDOM % 90 + 10))0
  if [ "$DRY" = 1 ]; then local b=0; case "$aw" in *=2048*|*INTER2=1*) b=1;; esac; local ef=0; case "$lab" in i_*) ef=${DRY_EFFECT:-12};; esac
    RD_LAST_WALL=$(( 220 + RANDOM % 9 - 4 - b * ef )); RD_LAST_RC=0; sleep 1
    { echo "all $NN nodes: VERIFY OK"; echo "total   $((RD_LAST_WALL - 6)).10 s   (bs 1 + dm 2)"; } > $OUT/log/$lab.log; RC=0
  else rd_run $lab $exp env MNRUN_NODES=$NN MNRUN_LABEL=1 COMM_PORT=$p ./mnrun.sh $NN env $LINE ECALC_SEGV_TRACE=1 $aw $EC $((NN * DPN)) < /dev/null; RC=$?; fi
  PL=$OUT/log/$lab.plain; sed -E 's/^ *[0-9]+: //' $OUT/log/$lab.log > $PL
  WALL=$RD_LAST_WALL; V=$(rd_verify $PL) || true; TOT=$(grep -a -m1 '^total' $PL | awk '{print ($2 ~ /^[0-9.]+$/) ? $2 : "NA"}'); [ -n "$TOT" ] || TOT=NA; }
row() { printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$RC" "$WALL" "$V" "$TOT" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" >> $OUT/ab.tsv; }
ab_run() { local tag=$1 r=$2 arm=$3 lab=${1}_r${2}_$3
  hold_ok $J || { alert "hold $J not running"; return 9; }
  [ $(( $(now) + 2 * RUN_S )) -lt $W ] || { rd_say "$lab: would reach W, stop"; return 8; }
  prep || { sleep 60; prep; } || return 7
  run_one $lab $RUN_S "${AW[${tag}_$arm]}"; row $tag $r $arm
  rd_say "$lab: rc $RC wall ${WALL}s $V | $(grep -a -m1 '^total' $PL | cut -c1-110)"
  [ $RC = 0 ] && [ "$V" = "VERIFY OK" ] || alert "$lab: rc $RC $V (log $OUT/log/$lab.log)"$'\n'"$(tail -12 $OUT/log/$lab.log | cut -c1-200)"
  [ $RC = 0 ] && [ "$V" = "VERIFY OK" ]; }
# ---------------- phase 1: ABBA
T1=$(now); r=0; bad=0; DONE_i=0; DONE_ii=0; ATT_i=0; ATT_ii=0; STOPALL=0
rd_say "phase 1 ABBA: budget ${ABBA_BUDGET_S}s from $(et $T1), rounds >= ${S47_MINR:-6}, cap ${S47_CAP:-10}"
while [ $STOPALL = 0 ] && { [ $DONE_i = 0 ] || [ $DONE_ii = 0 ]; }; do
  for tag in i ii; do
    eval "dn=\$DONE_$tag"; [ $dn = 1 ] && continue
    [ $(( $(now) - T1 + 2 * RUN_S )) -le $ABBA_BUDGET_S ] || { rd_say "ABBA time budget reached before a $tag round"; STOPALL=1; break; }
    eval "ATT_$tag=\$((ATT_$tag + 1))"; eval "k=\$ATT_$tag"; [ $k -gt $(( ${S47_CAP:-10} + 2 )) ] && { DONE_i=1; DONE_ii=1; STOPALL=1; break; }
    [ $((k % 2)) = 1 ] && order="A B" || order="B A"
    for arm in $order; do ab_run $tag $k $arm; rc=$?
      if [ $rc = 0 ]; then bad=0; else bad=$((bad + 1)); [ $rc -ge 7 ] && { STOPALL=1; break 2; }; fi
      [ $bad -ge $MAXBAD ] && { alert "$MAXBAD consecutive bad runs: ABBA stops"; STOPALL=1; break 2; }
    done
    out=$(python3 $HD/s47_stats.py $OUT/ab.tsv --rule $tag); src=$?; rd_say "rule $tag after attempt $k: $out"; [ $src = 0 ] && eval "DONE_$tag=1"
  done
done
digest; echo "ABBA finished $(TZ=America/New_York date): $(grep -E 'pairs,|no complete|1 pair' $OUT/stats.txt | cut -c1-230 | tr '\n' ';')" > $OUT/S47_ABBA_DONE
rd_say "phase 1 done: $(grep -E 'pairs,' $OUT/stats.txt | cut -c1-200 | tr '\n' ';')"
# ---------------- phase 2: queue
HANDOFF=0; skipped=""
kill_tree() { local p=$1 c; for c in $(pgrep -P $p 2>/dev/null); do kill_tree $c; done; kill -TERM $p 2>/dev/null; }
run_item() { local f=$1 est=$2 own=$3 base=${1%.sh} to pid t0=$(now) w=0
  to=$(( est * 90 + 300 )); rd_say "queue: start $(basename $f) owner $own est ${est} min, timeout ${to}s, hold $J"
  ( cd $(dirname $f) && SLURM_JOB_ID=$J exec bash $f ) > $base.log 2>&1 < /dev/null & pid=$!
  while kill -0 $pid 2>/dev/null; do sleep ${QPOLL:-5}; w=$((w + ${QPOLL:-5})); [ $w -ge $to ] && break; done
  if kill -0 $pid 2>/dev/null; then rd_say "queue: $(basename $f) TIMEOUT ${to}s: killing PID tree of $pid"; kill_tree $pid; sleep 20; kill -KILL $pid 2>/dev/null; echo "[driver] timeout ${to}s" >> $base.log; wait $pid 2>/dev/null; RCI=124
  else wait $pid; RCI=$?; fi
  echo $RCI > $base.rc; echo $RCI > $f.rc
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$(basename $f)" "$own" "$est" "$(TZ=America/New_York date -d @$t0 +%H:%M:%S)" "$(TZ=America/New_York date +%H:%M:%S)" "$RCI" >> $OUT/queue.tsv
  rd_say "queue: $(basename $f) rc $RCI after $(( $(now) - t0 )) s"; [ "$RCI" = 0 ] || alert "queue item $(basename $f) rc $RCI"; }
handoff() { # at/after W: release the hold, wait for B2
  rd_say "handoff at $(et $(now)): scancel $J, submit B2"
  if [ "$DRY" = 1 ]; then rd_say "DRY: scancel $J; sbatch -N4 -J b1011w --dependency=singleton"; HOLDS="$HOLDS 9999"; J=9999; HANDOFF=1; return 0; fi
  scancel $J; HOLDS=${HOLDS/$J/}; sleep 5
  local m=$(( (S - $(now)) / 60 )); local j2
  j2=$(sbatch -p $AAC7_PARTITION -N 4 --exclusive -c 192 --gpus-per-node=4 -x $EXCL -t $m -J b1011w --dependency=singleton --parsable --wrap "sleep $((m * 60))") || { alert "B2 submit failed"; return 1; }
  HOLDS="$HOLDS $j2"; rd_say "B2 = $j2 (-t ${m} min)"
  while [ "$(squeue -j $j2 -h -o %T 2>/dev/null)" != RUNNING ]; do
    [ -n "$(squeue -j $j2 -h 2>/dev/null)" ] || { alert "B2 $j2 vanished"; return 1; }
    [ $(now) -ge $((D - 600)) ] && { rd_say "B2 not running by D-10 min"; return 1; }
    sleep 60; done
  J=$j2; export SLURM_JOB_ID=$J; HANDOFF=1; rd_say "B2 running: $(squeue -j $J -h -o %N)"; return 0; }
rd_say "phase 2 queue: $Q until D=$(et $D); W=$(et $W)"
SEEN=""
while [ $(now) -lt $D ]; do
  if [ $HANDOFF = 0 ] && [ $(now) -ge $W ]; then handoff || break; continue; fi
  ran=0
  for f in $(ls $Q/*.sh 2>/dev/null | sort); do
    [ -e ${f%.sh}.rc ] || [ -e $f.rc ] && continue
    est=$(grep -m1 '^# EST_MIN=' $f | cut -d= -f2 | tr -dc 0-9); own=$(grep -m1 '^# OWNER=' $f | cut -d= -f2 | tr -d ' ')
    [ -n "$est" ] || { case " $SEEN " in *" $f "*) ;; *) SEEN="$SEEN $f"; alert "queue item $(basename $f) has no EST_MIN: skipped";; esac; continue; }
    fin=$(( $(now) + est * 60 ))
    [ $fin -le $D ] || continue
    [ $HANDOFF = 1 ] || [ $fin -le $W ] || continue
    hold_ok $J || { alert "hold $J not running at queue item"; break 2; }
    run_item $f $est "$own"; ran=1; break
  done
  [ $ran = 1 ] || sleep ${QIDLE:-30}
done
rd_say "queue phase over at $(et $(now))"
RD_VERDICT="SUCCESS: S47 done: $(grep -E 'pairs,' $OUT/stats.txt | cut -c1-160 | tr '\n' ';') queue items run: $(wc -l < $OUT/queue.tsv) (digest $OUT/results.txt)"

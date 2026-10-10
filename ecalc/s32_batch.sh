#!/bin/bash
# S32: the two open questions of results/S30.md.  Q1: the rc 139 at tl ~31 s after the init-time VMM background mapping (hypothesis: vmm_bg_map's hipStreamDestroy /
#   hipSetDevice tail racing level 1) -> ECALC_VMM_SAFE (s32 code, off by default, digits identical).  Q2: node 1 (host x9000c1s0b1n0) stalls ~20 s in recip_mn_body's first
#   stage: host or rank? -> NEWTON_MN_RECIP_TS=2 (db_init / first pool get / db_from_bi + host name) and a permuted rank-to-host map.
#   ARMED by its author's session: it only WAITS until ~/s31/S31_DONE exists (poll 300 s).  ALL runs: ECALC_SEGV_TRACE=1 and srun --label (MNRUN_LABEL=1; rank-prefixed log, plus
#   log/<label>.plain without the prefix, which every grep below uses); per-host MemAvailable + Cached before each run (meminfo.txt).
#   (0) WAIT  for ~/s31/S31_DONE, then hold 12331 or 12389 (RUNNING, 10 nodes, >= 2 h left).  BASE = S31's base (~/s31/summary.txt line 1) PLUS COMM_LAYER_VSLOT_POOL=1
#             COMM_LAYER_INTER2=1 ONLY IF ~/s31/x2_stats.txt's last block says "DECISION (all rounds): STOP significant" with mean B-A < 0 (p < 0.05); runtime check, printed.
#   (a) gates 2 nodes x 1e9 digits, digits identical to ~/ref/e_1000000000.txt: gate2_safe1 (ECALC_VMM_SAFE=1), gate2_safe2 (=2, joins).  A failed gate ends the batch.
#   (b) host-vs-rank  10 nodes x 6.441e10, BASE + NEWTON_MN_RECIP_TS=2: b_base1, b_base2 (nodelist unchanged: sorted, the suspect host is rank 1), b_perm1, b_perm2 (MNRUN_NODELIST
#             permuted + MNRUN_ARBITRARY=1: the suspect host is rank 7; the 'recip ts2 node N host H' lines show whether the permutation took effect).  hostrank.txt = verdict.
#   (c) if (b) says the stall follows the host: one run c_evict with the files of that host evicted first (sudo -n drop_caches if allowed, else dd iflag=nocache over our trees).
#   (d) soak  PARALLEL (2026-10-08): the 10 nodes as 5 disjoint 2-node jobs (s32_pairsoak.sh workers p0..p4) at 6.441e10 digits per node, each alternating BASE (A) and BASE +
#             ECALC_VMM_SAFE=$SOAK_SAFE (B), until 30 min before the hold ends; continues on the successor hold $H2 when $H1 was used; rc 139 counted per arm and node count; every
#             bad run raises an ALERT with the segv trace.  Outputs in ~/s32: summary.txt gates.txt hostrank.txt pairs.tsv soak_stats.txt meminfo.txt w_p*/ (worker logs)
#   results.txt (digest) res/ log/ ALERT S32_DONE.  Stop early: touch ~/s32/STOP.  Never cancels anything; never touches other jobs.  Fire-and-forget.
BUILD_REV=383327a
H1=12331; H2=12389
HD=$(cd "$(dirname "$0")" && pwd); WT=$HOME/ntt-wt/s32; E=$WT/ecalc; OUT=$HOME/s32; S31DONE=$HOME/s31/S31_DONE
MARGIN=1800; S=64410000000; RUN_S=450; STALL_HOST=${STALL_HOST:-x9000c1s0b1n0}; SOAK_SAFE=${SOAK_SAFE:-1}
mkdir -p $OUT/log $OUT/res
rm -f $OUT/S32_DONE $OUT/ALERT $OUT/STOP $OUT/soak.tsv $OUT/pairs.tsv
source $WT/tools/rundriver.sh; rd_init $OUT S32_DONE
source $HD/s31_lib.sh
cd $E || fail "cd $E"
source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"
check_binary 'ECALC_VMM_SAFE NEWTON_MN_RECIP_TS ECALC_SEGV_TRACE' ""
grep -q MNRUN_ARBITRARY $E/mnrun.sh || fail "mnrun.sh lacks MNRUN_ARBITRARY"

# --- (0) wait for S31, then pick the hold
hold_ok() { [ "$(squeue -j $1 -h -o %T 2>/dev/null)" = RUNNING ] || return 1
  [ "$(scontrol show hostnames "$(squeue -j $1 -h -o %N)" 2>/dev/null | wc -l)" = 10 ] || return 1
  [ "$(left_s $1)" -ge 7200 ]; }
rd_say "waiting for $S31DONE (poll 300 s); holds $H1 / $H2"
while :; do
  [ -e $OUT/STOP ] && fail "STOP file while waiting for S31"
  if [ -e $S31DONE ]; then
    if hold_ok $H1; then J=$H1; break; elif hold_ok $H2; then J=$H2; break; fi
    pend=$(squeue -j $H1,$H2 -h -o "%T" 2>/dev/null | grep -c 'PENDING\|RUNNING')
    [ "$pend" -ge 1 ] || fail "S31 is done but neither hold $H1 nor $H2 exists (RUNNING/PENDING)"
  fi
  sleep 300
done
rd_say "S31 done ($(head -c 150 $S31DONE 2>/dev/null)); using hold $J"
export SLURM_JOB_ID=$J
NODES=$(scontrol show hostnames "$(squeue -j $J -h -o %N)" | tr '\n' ' '); NN=$(echo $NODES | wc -w)
[ "$NN" = 10 ] || fail "hold $J has $NN nodes, need 10"
rd_say "hold $J nodes: $NODES ($(left_s $J) s left)"
rd_health $J $NODES || fail "unhealthy at start (leftover ecalc / t_* process or no meminfo)"
rd_disk $HOME 20 || fail "home nearly full"
MINAV=999999999
for nd in $NODES; do av=$(timeout 60 srun --jobid=$J -N1 -w $nd --overlap awk '/MemAvailable/{print $2}' /proc/meminfo 2>/dev/null); [ "${av:-0}" -lt $MINAV ] && MINAV=${av:-0}; done
[ "$MINAV" -ge 400000000 ] || fail "smallest MemAvailable $MINAV kB < 400 GB"
rd_say "smallest MemAvailable over the 10 nodes: $MINAV kB"

# --- the base line: S31's BASE (its summary.txt line 1), plus the X2 switches only if S31's x2_stats.txt says B faster at p < 0.05
S31SUM=$HOME/s31/summary.txt; SW_X2="COMM_LAYER_VSLOT_POOL=1 COMM_LAYER_INTER2=1"
if [ -s $S31SUM ] && head -1 $S31SUM | grep -q '^BASE used: .* | '; then
  BASE=$(head -1 $S31SUM | sed 's/^BASE used: .* | //'); BASE_NOTE="S31 base ($(head -1 $S31SUM | sed 's/^BASE used: \(.*\) | .*/\1/' | cut -c1-150))"
else BASE_NOTE="S31 summary has no BASE line: s31_lib.sh BASE"; fi
X2S=$HOME/s31/x2_stats.txt
if [ -s $X2S ]; then
  lastblk=$(awk '/^== /{b=""} {b=b"\n"$0} END{print b}' $X2S)
  dec=$(echo "$lastblk" | grep -a '^DECISION (all rounds)' | tail -1)
  mba=$(echo "$lastblk" | grep -a '^ALL rounds' | tail -1 | sed -n 's/.*mean B-A \([-+][0-9.]*\) s.*/\1/p')
  if echo "$dec" | grep -q 'STOP significant' && [ -n "$mba" ] && awk -v m="$mba" 'BEGIN{exit !(m < 0)}'; then
    case "$BASE" in *COMM_LAYER_VSLOT_POOL=1*) ;; *) BASE="$BASE $SW_X2";; esac
    BASE_NOTE="$BASE_NOTE + X2 switches (x2_stats: $dec, mean B-A $mba s)"
  else BASE_NOTE="$BASE_NOTE; X2 NOT added (x2_stats: '$dec', mean B-A '$mba')"; fi
else BASE_NOTE="$BASE_NOTE; X2 NOT added (no or empty $X2S)"; fi
COM="ECALC_SEGV_TRACE=1"                      # in every run
rd_say "BASE used: $BASE_NOTE"; echo "BASE used: $BASE_NOTE | $BASE" > $OUT/summary.txt

total=0; bad=0; consec=0; : > $OUT/gates.txt; : > $OUT/hostrank.txt; : > $OUT/meminfo.txt
evict() { timeout 120 srun --jobid=$J -N10 -n10 --overlap bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done; rm -f $HOME/s32/gate*.txt*' >/dev/null 2>&1; sleep 5; }
memlog() { { echo "## $1 $(TZ=America/New_York date +%H:%M:%S)"; timeout 90 srun --jobid=$J -N10 -n10 --overlap --label bash -c 'echo "$(hostname) $(awk "/^MemAvailable|^Cached/{printf \"%s%s \",\$1,\$2}" /proc/meminfo)"' 2>&1 | sort -n | cut -c1-120; } >> $OUT/meminfo.txt; }
ensure_time() { [ -e $OUT/STOP ] && { rd_say "STOP file: stopping"; return 1; }
  local l; l=$(left_s $J); [ "$l" -ge $(( $1 + 600 + MARGIN )) ] || { rd_say "hold $J has $l s left (< $1 + 600 + $MARGIN): no more runs"; return 1; }; return 0; }
plain() { sed -E 's/^ *[0-9]+: //' $1 > ${1%.log}.plain 2>/dev/null; echo ${1%.log}.plain; }
extract() { grep -aE '^total|wait-stats|^tl |VERIFY|peak|rank 0|recip ts|SEGV|mapper|background mapping' $2 | cut -c1-400 | head -150 > $OUT/res/$1.txt; }
segvtrace() { grep -a -m1 -A28 'ecalc: SEGV \[' $1 | cut -c1-200; }
# run_one <label> <n> <expected_s> <per-node digits> <env words...>: RUN_RC RUN_V RUN_T RUN_WALL RUN_PLAIN; returns 0 if clean.  MNRUN_EXTRA="VAR=val ..." goes to mnrun.sh itself.
run_one() { local l=$1 n=$2 x=$3 d=$4; shift 4; total=$((total+1))
  evict; memlog $l
  rd_run $l $x env MNRUN_NODES=$n MNRUN_LABEL=1 $MNRUN_EXTRA ./mnrun.sh $n env $BASE $COM "$@" ./ecalc $((n * d)); RUN_RC=$?
  RUN_PLAIN=$(plain $RD_LAST_LOG)
  RUN_V=$(rd_verify $RUN_PLAIN); RUN_T=$(grep -a -m1 '^total' $RUN_PLAIN | cut -c1-140); RUN_WALL=$RD_LAST_WALL
  extract $l $RUN_PLAIN
  rd_say "$l: n=$n rc $RUN_RC wall ${RUN_WALL}s $RUN_V | $RUN_T"
  if [ $RUN_RC != 0 ] || [ "$RUN_V" != "VERIFY OK" ]; then
    bad=$((bad+1)); consec=$((consec+1))
    alert "$l: rc $RUN_RC $RUN_V (log $RD_LAST_LOG)"$'\n'"$(segvtrace $RD_LAST_LOG)"
    rd_health $J $NODES || { sleep 90; rd_health $J $NODES; } || { RD_VERDICT="FAILED: unhealthy after $l ($bad bad runs of $total)"; exit 0; }
    [ $consec -ge 3 ] && { RD_VERDICT="FAILED: 3 consecutive bad runs, last $l ($bad bad of $total)"; exit 0; }
    return 1
  fi
  consec=0; return 0; }

digest() {
  { echo "S32 results digest, $(TZ=America/New_York date), worktree $(git -C $WT rev-parse --short HEAD), hold ${J:-none} ($( [ -n "$J" ] && left_s $J ) s left); $BASE_NOTE"
    echo "runs $total, bad $bad"
    echo; echo "######## (a) gates"; cat $OUT/gates.txt | cut -c1-300
    echo; echo "######## (b)/(c) host vs rank (hostrank.txt)"; cat $OUT/hostrank.txt | cut -c1-300
    echo; echo "######## (d) parallel soak: soak_stats.txt, then pairs.tsv (tag nn arm rc wall verify)"; cat $OUT/soak_stats.txt 2>/dev/null; cut -f1-6 $OUT/pairs.tsv 2>/dev/null | cut -c1-120
    echo; echo "######## ALERT"; cat $OUT/ALERT 2>/dev/null | cut -c1-220 | head -80
  } > $OUT/results.txt; }
trap 'digest; rd_finish' EXIT

# --- (a) gates
gate() { local l=$1 n=$2 G=$OUT/$1.txt; shift 2
  ensure_time 400 || fail "no time/STOP at gate $l"; evict; memlog $l
  rd_run $l 400 env MNRUN_NODES=$n MNRUN_LABEL=1 ./mnrun.sh $n env $BASE $COM "$@" ./ecalc 1000000000 $G; local grc=$?
  local P gv gc; P=$(plain $RD_LAST_LOG); gv=$(rd_verify $P); gc=$($E/digcmp.sh $G $HOME/ref/e_1000000000.txt); rm -rf $G $G.*
  extract $l $P
  echo "$l: nodes $n rc $grc $gv digits $gc | $(grep -a -m1 '^total' $P | cut -c1-120)" >> $OUT/gates.txt
  rd_say "$l: rc $grc $gv digits $gc"
  if [ $grc != 0 ] || [ "$gv" != "VERIFY OK" ] || ! grep -aq "mn: all $n nodes: VERIFY OK" $P || [ "$gc" != identical ]; then
    alert "GATE $l FAILED: rc $grc $gv digits $gc"$'\n'"$(segvtrace $RD_LAST_LOG)"; fail "GATE $l FAILED: rc $grc $gv digits $gc (log $RD_LAST_LOG)"; fi
  rd_health $J $NODES || fail "unhealthy after gate $l"; }
gate gate2_safe1 2 ECALC_VMM_SAFE=1
gate gate2_safe2 2 ECALC_VMM_SAFE=2
rd_say "gates done"; digest

# --- (b) host vs rank.  hr_analyse <label>: appends "label: stall node N host H total T s (median of the others M); permutation=..." to hostrank.txt; sets HR_NODE HR_HOST HR_T
hr_analyse() { local l=$1 f=$OUT/log/$1.plain
  grep -a 'recip ts2 node' $f | sed -n 's/.*recip ts2 node \([0-9]*\) host \([^ ]*\):.*total +\([0-9.]*\).*/\1 \2 \3/p' | sort -n > $OUT/res/$l.ts2
  if [ ! -s $OUT/res/$l.ts2 ]; then echo "$l: no 'recip ts2' lines (run failed before the stage?)" >> $OUT/hostrank.txt; HR_NODE=; return; fi
  read HR_NODE HR_HOST HR_T < <(sort -k3 -g $OUT/res/$l.ts2 | tail -1)
  local med; med=$(sort -k3 -g $OUT/res/$l.ts2 | awk '{a[NR]=$3} END{print a[int((NR+1)/2)]}')
  local h7; h7=$(awk '$1==7{print $2}' $OUT/res/$l.ts2)
  echo "$l: slowest node $HR_NODE host $HR_HOST total $HR_T s (median $med s); rank 7 is host $h7; rank 1 is host $(awk '$1==1{print $2}' $OUT/res/$l.ts2); stage lines: $(awk '$1=='"$HR_NODE"'' $OUT/res/$l.ts2 | head -1) | $(grep -a "recip ts2 node $HR_NODE " $f | cut -c1-220 | head -1)" >> $OUT/hostrank.txt; }
perm_list() { # the sorted node list with $STALL_HOST (or element 2 if absent) swapped to index 7
  echo $NODES | tr ' ' '\n' | awk -v h=$STALL_HOST '{a[NR]=$1} END{ if (NR<8) {for(i=1;i<=NR;i++) print a[i]; exit} k=2; for(i=1;i<=NR;i++) if (a[i]==h) k=i; t=a[k]; a[k]=a[8]; a[8]=t; for(i=1;i<=NR;i++) print a[i] }' | paste -sd,; }
PERM=$(perm_list); rd_say "permuted nodelist: $PERM"
STALL_B=0; STALL_P=0; FOLLOWS=unknown
for r in b_base1 b_perm1 b_base2 b_perm2; do
  ensure_time $RUN_S || { rd_say "no time for stage (b) $r"; break; }
  case $r in b_perm*) MNRUN_EXTRA="MNRUN_NODELIST=$PERM MNRUN_ARBITRARY=1";; *) MNRUN_EXTRA="";; esac
  run_one $r 10 $RUN_S $S NEWTON_MN_RECIP_TS=2; hr_analyse $r; MNRUN_EXTRA=""
  stall=$(awk -v t="${HR_T:-0}" 'BEGIN{print (t >= 5) ? 1 : 0}')
  if [ -n "$HR_NODE" ] && [ "$stall" = 1 ]; then
    case $r in b_base*) [ "$HR_HOST" = "$STALL_HOST" ] && STALL_B=$((STALL_B+1));;
               b_perm*) if [ "$HR_HOST" = "$STALL_HOST" ]; then STALL_P=$((STALL_P+1)); HOSTFOLLOW=$((${HOSTFOLLOW:-0}+1)); else RANKFOLLOW=$((${RANKFOLLOW:-0}+1)); fi;; esac
  fi
  digest
done
if [ "${HOSTFOLLOW:-0}" -ge 1 ] && [ "${RANKFOLLOW:-0}" = 0 ]; then FOLLOWS=host
elif [ "${RANKFOLLOW:-0}" -ge 1 ] && [ "${HOSTFOLLOW:-0}" = 0 ]; then FOLLOWS=rank
elif [ "${HOSTFOLLOW:-0}" -ge 1 ]; then FOLLOWS=mixed; else FOLLOWS="none-in-permuted-runs"; fi
echo "VERDICT (b): stall on $STALL_HOST in $STALL_B of the unchanged runs; in the permuted runs the stall followed the HOST ${HOSTFOLLOW:-0}x, the RANK ${RANKFOLLOW:-0}x (stall = a node's db_init..db_from_bi total >= 5 s) => $FOLLOWS (check each line: 'rank 7 is host' must be $STALL_HOST, else the permutation did not take)" | tee -a $OUT/hostrank.txt >/dev/null
rd_say "stage (b): $FOLLOWS (host ${HOSTFOLLOW:-0}, rank ${RANKFOLLOW:-0})"; digest

# --- (c) page-cache eviction on the suspect host if the stall follows the host
if [ "$FOLLOWS" = host ] && ensure_time $RUN_S; then
  { echo "## (c) before eviction on $STALL_HOST:"; timeout 60 srun --jobid=$J -N1 -w $STALL_HOST --overlap awk '/^MemAvailable|^Cached|^MemFree/' /proc/meminfo; } >> $OUT/hostrank.txt 2>&1
  if timeout 60 srun --jobid=$J -N1 -w $STALL_HOST --overlap sudo -n true >/dev/null 2>&1; then EV=drop_caches
    timeout 120 srun --jobid=$J -N1 -w $STALL_HOST --overlap sudo -n sh -c 'sync; echo 1 > /proc/sys/vm/drop_caches' >> $OUT/hostrank.txt 2>&1
  else EV=dd-nocache
    timeout 300 srun --jobid=$J -N1 -w $STALL_HOST --overlap bash -c 'sync; for f in $(find $HOME/ntt-wt/s32 $HOME/ref $HOME/gmp $HOME/s31 $HOME/s32 -type f 2>/dev/null | head -20000); do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done' >> $OUT/hostrank.txt 2>&1
  fi
  { echo "## (c) after eviction ($EV) on $STALL_HOST:"; timeout 60 srun --jobid=$J -N1 -w $STALL_HOST --overlap awk '/^MemAvailable|^Cached|^MemFree/' /proc/meminfo; } >> $OUT/hostrank.txt 2>&1
  run_one c_evict 10 $RUN_S $S NEWTON_MN_RECIP_TS=2; hr_analyse c_evict
  echo "(c) c_evict ($EV on $STALL_HOST): rc $RUN_RC; compare its 'slowest node' line above with b_base*" >> $OUT/hostrank.txt; digest
elif [ "$FOLLOWS" = host ]; then echo "(c) skipped: no time" >> $OUT/hostrank.txt
else echo "(c) skipped: the stall did not follow the host ($FOLLOWS)" >> $OUT/hostrank.txt; fi

# --- (d) crash-rate soak, PARALLEL: the 10 nodes as 5 disjoint 2-node jobs (s32_pairsoak.sh workers p0..p4, one network program per node), 6.441e10 digits per node, each worker
#     alternating BASE (A) and BASE + ECALC_VMM_SAFE=$SOAK_SAFE (B), the first arm alternating over the slots; until 30 min before the hold's end (RUN_S + 600 + MARGIN) or STOP.
#     On the successor hold ($H2 when this one was $H1) the soak continues when that hold is RUNNING.  Rows: ~/s32/pairs.tsv (tag nn arm rc wall verify total_line end nodes).
export BASE SOAK_SAFE RUN_S; export MARGIN
rm -f $OUT/pairs.tsv; : > $OUT/pairs.tsv; SOAK_HOLDS=$J; [ $J = $H1 ] && SOAK_HOLDS="$J $H2"
soak_stats() { awk -f $HD/pairstats.awk $OUT/pairs.tsv | sort > $OUT/soak_stats.txt
  awk -F'\t' '$4==139 { c[$3]++ } END { printf "rc139 by arm:"; for (a in c) printf " %s=%d", a, c[a]; printf "\n" }' $OUT/pairs.tsv >> $OUT/soak_stats.txt; }
nr=0
for SH in $SOAK_HOLDS; do
  while :; do
    [ -e $OUT/STOP ] && break 2
    st=$(squeue -j $SH -h -o %T 2>/dev/null)
    [ -z "$st" ] && { rd_say "hold $SH is gone: skipped"; continue 2; }
    [ "$st" = RUNNING ] && break
    rd_say "soak: waiting for hold $SH ($st)"; sleep 300
  done
  NODES=$(scontrol show hostnames "$(squeue -j $SH -h -o %N)" | tr '\n' ' ')
  [ "$(echo $NODES | wc -w)" = 10 ] || { rd_say "hold $SH has not 10 nodes: skipped"; continue; }
  [ "$(left_s $SH)" -ge $(( RUN_S + 600 + MARGIN )) ] || { rd_say "hold $SH: too little time left: skipped"; continue; }
  export SLURM_JOB_ID=$SH; J=$SH
  rd_health $SH $NODES || { sleep 90; rd_health $SH $NODES; } || { alert "hold $SH unhealthy at the soak start: skipped"; continue; }
  rd_say "soak on hold $SH: nodes $NODES, $(left_s $SH) s left"
  set -- $NODES; i=0; PIDS=""
  while [ $# -ge 2 ]; do
    if [ $((i % 2)) = 0 ]; then fa=A; else fa=B; fi
    SOAK_SAFE=$SOAK_SAFE nohup bash $HD/s32_pairsoak.sh $SH $OUT p$i $1,$2 $fa $MARGIN $S > $OUT/w_p$i.nohup 2>&1 < /dev/null &
    PIDS="$PIDS $!"; i=$((i+1)); shift 2; sleep 20      # staggered starts (the plan call of mnrun.sh runs on the login node)
  done
  while :; do alive=0; for p in $PIDS; do kill -0 $p 2>/dev/null && alive=1; done; [ $alive = 0 ] && break
    sleep 600; soak_stats; digest; done
  nr=$((nr+1)); soak_stats; digest
done
soak_stats; digest
RD_VERDICT="SUCCESS: $bad bad runs of $total (stages a-c); (b) stall follows: $FOLLOWS; parallel soak: $(tr '\n' ';' < $OUT/soak_stats.txt | cut -c1-400) (digest $OUT/results.txt)"

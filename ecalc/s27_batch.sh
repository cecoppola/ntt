#!/bin/bash
# S27: X1 of results/XEFF.md -- COMM_XSTATS (per-kind exchange stats) and COMM_SHMEM_PEER_ORDER=rot (rotated, ready-first peer service); 10-node A/B on the S25 base line.
#   NOT armed by its author.  Run from aac7 home under nohup (arming command in results/S27.md / the agent report); copy s27_batch.sh s27_lib.sh s27_xagg.awk to ~/s27 first.
#   The worktree ~/ntt-wt/s27 must stay at BUILD_REV, built by hand (make -B SHMEM_CRAY=1 GMP_HOME=$HOME/gmp ecalc tools tests/t_*).
#   (0) WAIT   until ~/s25/S25_DONE exists (S25 runs on hold 12331; touch ~/s25/STOP ends its repeats), polling every 5 min; then the hold is 12331 if it is RUNNING
#              with 10 nodes and >= 2 h left, else the successor 12389 (RUNNING, 10 nodes, >= 2 h left); neither: keeps waiting while one of them is PENDING, else fails.
#   (a) gates  2 nodes, 1e9 digits, three runs, each VERIFY OK + digcmp identical to ~/ref/e_1000000000.txt: gate_base (S25 BASE), gate_rot (BASE + COMM_SHMEM_PEER_ORDER=rot),
#              gate_xs (BASE + COMM_XSTATS=1 MN_WAIT_STATS=1; also: xstats lines present).
#   (b) stats  10 nodes x 2 at 6.441e10 digits/node (6.441e11 total), BASE order + COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1, no digit writes, VERIFY OK; res/stats_r*.txt and
#              xagg/stats_r*.txt (s27_xagg.awk: per phase/kind seconds per APU thread: t_post, t_call, t_wait, roff (skew), sig (transfer/tail)).
#   (c) ABBA   A = BASE, B = BASE + COMM_SHMEM_PEER_ORDER=rot, ECALC_INIT_TL=1 only (print only; as S22's D2), VERIFY OK; odd round A B, even round B A; blocks of 4 rounds;
#              after each block the paired B-A of `total` -> ~/s27/x1_stats.txt (S22's d2stats logic).  STOP when |t| > the two-sided 5 % critical value, or the 95 % CI
#              half-width <= 15 s, or after 16 rounds.  Each (c) run leaves time for (d).
#   (d) xs rot 1 run: BASE + COMM_SHMEM_PEER_ORDER=rot COMM_XSTATS=1, VERIFY OK, + xagg.
# BASE = the S25 base line: DM_MN_LEAN=1 MN_T_CHUNK_MB=2048, DIST_CHUNKS default 8.
# Outputs in ~/s27: summary.txt, x1.tsv, x1_stats.txt, results.txt (digest, rewritten after every run), res/<label>.txt, xagg/<label>.txt, log/<label>.log, ALERT (any problem),
#   S27_DONE (SUCCESS: ... | FAILED: ...).  To stop early and cleanly: touch ~/s27/STOP (takes effect between runs, and ends the wait).
# Never cancels anything; kills only the PID rd_run started (rundriver.sh watchdog); never touches other jobs.  Fire-and-forget.
BUILD_REV=3fe078d6
H1=12331; H2=12389
HD=$(cd "$(dirname "$0")" && pwd); WT=$HOME/ntt-wt/s27; E=$WT/ecalc; OUT=$HOME/s27; S25DONE=$HOME/s25/S25_DONE
MARGIN=1800            # keep >= 30 min of the hold unused after the last run
S=64410000000          # one node's share at the 3.71e13 target
CAP_ROUNDS=16; BLOCK=4; CI_STOP=15; RUN_S=450; TAIL_S=700   # TAIL_S: the time (d) needs after (c)'s last run
mkdir -p $OUT/log $OUT/res $OUT/xagg
rm -f $OUT/S27_DONE $OUT/ALERT $OUT/STOP; [ -f $OUT/x1.tsv ] && mv $OUT/x1.tsv $OUT/x1.tsv.$(date +%s)
source $WT/tools/rundriver.sh; rd_init $OUT S27_DONE
source $HD/s27_lib.sh
[ -f $HD/s27_xagg.awk ] || fail "no $HD/s27_xagg.awk"
cd $E || fail "cd $E"
source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"
check_binary 'COMM_XSTATS COMM_SHMEM_PEER_ORDER xstats MN_WAIT_STATS' ""

# --- (0) wait for S25, then pick the hold
hold_ok() { # hold_ok <jobid>: RUNNING, 10 nodes, >= 7200 s left
  [ "$(squeue -j $1 -h -o %T 2>/dev/null)" = RUNNING ] || return 1
  [ "$(scontrol show hostnames "$(squeue -j $1 -h -o %N)" 2>/dev/null | wc -l)" = 10 ] || return 1
  [ "$(left_s $1)" -ge 7200 ]; }
rd_say "waiting for $S25DONE (poll 300 s); holds $H1 / $H2"
while :; do
  [ -e $OUT/STOP ] && fail "STOP file while waiting for S25"
  if [ -e $S25DONE ]; then
    if hold_ok $H1; then J=$H1; break; elif hold_ok $H2; then J=$H2; break; fi
    pend=$(squeue -j $H1,$H2 -h -o "%T" 2>/dev/null | grep -c 'PENDING\|RUNNING')
    [ "$pend" -ge 1 ] || fail "S25 is done but neither hold $H1 nor $H2 exists (RUNNING/PENDING)"
  fi
  sleep 300
done
rd_say "S25 done ($(cat $S25DONE 2>/dev/null | head -c 150)); using hold $J"
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

total=0; bad=0; consec=0
SW_ROT="COMM_SHMEM_PEER_ORDER=rot"; SW_XS="COMM_XSTATS=1"
evict() { timeout 120 srun --jobid=$J -N10 -n10 --overlap bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done; rm -f $HOME/s27/gate_*.txt*' >/dev/null 2>&1; sleep 5; }
# ensure_time <expected_s>: 0 if the run (expected + 600 s slack) ends with >= MARGIN left, and no STOP file
ensure_time() { [ -e $OUT/STOP ] && { rd_say "STOP file: stopping"; return 1; }
  local l; l=$(left_s $J); [ "$l" -ge $(( $1 + 600 + MARGIN )) ] || { rd_say "hold $J has $l s left (< $1 + 600 + $MARGIN): no more runs"; return 1; }; return 0; }
extract() { grep -aE '^total|wait-stats|^tl |^xstats|^comm_shmem: COMM_SHMEM_PEER_ORDER|VERIFY' $2 | cut -c1-700 > $OUT/res/$1.txt
  grep -aq '^xstats' $2 && awk -f $HD/s27_xagg.awk $2 > $OUT/xagg/$1.txt 2>/dev/null; }
# run_one <label> <n> <expected_s> <per-node digits> <env words...>: sets RUN_RC RUN_V RUN_T RUN_P RUN_WALL RUN_DM; returns 0 if clean
run_one() { local l=$1 n=$2 x=$3 d=$4; shift 4; total=$((total+1))
  evict
  rd_run $l $x env MNRUN_NODES=$n ./mnrun.sh $n env $BASE "$@" ./ecalc $((n * d)); RUN_RC=$?
  RUN_V=$(rd_verify $RD_LAST_LOG); RUN_T=$(grep -a -m1 '^total' $RD_LAST_LOG | cut -c1-120); RUN_WALL=$RD_LAST_WALL
  RUN_P=$(rd_peaks $RD_LAST_LOG 2>/dev/null | grep -m1 'rank 0' | grep -o 'peak=[0-9.]*GB')
  RUN_DM=$(echo "$RUN_T" | sed -n 's/.*+ dm \([0-9.]*\).*/\1/p')
  extract $l $RD_LAST_LOG
  rd_say "$l: n=$n d=$d rc $RUN_RC wall ${RUN_WALL}s $RUN_V | $RUN_T $RUN_P"
  if [ $RUN_RC != 0 ] || [ "$RUN_V" != "VERIFY OK" ]; then
    bad=$((bad+1)); consec=$((consec+1)); alert "$l: rc $RUN_RC $RUN_V (log $RD_LAST_LOG)"
    rd_health $J $NODES || { RD_VERDICT="FAILED: unhealthy after $l ($bad bad runs of $total)"; exit 0; }
    [ $consec -ge 2 ] && { RD_VERDICT="FAILED: 2 consecutive bad runs, last $l ($bad bad of $total)"; exit 0; }
    return 1
  fi
  consec=0; return 0; }

# --- paired statistics (S22's d2stats.awk, A = base, B = rot; the stop rule: |t| > crit, or CI half-width <= CI)
cat > $OUT/x1stats.awk <<'AWK'
BEGIN { FS = "\t"
  split("12.706 4.303 3.182 2.776 2.571 2.447 2.365 2.306 2.262 2.228 2.201 2.179 2.160 2.145 2.131 2.120 2.110 2.101 2.093 2.086 2.080 2.074 2.069 2.064 2.060 2.056 2.052 2.048 2.045 2.042", TC, " ") }
function num(s, key,   p) { if (match(s, key " +[0-9.]+")) { p = substr(s, RSTART, RLENGTH); sub(key " +", "", p); return p + 0 } return -1 }
FNR == 1 && $1 == "round" { next }
/^round/ { next }
{ r = $1 + 0; c = $3
  if ($4 != 0 || $6 != "VERIFY OK") { badr[r] = 1; next }
  t = num($7, "total"); dm = num($7, "dm"); if (t < 0) { badr[r] = 1; next }
  if (c == "A") { A[r] = t; dA[r] = dm } else { B[r] = t; dB[r] = dm }
  ndm++; dms[ndm] = dm }
function median(a, n,   i, j, v) { for (i = 2; i <= n; i++) { v = a[i]; for (j = i - 1; j >= 1 && a[j] > v; j--) a[j + 1] = a[j]; a[j + 1] = v } return n % 2 ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2 }
function stat(excl, lab,   r, n, s, ss, d, m, sd, se, t, df, cr, h, np, nm, sa, sb) {
  n = s = ss = np = nm = sa = sb = 0
  for (r in A) if ((r in B) && !(r in badr) && !(excl && ((dA[r] > 1.5 * med) || (dB[r] > 1.5 * med)))) {
    d = B[r] - A[r]; n++; s += d; ss += d * d; sa += A[r]; sb += B[r]; if (d > 0) np++; else if (d < 0) nm++ }
  if (n < 2) { printf "%s: n=%d (too few rounds)\n", lab, n; DEC[excl] = "CONTINUE"; return }
  m = s / n; sd = sqrt((ss - n * m * m) / (n - 1)); se = sd / sqrt(n); t = se > 0 ? m / se : 0; df = n - 1
  cr = df <= 30 ? TC[df] : 1.96 + 2.4 / df; h = cr * se
  printf "%s: n=%d rounds, mean A %.1f s, mean B %.1f s, mean B-A %+.2f s, sd %.2f, se %.2f, t=%+.2f (df %d, crit %.3f), 95%% CI %+.1f .. %+.1f (half %.1f s), B slower in %d, faster in %d\n", lab, n, sa / n, sb / n, m, sd, se, t, df, cr, m - h, m + h, h, np, nm
  if ((t < 0 ? -t : t) > cr) DEC[excl] = "STOP significant"; else if (h <= CI) DEC[excl] = "STOP null (CI half <= " CI " s)"; else DEC[excl] = "CONTINUE"
  N[excl] = n }
END { med = ndm ? median(dms, ndm) : 0
  if (mode == "median") { printf "%.3f\n", med; exit }
  printf "median dm over %d OK runs: %.1f s (blow-up = dm > %.1f s)\n", ndm, med, 1.5 * med
  stat(0, "ALL rounds"); stat(1, "excluding rounds with a dm blow-up run")
  printf "DECISION (all rounds): %s\n", DEC[0] }
AWK
x1_stats() { # x1_stats <label>: append to x1_stats.txt; echoes STOP / CONTINUE
  { echo "== $1 ($(TZ=America/New_York date))"; awk -v CI=$CI_STOP -f $OUT/x1stats.awk $OUT/x1.tsv; } > $OUT/x1_block.tmp
  cat $OUT/x1_block.tmp >> $OUT/x1_stats.txt; rd_say "X1 stats $1: $(grep -a 'ALL rounds' $OUT/x1_block.tmp | cut -c1-230) | $(grep -a DECISION $OUT/x1_block.tmp)"
  grep -a '^DECISION' $OUT/x1_block.tmp | grep -q 'STOP' && echo STOP || echo CONTINUE; }

digest() {
  { echo "S27 results digest, $(TZ=America/New_York date), worktree $(git -C $WT rev-parse --short HEAD), hold ${J:-none} ($( [ -n "$J" ] && left_s $J ) s left), base line S25 BASE (DM_MN_LEAN=1 MN_T_CHUNK_MB=2048, DIST_CHUNKS default 8)"
    echo "runs $total, bad $bad"
    echo; echo "######## (a) gates (res/gate_*.txt; gates.txt)"; cat $OUT/gates.txt 2>/dev/null | cut -c1-300
    echo; echo "######## (b) COMM_XSTATS=1 MN_WAIT_STATS=1, base order, 10 nodes x 6.441e10"
    for f in $OUT/res/stats_r*.txt; do [ -f "$f" ] || continue; b=$(basename $f .txt); echo "== $b"; grep -aE '^total|wait-stats max/min|wait-stats node 0:|VERIFY' $f | cut -c1-500; cat $OUT/xagg/$b.txt 2>/dev/null | cut -c1-200; done
    echo; echo "######## (c) ABBA x1.tsv (round slot config rc wall verify total_line)"; cut -f1-5,7 $OUT/x1.tsv 2>/dev/null | cut -c1-150
    echo; echo "######## x1_stats.txt"; cat $OUT/x1_stats.txt 2>/dev/null
    echo; echo "######## (d) rot + COMM_XSTATS=1"
    for f in $OUT/res/xsrot_r*.txt; do [ -f "$f" ] || continue; b=$(basename $f .txt); echo "== $b"; grep -aE '^total|VERIFY' $f | cut -c1-500; cat $OUT/xagg/$b.txt 2>/dev/null | cut -c1-200; done
  } > $OUT/results.txt; }
trap 'digest; rd_finish' EXIT

# --- (a) gates: 2 nodes, 1e9 digits
: > $OUT/gates.txt
gate() { # gate <label> <env words...>: fails the batch unless VERIFY OK and the digits are identical
  local l=$1 G=$OUT/$1.txt; shift
  ensure_time 300 || fail "no time/STOP at gate $l"; evict
  rd_run $l 300 env MNRUN_NODES=2 ./mnrun.sh 2 env $BASE "$@" ./ecalc 1000000000 $G; local grc=$?
  local gv gc; gv=$(rd_verify $RD_LAST_LOG); gc=$($E/digcmp.sh $G $HOME/ref/e_1000000000.txt); rm -rf $G $G.*
  extract $l $RD_LAST_LOG
  echo "$l: rc $grc $gv digits $gc | $(grep -a -m1 '^total' $RD_LAST_LOG | cut -c1-120) | xstats lines $(grep -ac '^xstats' $RD_LAST_LOG)" >> $OUT/gates.txt
  rd_say "$l: rc $grc $gv digits $gc"
  if [ $grc != 0 ] || [ "$gv" != "VERIFY OK" ] || ! grep -aq 'mn: all 2 nodes: VERIFY OK' $RD_LAST_LOG || [ "$gc" != identical ]; then fail "GATE $l FAILED: rc $grc $gv digits $gc (log $RD_LAST_LOG)"; fi
  rd_health $J $NODES || fail "unhealthy after gate $l"; }
gate gate_base
gate gate_rot $SW_ROT
grep -aq 'PEER_ORDER=rot' $OUT/res/gate_rot.txt || fail "gate_rot: no 'COMM_SHMEM_PEER_ORDER=rot' line in the log (switch not effective)"
gate gate_xs $SW_XS MN_WAIT_STATS=1
[ "$(grep -ac '^xstats pe .* kind ' $OUT/res/gate_xs.txt)" -ge 1 ] || fail "gate_xs passed but no xstats lines (COMM_XSTATS not effective): see $OUT/log/gate_xs.log"

# --- (b) stats: base order with COMM_XSTATS
for r in 1 2; do ensure_time $RUN_S || break; run_one stats_r$r 10 $RUN_S $S $SW_XS MN_WAIT_STATS=1 ECALC_INIT_TL=1; digest; done

# --- (c) ABBA, blocks of 4 rounds, until the stop rule (every run leaves TAIL_S for (d))
echo -e "round\tslot\tconfig\trc\twall_s\tverify\ttotal_line\tpeak\tflag" > $OUT/x1.tsv
: > $OUT/x1_stats.txt
nr=0; blk=0; stopword=CONTINUE; cdone=0
while [ $nr -lt $CAP_ROUNDS ] && [ $cdone = 0 ]; do
  blk=$((blk+1))
  for k in 1 2 3 4; do
    nr=$((nr+1)); r=$nr
    if [ $((r % 2)) = 1 ]; then ord="A B"; else ord="B A"; fi
    s=0; for c in $ord; do s=$((s+1))
      if ! ensure_time $((RUN_S + TAIL_S)); then cdone=1; break 2; fi
      if [ $c = A ]; then cw=""; else cw=$SW_ROT; fi
      run_one x1_r${r}s${s}_$c 10 $RUN_S $S $cw ECALC_INIT_TL=1
      med=$(awk -v mode=median -v CI=$CI_STOP -f $OUT/x1stats.awk $OUT/x1.tsv); flag=""
      [ -n "$RUN_DM" ] && awk -v d="$RUN_DM" -v m="$med" 'BEGIN{exit !(m > 0 && d > 1.5 * m)}' && flag=dmblowup
      [ -n "$flag" ] && alert "$flag: x1_r${r}s${s}_$c dm $RUN_DM s vs median $med s (log kept: $RD_LAST_LOG)"
      echo -e "$r\t$s\t$c\t$RUN_RC\t$RD_LAST_WALL\t$RUN_V\t$RUN_T\t$RUN_P\t$flag" >> $OUT/x1.tsv; digest
    done
  done
  stopword=$(x1_stats "block $blk (rounds $((nr-BLOCK+1))..$nr)")
  [ "$stopword" = STOP ] && break
done
[ "$cdone" = 1 ] && { x1_stats "ended early (time/STOP) after round $nr"; stopword="ended early"; } >/dev/null

# --- (d) rot with COMM_XSTATS
if ensure_time $RUN_S; then run_one xsrot_r1 10 $RUN_S $S $SW_ROT $SW_XS ECALC_INIT_TL=1; fi
RD_VERDICT="SUCCESS: $bad bad runs of $total; ABBA $stopword after $nr rounds (see $OUT/x1_stats.txt; digest $OUT/results.txt)"

#!/bin/bash
# S30: the reciprocal's late node (results/S27.md section 2.1 item 1).  NEWTON_MN_RECIP_TS=1 (per-node stage stamps, print only) and NEWTON_MN_CHAIN_BCAST=1 (node 0 runs the
#   single-node chain, r0 broadcast; digits identical).  ARMED by its author's session: it WAITS until ~/s28/S28_DONE exists (poll 300 s), then uses hold 12331 if RUNNING with 10 nodes
#   and >= 2 h left, else 12389 (same test).  Files in ~/s30: s30_batch.sh s30_lib.sh s30_xagg.awk; the worktree ~/ntt-wt/s30 must stay at BUILD_REV, built by hand
#   (make -B -j16 SHMEM_CRAY=1 GMP_HOME=$HOME/gmp ecalc tools).
#   BASE = S28's base (the BASE line of ~/s28/summary.txt, i.e. S27's base +/- rot), PLUS COMM_LAYER_VSLOT_POOL=1 COMM_LAYER_INTER2=1 ONLY IF ~/s28/x2_stats.txt's last block says
#          "DECISION (all rounds): STOP significant" with mean B-A < 0 (p < 0.05); runtime check, the base used is printed.
#   (a) diag   10 nodes x 1 at 6.441e10 digits/node, BASE + NEWTON_MN_RECIP_TS=1 COMM_XSTATS=1 MN_WAIT_STATS=1 NEWTON_DOUBLING_TS=1 ECALC_LOG_CLOCKS=1 ECALC_INIT_TL=1: the "recip ts node" lines say which node is late and where.
#   (b) rot    2 runs BASE + COMM_SHMEM_PEER_ORDER=rot + COMM_XSTATS=1 + RECIP_TS (the 358 s outlier of S27).
#   (c) gates  2 and 3 nodes, 1e9 digits, BASE + the fix: VERIFY OK and digits identical to ~/ref/e_1000000000.txt (digcmp); the log must show "chain on node 0 only".
#   (d) ABBA   A = BASE, B = BASE + NEWTON_MN_CHAIN_BCAST=1 (both with NEWTON_MN_RECIP_TS=1 ECALC_INIT_TL=1); odd round A B, even round B A; blocks of 4 rounds; STOP when |t| > the two-sided 5 %
#              critical value, or the 95 % CI half-width <= 15 s, or after 16 rounds (S22's stop rule).
# Outputs in ~/s30: summary.txt, gates.txt, d30.tsv, d30_stats.txt, results.txt (digest), res/<label>.txt (+ .ts: the recip ts lines), xagg/<label>.txt, log/<label>.log, ALERT, S30_DONE.
#   Stop early and cleanly: touch ~/s30/STOP (between runs; also ends the wait).  Never cancels anything; never touches other jobs.  Fire-and-forget.
BUILD_REV=39b2766
H1=12331; H2=12389
HD=$(cd "$(dirname "$0")" && pwd); WT=$HOME/ntt-wt/s30; E=$WT/ecalc; OUT=$HOME/s30; S27DONE=$HOME/s28/S28_DONE
MARGIN=1800            # keep >= 30 min of the hold unused after the last run
S=64410000000          # one node's share at the 3.71e13 target
CAP_ROUNDS=16; BLOCK=4; CI_STOP=15; RUN_S=450
mkdir -p $OUT/log $OUT/res $OUT/xagg
rm -f $OUT/S30_DONE $OUT/ALERT $OUT/STOP; [ -f $OUT/d30.tsv ] && mv $OUT/d30.tsv $OUT/d30.tsv.$(date +%s)
source $WT/tools/rundriver.sh; rd_init $OUT S30_DONE
source $HD/s30_lib.sh
[ -f $HD/s30_xagg.awk ] || fail "no $HD/s30_xagg.awk"
cd $E || fail "cd $E"
source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"
check_binary 'NEWTON_MN_CHAIN_BCAST NEWTON_MN_RECIP_TS COMM_XSTATS COMM_SHMEM_PEER_ORDER' ""

# --- (0) wait for S27, then pick the hold
hold_ok() { # hold_ok <jobid>: RUNNING, 10 nodes, >= 7200 s left
  [ "$(squeue -j $1 -h -o %T 2>/dev/null)" = RUNNING ] || return 1
  [ "$(scontrol show hostnames "$(squeue -j $1 -h -o %N)" 2>/dev/null | wc -l)" = 10 ] || return 1
  [ "$(left_s $1)" -ge 7200 ]; }
rd_say "waiting for $S27DONE (poll 300 s); holds $H1 / $H2"
while :; do
  [ -e $OUT/STOP ] && fail "STOP file while waiting for S28"
  if [ -e $S27DONE ]; then
    if hold_ok $H1; then J=$H1; break; elif hold_ok $H2; then J=$H2; break; fi
    pend=$(squeue -j $H1,$H2 -h -o "%T" 2>/dev/null | grep -c 'PENDING\|RUNNING')
    [ "$pend" -ge 1 ] || fail "S27 is done but neither hold $H1 nor $H2 exists (RUNNING/PENDING)"
  fi
  sleep 300
done
rd_say "S28 done ($(cat $S27DONE 2>/dev/null | head -c 150)); using hold $J"
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

# --- the base line: S28's BASE (its summary.txt first line: "BASE used: <note> | <env words>"), plus X2's two switches only when S28's x2_stats.txt says B faster at p < 0.05
S28SUM=$HOME/s28/summary.txt
BASE_NOTE="S28 base"
if [ -s $S28SUM ] && head -1 $S28SUM | grep -q '^BASE used: .* | '; then
  BASE=$(head -1 $S28SUM | sed 's/^BASE used: .* | //'); BASE_NOTE="S28 base ($(head -1 $S28SUM | sed 's/^BASE used: \(.*\) | .*/\1/' | cut -c1-150))"
else BASE_NOTE="S28 summary has no BASE line: S28 base rebuilt from s30_lib.sh BASE (S27 base)"; fi
X2S=$HOME/s28/x2_stats.txt
if [ -s $X2S ]; then
  lastblk=$(awk '/^== /{b=""} {b=b"\n"$0} END{print b}' $X2S)
  dec=$(echo "$lastblk" | grep -a '^DECISION (all rounds)' | tail -1)
  mba=$(echo "$lastblk" | grep -a '^ALL rounds' | tail -1 | sed -n 's/.*mean B-A \([-+][0-9.]*\) s.*/\1/p')
  if echo "$dec" | grep -q 'STOP significant' && [ -n "$mba" ] && awk -v m="$mba" 'BEGIN{exit !(m < 0)}'; then
    BASE="$BASE COMM_LAYER_VSLOT_POOL=1 COMM_LAYER_INTER2=1"; BASE_NOTE="$BASE_NOTE + X2 switches (x2_stats: $dec, mean B-A $mba s)"
  else BASE_NOTE="$BASE_NOTE; X2 NOT added (x2_stats: '$dec', mean B-A '$mba')"; fi
else BASE_NOTE="$BASE_NOTE; X2 NOT added (no or empty $X2S: S28 failed, the second-mesh comm pool was too small)"; fi
rd_say "BASE used: $BASE_NOTE"; echo "BASE used: $BASE_NOTE | $BASE" > $OUT/summary.txt

total=0; bad=0; consec=0
SW_FIX="NEWTON_MN_CHAIN_BCAST=1"; SW_TS="NEWTON_MN_RECIP_TS=1"; SW_XS="COMM_XSTATS=1"
evict() { timeout 120 srun --jobid=$J -N10 -n10 --overlap bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done; rm -f $HOME/s30/gate*.txt*' >/dev/null 2>&1; sleep 5; }
# ensure_time <expected_s>: 0 if the run (expected + 600 s slack) ends with >= MARGIN left, and no STOP file
ensure_time() { [ -e $OUT/STOP ] && { rd_say "STOP file: stopping"; return 1; }
  local l; l=$(left_s $J); [ "$l" -ge $(( $1 + 600 + MARGIN )) ] || { rd_say "hold $J has $l s left (< $1 + 600 + $MARGIN): no more runs"; return 1; }; return 0; }
extract() { grep -aE '^total|wait-stats|^tl |^xstats|VERIFY|peak|comm_shmem pool|comm_ofi pool|v-slot|vslot|COMM_LAYER|rank 0' $2 | cut -c1-500 | head -120 > $OUT/res/$1.txt
  grep -a 'recip ts node' $2 | sort -t' ' -k4,4n | cut -c1-300 > $OUT/res/$1.ts
  grep -aq '^xstats' $2 && awk -f $HD/s30_xagg.awk $2 > $OUT/xagg/$1.txt 2>/dev/null; }
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

# --- paired statistics (S22's d2stats.awk, A = base, B = base + both X2 switches; the stop rule: |t| > crit, or CI half-width <= CI)
cat > $OUT/d30stats.awk <<'AWK'
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
d30_stats() { # d30_stats <label>: append to d30_stats.txt; echoes STOP / CONTINUE
  { echo "== $1 ($(TZ=America/New_York date))"; awk -v CI=$CI_STOP -f $OUT/d30stats.awk $OUT/d30.tsv; } > $OUT/d30_block.tmp
  cat $OUT/d30_block.tmp >> $OUT/d30_stats.txt; rd_say "X2 stats $1: $(grep -a 'ALL rounds' $OUT/d30_block.tmp | cut -c1-230) | $(grep -a DECISION $OUT/d30_block.tmp)"
  grep -a '^DECISION' $OUT/d30_block.tmp | grep -q 'STOP' && echo STOP || echo CONTINUE; }

digest() {
  { echo "S30 results digest, $(TZ=America/New_York date), worktree $(git -C $WT rev-parse --short HEAD), hold ${J:-none} ($( [ -n "$J" ] && left_s $J ) s left); $BASE_NOTE"
    echo "runs $total, bad $bad"
    echo; echo "######## (a) diag + (b) rot runs (res/*.ts = recip ts node lines; res/<label>.txt; xagg)"
    for b in diag_r1 rot_r1 rot_r2; do f=$OUT/res/$b.txt; [ -f "$f" ] || continue; echo "== $b"; grep -aE '^total|wait-stats max/min|VERIFY' $f | cut -c1-400; cat $OUT/res/$b.ts 2>/dev/null; cat $OUT/xagg/$b.txt 2>/dev/null | grep -aE 'recip addsh|all direct' | cut -c1-200; done
    echo; echo "######## (c) gates (gates.txt)"; cat $OUT/gates.txt 2>/dev/null | cut -c1-300
    echo; echo "######## (d) ABBA d30.tsv (round slot config rc wall verify total_line)"; cut -f1-5,7 $OUT/d30.tsv 2>/dev/null | cut -c1-170
    echo; echo "######## d30_stats.txt"; cat $OUT/d30_stats.txt 2>/dev/null
    echo; echo "######## recip ts of the last A and last B run"; for c in A B; do f=$(ls -t $OUT/res/d30_r*_$c.ts 2>/dev/null | head -1); [ -n "$f" ] && { echo "== $f"; cat $f; }; done
  } > $OUT/results.txt; }
mem_cmp() { :; }
trap 'mem_cmp; digest; rd_finish' EXIT

# --- (a) diag, (b) rot runs: base, print-only switches
rd_say "BASE: $BASE"
ensure_time $RUN_S && run_one diag_r1 10 $RUN_S $S $SW_TS $SW_XS MN_WAIT_STATS=1 NEWTON_DOUBLING_TS=1 ECALC_LOG_CLOCKS=1 ECALC_INIT_TL=1; digest
for r in 1 2; do ensure_time $RUN_S || break; run_one rot_r$r 10 $RUN_S $S COMM_SHMEM_PEER_ORDER=rot $SW_TS $SW_XS ECALC_INIT_TL=1; digest; done

# --- (c) gates for the fix: digits identical + VERIFY OK + the broadcast path really ran
: > $OUT/gates.txt
gate() { # gate <label> <nodes> <env words...>: fails the batch unless VERIFY OK, the digits are identical and the log shows the chain on node 0 only
  local l=$1 n=$2 G=$OUT/$1.txt; shift 2
  ensure_time 400 || fail "no time/STOP at gate $l"; evict
  rd_run $l 400 env MNRUN_NODES=$n ./mnrun.sh $n env $BASE "$@" ./ecalc 1000000000 $G; local grc=$?
  local gv gc; gv=$(rd_verify $RD_LAST_LOG); gc=$($E/digcmp.sh $G $HOME/ref/e_1000000000.txt); rm -rf $G $G.*
  extract $l $RD_LAST_LOG
  echo "$l: nodes $n rc $grc $gv digits $gc | $(grep -a -m1 '^total' $RD_LAST_LOG | cut -c1-120) | $(grep -a -m1 'recip ts node' $RD_LAST_LOG | cut -c1-200)" >> $OUT/gates.txt
  rd_say "$l: rc $grc $gv digits $gc"
  if [ $grc != 0 ] || [ "$gv" != "VERIFY OK" ] || ! grep -aq "mn: all $n nodes: VERIFY OK" $RD_LAST_LOG || [ "$gc" != identical ]; then fail "GATE $l FAILED: rc $grc $gv digits $gc (log $RD_LAST_LOG)"; fi
  grep -aq 'chain on node 0 only' $RD_LAST_LOG || fail "GATE $l: no 'chain on node 0 only' stamp (the broadcast path did not run; log $RD_LAST_LOG)"
  rd_health $J $NODES || fail "unhealthy after gate $l"; }
gate gate2_fix 2 $SW_FIX $SW_TS
gate gate3_fix 3 $SW_FIX $SW_TS
rd_say "gates done"; digest

# --- (d) ABBA, blocks of 4 rounds, until the stop rule
echo -e "round\tslot\tconfig\trc\twall_s\tverify\ttotal_line\tpeak\tflag" > $OUT/d30.tsv
: > $OUT/d30_stats.txt
nr=0; blk=0; stopword=CONTINUE; cdone=0
while [ $nr -lt $CAP_ROUNDS ] && [ $cdone = 0 ]; do
  blk=$((blk+1))
  for k in 1 2 3 4; do
    nr=$((nr+1)); r=$nr
    if [ $((r % 2)) = 1 ]; then ord="A B"; else ord="B A"; fi
    s=0; for c in $ord; do s=$((s+1))
      if ! ensure_time $RUN_S; then cdone=1; break 2; fi
      if [ $c = A ]; then cw=""; else cw=$SW_FIX; fi
      run_one d30_r${r}s${s}_$c 10 $RUN_S $S $cw $SW_TS ECALC_INIT_TL=1
      med=$(awk -v mode=median -v CI=$CI_STOP -f $OUT/d30stats.awk $OUT/d30.tsv); flag=""
      [ -n "$RUN_DM" ] && awk -v d="$RUN_DM" -v m="$med" 'BEGIN{exit !(m > 0 && d > 1.5 * m)}' && flag=dmblowup
      [ -n "$flag" ] && alert "$flag: d30_r${r}s${s}_$c dm $RUN_DM s vs median $med s (log kept: $RD_LAST_LOG)"
      echo -e "$r\t$s\t$c\t$RUN_RC\t$RD_LAST_WALL\t$RUN_V\t$RUN_T\t$RUN_P\t$flag" >> $OUT/d30.tsv; mem_cmp; digest
    done
  done
  stopword=$(d30_stats "block $blk (rounds $((nr-BLOCK+1))..$nr)")
  [ "$stopword" = STOP ] && break
done
[ "$cdone" = 1 ] && { d30_stats "ended early (time/STOP) after round $nr"; stopword="ended early"; } >/dev/null
RD_VERDICT="SUCCESS: $bad bad runs of $total; ABBA $stopword after $nr rounds (see $OUT/d30_stats.txt; digest $OUT/results.txt)"

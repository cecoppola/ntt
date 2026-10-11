#!/bin/bash
# S22: 10-node scaling sweep with the new print-only counters + D2 sequential ABBA (T 1024 vs 2048) on hold 12331 HOLDA2.  Run from aac7 home under nohup.  NOT armed by its author.
#   Build: NONE here.  ~/ntt-wt/s22 was built by hand at BUILD_REV (make -B -j6 SHMEM_CRAY=1 ecalc tools ...); the driver only verifies that rev, that no
#   source is newer than the binary, that the binary has the new strings, and that tools/unpack_digits exists.  Copy this script outside the worktree to run it.
#   (a) gate: 2 nodes, 1e9 digits, every new switch ON (MN_WAIT_STATS NTT_SIZE_STATS ECALC_INIT_TL MN_COMM_MARK), digits identical to the reference + the new lines present
#   (b) scaling: n = 2 4 6 8 10 nodes x 2 reps at 6.441e10 digits/node (n x that in total, no write); config A = the S20 LINE + DM_MN_LEAN=1 (DIST_CHUNKS default 8)
#       with MN_WAIT_STATS=1 (barrier / wait / ready) ECALC_INIT_TL=1 NTT_SIZE_STATS=1 MN_COMM_MARK=1
#   (c) D2: sequential ABBA, A = config A, B = A + MN_T_CHUNK_MB=2048, all with ECALC_INIT_TL=1; blocks of 4 rounds (rounds 9.. continue S20's 8 in ~/s20/ab.tsv, same parity
#       rule: odd round A B, even round B A).  After each block the paired B-A of `total` over ALL rounds incl. S20's goes to ~/s22/d2_stats.txt; STOP when |t| > the
#       two-sided 5 % critical value (significant), or the 95 % CI half-width < 15 s (conclusive null), or after 16 new rounds (cap).  dm > 1.5 x median dm -> flag
#       "dmblowup" in the tsv (logs are never deleted).
# Outputs in ~/s22: summary.txt, d2.tsv, d2_stats.txt, results.txt (digest), res/<label>.txt (extracts: total, wait-stats, ntt-size-stats, comm-mark, tl lines),
#   log/<label>.log (full logs), ALERT (any problem), S22_DONE (SUCCESS: ... | FAILED: ...).
# Never cancels anything; kills only the PID rd_run started (rundriver.sh watchdog); never touches other jobs.  Fire-and-forget.
J=12331; BUILD_REV=83a124bb
WT=$HOME/ntt-wt/s22; E=$WT/ecalc; OUT=$HOME/s22; S20TSV=$HOME/s20/ab.tsv
MIN_LEFT=1800         # never start a 10-node run with < 30 min left on the hold
S=64410000000         # one node's share at the 3.71e13 target
CAP_ROUNDS=16; BLOCK=4; CI_STOP=15
mkdir -p $OUT/log $OUT/res
rm -f $OUT/S22_DONE $OUT/ALERT
[ -f $OUT/d2.tsv ] && mv $OUT/d2.tsv $OUT/d2.tsv.$(date +%s)
alert() { { echo "$(TZ=America/New_York date) $*"; } >> $OUT/ALERT; }
source $WT/tools/rundriver.sh; rd_init $OUT S22_DONE
fail() { alert "$*"; RD_VERDICT="FAILED: $*"; exit 0; }

# --- the binary: rev, freshness, new strings, tools
[ -e $WT/.git ] || fail "no worktree $WT"
git -C $WT rev-parse HEAD | grep -q "^$BUILD_REV" || fail "worktree HEAD $(git -C $WT rev-parse --short HEAD) is not BUILD_REV $BUILD_REV"
git -C $WT diff --quiet -- ecalc || fail "tracked files in $WT/ecalc are modified"
[ -x $E/ecalc ] || fail "no $E/ecalc"
[ -z "$(find $E -maxdepth 1 \( -name '*.c' -o -name '*.h' \) -newer $E/ecalc | head -1)" ] || fail "a source in $E is newer than the ecalc binary (rebuild needed)"
for s in 'ntt-size-stats' 'ready %.3f s' 'NTT_SIZE_STATS' 'MN_WAIT_STATS'; do grep -aq "$s" $E/ecalc || fail "ecalc binary lacks the string '$s'"; done
[ -x $WT/tools/unpack_digits ] || fail "no $WT/tools/unpack_digits (make tools)"
[ -x $E/digcmp.sh ] && [ -x $E/mnrun.sh ] || fail "digcmp.sh / mnrun.sh missing in $E"
[ -f $S20TSV ] || fail "no $S20TSV (S20's rounds)"
rd_say "binary OK: worktree $(git -C $WT rev-parse --short HEAD) = BUILD_REV $BUILD_REV, ecalc $(stat -c %y $E/ecalc | cut -c1-19)"
cd $E || fail "cd $E"

# --- the hold
left_s() { local t; t=$(squeue -j $1 -h -o %L -t R 2>/dev/null); [ -z "$t" ] && { echo 0; return; }
  echo "$t" | awk -F'[-:]' '{n=NF; s=$n+60*$(n-1); if(n>=3) s+=3600*$(n-2); if(n>=4) s+=86400*$(n-3); print s}'; }
[ "$(squeue -j $J -h -o %T 2>/dev/null)" = RUNNING ] || fail "hold $J not RUNNING ($(squeue -j $J -h -o %T 2>&1))"
export SLURM_JOB_ID=$J
NODES=$(scontrol show hostnames "$(squeue -j $J -h -o %N)" | tr '\n' ' '); NN=$(echo $NODES | wc -w)
[ "$NN" = 10 ] || fail "hold $J has $NN nodes, need 10"
[ "$(left_s $J)" -ge 7200 ] || fail "hold $J has $(left_s $J) s left, need >= 7200 at start"
rd_say "hold $J nodes: $NODES ($(left_s $J) s left)"
rd_health $J $NODES || fail "unhealthy at start (leftover ecalc / t_* process or no meminfo)"
rd_disk $HOME 20 || fail "home nearly full"

LINE="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 COMM_OFI_PLAN_CXI=1"
CA="DM_MN_LEAN=1"; CB="DM_MN_LEAN=1 MN_T_CHUNK_MB=2048"   # later assignment wins in env(1); DIST_CHUNKS deliberately unset (default 8)
STATS="MN_WAIT_STATS=1 ECALC_INIT_TL=1 NTT_SIZE_STATS=1 MN_COMM_MARK=1"
total=0; bad=0; consec=0

# --- D2 statistics (awk): paired B-A of `total` over ~/s20/ab.tsv + ~/s22/d2.tsv.  tsv layout (S20's): round slot config rc wall verify total_line peak [flag]
cat > $OUT/d2stats.awk <<'AWK'
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
  if ((t < 0 ? -t : t) > cr) DEC[excl] = "STOP significant"; else if (h < CI) DEC[excl] = "STOP null (CI half < " CI " s)"; else DEC[excl] = "CONTINUE"
  N[excl] = n }
END { med = ndm ? median(dms, ndm) : 0
  if (mode == "median") { printf "%.3f\n", med; exit }
  printf "median dm over %d OK runs: %.1f s (blow-up = dm > %.1f s)\n", ndm, med, 1.5 * med
  stat(0, "ALL rounds"); stat(1, "excluding rounds with a dm blow-up run")
  printf "DECISION (all rounds): %s\n", DEC[0] }
AWK
d2_stats() { # d2_stats <label>: append to d2_stats.txt; echoes the decision word STOP / CONTINUE
  { echo "== $1 ($(TZ=America/New_York date))"; awk -v CI=$CI_STOP -f $OUT/d2stats.awk $S20TSV $OUT/d2.tsv; } > $OUT/d2_block.tmp
  cat $OUT/d2_block.tmp >> $OUT/d2_stats.txt; rd_say "D2 stats $1: $(grep -a 'ALL rounds' $OUT/d2_block.tmp | cut -c1-230) | $(grep -a DECISION $OUT/d2_block.tmp)"
  grep -a '^DECISION' $OUT/d2_block.tmp | grep -q 'STOP' && echo STOP || echo CONTINUE; }

# --- helpers
evict() { timeout 120 srun --jobid=$J -N10 -n10 --overlap bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done; rm -f $HOME/s22/gate.txt*' >/dev/null 2>&1; sleep 5; }
ensure_time() { local l; l=$(left_s $J); [ "$l" -ge $MIN_LEFT ] || fail "hold $J has $l s left (< $MIN_LEFT): stopping with the results so far"; }
extract() { # extract <label> <log>: the interesting lines of a log
  grep -aE '^total|wait-stats|ntt-size-stats|comm-mark|^tl |dcstats|VERIFY' $2 | cut -c1-700 > $OUT/res/$1.txt; }
# run_one <label> <n> <expected_s> <env words...>; sets RUN_V RUN_T RUN_TOTAL RUN_DM RUN_P; returns 0 if clean
run_one() { local l=$1 n=$2 x=$3; shift 3; total=$((total+1))
  ensure_time; evict
  rd_run $l $x env MNRUN_NODES=$n ./mnrun.sh $n env $LINE "$@" ./ecalc $((n * S)); RUN_RC=$?
  RUN_V=$(rd_verify $RD_LAST_LOG); RUN_T=$(grep -a -m1 '^total' $RD_LAST_LOG | cut -c1-110); RUN_P=$(rd_peaks $RD_LAST_LOG 2>/dev/null | grep -m1 'rank 0' | grep -o 'peak=[0-9.]*GB')
  RUN_DM=$(echo "$RUN_T" | sed -n 's/.*+ dm \([0-9.]*\).*/\1/p')
  extract $l $RD_LAST_LOG
  rd_say "$l: n=$n rc $RUN_RC wall ${RD_LAST_WALL}s $RUN_V | $RUN_T $RUN_P"
  if [ $RUN_RC != 0 ] || [ "$RUN_V" != "VERIFY OK" ]; then
    bad=$((bad+1)); consec=$((consec+1)); alert "$l: rc $RUN_RC $RUN_V (log $RD_LAST_LOG)"
    rd_health $J $NODES || { RD_VERDICT="FAILED: unhealthy after $l ($bad bad runs of $total)"; exit 0; }
    [ $consec -ge 2 ] && { RD_VERDICT="FAILED: 2 consecutive bad runs, last $l ($bad bad of $total)"; exit 0; }
    return 1
  fi
  consec=0; return 0; }

# --- the digest (also on every exit)
digest() {
  { echo "S22 results digest, $(TZ=America/New_York date), worktree $(git -C $WT rev-parse --short HEAD), hold $J"
    echo; echo "######## scaling sweep (n nodes x 6.441e10 digits/node; config A + MN_WAIT_STATS ECALC_INIT_TL NTT_SIZE_STATS MN_COMM_MARK; res/scale_*.txt hold the full extracts)"
    for f in $OUT/res/scale_n*.txt; do [ -f "$f" ] || continue
      echo "== $(basename $f .txt)"; grep -aE '^total|wait-stats max/min|wait-stats node 0:|ntt-size-stats node 0: total|VERIFY' $f | cut -c1-900; echo "  comm-mark lines: $(grep -ac comm-mark $f); ntt-size-stats node-0 detail lines: $(grep -ac 'ntt-size-stats node 0:' $f)"; done
    echo; echo "######## D2 (d2.tsv: round slot config rc wall verify total_line peak flag)"; cut -f1-5,7,9 $OUT/d2.tsv 2>/dev/null | cut -c1-150
    echo; echo "######## d2_stats.txt"; cat $OUT/d2_stats.txt 2>/dev/null
  } > $OUT/results.txt; }
trap 'digest; rd_finish' EXIT

# --- (a) gate
G=$OUT/gate.txt
ensure_time; evict
rd_run gate 300 env MNRUN_NODES=2 ./mnrun.sh 2 env $LINE $CA $STATS ./ecalc 1000000000 $G; grc=$?
gv=$(rd_verify $RD_LAST_LOG); gc=$($E/digcmp.sh $G $HOME/ref/e_1000000000.txt); rm -rf $G $G.*
extract gate $RD_LAST_LOG
gr=$(grep -a 'wait-stats' $RD_LAST_LOG | grep -ac ' ready '); gn=$(grep -ac 'ntt-size-stats' $RD_LAST_LOG); gm=$(grep -ac 'comm-mark' $RD_LAST_LOG)
rd_say "gate: rc $grc $gv digits $gc | wait-stats lines with 'ready': $gr, ntt-size-stats lines: $gn, comm-mark lines: $gm"
if [ $grc != 0 ] || [ "$gv" != "VERIFY OK" ] || ! grep -aq 'mn: all 2 nodes: VERIFY OK' $RD_LAST_LOG || [ "$gc" != identical ]; then fail "GATE FAILED: rc $grc $gv digits $gc (log $RD_LAST_LOG)"; fi
[ "$gr" -ge 1 ] && [ "$gn" -ge 2 ] || fail "gate passed but the new counters are missing (ready lines $gr, ntt-size-stats lines $gn): see $RD_LAST_LOG"
[ "$gm" -ge 1 ] || alert "gate: no comm-mark lines (MN_COMM_MARK not effective?); continuing"
rd_health $J $NODES || fail "unhealthy after gate"

# --- (b) scaling sweep: 2 reps of n = 2 4 6 8 10
for rep in 1 2; do for n in 2 4 6 8 10; do
  run_one scale_n${n}_r$rep $n 500 $CA $STATS
done; done

# --- (c) D2 sequential ABBA, blocks of 4 rounds, until the stop rule
echo -e "round\tslot\tconfig\trc\twall_s\tverify\ttotal_line\tpeak\tflag" > $OUT/d2.tsv
: > $OUT/d2_stats.txt
nr=0; blk=0; stopword=CONTINUE
while [ $nr -lt $CAP_ROUNDS ]; do
  blk=$((blk+1))
  for k in 1 2 3 4; do
    nr=$((nr+1)); r=$((8+nr))
    if [ $((r % 2)) = 1 ]; then ord="A B"; else ord="B A"; fi
    s=0; for c in $ord; do s=$((s+1)); eval "cw=\$C$c"
      run_one d2_r${r}s${s}_$c 10 450 $cw ECALC_INIT_TL=1
      med=$(awk -v mode=median -v CI=$CI_STOP -f $OUT/d2stats.awk $S20TSV $OUT/d2.tsv); flag=""
      [ -n "$RUN_DM" ] && awk -v d="$RUN_DM" -v m="$med" 'BEGIN{exit !(m > 0 && d > 1.5 * m)}' && flag=dmblowup
      [ -n "$flag" ] && alert "$flag: d2_r${r}s${s}_$c dm $RUN_DM s vs median $med s (log kept: $RD_LAST_LOG)"
      echo -e "$r\t$s\t$c\t$RUN_RC\t$RD_LAST_WALL\t$RUN_V\t$RUN_T\t$RUN_P\t$flag" >> $OUT/d2.tsv
    done
  done
  stopword=$(d2_stats "block $blk (new rounds $((nr-BLOCK+1))..$nr, i.e. rounds $((8+nr-BLOCK+1))..$((8+nr)))")
  [ "$stopword" = STOP ] && break
done
RD_VERDICT="SUCCESS: $bad bad runs of $total; D2 $stopword after $nr new rounds (see $OUT/d2_stats.txt; digest $OUT/results.txt)"

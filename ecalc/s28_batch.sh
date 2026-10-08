#!/bin/bash
# S28: X2 of results/XEFF.md -- COMM_LAYER_VSLOT_POOL=1 (v-slots from the comm pool: unstaged, no rounds) + COMM_LAYER_INTER2=1 (second inter mesh, two v-exchanges in flight);
#   the 576-level lockstep rounds are what it removes (modelled -10..-25 s at 10 nodes, memory-neutral).  ARMED by its author's session: it only WAITS until ~/s27/S27_DONE exists.
#   Files in ~/s28: s28_batch.sh s28_lib.sh s28_xagg.awk (copied there; the worktree ~/ntt-wt/s28 must stay at BUILD_REV, built by hand: make -B SHMEM_CRAY=1 GMP_HOME=$HOME/gmp ecalc tools tests/t_*).
#   (0) WAIT   until ~/s27/S27_DONE exists (poll 300 s); then the hold is 12331 if RUNNING with 10 nodes and >= 2 h left, else 12389 (same test); neither: keeps waiting while one is PENDING, else fails.
#       BASE   = S27's base (DM_MN_LEAN=1 MN_T_CHUNK_MB=2048) PLUS COMM_SHMEM_PEER_ORDER=rot ONLY IF ~/s27/x1_stats.txt's last block says "DECISION (all rounds): STOP significant" with mean B-A < 0
#              (rot faster, |t| > the 5 % critical value); runtime check, the base used is printed (rd_say + summary).
#   (a) gates  digits identical to ~/ref/e_1000000000.txt (digcmp), VERIFY OK, 1e9 digits: 3 nodes (the general map: v-exchanges): gate3_base, gate3_vp (VSLOT_POOL), gate3_both (VSLOT_POOL + INTER2),
#              6 nodes: gate6_both; 2 nodes (power of two: the switches must be harmless): gate2_vp, gate2_both; and gate2_i2neg: INTER2 alone must stop at init with the "needs COMM_LAYER_VSLOT_POOL" fatal.
#   (b) xs     10 nodes x 1 at 6.441e10 digits/node, BASE + both + COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1, VERIFY OK, xagg.
#   (c) ABBA   A = BASE, B = BASE + both, ECALC_INIT_TL=1; odd round A B, even round B A; blocks of 4 rounds; STOP when |t| > the two-sided 5 % critical value, or the 95 % CI half-width <= 15 s, or after 16 rounds
#              (S22's stop rule); every run's MEM_REPORT_DEVS peak (rd_peaks) goes to x2.tsv; mem_cmp.txt = mean peak A vs B and the pool lines of one A and one B run.
# Outputs in ~/s28: summary.txt, gates.txt, x2.tsv, x2_stats.txt, mem_cmp.txt, results.txt (digest), res/<label>.txt, xagg/<label>.txt, log/<label>.log, ALERT, S28_DONE (SUCCESS: ... | FAILED: ...).
#   Stop early and cleanly: touch ~/s28/STOP (between runs; also ends the wait).  Never cancels anything; never touches other jobs.  Fire-and-forget.
BUILD_REV=e2c92b74
H1=12331; H2=12389
HD=$(cd "$(dirname "$0")" && pwd); WT=$HOME/ntt-wt/s28; E=$WT/ecalc; OUT=$HOME/s28; S27DONE=$HOME/s27/S27_DONE
MARGIN=1800            # keep >= 30 min of the hold unused after the last run
S=64410000000          # one node's share at the 3.71e13 target
CAP_ROUNDS=16; BLOCK=4; CI_STOP=15; RUN_S=450
mkdir -p $OUT/log $OUT/res $OUT/xagg
rm -f $OUT/S28_DONE $OUT/ALERT $OUT/STOP; [ -f $OUT/x2.tsv ] && mv $OUT/x2.tsv $OUT/x2.tsv.$(date +%s)
source $WT/tools/rundriver.sh; rd_init $OUT S28_DONE
source $HD/s28_lib.sh
[ -f $HD/s28_xagg.awk ] || fail "no $HD/s28_xagg.awk"
cd $E || fail "cd $E"
source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"
check_binary 'COMM_LAYER_INTER2 COMM_LAYER_VSLOT_POOL COMM_XSTATS COMM_SHMEM_PEER_ORDER' "tests/t_mn_grid"

# --- (0) wait for S27, then pick the hold
hold_ok() { # hold_ok <jobid>: RUNNING, 10 nodes, >= 7200 s left
  [ "$(squeue -j $1 -h -o %T 2>/dev/null)" = RUNNING ] || return 1
  [ "$(scontrol show hostnames "$(squeue -j $1 -h -o %N)" 2>/dev/null | wc -l)" = 10 ] || return 1
  [ "$(left_s $1)" -ge 7200 ]; }
rd_say "waiting for $S27DONE (poll 300 s); holds $H1 / $H2"
while :; do
  [ -e $OUT/STOP ] && fail "STOP file while waiting for S27"
  if [ -e $S27DONE ]; then
    if hold_ok $H1; then J=$H1; break; elif hold_ok $H2; then J=$H2; break; fi
    pend=$(squeue -j $H1,$H2 -h -o "%T" 2>/dev/null | grep -c 'PENDING\|RUNNING')
    [ "$pend" -ge 1 ] || fail "S27 is done but neither hold $H1 nor $H2 exists (RUNNING/PENDING)"
  fi
  sleep 300
done
rd_say "S27 done ($(cat $S27DONE 2>/dev/null | head -c 150)); using hold $J"
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

# --- the base line: S27's base, plus rot only when S27 measured it significantly faster
BASE_NOTE="S27 base (BASE of s28_lib.sh: DM_MN_LEAN=1 MN_T_CHUNK_MB=2048)"
X1S=$HOME/s27/x1_stats.txt
if [ -s $X1S ]; then
  lastblk=$(awk '/^== /{b=""} {b=b"\n"$0} END{print b}' $X1S)
  dec=$(echo "$lastblk" | grep -a '^DECISION (all rounds)' | tail -1)
  mba=$(echo "$lastblk" | grep -a '^ALL rounds' | tail -1 | sed -n 's/.*mean B-A \([-+][0-9.]*\) s.*/\1/p')
  if echo "$dec" | grep -q 'STOP significant' && [ -n "$mba" ] && awk -v m="$mba" 'BEGIN{exit !(m < 0)}'; then
    BASE="$BASE COMM_SHMEM_PEER_ORDER=rot"; BASE_NOTE="S27 base + COMM_SHMEM_PEER_ORDER=rot (x1_stats: $dec, mean B-A $mba s)"
  else BASE_NOTE="$BASE_NOTE; rot NOT added (x1_stats: '$dec', mean B-A '$mba')"; fi
else BASE_NOTE="$BASE_NOTE; rot NOT added (no $X1S)"; fi
rd_say "BASE used: $BASE_NOTE"; echo "BASE used: $BASE_NOTE | $BASE" > $OUT/summary.txt

total=0; bad=0; consec=0
SW_X2="COMM_LAYER_VSLOT_POOL=1 COMM_LAYER_INTER2=1"; SW_VP="COMM_LAYER_VSLOT_POOL=1"; SW_XS="COMM_XSTATS=1"
evict() { timeout 120 srun --jobid=$J -N10 -n10 --overlap bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done; rm -f $HOME/s28/gate*.txt*' >/dev/null 2>&1; sleep 5; }
# ensure_time <expected_s>: 0 if the run (expected + 600 s slack) ends with >= MARGIN left, and no STOP file
ensure_time() { [ -e $OUT/STOP ] && { rd_say "STOP file: stopping"; return 1; }
  local l; l=$(left_s $J); [ "$l" -ge $(( $1 + 600 + MARGIN )) ] || { rd_say "hold $J has $l s left (< $1 + 600 + $MARGIN): no more runs"; return 1; }; return 0; }
extract() { grep -aE '^total|wait-stats|^tl |^xstats|VERIFY|peak|comm_shmem pool|comm_ofi pool|v-slot|vslot|COMM_LAYER|rank 0' $2 | cut -c1-500 | head -120 > $OUT/res/$1.txt
  grep -aq '^xstats' $2 && awk -f $HD/s28_xagg.awk $2 > $OUT/xagg/$1.txt 2>/dev/null; }
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
cat > $OUT/x2stats.awk <<'AWK'
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
x2_stats() { # x2_stats <label>: append to x2_stats.txt; echoes STOP / CONTINUE
  { echo "== $1 ($(TZ=America/New_York date))"; awk -v CI=$CI_STOP -f $OUT/x2stats.awk $OUT/x2.tsv; } > $OUT/x2_block.tmp
  cat $OUT/x2_block.tmp >> $OUT/x2_stats.txt; rd_say "X2 stats $1: $(grep -a 'ALL rounds' $OUT/x2_block.tmp | cut -c1-230) | $(grep -a DECISION $OUT/x2_block.tmp)"
  grep -a '^DECISION' $OUT/x2_block.tmp | grep -q 'STOP' && echo STOP || echo CONTINUE; }

digest() {
  { echo "S28 results digest, $(TZ=America/New_York date), worktree $(git -C $WT rev-parse --short HEAD), hold ${J:-none} ($( [ -n "$J" ] && left_s $J ) s left); $BASE_NOTE"
    echo "runs $total, bad $bad"
    echo; echo "######## (a) gates (res/gate*.txt; gates.txt)"; cat $OUT/gates.txt 2>/dev/null | cut -c1-300
    echo; echo "######## (b) both switches + COMM_XSTATS=1 MN_WAIT_STATS=1, 10 nodes x 6.441e10"
    for f in $OUT/res/xs_r*.txt; do [ -f "$f" ] || continue; b=$(basename $f .txt); echo "== $b"; grep -aE '^total|wait-stats max/min|VERIFY' $f | cut -c1-500; cat $OUT/xagg/$b.txt 2>/dev/null | cut -c1-200; done
    echo; echo "######## (c) ABBA x2.tsv (round slot config rc wall verify total_line peak)"; cut -f1-5,7,8 $OUT/x2.tsv 2>/dev/null | cut -c1-170
    echo; echo "######## x2_stats.txt"; cat $OUT/x2_stats.txt 2>/dev/null
    echo; echo "######## mem_cmp.txt"; cat $OUT/mem_cmp.txt 2>/dev/null
  } > $OUT/results.txt; }
mem_cmp() { # mean rank-0 peak (GB) of the A runs vs the B runs from x2.tsv column 8; the pool / v-slot lines of one A and one B run
  { echo "MEM_REPORT_DEVS peak (rd_peaks, rank 0, GB), per run (config A = base, B = base + both):"
    awk -F'\t' 'NR>1 && $6=="VERIFY OK" { v=$8; sub(/peak=/,"",v); sub(/GB/,"",v); if (v+0>0) { n[$3]++; s[$3]+=v; if (!(($3) in mx) || v+0>mx[$3]) mx[$3]=v+0; print $1, $3, v } }
         END { for (c in n) printf "config %s: %d runs, mean peak %.2f GB, max %.2f GB\n", c, n[c], s[c]/n[c], mx[c]; if (n["A"] && n["B"]) printf "B - A (mean) = %+.2f GB\n", s["B"]/n["B"] - s["A"]/n["A"] }' $OUT/x2.tsv
    for c in A B; do f=$(ls $OUT/res/x2_r*_$c.txt 2>/dev/null | head -1); [ -n "$f" ] && { echo "== $f"; grep -aiE 'peak|pool|slot' $f | head -12 | cut -c1-300; }; done
  } > $OUT/mem_cmp.txt; }
trap 'mem_cmp; digest; rd_finish' EXIT

# --- (a) gates: digits identical + VERIFY OK
: > $OUT/gates.txt
gate() { # gate <label> <nodes> <env words...>: fails the batch unless VERIFY OK and the digits are identical
  local l=$1 n=$2 G=$OUT/$1.txt; shift 2
  ensure_time 400 || fail "no time/STOP at gate $l"; evict
  rd_run $l 400 env MNRUN_NODES=$n ./mnrun.sh $n env $BASE "$@" ./ecalc 1000000000 $G; local grc=$?
  local gv gc; gv=$(rd_verify $RD_LAST_LOG); gc=$($E/digcmp.sh $G $HOME/ref/e_1000000000.txt); rm -rf $G $G.*
  extract $l $RD_LAST_LOG
  echo "$l: nodes $n rc $grc $gv digits $gc | $(grep -a -m1 '^total' $RD_LAST_LOG | cut -c1-120) | $(grep -aE 'comm_shmem pool: COMM_OFI' $RD_LAST_LOG | head -1 | cut -c1-200)" >> $OUT/gates.txt
  rd_say "$l: rc $grc $gv digits $gc"
  if [ $grc != 0 ] || [ "$gv" != "VERIFY OK" ] || ! grep -aq "mn: all $n nodes: VERIFY OK" $RD_LAST_LOG || [ "$gc" != identical ]; then fail "GATE $l FAILED: rc $grc $gv digits $gc (log $RD_LAST_LOG)"; fi
  rd_health $J $NODES || fail "unhealthy after gate $l"; }
gate gate3_base 3
gate gate3_vp 3 $SW_VP
gate gate3_both 3 $SW_X2
gate gate6_both 6 $SW_X2
gate gate2_vp 2 $SW_VP
gate gate2_both 2 $SW_X2
# negative gate: INTER2 without VSLOT_POOL must stop at init with the fatal (no digits)
ensure_time 300 || fail "no time/STOP at gate2_i2neg"; evict
rd_run gate2_i2neg 300 env MNRUN_NODES=2 ./mnrun.sh 2 env $BASE COMM_LAYER_INTER2=1 ./ecalc 1000000000
if grep -aq 'COMM_LAYER_INTER2=1 needs COMM_LAYER_VSLOT_POOL=1' $RD_LAST_LOG; then echo "gate2_i2neg: fatal as designed" >> $OUT/gates.txt; else fail "gate2_i2neg: INTER2 without VSLOT_POOL did not stop with the fatal (log $RD_LAST_LOG)"; fi
rd_health $J $NODES || { sleep 20; rd_health $J $NODES; } || fail "unhealthy after gate2_i2neg"
rd_say "gates done"; digest

# --- (b) one 10-node run with the stats on
for r in 1; do ensure_time $RUN_S || break; run_one xs_r$r 10 $RUN_S $S $SW_X2 $SW_XS MN_WAIT_STATS=1 ECALC_INIT_TL=1; digest; done

# --- (c) ABBA, blocks of 4 rounds, until the stop rule
echo -e "round\tslot\tconfig\trc\twall_s\tverify\ttotal_line\tpeak\tflag" > $OUT/x2.tsv
: > $OUT/x2_stats.txt
nr=0; blk=0; stopword=CONTINUE; cdone=0
while [ $nr -lt $CAP_ROUNDS ] && [ $cdone = 0 ]; do
  blk=$((blk+1))
  for k in 1 2 3 4; do
    nr=$((nr+1)); r=$nr
    if [ $((r % 2)) = 1 ]; then ord="A B"; else ord="B A"; fi
    s=0; for c in $ord; do s=$((s+1))
      if ! ensure_time $RUN_S; then cdone=1; break 2; fi
      if [ $c = A ]; then cw=""; else cw=$SW_X2; fi
      run_one x2_r${r}s${s}_$c 10 $RUN_S $S $cw ECALC_INIT_TL=1
      med=$(awk -v mode=median -v CI=$CI_STOP -f $OUT/x2stats.awk $OUT/x2.tsv); flag=""
      [ -n "$RUN_DM" ] && awk -v d="$RUN_DM" -v m="$med" 'BEGIN{exit !(m > 0 && d > 1.5 * m)}' && flag=dmblowup
      [ -n "$flag" ] && alert "$flag: x2_r${r}s${s}_$c dm $RUN_DM s vs median $med s (log kept: $RD_LAST_LOG)"
      echo -e "$r\t$s\t$c\t$RUN_RC\t$RD_LAST_WALL\t$RUN_V\t$RUN_T\t$RUN_P\t$flag" >> $OUT/x2.tsv; mem_cmp; digest
    done
  done
  stopword=$(x2_stats "block $blk (rounds $((nr-BLOCK+1))..$nr)")
  [ "$stopword" = STOP ] && break
done
[ "$cdone" = 1 ] && { x2_stats "ended early (time/STOP) after round $nr"; stopword="ended early"; } >/dev/null
RD_VERDICT="SUCCESS: $bad bad runs of $total; ABBA $stopword after $nr rounds (see $OUT/x2_stats.txt, $OUT/mem_cmp.txt; digest $OUT/results.txt)"

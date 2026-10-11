#!/bin/bash
# S23: single-node experiments in parallel on hold 12377 HOLDB (3 SPX nodes, one chain per node) + CPU-only b_seed64 on a CPX hold (12294, else its successor 12332).
# Run from aac7 home under nohup.  NOT armed by its author.  Built by hand at BUILD_REV in ~/ntt-wt/s22 (the driver does NOT rebuild; it verifies rev, freshness, strings, tools).
#   node A  A37-Q1 init timeline at 6.441e10: 6 reps ECALC_INIT_TL=1; then 2 reps with BS_SEED_THREADS=96 (seed threads halved from 192); 2 reps with BS_SEED_FILL=0 (the
#           seed-span auto-fill off: the plain BS_SEED_TERMS=256 span) -- seed / mapping sensitivity (all with ECALC_INIT_TL=1)
#   node B  A37-Q4 NTT sweep: tests/t_ntt vs tests/t_ntt_nop (NOP modmul) `bench 31 q4sweep` (L = 24..31: whole fwd/inv ms + every pass), 3 reps each, order R N N R R N;
#           then `r3bench 22 29` (3*2^k fwd/inv/inv+pw, ms per call, about 3*2^28 points per call; the radix-3 stage is not NOP'd) real vs NOP, 3 reps each, same order
#   node C  2 full runs at 6.441e10 with NTT_SIZE_STATS=1 DC_STATS=1; then the dm-variance probe: 6 runs with NEWTON_DOUBLING_TS=1 (dm and the recip timestamps per run;
#           "dm blow-up" = dm > 1.5 x the median dm of the 6)
#   CPX     tests/b_seed64, 3 reps each at the 10-node share (ranks 0 and 5) and the 576-node share (ranks 0 and 288), 192 threads (all-core wall)
# The single-node line is the bare `./ecalc 64410000000` (no output file, nothing written), as in S21; no mn / SHMEM variables.
# Gate first (3 checks in parallel, one per node): 1e9 digits identical with every new switch off; the same with every new switch on (DC_STATS NEWTON_DOUBLING_TS ECALC_INIT_TL
#   NTT_SIZE_STATS MN_WAIT_STATS); tests/t_ntt 24.  Any failure ends the driver before the experiments.
# Outputs in ~/s23: summary.txt, results.txt (digest), res/*.txt (per experiment), log/*.log, ALERT, S23_DONE (SUCCESS: ... | FAILED: ...).
# Never cancels anything; kills only the PID rd_run started; never touches other jobs.  Fire-and-forget: waits only for its own chains.
J_SPX=12377; J_CPX1=12294; J_CPX2=12332; BUILD_REV=83a124bb
WT=$HOME/ntt-wt/s22; E=$WT/ecalc; OUT=$HOME/s23
D1=64410000000
mkdir -p $OUT/log $OUT/res
rm -f $OUT/S23_DONE $OUT/ALERT $OUT/chain_*.status
alert() { { echo "$(TZ=America/New_York date) $*"; } >> $OUT/ALERT; }
source $WT/tools/rundriver.sh; rd_init $OUT S23_DONE
fail() { alert "$*"; RD_VERDICT="FAILED: $*"; exit 0; }
left_s() { local t; t=$(squeue -j $1 -h -o %L -t R 2>/dev/null); [ -z "$t" ] && { echo 0; return; }
  echo "$t" | awk -F'[-:]' '{n=NF; s=$n+60*$(n-1); if(n>=3) s+=3600*$(n-2); if(n>=4) s+=86400*$(n-3); print s}'; }

# --- the binary
[ -e $WT/.git ] || fail "no worktree $WT"
git -C $WT rev-parse HEAD | grep -q "^$BUILD_REV" || fail "worktree HEAD $(git -C $WT rev-parse --short HEAD) is not BUILD_REV $BUILD_REV"
git -C $WT diff --quiet -- ecalc || fail "tracked files in $WT/ecalc are modified"
for f in ecalc tests/t_ntt tests/t_ntt_nop tests/b_seed64; do [ -x $E/$f ] || fail "no $E/$f"; done
[ -z "$(find $E -maxdepth 1 \( -name '*.c' -o -name '*.h' \) -newer $E/ecalc | head -1)" ] || fail "a source in $E is newer than the ecalc binary (rebuild needed)"
for s in 'ntt-size-stats' 'NTT_SIZE_STATS' 'DC_STATS' 'NEWTON_DOUBLING_TS'; do grep -aq "$s" $E/ecalc || fail "ecalc binary lacks the string '$s'"; done
grep -aq 'q4sweep' $E/tests/t_ntt || fail "tests/t_ntt lacks q4sweep"
[ -x $WT/tools/unpack_digits ] || fail "no $WT/tools/unpack_digits (make tools)"
[ -x $E/digcmp.sh ] || fail "no digcmp.sh"
rd_say "binary OK: worktree $(git -C $WT rev-parse --short HEAD) = BUILD_REV $BUILD_REV"
source $E/aac7env.sh >/dev/null 2>&1     # MNRUN_MODULES
[ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"

# --- the SPX hold
[ "$(squeue -j $J_SPX -h -o %T 2>/dev/null)" = RUNNING ] || fail "hold $J_SPX not RUNNING ($(squeue -j $J_SPX -h -o %T 2>&1))"
NODES=($(scontrol show hostnames "$(squeue -j $J_SPX -h -o %N)")); [ ${#NODES[@]} = 3 ] || fail "hold $J_SPX has ${#NODES[@]} nodes, need 3"
[ "$(left_s $J_SPX)" -ge 7200 ] || fail "hold $J_SPX has $(left_s $J_SPX) s left, need >= 7200"
NA=${NODES[0]}; NB=${NODES[1]}; NC=${NODES[2]}
rd_say "SPX hold $J_SPX nodes: A=$NA B=$NB C=$NC ($(left_s $J_SPX) s left)"
rd_health $J_SPX $NA $NB $NC || fail "SPX node unhealthy at start"
for nd in $NA $NB $NC; do
  av=$(timeout 60 srun --jobid=$J_SPX -N1 -w $nd --overlap awk '/MemAvailable/{print $2}' /proc/meminfo 2>/dev/null)
  [ "${av:-0}" -ge 380000000 ] || fail "$nd MemAvailable ${av:-?} kB < 380 GB"
done
rd_disk $HOME 20 || fail "home nearly full"

PRE="module unload rocm/7.0.3 >/dev/null 2>&1; module load $MNRUN_MODULES; cd $E;"
evict_node() { timeout 120 srun --jobid=$J_SPX -N1 -w $1 --overlap bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done' >/dev/null 2>&1; sleep 2; }
# one_run <node> <label> <expected_s> "<env words>" <digits> [<outfile>]: ecalc on one SPX node; 0 if rc 0 and VERIFY OK.  Sets RUN_V RUN_T RUN_DM
one_run() { local nd=$1 lab=$2 x=$3 envw=$4 d=$5 of=${6:-}
  [ "$(left_s $J_SPX)" -ge 1200 ] || { alert "$lab: hold $J_SPX has < 20 min left"; return 1; }
  evict_node $nd
  rd_run $lab $x srun --jobid=$J_SPX -N1 -w $nd --gpus=4 -c 192 --overlap bash -lc "$PRE env $envw ./ecalc $d $of"; local rc=$?
  RUN_V=$(rd_verify $RD_LAST_LOG); RUN_T=$(grep -a -m1 '^total' $RD_LAST_LOG | cut -c1-140)
  RUN_DM=$(echo "$RUN_T" | sed -n 's/.*+ dm \([0-9.]*\).*/\1/p')
  rd_say "$lab on $nd: rc $rc wall ${RD_LAST_WALL}s $RUN_V | $RUN_T"
  if [ $rc != 0 ] || [ "$RUN_V" != "VERIFY OK" ]; then alert "$lab on $nd: rc $rc $RUN_V (log $RD_LAST_LOG)"; return 1; fi
  return 0; }

# --- gate
gate_digits() { # gate_digits <node> <label> "<env words>"
  local G=$OUT/$2.txt
  one_run $1 $2 120 "$3" 1000000000 $G || { rm -rf $G $G.*; echo "FAILED: $2 run" > $OUT/chain_$2.status; return; }
  local c; c=$(timeout 600 srun --jobid=$J_SPX -N1 -w $1 --overlap bash -c "$E/digcmp.sh $G $HOME/ref/e_1000000000.txt" 2>>$RD_LAST_LOG); rm -rf $G $G.*
  rd_say "$2: digits vs the 1e9 reference: $c"
  [ "$c" = identical ] && echo OK > $OUT/chain_$2.status || echo "FAILED: $2 digits $c" > $OUT/chain_$2.status; }
( gate_digits $NA gate_off "" ) & GP1=$!
( gate_digits $NB gate_on "DC_STATS=1 NEWTON_DOUBLING_TS=1 ECALC_INIT_TL=1 NTT_SIZE_STATS=1 MN_WAIT_STATS=1" ) & GP2=$!
( rd_run gate_tntt 900 srun --jobid=$J_SPX -N1 -w $NC --gpus=4 -c 192 --overlap bash -lc "$PRE ./tests/t_ntt 24"; rc=$?
  v=$(rd_verify $RD_LAST_LOG); rd_say "gate t_ntt 24 on $NC: rc $rc $v"
  [ $rc = 0 ] && [ "$v" = "VERIFY OK" ] && echo OK > $OUT/chain_gate_tntt.status || echo "FAILED: gate t_ntt rc $rc $v" > $OUT/chain_gate_tntt.status ) & GP3=$!
wait $GP1 $GP2 $GP3
for g in gate_off gate_on gate_tntt; do [ "$(cat $OUT/chain_$g.status 2>/dev/null)" = OK ] || fail "gate $g: $(cat $OUT/chain_$g.status 2>/dev/null || echo 'no status')"; done
grep -aq 'ntt-size-stats' $OUT/log/gate_on.log || fail "gate_on: no ntt-size-stats line in the log (NTT_SIZE_STATS not effective)"
rd_say "gates passed"

# --- chain A: Q1 init timeline and seed sensitivity
chain_A() { local bad=0 i lab w
  : > $OUT/res/q1.txt
  for spec in "base1:ECALC_INIT_TL=1" "base2:ECALC_INIT_TL=1" "base3:ECALC_INIT_TL=1" "base4:ECALC_INIT_TL=1" "base5:ECALC_INIT_TL=1" "base6:ECALC_INIT_TL=1" \
              "seedthr96_1:ECALC_INIT_TL=1 BS_SEED_THREADS=96" "seedthr96_2:ECALC_INIT_TL=1 BS_SEED_THREADS=96" \
              "seedfill0_1:ECALC_INIT_TL=1 BS_SEED_FILL=0" "seedfill0_2:ECALC_INIT_TL=1 BS_SEED_FILL=0"; do
    lab=${spec%%:*}; w=${spec#*:}
    one_run $NA q1_$lab 200 "$w" $D1 || bad=$((bad+1))
    { echo "== q1_$lab on $NA ($w): $RUN_V | $RUN_T"; grep -a '^tl ' $RD_LAST_LOG | grep -aE 'rns_init begins|plane pools done|seed thread (starts|ends)|every span|waiting for the region|pools are there|background mapping done|waited .* for chunks|joining the seed|seeds are joined|level 1 (starts|done)'; } >> $OUT/res/q1.txt
  done
  [ $bad = 0 ] && echo OK > $OUT/chain_A.status || echo "FAILED: $bad bad Q1 runs" > $OUT/chain_A.status; }
# --- chain B: Q4 NTT sweeps
chain_B() { local bad=0 n=0 lab bin rc
  : > $OUT/res/q4sweep.txt; : > $OUT/res/r3bench.txt
  for lab in real nop nop real real nop; do
    bin=tests/t_ntt; [ $lab = nop ] && bin=tests/t_ntt_nop; n=$((n+1))
    evict_node $NB
    rd_run q4s_${lab}_$n 900 srun --jobid=$J_SPX -N1 -w $NB --gpus=4 -c 192 --overlap bash -lc "$PRE ./$bin bench 31 q4sweep"; rc=$?
    { echo "== q4sweep $lab ($bin) rep $n rc $rc"; grep -a 'Q4' $RD_LAST_LOG; } >> $OUT/res/q4sweep.txt
    [ $rc = 0 ] || { alert "q4sweep $lab rc $rc (log $RD_LAST_LOG)"; bad=$((bad+1)); }
  done
  n=0
  for lab in real nop nop real real nop; do
    bin=tests/t_ntt; [ $lab = nop ] && bin=tests/t_ntt_nop; n=$((n+1))
    rd_run r3b_${lab}_$n 900 srun --jobid=$J_SPX -N1 -w $NB --gpus=4 -c 192 --overlap bash -lc "$PRE ./$bin r3bench 22 29"; rc=$?
    { echo "== r3bench $lab ($bin) rep $n rc $rc"; grep -aE '^[0-9]+ +[0-9]+ \||^3\*2\^k|r3bench' $RD_LAST_LOG; } >> $OUT/res/r3bench.txt
    [ $rc = 0 ] || { alert "r3bench $lab rc $rc (log $RD_LAST_LOG)"; bad=$((bad+1)); }
  done
  [ $bad = 0 ] && echo OK > $OUT/chain_B.status || echo "FAILED: $bad bad Q4 runs" > $OUT/chain_B.status; }
# --- chain C: NTT_SIZE_STATS + DC_STATS runs, then the dm-variance probe
chain_C() { local bad=0 i
  : > $OUT/res/sizestats.txt; : > $OUT/res/dmvar.txt
  for i in 1 2; do
    one_run $NC sizestats_r$i 200 "NTT_SIZE_STATS=1 DC_STATS=1" $D1 || bad=$((bad+1))
    { echo "== sizestats_r$i on $NC: $RUN_V | $RUN_T"; grep -aE 'ntt-size-stats|dcstats|^ *dc  *[0-9.]+ s  *digits' $RD_LAST_LOG | cut -c1-700; } >> $OUT/res/sizestats.txt
  done
  for i in 1 2 3 4 5 6; do
    one_run $NC dmvar_r$i 200 "NEWTON_DOUBLING_TS=1" $D1 || bad=$((bad+1))
    { echo "== dmvar_r$i on $NC: $RUN_V | $RUN_T | dm $RUN_DM"; grep -a 'recip ts' $RD_LAST_LOG | cut -c1-260; } >> $OUT/res/dmvar.txt
    echo "$i $RUN_DM" >> $OUT/res/dmvar_dm.txt
  done
  local med; med=$(awk '{print $2}' $OUT/res/dmvar_dm.txt | sort -n | awk '{a[NR]=$1} END{print NR%2 ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}')
  { echo "== dm per run (s): $(awk '{printf "%s ", $2}' $OUT/res/dmvar_dm.txt); median $med; blow-ups (> 1.5 x median): $(awk -v m="$med" '$2 > 1.5 * m {printf "run %s (%s) ", $1, $2}' $OUT/res/dmvar_dm.txt)"; } >> $OUT/res/dmvar.txt
  [ $bad = 0 ] && echo OK > $OUT/chain_C.status || echo "FAILED: $bad bad C runs" > $OUT/chain_C.status; }
rm -f $OUT/res/dmvar_dm.txt
# --- chain X: b_seed64 on a CPX hold (whichever of 12294 / 12332 is RUNNING with >= 10 min left for the run; waits up to 3 h in total)
chain_X() { local bad=0 w=0 j CJ rc rep cfg
  : > $OUT/res/r2.txt
  for rep in 1 2 3; do for cfg in "64410000000 10 0,5" "64410000000 576 0,288"; do
    CJ=""
    while [ -z "$CJ" ]; do
      for j in $J_CPX1 $J_CPX2; do [ "$(squeue -j $j -h -o %T 2>/dev/null)" = RUNNING ] && [ "$(left_s $j)" -ge 600 ] && { CJ=$j; break; }; done
      [ -n "$CJ" ] && break
      [ $w -ge 10800 ] && { alert "no CPX hold ($J_CPX1 / $J_CPX2) RUNNING with >= 10 min left after 3 h of waiting"; echo "FAILED: no CPX hold ($bad bad so far)" > $OUT/chain_X.status; return; }
      sleep 120; w=$((w+120)); done
    rd_run r2_r${rep}_$(echo $cfg | cut -d' ' -f2) 240 srun --jobid=$CJ -N1 -c 192 --overlap bash -lc "$PRE ./tests/b_seed64 $cfg 192 20000"; rc=$?
    { echo "== r2 rep $rep ($cfg) on CPX hold $CJ ($(squeue -j $CJ -h -o %N)) rc $rc"; grep -a '^R2\|^b_seed64' $RD_LAST_LOG | cut -c1-420; } >> $OUT/res/r2.txt
    if [ $rc != 0 ] || ! grep -aq '^R2 done: all identical' $RD_LAST_LOG; then alert "r2 rep $rep ($cfg) rc $rc or not identical (log $RD_LAST_LOG)"; bad=$((bad+1)); fi
  done; done
  [ $bad = 0 ] && echo OK > $OUT/chain_X.status || echo "FAILED: $bad bad R2 runs" > $OUT/chain_X.status; }

( chain_A ) & PA=$!
( chain_B ) & PB=$!
( chain_C ) & PC=$!
( chain_X ) & PX=$!
wait $PA $PB $PC $PX

# --- digest + verdict
{ echo "S23 results digest, $(TZ=America/New_York date), worktree $(git -C $WT rev-parse --short HEAD)"
  for f in q1 q4sweep r3bench sizestats dmvar r2; do echo; echo "######## $f"; cat $OUT/res/$f.txt 2>/dev/null; done; } > $OUT/results.txt
st=""; badc=0
for c in A B C X; do s=$(cat $OUT/chain_$c.status 2>/dev/null || echo "no status"); st="$st $c:$s"; [ "$s" = OK ] || badc=$((badc+1)); done
rd_say "chains:$st"
[ $badc = 0 ] && RD_VERDICT="SUCCESS: all 4 chains OK (digest $OUT/results.txt)" || RD_VERDICT="FAILED: $badc of 4 chains not OK:$st (digest $OUT/results.txt)"

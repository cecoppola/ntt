#!/bin/bash
# S21: five single-node experiments run in parallel on the idle holds (A37CMP.md sections 2 and 4); run from aac7 home under nohup.  NOT armed by its author.
#   Hold 12377 HOLDB (3 SPX nodes, one independent single-node job per node, srun --jobid=12377 --overlap -N1 -w <node>):
#     node 1  A37-Q1: one-node 6.441e10 digits, ECALC_INIT_TL=1 (does the seed bind init?), 2 runs
#     node 2  A37-R4: the same run with DC_STATS=1 (what is `dc`: setup / fetch / reversal / residues / T2 / joins), 2 runs; then
#             A37-Q4: tests/t_ntt vs tests/t_ntt_nop `bench 31 q4` (2^31 forward with the real vs a NOP modmul), ABBA = 4 runs
#     node 3  A37-R6: the same run with NEWTON_DOUBLING_TS=1 (per-doubling timestamps of the reciprocal chain), 2 runs
#   Hold 12294 NIC16 (CPX) or, when it is gone, its successor 12332 NIC16B: whichever is RUNNING with >= 15 min left, CPU only:
#     A37-R2: tests/b_seed64 (ecalc's decimal seed leaf vs a37v1's base-2^64 sub-ranges + 10^19 convert + schoolbook combine), at the
#             6.441e10 share for 10 nodes (ranks 0 and 5) and 576 nodes (ranks 0 and 288), 2 repeats of each
#   NOT the 10-node hold 12331 and not ~/ntt-acc (busy with S20): everything is built in a separate worktree ~/ntt-wt/s21 (detached at origin/s21,
#   or S21_REV), log ~/s21/build.log.  No digit writes (the 1e9 gates write ~1 GB each, deleted at once).
#   Gate before any experiment: (node 1) 1e9 digits identical to the reference with every new switch off, (node 2) the same with DC_STATS=1
#   NEWTON_DOUBLING_TS=1 ECALC_INIT_TL=1 on, (node 3) tests/t_ntt 24 (the real build's own test).  Any failure ends the run before the experiments.
# Outputs in ~/s21: build.log, summary.txt, results.txt (the digest: one block per experiment), res/*.txt (per-experiment extracts), log/*.log (full logs),
#   ALERT (on any problem), S21_DONE (SUCCESS: ... | FAILED: ...).
# Never cancels anything; kills only the PID rd_run started; never touches other jobs.  Fire-and-forget: the driver waits for its own chains only.
J_SPX=12377; J_CPX1=12294; J_CPX2=12332
R=$HOME/ntt-acc; WT=$HOME/ntt-wt/s21; E=$WT/ecalc; OUT=$HOME/s21
D1=64410000000          # one node's share at the 3.71e13 target (6.441e10 digits); no output file is given: nothing is written
mkdir -p $OUT/log $OUT/res $HOME/ntt-wt
rm -f $OUT/S21_DONE $OUT/ALERT $OUT/chain_*.status
alert() { { echo "$(TZ=America/New_York date) $*"; } >> $OUT/ALERT; }
early_fail() { alert "$*"; echo "FAILED: $*" > $OUT/S21_DONE; exit 0; }

# --- the worktree (never ~/ntt-acc's checkout: S20 is using its binary)
cd $R && git fetch -q origin +refs/heads/s21:refs/remotes/origin/s21 || early_fail "git fetch origin s21 failed"
REV=${S21_REV:-origin/s21}
if [ -e $WT/.git ]; then git -C $WT checkout -q --detach $REV || early_fail "worktree checkout $REV failed"
else git -C $R worktree add -q --detach $WT $REV || early_fail "git worktree add $WT $REV failed"; fi
source $WT/tools/rundriver.sh; rd_init $OUT S21_DONE
rd_say "worktree $WT at $(git -C $WT rev-parse --short HEAD) ($REV)"

# seconds left on job $1 (0 if not running)
left_s() { local t; t=$(squeue -j $1 -h -o %L -t R 2>/dev/null); [ -z "$t" ] && { echo 0; return; }
  echo "$t" | awk -F'[-:]' '{n=NF; s=$n+60*$(n-1); if(n>=3) s+=3600*$(n-2); if(n>=4) s+=86400*$(n-3); print s}'; }
fail() { alert "$*"; RD_VERDICT="FAILED: $*"; exit 0; }

# --- the SPX hold: 3 nodes, running, >= 1 h left; nodes idle and with memory for a one-node run
[ "$(squeue -j $J_SPX -h -o %T 2>/dev/null)" = RUNNING ] || fail "hold $J_SPX not RUNNING ($(squeue -j $J_SPX -h -o %T 2>&1))"
NODES=($(scontrol show hostnames "$(squeue -j $J_SPX -h -o %N)")); [ ${#NODES[@]} = 3 ] || fail "hold $J_SPX has ${#NODES[@]} nodes, need 3"
[ "$(left_s $J_SPX)" -ge 3600 ] || fail "hold $J_SPX has $(left_s $J_SPX) s left, need >= 3600"
NA=${NODES[0]}; NB=${NODES[1]}; NC=${NODES[2]}
rd_say "SPX hold $J_SPX nodes: A=$NA B=$NB C=$NC ($(left_s $J_SPX) s left)"
rd_health $J_SPX $NA $NB $NC || fail "SPX node unhealthy at start (leftover ecalc / t_* process or no meminfo)"
for nd in $NA $NB $NC; do
  av=$(timeout 60 srun --jobid=$J_SPX -N1 -w $nd --overlap awk '/MemAvailable/{print $2}' /proc/meminfo 2>/dev/null)
  [ "${av:-0}" -ge 380000000 ] || fail "$nd MemAvailable ${av:-?} kB < 380 GB (a one-node 6.441e10 run needs the node)"
done
rd_disk $HOME 20 || fail "home nearly full"

# --- build in the worktree on the login node
cd $E && source aac7env.sh >/dev/null 2>&1 && make -B -j6 SHMEM_CRAY=1 ecalc tests/t_ntt tests/t_ntt_nop tests/b_seed64 tools > $OUT/build.log 2>&1 && test -x $WT/tools/unpack_digits || fail "build failed, see $OUT/build.log: $(tail -5 $OUT/build.log | tr '\n' ' ')"
for f in ecalc tests/t_ntt tests/t_ntt_nop tests/b_seed64; do [ -x $E/$f ] || fail "build left no $f"; done
rd_say "build OK ($OUT/build.log)"

PRE="module unload rocm/7.0.3 >/dev/null 2>&1; module load $MNRUN_MODULES; cd $E;"
evict_node() { timeout 120 srun --jobid=$J_SPX -N1 -w $1 --overlap bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done' >/dev/null 2>&1; sleep 2; }
# one_run <node> <label> <expected_s> "<env words>" <digits> [<outfile>]: ecalc on one SPX node under rd_run; 0 if rc 0 and VERIFY OK.  Sets RUN_V RUN_T
one_run() { local nd=$1 lab=$2 x=$3 envw=$4 d=$5 of=${6:-}
  evict_node $nd
  rd_run $lab $x srun --jobid=$J_SPX -N1 -w $nd --gpus=4 -c 192 --overlap bash -lc "$PRE env $envw ./ecalc $d $of"; local rc=$?
  RUN_V=$(rd_verify $RD_LAST_LOG); RUN_T=$(grep -a -m1 '^total' $RD_LAST_LOG | cut -c1-140)
  rd_say "$lab on $nd: rc $rc wall ${RD_LAST_WALL}s $RUN_V | $RUN_T"
  if [ $rc != 0 ] || [ "$RUN_V" != "VERIFY OK" ]; then alert "$lab on $nd: rc $rc $RUN_V (log $RD_LAST_LOG)"; return 1; fi
  return 0; }

# --- gate: three checks in parallel, one per node
gate_digits() { # gate_digits <node> <label> "<env words>"
  local G=$OUT/$2.txt
  one_run $1 $2 120 "$3" 1000000000 $G || { rm -rf $G $G.*; echo "FAILED: $2 run" > $OUT/chain_$2.status; return; }
  local c; c=$(timeout 600 srun --jobid=$J_SPX -N1 -w $1 --overlap bash -c "$E/digcmp.sh $G $HOME/ref/e_1000000000.txt" 2>>$RD_LAST_LOG); rm -rf $G $G.*
  rd_say "$2: digits vs the 1e9 reference: $c"
  [ "$c" = identical ] && echo OK > $OUT/chain_$2.status || echo "FAILED: $2 digits $c" > $OUT/chain_$2.status; }
( gate_digits $NA gate_off "" ) &
GP1=$!
( gate_digits $NB gate_on "DC_STATS=1 NEWTON_DOUBLING_TS=1 ECALC_INIT_TL=1" ) &
GP2=$!
( rd_run gate_tntt 900 srun --jobid=$J_SPX -N1 -w $NC --gpus=4 -c 192 --overlap bash -lc "$PRE ./tests/t_ntt 24"; rc=$?
  v=$(rd_verify $RD_LAST_LOG); rd_say "gate t_ntt 24 (real modmul build) on $NC: rc $rc $v"
  [ $rc = 0 ] && [ "$v" = "VERIFY OK" ] && echo OK > $OUT/chain_gate_tntt.status || echo "FAILED: gate t_ntt rc $rc $v" > $OUT/chain_gate_tntt.status ) &
GP3=$!
wait $GP1 $GP2 $GP3
for g in gate_off gate_on gate_tntt; do
  [ "$(cat $OUT/chain_$g.status 2>/dev/null)" = OK ] || fail "gate $g: $(cat $OUT/chain_$g.status 2>/dev/null || echo 'no status')"
done
rd_say "gates passed: digits identical with every switch off and on, t_ntt 24 VERIFY OK"

# --- the chains
# A: Q1
chain_A() { local bad=0 i
  : > $OUT/res/q1.txt
  for i in 1 2; do
    one_run $NA q1_r$i 200 "ECALC_INIT_TL=1" $D1 || bad=$((bad+1))
    { echo "== q1_r$i on $NA: $RUN_V | $RUN_T"; grep -a '^tl ' $RD_LAST_LOG | grep -aE 'rns_init begins|plane pools done|seed thread (starts|ends)|every span|waiting for the region|pools are there|background mapping done|waited .* for chunks|joining the seed|seeds are joined|level 1 (starts|done)'; } >> $OUT/res/q1.txt
  done
  [ $bad = 0 ] && echo OK > $OUT/chain_A.status || echo "FAILED: $bad bad Q1 runs" > $OUT/chain_A.status; }
# B: R4 x2, then Q4 ABBA
chain_B() { local bad=0 i n=0 lab bin
  : > $OUT/res/r4.txt; : > $OUT/res/q4.txt
  for i in 1 2; do
    one_run $NB r4_r$i 200 "DC_STATS=1" $D1 || bad=$((bad+1))
    { echo "== r4_r$i on $NB: $RUN_V | $RUN_T"; grep -aE 'dcstats|^ *dc  *[0-9.]+ s  *digits|streamed' $RD_LAST_LOG | cut -c1-600; } >> $OUT/res/r4.txt
  done
  for lab in real nop nop real; do
    bin=tests/t_ntt; [ $lab = nop ] && bin=tests/t_ntt_nop
    evict_node $NB
    n=$((n+1)); rd_run q4_${lab}_$n 300 srun --jobid=$J_SPX -N1 -w $NB --gpus=4 -c 192 --overlap bash -lc "$PRE ./$bin bench 31 q4"; local rc=$?
    { echo "== q4 $lab ($bin) rc $rc"; grep -a 'Q4' $RD_LAST_LOG; } >> $OUT/res/q4.txt
    [ $rc = 0 ] || { alert "q4 $lab rc $rc (log $RD_LAST_LOG)"; bad=$((bad+1)); }
  done
  [ $bad = 0 ] && echo OK > $OUT/chain_B.status || echo "FAILED: $bad bad R4/Q4 runs" > $OUT/chain_B.status; }
# C: R6 x2
chain_C() { local bad=0 i
  : > $OUT/res/r6.txt
  for i in 1 2; do
    one_run $NC r6_r$i 200 "NEWTON_DOUBLING_TS=1" $D1 || bad=$((bad+1))
    { echo "== r6_r$i on $NC: $RUN_V | $RUN_T"; grep -a 'recip ts' $RD_LAST_LOG | cut -c1-260; } >> $OUT/res/r6.txt
  done
  [ $bad = 0 ] && echo OK > $OUT/chain_C.status || echo "FAILED: $bad bad R6 runs" > $OUT/chain_C.status; }
# X: R2 on the CPX hold (whichever of 12294 / 12332 is RUNNING with >= 15 min left; waits up to 3 h)
chain_X() { local bad=0 w=0 j CJ="" cfg rep
  : > $OUT/res/r2.txt
  while [ -z "$CJ" ]; do
    for j in $J_CPX1 $J_CPX2; do [ "$(squeue -j $j -h -o %T 2>/dev/null)" = RUNNING ] && [ "$(left_s $j)" -ge 900 ] && { CJ=$j; break; }; done
    [ -n "$CJ" ] && break
    [ $w -ge 10800 ] && { alert "no CPX hold ($J_CPX1 / $J_CPX2) RUNNING with >= 15 min left after 3 h"; echo "FAILED: no CPX hold" > $OUT/chain_X.status; return; }
    sleep 120; w=$((w+120)); done
  rd_say "R2 on CPX hold $CJ ($(squeue -j $CJ -h -o %N), $(left_s $CJ) s left, waited ${w}s)"
  for rep in 1 2; do for cfg in "64410000000 10 0,5" "64410000000 576 0,288"; do
    rd_run r2_r${rep}_$(echo $cfg | cut -d' ' -f2) 240 srun --jobid=$CJ -N1 -c 192 --overlap bash -lc "$PRE ./tests/b_seed64 $cfg 192 20000"; local rc=$?
    { echo "== r2 rep $rep ($cfg) on CPX $CJ rc $rc"; grep -a '^R2\|^b_seed64' $RD_LAST_LOG | cut -c1-420; } >> $OUT/res/r2.txt
    if [ $rc != 0 ] || ! grep -aq '^R2 done: all identical' $RD_LAST_LOG; then alert "r2 rep $rep ($cfg) rc $rc or not identical (log $RD_LAST_LOG)"; bad=$((bad+1)); fi
  done; done
  [ $bad = 0 ] && echo OK > $OUT/chain_X.status || echo "FAILED: $bad bad R2 runs" > $OUT/chain_X.status; }

( chain_A ) & PA=$!
( chain_B ) & PB=$!
( chain_C ) & PC=$!
( chain_X ) & PX=$!
wait $PA $PB $PC $PX

# --- digest + verdict
{ echo "S21 results digest, $(TZ=America/New_York date), worktree $(git -C $WT rev-parse --short HEAD)"
  for f in q1 r4 r6 q4 r2; do echo; echo "######## $f"; cat $OUT/res/$f.txt 2>/dev/null; done; } > $OUT/results.txt
st=""; badc=0
for c in A B C X; do s=$(cat $OUT/chain_$c.status 2>/dev/null || echo "no status"); st="$st $c:$s"; [ "$s" = OK ] || badc=$((badc+1)); done
rd_say "chains:$st"
[ $badc = 0 ] && RD_VERDICT="SUCCESS: all 4 chains OK (digest $OUT/results.txt)" || RD_VERDICT="FAILED: $badc of 4 chains not OK:$st"

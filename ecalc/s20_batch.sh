#!/bin/bash
# S20: D3 wait-stats profile + D2 ABBA A/B on the 10-node hold 12331 (HOLDA2), run from aac7 home under nohup. NOT armed by its author.
#   D3: 2 runs at 10 nodes x 6.441e10 digits/node (the 3.71e13 share, no write), MN_WAIT_STATS=1, config A then B.
#   D2: 8 rounds ABBA (R1 A B, R2 B A, ...), 16 runs, no stats.  A = DM_MN_LEAN=1 (DIST_CHUNKS default 8); B = A + MN_T_CHUNK_MB=2048.
# Outputs in ~/s20: build.log, ab.tsv, waitstats.txt, summary.txt, ALERT (on any problem), S20_DONE (SUCCESS: n bad runs of m | FAILED: ...).
# Never cancels anything; kills only the PID rd_run started; never touches other jobs.
export SLURM_JOB_ID=12331; J=12331; R=$HOME/ntt-acc; E=$R/ecalc; OUT=$HOME/s20
mkdir -p $OUT/log
alert() { { echo "$(TZ=America/New_York date) $*"; } >> $OUT/ALERT; }
source $R/tools/rundriver.sh; rd_init $OUT S20_DONE   # (the clone's own copy; identical on main and s20)

# --- wait for the hold to run: one blocking loop, 5-min steps, 3 h limit
w=0; until [ "$(squeue -j $J -h -o %T 2>/dev/null)" = RUNNING ]; do
  [ $w -ge 10800 ] && { alert "job $J not RUNNING after 3 h (state: $(squeue -j $J -h -o %T 2>&1))"; RD_VERDICT="FAILED: hold $J not running after 3 h"; exit 0; }
  sleep 300; w=$((w+300)); done
rd_say "job $J RUNNING after ${w}s wait"
NODES=$(scontrol show hostnames "$(squeue -j $J -h -o %N)" | tr '\n' ' '); NN=$(echo $NODES | wc -w)
[ "$NN" = 10 ] || { alert "job $J has $NN nodes, need 10"; RD_VERDICT="FAILED: $NN nodes"; exit 0; }

# --- no running ecalc of ours may use the binary (login node and the hold's nodes); never kill, just stop
busy=$(pgrep -u $USER -a -x ecalc 2>/dev/null; timeout 120 srun --jobid=$J -N10 -n10 --overlap bash -c 'pgrep -u $USER -a -x ecalc' 2>/dev/null)
[ -z "$busy" ] || { alert "ecalc processes running, not rebuilding: $busy"; RD_VERDICT="FAILED: ecalc busy before build"; exit 0; }

# --- branch s20 + build
cd $R && git fetch -q && git checkout -q s20 && git pull -q --ff-only && rd_say "ntt-acc $(git rev-parse --short HEAD) on $(git rev-parse --abbrev-ref HEAD)" || { alert "git fetch/checkout/pull s20 failed"; RD_VERDICT="FAILED: git"; exit 0; }
cd $E && source aac7env.sh >/dev/null 2>&1 && make SHMEM_CRAY=1 > $OUT/build.log 2>&1 || { alert "build failed, see $OUT/build.log: $(tail -5 $OUT/build.log)"; RD_VERDICT="FAILED: build"; exit 0; }
rd_say "build OK"

LINE="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 COMM_OFI_PLAN_CXI=1"
CA="DM_MN_LEAN=1"; CB="DM_MN_LEAN=1 MN_T_CHUNK_MB=2048"      # later assignment wins in env(1); DIST_CHUNKS deliberately unset (default 8)
D=644100000000; bad=0; total=0; consec=0
evict() { timeout 120 srun --jobid=$J -N10 -n10 --overlap bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done; rm -f $HOME/s20/gate.txt*' >/dev/null 2>&1; sleep 5; }
# run_one <label> <expected_s> <digits> <env words...> ; sets RUN_V RUN_T RUN_P ; returns 0 if clean
run_one() { local l=$1 x=$2 d=$3; shift 3; total=$((total+1))
  evict
  rd_run $l $x ./mnrun.sh 10 env $LINE "$@" ./ecalc $d; RUN_RC=$?
  RUN_V=$(rd_verify $RD_LAST_LOG); RUN_T=$(grep -a -m1 '^total' $RD_LAST_LOG | cut -c1-110); RUN_P=$(rd_peaks $RD_LAST_LOG 2>/dev/null | grep -m1 'rank 0' | grep -o 'peak=[0-9.]*GB')
  rd_say "$l: rc $RUN_RC wall ${RD_LAST_WALL}s $RUN_V | $RUN_T $RUN_P"
  if [ $RUN_RC != 0 ] || [ "$RUN_V" != "VERIFY OK" ]; then
    bad=$((bad+1)); consec=$((consec+1)); alert "$l: rc $RUN_RC $RUN_V (log $RD_LAST_LOG)"
    rd_health $J $NODES || { RD_VERDICT="FAILED: unhealthy after $l ($bad bad runs of $total)"; exit 0; }
    [ $consec -ge 2 ] && { RD_VERDICT="FAILED: 2 consecutive bad runs, last $l ($bad bad of $total)"; exit 0; }
    return 1
  fi
  consec=0; return 0; }

# --- (c) correctness gate: 2 nodes, 1e9 digits, MN_WAIT_STATS=1, VERIFY + digit compare against the reference
rd_disk $HOME 20 || { alert "home nearly full"; RD_VERDICT="FAILED: disk"; exit 0; }
G=$OUT/gate.txt
evict
rd_run gate 300 ./mnrun.sh 2 env $LINE $CA MN_WAIT_STATS=1 ./ecalc 1000000000 $G; grc=$?
gv=$(rd_verify $RD_LAST_LOG); gc=$($E/digcmp.sh $G $HOME/ref/e_1000000000.txt); rm -rf $G $G.*
gw=$(grep -ac 'wait-stats' $RD_LAST_LOG)
rd_say "gate: rc $grc $gv digits $gc wait-stats-lines $gw"
if [ $grc != 0 ] || [ "$gv" != "VERIFY OK" ] || ! grep -aq 'mn: all 2 nodes: VERIFY OK' $RD_LAST_LOG || [ "$gc" != identical ]; then
  alert "GATE FAILED: rc $grc $gv digits $gc (log $RD_LAST_LOG)"; RD_VERDICT="FAILED: correctness gate"; exit 0; fi
[ "$gw" -ge 1 ] || { alert "gate passed but no wait-stats line in the log (MN_WAIT_STATS not effective?); continuing"; }
rd_health $J $NODES || { RD_VERDICT="FAILED: unhealthy after gate"; exit 0; }

# --- (d) D3: wait-stats profile, A then B, 10 nodes, no write
: > $OUT/waitstats.txt
for c in A B; do eval "cw=\$C$c"
  run_one d3_$c 450 $D $cw MN_WAIT_STATS=1
  { echo "== d3_$c ($cw) rc $RUN_RC $RUN_V | $RUN_T $RUN_P"; grep -a 'wait-stats' $RD_LAST_LOG; } >> $OUT/waitstats.txt
done

# --- (e) D2: ABBA, 8 rounds, no stats
echo -e "round\tslot\tconfig\trc\twall_s\tverify\ttotal_line\tpeak" > $OUT/ab.tsv
for r in 1 2 3 4 5 6 7 8; do
  if [ $((r % 2)) = 1 ]; then ord="A B"; else ord="B A"; fi
  s=0; for c in $ord; do s=$((s+1)); eval "cw=\$C$c"
    run_one ab_r${r}s${s}_$c 450 $D $cw
    echo -e "$r\t$s\t$c\t$RUN_RC\t$RD_LAST_WALL\t$RUN_V\t$RUN_T\t$RUN_P" >> $OUT/ab.tsv
  done
done
RD_VERDICT="SUCCESS: $bad bad runs of $total"

#!/bin/bash
# S30 Part 4: one-node digit ladder on hold 12377 node x9000c1s6b0n0 (soak role C's node): does the x1.5-1.9 superlinear wall growth seen at 10 nodes (results/S27.md section 3) appear on ONE node?
#   (0) stops soak role C (touch ~/s26/C/STOP; waits for ~/s26/x9000c1s6b0n0_DONE newer than the touch, <= 30 min); roles A and B keep running.
#   (1) BS_LAYOUT_ONLY fit check per size on the login node ("room: ... fits 1" and node need <= 0.95 x MemAvailable of the node); a size that does not fit is skipped.
#   (2) single-node ecalc, default switches, ~/ntt-wt/s28 binaries READ-ONLY, no digit writes, VERIFY: 4.0e10, 5.0e10, 6.441e10, 7.0e10, 7.64e10 digits, two rounds (each round the five sizes in order),
#       ECALC_VERBOSE=2 ECALC_INIT_TL=1 MEM_REPORT_DEVS=1.  Modelled duration: one node ran 6.441e10 in 93 s (measured, S26 q9) -> sizes 60-130 s each, 10 runs ~ 25 min with evict/start-up.
#   (3) per-phase tables (s25_phases.awk) in phases/, ladder.tsv, ladder_table.txt (s per 1e10 digits, exponents between sizes, next to the 10-node figures of S27 section 3).
#   (4) ALWAYS at the end (trap, also after a failure, only if step 0 stopped the soak): mv ~/s26/C ~/s26/C_run2 (suffix if it exists) and restart role C:
#       cd ~/s25 && setsid nohup bash ~/s25/s26_node.sh C > ~/s26/C.nohup3 2>&1 < /dev/null &
# Files: ~/s30/ladder1n.sh, ~/s30/s30_lib.sh, ~/s30/s25_phases.awk; outputs ~/s30/L1/ (summary.txt, ladder.tsv, ladder_table.txt, phases/, res/, log/, ALERT); marker ~/s30/L1_DONE.  Touch ~/s30/L1/STOP: stop between runs.
J=12377; NODE=x9000c1s6b0n0; BUILD_REV=3941592
HD=$(cd "$(dirname "$0")" && pwd); WT=$HOME/ntt-wt/s28; E=$WT/ecalc; OUT=$HOME/s30/L1
SIZES="40000000000 50000000000 64410000000 70000000000 76400000000"; ROUNDS=2
mkdir -p $OUT/log $OUT/res $OUT/phases
rm -f $OUT/ALERT $OUT/STOP $HOME/s30/L1_DONE
source $WT/tools/rundriver.sh; rd_init $OUT DONE_UNUSED; RD_MARKER=$HOME/s30/L1_DONE; rm -f $OUT/DONE_UNUSED
source $HD/s30_lib.sh
SOAK_STOPPED=0; RESTARTED=0
restart_soak() { [ $SOAK_STOPPED = 1 ] && [ $RESTARTED = 0 ] || return 0; RESTARTED=1
  local dst=$HOME/s26/C_run2; [ -e $dst ] && dst=$dst.$(date +%s)
  mv $HOME/s26/C $dst 2>/dev/null; rm -f $dst/STOP
  rd_say "restarting soak role C (old dir -> $dst)"
  ( cd $HOME/s25 && setsid nohup bash $HOME/s25/s26_node.sh C > $HOME/s26/C.nohup3 2>&1 < /dev/null & ); sleep 3; }
table() {
  { echo "S30 one-node ladder, $(TZ=America/New_York date), node $NODE, binaries $(git -C $WT rev-parse --short HEAD) (default switches)"
    echo "10-node reference (S27 section 3, per-node share): 6.441e10 410.6 s, 7.00e10 464.4 s, 7.64e10 528.5 s (exponent 1.48 end to end; init 25-26 s flat)"
    awk -F'\t' 'NR>1 && $4>0 { n[$2]++; t[$2]+=$4; b[$2]+=$5; d[$2]+=$6; i[$2]+=$7; if(!($2 in lo)||$4<lo[$2])lo[$2]=$4; if($4>hi[$2])hi[$2]=$4 }
      END { printf "%-14s %2s %9s %7s %7s %7s %9s  %s\n","digits","n","total_s","bs","dm","init","s/1e10","min..max"
            k=0; for (x in n) ds[++k]=x; for(a=1;a<=k;a++)for(c=a+1;c<=k;c++)if(ds[c]+0<ds[a]+0){v=ds[a];ds[a]=ds[c];ds[c]=v}
            for (a=1;a<=k;a++) { x=ds[a]; m=n[x]; T[a]=t[x]/m; printf "%-14s %2d %9.1f %7.1f %7.1f %7.1f %9.2f  %.1f..%.1f\n", x, m, T[a], b[x]/m, d[x]/m, i[x]/m, T[a]/(x/1e10), lo[x], hi[x] }
            for (a=2;a<=k;a++) printf "exponent %s -> %s: wall^%.2f\n", ds[a-1], ds[a], log(T[a]/T[a-1])/log(ds[a]/ds[a-1])
            if (k>1) printf "exponent end to end: wall^%.2f\n", log(T[k]/T[1])/log(ds[k]/ds[1]) }' $OUT/ladder.tsv
  } > $OUT/ladder_table.txt; }
trap 'table; restart_soak; rd_finish' EXIT
check_binary 'ntt-size-stats ECALC_INIT_TL' "tests/t_ntt"
cd $E || fail "cd $E"
source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || fail "aac7env.sh gave no MNRUN_MODULES"
[ "$(squeue -j $J -h -o %T 2>/dev/null)" = RUNNING ] || fail "hold $J not RUNNING"
[ "$(left_s $J)" -ge 7200 ] || fail "hold $J has $(left_s $J) s left, need >= 7200"
PRE="module unload rocm/7.0.3 >/dev/null 2>&1; module load $MNRUN_MODULES; cd $E;"
N() { timeout ${NT:-120} srun --jobid=$J -N1 -w $NODE --overlap bash -c "$*" < /dev/null; }
evict_node() { N 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done' >/dev/null 2>&1; sleep 2; }

# --- (0) stop soak role C
if [ -d $HOME/s26/C ] && pgrep -u $USER -f "s26_node.sh C" >/dev/null; then
  touch $HOME/s26/C/STOP; cp /dev/null $OUT/stop_stamp; rd_say "soak C: STOP touched; waiting for $HOME/s26/${NODE}_DONE"
  w=0; while :; do
    if [ -e $HOME/s26/${NODE}_DONE ] && [ $HOME/s26/${NODE}_DONE -nt $OUT/stop_stamp ]; then break; fi
    pgrep -u $USER -f "s26_node.sh C" >/dev/null || break
    [ $w -ge 1800 ] && fail "soak C did not stop within 30 min (STOP left in place; nothing restarted)"
    sleep 20; w=$((w+20)); done
  SOAK_STOPPED=1; rd_say "soak C stopped after ${w}s: $(tail -1 $HOME/s26/C/summary.txt 2>/dev/null | cut -c1-160)"
else rd_say "soak C is not running: nothing to stop (it will not be restarted by this script)"; fi
rd_health $J $NODE || fail "$NODE unhealthy after the soak stop (leftover ecalc / t_* process or no meminfo)"
av=$(N 'awk "/MemAvailable/{print \$2}" /proc/meminfo'); rd_say "MemAvailable on $NODE: $av kB"

# --- (1) fit check
FIT=""
for d in $SIZES; do
  out=$(BS_LAYOUT_ONLY=$d:1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 ECALC_MEM_GUARD_GB=6 timeout 120 ./ecalc $d 2>&1 | grep -a '^room:' | head -1)
  nd=$(echo "$out" | sed -n 's/.* node \([0-9]*\) budget.*/\1/p'); fits=$(echo "$out" | sed -n 's/.*fits \([01]\).*/\1/p')
  if [ "$fits" = 1 ] && [ -n "$nd" ] && [ $(( nd / 1000 )) -le $(( ${av:-0} * 95 / 100 )) ]; then FIT="$FIT $d"; rd_say "layout $d: node need $nd B fits"
  else rd_say "layout $d: SKIPPED (fits '$fits', node need '$nd' B, MemAvailable $av kB)"; fi; done
[ -n "$FIT" ] || fail "no size fits"

# --- (2) the ladder
one_run() { local lab=$1 x=$2 envw=$3 d=$4
  evict_node
  rd_run $lab $x srun --jobid=$J -N1 -w $NODE --gpus=4 -c 192 --overlap bash -lc "$PRE env $envw ./ecalc $d"; local rc=$?
  RUN_V=$(rd_verify $RD_LAST_LOG); RUN_T=$(grep -a -m1 '^total' $RD_LAST_LOG | cut -c1-160)
  rd_say "$lab on $NODE: rc $rc wall ${RD_LAST_WALL}s $RUN_V | $RUN_T"
  if [ $rc != 0 ] || [ "$RUN_V" != "VERIFY OK" ]; then alert "$lab on $NODE: rc $rc $RUN_V (log $RD_LAST_LOG)"; return 1; fi
  return 0; }
echo -e "label\tdigits\trep\ttotal_s\tbs\tdm\tinit\tpeak" > $OUT/ladder.tsv
bad=0
for r in $(seq $ROUNDS); do for d in $FIT; do
  [ -e $OUT/STOP ] && { rd_say "STOP file"; break 2; }
  [ "$(left_s $J)" -ge 3600 ] || { rd_say "hold $J short: stop"; break 2; }
  lab=l1_${d}_r$r
  if one_run $lab 200 "ECALC_VERBOSE=2 ECALC_INIT_TL=1 MEM_REPORT_DEVS=1" $d; then
    grep -aE '^total|^tl |^bs  |^bs: level|^dm  |VERIFY' $RD_LAST_LOG | cut -c1-500 > $OUT/res/$lab.txt; awk -f $HD/s25_phases.awk $RD_LAST_LOG > $OUT/phases/$lab.txt 2>/dev/null
    tl=$(grep -a -m1 '^total' $RD_LAST_LOG)
    pk=$(rd_peaks $RD_LAST_LOG 2>/dev/null | grep -m1 'rank 0' | grep -o 'peak=[0-9.]*GB')
    echo -e "$lab\t$d\t$r\t$(echo "$tl" | awk '{print $2}')\t$(echo "$tl" | sed -n 's/.*(bs \([0-9.]*\) .*/\1/p')\t$(echo "$tl" | sed -n 's/.*+ dm \([0-9.]*\) .*/\1/p')\t$(echo "$tl" | sed -n 's/.*init \([0-9.]*\);.*/\1/p')\t$pk" >> $OUT/ladder.tsv
    bad=0
  else bad=$((bad+1)); [ $bad -ge 2 ] && fail "2 consecutive bad runs, last $lab"; fi
  rd_health $J $NODE || fail "$NODE unhealthy after $lab"
done; table; done
RD_VERDICT="SUCCESS: ladder done ($(grep -c . $OUT/ladder.tsv) rows incl. header); table $OUT/ladder_table.txt; soak C restarted"

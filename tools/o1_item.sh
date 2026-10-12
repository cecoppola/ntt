#!/bin/bash
# aac7q queue item O1 (TASKS 42, results/O1.md): overnight 6-node paired ABBA, MN_GROUPS default (2,6) = arm A vs single level 6 = arm B.
# Copy to ~/aac7q/queue/60_o1_abba_groups.sh (queued by writing a temp name and mv).  STUB=1: dry run without Slurm (no real runs).
# OWNER=o1
# NODES=6
# EST_MIN=130
# APPROVED=yes
# NOT_AFTER=1791802800
# EXCLUSIVE=0
# Needs: ~/o1/o1_stats.py, ~/o1/envprobe.sh (copy of tools/envprobe.sh), build in ~/ntt-wt/o1 (main 0298fe2, SHMEM, -DCOMM_SHMEM in ~/o1_build.log).
# Output ~/o1/run/ : totals.tsv (see o1_stats.py), r<seq>_<arm>.log, grp<seq>.pre/.post, drv.log, summary.txt, O1_DONE (marker).
set -u
STUB=${STUB:-0}
O1=${O1_HOME:-$HOME/o1}; WT=${WT:-$HOME/ntt-wt/o1}; OUT=${O1_OUT:-$O1/run}; EP=$O1/envprobe.sh; ST=$O1/o1_stats.py
J=${SLURM_JOB_ID:-}; NN=${AQ_NODES:-6}
BUDGET_S=${BUDGET_S:-7500}   # driver budget from item start (125 min); the item header EST is 130 min, the daemon kills at 200 min
RUN_S=${RUN_S:-340}          # assumed duration of one run incl. NIC snapshots; a quartet is started only if 4*RUN_S is left
MINR=${MINR:-3} HW=${HW:-5} MAXQ=${MAXQ:-8} FIRST=${FIRST:-ABBA}
D=$((NN * 64410000000))      # 6.441e10 digits per node
LINE="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=2048 COMM_LAYER_INTER2=1 COMM_LAYER_VSLOT_POOL=1 COMM_LAYER_VSLOT_SHARE=1 DM_MN_LEAN=1 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ECALC_LOG_CLOCKS=1 COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1"
mkdir -p "$OUT"; rm -f "$OUT/O1_DONE" "$OUT/totals.tsv"; : > "$OUT/summary.txt"; : > "$OUT/totals.tsv"
T0=$(date +%s)
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a "$OUT/drv.log"; }
finish() { # $1 = SUCCESS|FAILED|BLOCKED, $2 = reason
  if [ "$STUB" != 1 ] && [ -n "$J" ]; then squeue -s -j "$J" -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 | sed 's/^/step still listed: /' | tee -a "$OUT/drv.log"; fi
  { echo "== O1 final ($1: $2) =="; python3 -I "$ST" "$OUT/totals.tsv" --min-rounds "$MINR" --hw "$HW"; } > "$OUT/summary.txt" 2>&1
  cat "$OUT/summary.txt" >> "$OUT/drv.log"
  echo "$1 $2 $(date +%s)" > "$OUT/O1_DONE"; }
trap 'say "item interrupted"; finish FAILED interrupted; exit 1' INT TERM
if [ "$STUB" != 1 ]; then
  [ -n "$J" ] && [ "$(squeue -j "$J" -h -o %T)" = RUNNING ] || { say "hold '$J' not running"; finish BLOCKED "no hold"; exit 2; }
  H=$(git -C "$WT" rev-parse --short=7 HEAD 2>/dev/null)
  [ "$H" = 0298fe2 ] || { say "worktree $WT at '$H', expected 0298fe2"; finish BLOCKED "wrong commit"; exit 2; }
  # binary must be a SHMEM build of that tree; rebuild (login node) when the log or binary is missing
  if [ ! -x "$WT/ecalc/ecalc" ] || ! grep -aq -e -DCOMM_SHMEM "$HOME/o1_build.log" 2>/dev/null; then
    say "rebuilding with SHMEM"; ( cd "$WT/ecalc" && source aac7env.sh >/dev/null 2>&1; module load cray-dsmml cray-openshmemx >/dev/null 2>&1
      make -B -j16 SHMEM_CRAY=1 GMP_HOME=$HOME/gmp > "$HOME/o1_build.log" 2>&1 ) < /dev/null
    grep -aq -e -DCOMM_SHMEM "$HOME/o1_build.log" && [ -x "$WT/ecalc/ecalc" ] || { say "build failed or lacks -DCOMM_SHMEM"; finish BLOCKED build; exit 2; }
  fi
  cd "$WT/ecalc" || exit 2; source aac7env.sh >/dev/null 2>&1
  export MNRUN_CPUS_PER_TASK=auto SLURM_JOB_ID=$J
  say "O1: hold $J ($NN nodes: $(squeue -j "$J" -h -o %N)); main $(git -C "$WT" log --oneline -1 | cut -c1-60); $D digits; budget $BUDGET_S s"
else
  say "O1 STUB: $NN nodes, $D digits, budget $BUDGET_S s"
fi

one_run() { # $1 seq $2 arm $3 round -> appends a line to totals.tsv
  local i=$1 k=$2 rnd=$3 lg=$OUT/r${1}_$2.log gr="" left tot init rate ok grp F rc t0
  [ "$k" = B ] && gr=6
  left=$(( BUDGET_S - ($(date +%s) - T0) ))
  if [ "$STUB" = 1 ]; then
    t0=$SECONDS; rc=0
    tot=$(awk -v a=$k -v s=$RANDOM 'BEGIN{srand(s); printf "%.2f", 250 + (a=="B"?-15:0) + (rand()-0.5)*40}')
    init=28.5; rate=10.1; ok=1; grp=$([ $((RANDOM % 2)) = 0 ] && echo A || echo B); F=500
    grp="$grp"; sleep 1
  else
    bash "$EP" --snap "$OUT/grp${i}.pre" > /dev/null 2>&1
    t0=$SECONDS
    timeout 700 ./mnrun.sh "$NN" env $LINE ${gr:+MN_GROUPS=$gr} ./ecalc $D > "$lg" 2>&1 < /dev/null; rc=$?
    bash "$EP" --snap "$OUT/grp${i}.post" > /dev/null 2>&1
    read -r grp F < <(bash "$EP" --group "$OUT/grp${i}.pre" "$OUT/grp${i}.post" 2>&1 | tail -1); : "${grp:=NA}" "${F:=NA}"
    tot=$(grep -a -m1 -E '^total +[0-9.]+ s' "$lg" | sed -E 's/^total +([0-9.]+) s.*/\1/')
    init=$(grep -a -m1 -E '^total +[0-9.]+ s' "$lg" | sed -nE 's/.*init ([0-9.]+);.*/\1/p')
    rate=$(grep -a -E 'all-to-all .* = [0-9.]+ GB/s per APU thread' "$lg" | sed -E 's/.* = ([0-9.]+) GB\/s per APU thread.*/\1/' | awk '{s+=$1;n++} END{if(n) printf "%.2f", s/n}')
    ok=0; grep -aq -E "mn: all [0-9]+ nodes: VERIFY OK" "$lg" && [ "$rc" = 0 ] && [ -n "${tot:-}" ] && ok=1
  fi
  say "run=$i round=$rnd arm=$k groups=${gr:-default} rc=$rc total=${tot:-none} init=${init:-NA} rate=${rate:-NA} nic=$grp fgfc_M=${F:-NA} ok=$ok elapsed=$((SECONDS - t0))"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$i" "$k" "$rnd" "${tot:-none}" "${init:-NA}" "${rate:-NA}" "${grp:-NA}" "${F:-NA}" "$ok" >> "$OUT/totals.tsv"
}

i=0; q=0; O=$FIRST; stopwhy="round cap"; consec_bad=0
while [ $q -lt $MAXQ ]; do
  left=$(( BUDGET_S - ($(date +%s) - T0) ))
  [ $left -ge $((4 * RUN_S)) ] || { stopwhy="time: $left s left < 4 runs"; break; }
  [ -e "$OUT/STOP" ] && { stopwhy="STOP file"; break; }
  q=$((q + 1))
  for k in $(echo $O | fold -w1); do i=$((i + 1)); one_run $i $k $q; done
  [ "$O" = ABBA ] && O=BAAB || O=ABBA
  bad=$(awk -F'\t' -v q=$q '$3==q && $9!=1' "$OUT/totals.tsv" | wc -l)
  if [ "$bad" -ge 3 ]; then consec_bad=$((consec_bad + 1)); else consec_bad=0; fi
  [ $consec_bad -ge 2 ] && { stopwhy="two rounds with >= 3 failed runs"; break; }
  st=$(python3 -I "$ST" "$OUT/totals.tsv" --min-rounds "$MINR" --hw "$HW" | tee "$OUT/last_stats.txt" | tail -1)
  say "after round $q: $st"
  case "$st" in "STOP yes"*) stopwhy="stop rule: ${st#STOP yes }"; break;; esac
done
say "loop ended: $stopwhy after $q rounds ($i runs)"
nok=$(awk -F'\t' '$9==1' "$OUT/totals.tsv" | wc -l)
if [ "$nok" -ge 8 ]; then finish SUCCESS "$stopwhy; $q rounds, $nok valid runs"; else finish FAILED "only $nok valid runs; $stopwhy"; fi

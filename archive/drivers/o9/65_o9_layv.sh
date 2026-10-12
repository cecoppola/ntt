#!/bin/bash
# aac7q queue item O9 (TASKS 12, results/O9.md): why the non-power-of-2 `layv` exchange looks slower than `layeq`.  Diagnostic, no code change:
# main 0298fe2 SHMEM build (~/ntt-wt/o9, log ~/o9/build.log with -DCOMM_SHMEM), the aac7 LINE of e16_headline.sh + COMM_XSTATS=1 MN_WAIT_STATS=1
# ECALC_INIT_TL=1 COMM_LAYER_STATS=1, 6.441e10 digits per node, no digit output.  Arm A = defaults; arm B = + DIST_GEN=1 (the general map, i.e.
# the `layv` exchange, also at power-of-two group sizes: at 4 nodes every level becomes layv; at 6 nodes the 2-node level of the default [2,6]).
# Order 4A 4B 6A 6B 4B 4A [6B 6A]: each run starts only if its assumed length fits the budget (the bracket runs come last).
# Copy to ~/aac7q/queue/65_o9_layv.sh (temp name + mv).  STUB=1: dry run without Slurm.
# OWNER=o9
# NODES=6
# EST_MIN=45
# APPROVED=yes
# EXCLUSIVE=0
# Output ~/o9/run/ : runs.tsv, <label>.log, grp<seq>.pre/.post, drv.log, summary.txt, O9_DONE (marker, written last: SUCCESS|PARTIAL|FAILED|BLOCKED ...).
set -u
STUB=${STUB:-0}
O9=${O9_HOME:-$HOME/o9}; WT=${WT:-$HOME/ntt-wt/o9}; R=${O9_OUT:-$O9/run}; EP=$O9/envprobe.sh; ST=$O9/o9_sum.py
J=${SLURM_JOB_ID:-}; DPN=64410000000
BUDGET_S=${BUDGET_S:-2640}                   # 44 min from item start
RUN4=${RUN4:-300} RUN6=${RUN6:-430}          # assumed run lengths incl. launch + NIC snapshots (S48: 4 n total 182 s, 6 n 283-304 s; init and the plan pass on top)
PLAN=${PLAN:-"4A 4B 6A 6B 4B 4A 6B 6A"}
STATS="COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1 COMM_LAYER_STATS=1"
mkdir -p "$R"; rm -f "$R/O9_DONE"; : > "$R/summary.txt"; printf 'seq\tlabel\tnodes\tarm\trc\twall\tgroup\n' > "$R/runs.tsv"
T0=$(date +%s)
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a "$R/drv.log"; }
finish() { # $1 status $2 reason
  if [ "$STUB" != 1 ] && [ -n "$J" ]; then squeue -s -j "$J" -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 | sed 's/^/step still listed: /' | tee -a "$R/drv.log"; fi
  { echo "== O9 ($1: $2) $(TZ=America/New_York date '+%m-%d %H:%M ET')"; python3 -I "$ST" "$R"; } > "$R/summary.txt" 2>&1
  echo "$1 $2 $(date +%s)" > "$R/O9_DONE"; }
trap 'say "item interrupted"; finish FAILED interrupted; exit 1' INT TERM
if [ "$STUB" != 1 ]; then
  [ -n "$J" ] && [ "$(squeue -j "$J" -h -o %T)" = RUNNING ] || { say "hold '$J' not running"; finish BLOCKED "no hold"; exit 2; }
  have=$(squeue -j "$J" -h -o %D); [ "$have" -ge 6 ] 2>/dev/null || { say "hold has $have nodes"; finish BLOCKED "hold too small"; exit 2; }
  H=$(git -C "$WT" rev-parse --short=7 HEAD 2>/dev/null); [ "$H" = 0298fe2 ] || { say "worktree at '$H'"; finish BLOCKED "wrong commit"; exit 2; }
  grep -aq -e -DCOMM_SHMEM "$O9/build.log" && [ -x "$WT/ecalc/ecalc" ] || { say "no SHMEM build"; finish BLOCKED build; exit 2; }
  cd "$WT/ecalc" || exit 2; source aac7env.sh > /dev/null 2>&1
  LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ "$WT/ecalc/e16_headline.sh")
  [ -n "$LINE" ] || { say "no LINE"; finish BLOCKED "no LINE"; exit 2; }
  unset SLURM_CPUS_PER_TASK SLURM_NTASKS SLURM_NPROCS SLURM_NTASKS_PER_NODE SLURM_TASKS_PER_NODE SLURM_NNODES SLURM_JOB_NUM_NODES SLURM_NODELIST \
        SLURM_JOB_NODELIST SLURM_GPUS_PER_NODE SLURM_TRES_PER_TASK SLURM_MEM_PER_NODE SLURM_MEM_PER_CPU SLURM_JOB_CPUS_PER_NODE SLURM_DISTRIBUTION SLURM_CPU_BIND SLURM_GPUS
  say "O9: hold $J ($(squeue -j "$J" -h -o %N)); binary md5 $(md5sum ecalc | cut -c1-12); budget $BUDGET_S s; line: $LINE $STATS"
else
  LINE="STUB=1"; say "O9 STUB: budget $BUDGET_S s"
fi
evict() { python3 -c "import os,sys; fd=os.open(sys.argv[1],os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)" "$1" 2>/dev/null; }
seq=0; ok=0; bad=0
for it in $PLAN; do
  N=${it%?}; A=${it#?}; [ "$N" = 4 ] && est=$RUN4 || est=$RUN6
  left=$(( BUDGET_S - ($(date +%s) - T0) ))
  if [ $left -lt $est ]; then say "$it: $left s left < $est s: not started (and the rest)"; break; fi
  seq=$((seq + 1)); lab=r${seq}_n${N}${A}; L=$R/$lab.log; gx=""; [ "$A" = B ] && gx="DIST_GEN=1"
  tmo=$(( left + 60 )); [ $tmo -gt $(( 2 * est )) ] && tmo=$(( 2 * est ))
  t1=$(date +%s)
  if [ "$STUB" = 1 ]; then
    { echo "0: VERIFY OK"; echo "0: total   $((150 + 20 * N)).50 s   (bs 1 + dm 2; init 30.0; other 0.1)";
      echo "0: xstats pe 0 phase dm kind layv: n 10 rounds 10 GB 100.0 t_post 12.0 s t_call 7.0 s t_wait 5.0 s roff_wait 2.0 s sig_wait 1.0 s (x)"; } > "$L"; rc=0; grp=NA; sleep 1
  else
    for f in $HOME/ref/e_1000000000.txt $HOME/ref/e_1e11.out; do evict "$f"; done
    OUT=$R bash "$EP" --snap "$R/grp$seq.pre" > /dev/null 2>&1
    SLURM_JOB_ID=$J MNRUN_NODES=$N MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) \
      timeout $tmo ./mnrun.sh $N env $LINE $STATS $gx ECALC_SEGV_TRACE=1 ./ecalc $((N * DPN)) > "$L" 2>&1 < /dev/null; rc=$?
    OUT=$R bash "$EP" --snap "$R/grp$seq.post" > /dev/null 2>&1
    grp=$(bash "$EP" --group "$R/grp$seq.pre" "$R/grp$seq.post" 2>/dev/null | awk '{print $1}'); grp=${grp:-NA}
  fi
  wall=$(( $(date +%s) - t1 ))
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $seq $lab $N $A $rc $wall $grp >> "$R/runs.tsv"
  tot=$(sed -E 's/^ *[0-9]+: //' "$L" | grep -a -m1 '^total' | awk '{print $2}')
  say "$lab: rc $rc wall ${wall}s total ${tot:-NA} group $grp $gx"
  if [ $rc = 0 ] && grep -aq 'VERIFY OK' "$L"; then ok=$((ok + 1)); bad=0; else bad=$((bad + 1)); fi
  if [ "$STUB" != 1 ]; then st=$(squeue -s -j "$J" -h 2>/dev/null | grep -v '\.batch\|\.extern' | wc -l); [ "$st" -gt 0 ] && say "WARNING: $st step(s) still listed after $lab"; fi
  [ $bad -ge 2 ] && { say "2 bad runs in a row: stop"; break; }
done
[ $ok -ge 6 ] && finish SUCCESS "ok_runs=$ok of $seq" || finish PARTIAL "ok_runs=$ok of $seq"
say "done: $(cat $R/O9_DONE)"

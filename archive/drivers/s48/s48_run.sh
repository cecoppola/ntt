#!/bin/bash
# s48_run.sh <nodes> <runs> : S48 scaling study point on aac7 - <runs> timed runs of ecalc at 6.441e10 digits per node on the first <nodes>
# nodes of the allocation $SLURM_JOB_ID (a hold, or the 6-node job J6 itself), current defaults = the aac7 launch line (LINE of
# ecalc/e16_headline.sh, parsed) + COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1.  No digit output (no output file argument).
#   Output dir $S48DIR (default ~/s48; set via S48_OUT, never via an inherited OUT): log/n<N>_r<k>.log (raw, rank-labelled), summary.tsv (one row per run), done_n<N> (marker, written last).
#   DRY=1: stub runs (no srun, no SLURM_JOB_ID needed).  D_EPOCH (default 1791723386 = D): a run that cannot finish by D is not started.
#   One retry (only) when a run dies in the first 150 s without a total (the known Cray shmem_init failure); both tries are in the table.
#   Kills only its own timeout'ed srun child (timeout sends TERM to that PID); never scancel.
N=${1:?nodes}; R=${2:?runs}; DRY=${DRY:-0}; S48DIR=${S48_OUT:-$HOME/s48}; D_EPOCH=${D_EPOCH:-1791723386}
EC=${EC:-$HOME/s48/ecalc_s48}; E=${E:-$HOME/ntt-wt/s48/ecalc}; DPN=64410000000
case $N in 1) TMO=330;; 2) TMO=480;; 4) TMO=660;; 6) TMO=900;; *) TMO=900;; esac
mkdir -p "$S48DIR/log"; MARK=$S48DIR/done_n$N; rm -f "$MARK"
say() { echo "s48_run n$N: $(TZ=America/New_York date '+%m-%d %H:%M:%S %Z') $*"; }
if [ "$DRY" != 1 ]; then
  [ -n "$SLURM_JOB_ID" ] || { say "no SLURM_JOB_ID"; echo "FAILED no SLURM_JOB_ID" > "$MARK"; exit 1; }
  J=$SLURM_JOB_ID; have=$(squeue -j "$J" -h -o %D); [ "$have" -ge "$N" ] 2>/dev/null || { say "hold $J has $have node(s), need $N"; echo "FAILED hold too small" > "$MARK"; exit 1; }
  [ -x "$EC" ] && [ -x "$E/mnrun.sh" ] || { say "binary or mnrun.sh missing"; echo "FAILED missing binary" > "$MARK"; exit 1; }
  cd "$E" && . ./aac7env.sh > /dev/null 2>&1
  LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ "$E/e16_headline.sh")
  [ -n "$LINE" ] || { say "no LINE"; echo "FAILED no LINE" > "$MARK"; exit 1; }
else
  J=0; LINE="STUB_LINE=1"
fi
STATS="COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1"
unset SLURM_CPUS_PER_TASK SLURM_NTASKS SLURM_NPROCS SLURM_NTASKS_PER_NODE SLURM_TASKS_PER_NODE SLURM_NNODES SLURM_JOB_NUM_NODES SLURM_NODELIST \
      SLURM_JOB_NODELIST SLURM_GPUS_PER_NODE SLURM_TRES_PER_TASK SLURM_MEM_PER_NODE SLURM_MEM_PER_CPU SLURM_JOB_CPUS_PER_NODE SLURM_DISTRIBUTION SLURM_CPU_BIND SLURM_GPUS
evict() { python3 -c "import os,sys; fd=os.open(sys.argv[1],os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)" "$1" 2>/dev/null; }
S0=$(wc -l < $S48DIR/summary.tsv 2>/dev/null || echo 0); DIG=$((N * DPN)); bad=0; k=0; tries=0
say "start: $R runs, $DIG digits, hold $J, DRY=$DRY, binary $(md5sum "$EC" 2>/dev/null | cut -c1-12), line: $LINE $STATS"
while [ $k -lt $R ]; do
  k=$((k + 1)); try=1
  while :; do
    lab=n${N}_r${k}$([ $try = 2 ] && echo b)
    if [ "$DRY" != 1 ] && [ $(( $(date +%s) + TMO )) -gt "$D_EPOCH" ]; then say "$lab: would cross D, not started"; break 2; fi
    for f in $HOME/ref/e_1000000000.txt $HOME/ref/e_1e11.out; do [ "$DRY" = 1 ] || evict "$f"; done   # protocol: reference out of the page cache (none is read here)
    t1=$(date +%s); L=$S48DIR/log/$lab.log
    if [ "$DRY" = 1 ]; then
      { echo "0: all $N nodes: VERIFY OK"; echo "0: total   $((70 + 10 * N)).50 s   (bs 1 + dm 2)"; echo "0: xstats pe 0 phase bs kind redist: n 4 GB 1.0 t_post 1.0"; } > $L; rc=0
    else
      SLURM_JOB_ID=$J MNRUN_NODES=$N MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) \
        timeout $TMO ./mnrun.sh $N env $LINE $STATS ECALC_SEGV_TRACE=1 $EC $DIG > $L 2>&1 < /dev/null; rc=$?
    fi
    wall=$(( $(date +%s) - t1 ))
    pl=$(sed -E 's/^ *[0-9]+: //' $L); V=none; echo "$pl" | grep -aqE 'VERIFY OK' && V="VERIFY OK"
    tot=$(echo "$pl" | grep -a -m1 '^total' | awk '{print ($2 ~ /^[0-9.]+$/) ? $2 : "NA"}'); tot=${tot:-NA}
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$N" "$lab" "$rc" "$wall" "$V" "$tot" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" >> $S48DIR/summary.tsv
    say "$lab: rc $rc wall ${wall}s $V total $tot"
    if [ $rc = 0 ] && [ "$V" = "VERIFY OK" ]; then bad=0; break; fi
    bad=$((bad + 1))
    if [ $try = 1 ] && [ $tries = 0 ] && [ $wall -lt 150 ] && [ "$tot" = NA ]; then try=2; tries=1; say "$lab: early death, one retry"; sleep 20; continue; fi
    break
  done
  [ $bad -ge 2 ] && { say "2 bad runs in a row: stop"; break; }
done
ok=$(tail -n +$((S0 + 1)) $S48DIR/summary.tsv | awk -F'\t' -v n=$N '$1==n && $3==0 && $5=="VERIFY OK"' | wc -l)
echo "$([ $ok -ge $R ] && echo DONE || echo PARTIAL) n$N ok_runs=$ok of $R $(TZ=America/New_York date '+%m-%d %H:%M %Z')" > "$MARK"
say "marker: $(cat $MARK)"
[ $ok -ge $R ]

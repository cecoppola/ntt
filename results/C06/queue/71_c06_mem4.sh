#!/bin/bash
# OWNER=c06
# NODES=4
# EST_MIN=28
# APPROVED=coordinator-2026-10-11
# c06 M3 memory measurement (results/C06.md): 4 nodes x 6.441e10 digits per node (2.5764e11 total), the aac7 line (X2 + SHARE + T2048 ...) + DIST_GEN=1 (general map at g = 2 and 4, so the v-slots really exist),
#  A = as is, B = + COMM_LAYER_VSLOT_RELEASE=1.  MEM_REPORT_DEVS=1 and COMM_OFI_VERBOSE=1: the per-node device peak (sum of the 4 APUs' driver view, max over phases: results/S36_driver/s36_devpeak.py) and the comm_ofi pool peaks.
#  Expectation (modelled): the comm_ofi pool 4608 -> 2560 MiB per APU = -8.6 GB per node device in B; wall unchanged.  If A fails (memory), both are repeated at 5e10 per node (the S36 size).  No digit writes.
# Uses the worktree build ~/ntt-wt/c06 (built on the login node).  Outputs only ~/sess1011c/c06/M/.  Exits by itself.  STUB=1: dry run (no srun).
WT=$HOME/ntt-wt/c06; E=$WT/ecalc; O=$HOME/sess1011c/c06/M; mkdir -p $O/log; : > $O/summary.txt
for f in $E/ecalc $E/mnrun.sh $E/e16_headline.sh $WT/results/S36_driver/s36_devpeak.py; do [ -e $f ] || { echo "missing $f"; exit 2; }; done
strings $E/ecalc | grep -q COMM_LAYER_VSLOT_RELEASE || { echo "ecalc built without M3"; exit 2; }
[ -z "$(find $E -maxdepth 1 \( -name '*.c' -o -name '*.h' \) -newer $E/ecalc | head -1)" ] || { echo "a source is newer than the ecalc binary"; exit 2; }
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ $E/e16_headline.sh); [ -n "$LINE" ] || { echo "no LINE"; exit 2; }
if [ "${STUB:-0}" = 1 ]; then echo "STUB ok: would run 2-4 x ecalc on 4 nodes of \$SLURM_JOB_ID; line: $(echo $LINE | cut -c1-80)...; outputs $O"; bash -n $0 && echo syntax ok; exit 0; fi
: ${SLURM_JOB_ID:?need the runner allocation}
J=$SLURM_JOB_ID; T0=$(date +%s)
cd $E; source aac7env.sh >/dev/null 2>&1
unset SLURM_CPUS_PER_TASK SLURM_NTASKS SLURM_NPROCS SLURM_NTASKS_PER_NODE SLURM_TASKS_PER_NODE SLURM_NNODES SLURM_JOB_NUM_NODES SLURM_NODELIST SLURM_JOB_NODELIST SLURM_GPUS_PER_NODE SLURM_TRES_PER_TASK SLURM_MEM_PER_NODE SLURM_MEM_PER_CPU SLURM_JOB_CPUS_PER_NODE SLURM_DISTRIBUTION SLURM_CPU_BIND SLURM_GPUS
run() { # label digits-per-node env...
  local lab=$1 dpn=$2; shift 2
  [ $(( $(date +%s) - T0 )) -lt 1260 ] || { echo "$lab skipped (time)" | tee -a $O/summary.txt; RC=99; return; }
  local t1=$(date +%s)
  SLURM_JOB_ID=$J MNRUN_NODES=4 MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) timeout 700 ./mnrun.sh 4 env $LINE ECALC_SEGV_TRACE=1 MEM_REPORT_DEVS=1 COMM_OFI_VERBOSE=1 DIST_GEN=1 "$@" ./ecalc $((4 * dpn)) < /dev/null > $O/log/$lab.log 2>&1; RC=$?
  sed -E 's/^ *[0-9]+: //' $O/log/$lab.log > $O/log/$lab.plain
  local v=none; grep -aqE 'VERIFY OK' $O/log/$lab.plain && v="VERIFY OK"
  local tot=$(grep -a -m1 '^total' $O/log/$lab.plain | awk '{print $2}')
  echo "$lab rc=$RC $v wall=$(( $(date +%s) - t1 ))s total=${tot:-NA} | $(python3 $WT/results/S36_driver/s36_devpeak.py $O/log/$lab.plain) | $*" | tee -a $O/summary.txt
  grep -a -E "pool peak" $O/log/$lab.plain | sort | uniq | head -4 | cut -c1-200 >> $O/summary.txt
  grep -a -m1 "COMM_SHMEM_POOL_MB=" $O/log/$lab.plain | cut -c1-250 >> $O/summary.txt; }
run A_6441 64410000000
RA=$RC; run B_6441 64410000000 COMM_LAYER_VSLOT_RELEASE=1
if [ $RA != 0 ] || ! grep -aq 'VERIFY OK' $O/log/A_6441.plain; then
  run A_5e10 50000000000
  run B_5e10 50000000000 COMM_LAYER_VSLOT_RELEASE=1
fi
echo "DONE $(date)" >> $O/summary.txt

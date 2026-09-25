#!/bin/bash
# N414 A4(a): t_mn_grid at 3 real nodes over SOS (and t_comm at 3 through a wrapper).  Unattended; cancels its job.
OUT=~/N414/a4; mkdir -p $OUT; D=~/ntt-N414-sos/ecalc; cd $D
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a $OUT/summary.txt; }
J=$(sbatch -p PPAC_MI300A_SPX -N3 --gpus-per-node=4 -t 0:30:00 -J N4 --parsable --wrap "sleep 1800")
log "job $J submitted (-N3)"
trap "scancel $J; log 'job $J cancelled'" EXIT
until [ "$(squeue -j $J -h -o %T 2>/dev/null)" = RUNNING ]; do sleep 10; [ -z "$(squeue -j $J -h -o %T 2>/dev/null)" ] && { log "job $J gone"; exit 1; }; done
log "job $J running on $(squeue -j $J -h -o %N)"
export SLURM_JOB_ID=$J COMM_TRANSPORT=shmem
tst() { local tag=$1 to=$2 p=$3; shift 3; local t0=$(date +%s.%N)
  timeout $to ./mnrun.sh $p "$@" > $OUT/$tag.log 2>&1; local rc=$?
  local t1=$(date +%s.%N); local ok=$(grep -ac 'VERIFY OK' $OUT/$tag.log); local bad=$(grep -ac 'VERIFY FAILED' $OUT/$tag.log)
  log "$tag: rc $rc, VERIFY OK $ok, FAILED $bad, $(echo "$t1 - $t0" | bc) s launch-to-exit"; }
tst comm3_chain 300 3 env X=1 stdbuf -oL timeout 250 ./tests/t_comm
tst grid3_010 1200 3 stdbuf -oL ./tests/t_mn_grid 0.1 25
log "a4b done"

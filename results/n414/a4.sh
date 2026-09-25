#!/bin/bash
# N414 A4: t_mn_grid on real nodes over SOS, and mnrun.sh's detection through wrappers.  Runs unattended; cancels its job.
OUT=~/N414/a4; mkdir -p $OUT; D=~/ntt-N414-sos/ecalc; cd $D
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a $OUT/summary.txt; }
J=$(sbatch -p PPAC_MI300A_SPX -N3 --gpus-per-node=4 -t 0:45:00 -J N4 --parsable --wrap "sleep 2700")
log "job $J submitted (-N3)"
until [ "$(squeue -j $J -h -o %T 2>/dev/null)" = RUNNING ]; do sleep 10; [ -z "$(squeue -j $J -h -o %T 2>/dev/null)" ] && { log "job $J gone"; exit 1; }; done
log "job $J running on $(squeue -j $J -h -o %N)"
DEADLINE=$(( $(date +%s) + 42 * 60 )); left() { echo $(( DEADLINE - $(date +%s) )); }
export SLURM_JOB_ID=$J COMM_TRANSPORT=shmem
# (b) detection only (MNRUN_SHOW_IMPL=only): the SOS binary and the OSHMEM binary behind each wrapper
OSH=~/ntt-S13d-osh/ecalc/tests/t_comm
for w in "" "env X=1" "stdbuf -oL" "timeout 600" "numactl --interleave=all" "nice -n 0" "setarch x86_64 -L" "env X=1 stdbuf -oL timeout 600 numactl --interleave=all"; do
  a=$(MNRUN_SHOW_IMPL=only ./mnrun.sh 2 $w ./tests/t_comm 2>&1 | tr '\n' ' ')
  b=$(MNRUN_SHOW_IMPL=only ./mnrun.sh 2 $w $OSH 2>&1 | tr '\n' ' ')
  log "detect [${w:-none}]: sos binary -> $a| oshmem binary -> $b"
done
a=$(MNRUN_SHOW_IMPL=only ./mnrun.sh 2 bash -c ./tests/t_comm 2>&1 | tr '\n' ' '); log "detect [bash -c]: $a"
a=$(COMM_SHMEM_IMPL=sos MNRUN_SHOW_IMPL=only ./mnrun.sh 2 bash -c ./tests/t_comm 2>&1 | tr '\n' ' '); log "detect [bash -c, COMM_SHMEM_IMPL=sos]: $a"
a=$(COMM_SHMEM_IMPL=bogus ./mnrun.sh 2 ./tests/t_comm 2>&1 | tr '\n' ' '); log "detect [COMM_SHMEM_IMPL=bogus]: $a"
# real launches through the wrappers (no COMM_SHMEM_IMPL): t_comm at 2 PEs
tst() { local tag=$1 to=$2 p=$3; shift 3; local t0=$(date +%s.%N)
  timeout $to ./mnrun.sh $p "$@" > $OUT/$tag.log 2>&1; local rc=$?
  local t1=$(date +%s.%N); local ok=$(grep -ac 'VERIFY OK' $OUT/$tag.log); local bad=$(grep -ac 'VERIFY FAILED' $OUT/$tag.log)
  log "$tag: rc $rc, VERIFY OK $ok, FAILED $bad, $(echo "$t1 - $t0" | bc) s launch-to-exit"; }
tst comm_stdbuf 300 2 stdbuf -oL ./tests/t_comm
tst comm_timeout 300 2 timeout 250 ./tests/t_comm
tst comm_numactl 300 2 numactl --interleave=all ./tests/t_comm
tst comm_chain 300 2 env X=1 stdbuf -oL timeout 250 ./tests/t_comm
# (a) t_mn_grid at 3 and 2 real nodes
tst grid3_010 900 3 stdbuf -oL ./tests/t_mn_grid 0.1 25
tst grid2_010 900 2 stdbuf -oL ./tests/t_mn_grid 0.1 25
[ $(left) -gt 900 ] && tst grid2_025 $(( $(left) - 60 )) 2 stdbuf -oL ./tests/t_mn_grid 0.25 25
[ $(left) -gt 600 ] && tst grid3_025 $(( $(left) - 60 )) 3 stdbuf -oL ./tests/t_mn_grid 0.25 25
scancel $J; log "job $J cancelled; done"

#!/bin/bash
# G13d batch runner (Phase 13d agent G), pinned to s24-16.  Run unattended on aac6:
#   setsid nohup bash ~/ntt-G13d/ecalc/g13d_run.sh <batch> <runlist> > ~/g13d/<batch>.out 2>&1 &
# runlist lines:  <tag> <est_s> <procs> <digits> <ref|-> [ENV=VAL ...]
#   procs 1: srun ecalc; procs > 1: mnrun.sh <procs> on the one node.  ref: a sha1 file to compare the output against
#   (the output then goes to the node's /tmp and is removed after), or '-' (no outfile: the run's own VERIFY is the check).
# A run is skipped when it would not finish inside the job (45 min) or before 22:38 CDT (23:38 EDT, the hard stop).
set -u
B=$1; L=$2; D=~/g13d; mkdir -p $D; S=$D/summary.txt
cd ~/ntt-G13d/ecalc || exit 1
log() { echo "$(date +%T) [$B] $*" | tee -a $S; }
J=$(sbatch -p PPAC_MI300A_SPX -N1 -w ppac-pl1-s24-16 --gpus=4 -t 0:45:00 -J G --parsable --wrap "sleep 2700")
log "job $J submitted"
trap "scancel $J; log 'job $J cancelled'" EXIT
until [ "$(squeue -j $J -h -o %T)" = RUNNING ]; do sleep 15; done
T0=$(date +%s); log "job $J running on $(squeue -j $J -h -o %N)"
STOP=$(date -d "${G_STOP:-22:38}" +%s)   # G_STOP: the hard stop (CDT on aac6), 22:38 = 23:38 EDT unless lifted
while read -r -u 3 tag est np dg ref envs; do
  [ -z "$tag" ] || [ "${tag:0:1}" = "#" ] && continue
  now=$(date +%s)
  if [ $((now - T0 + est)) -gt 2640 ] || [ $((now + est)) -gt $STOP ]; then log "SKIP $tag (est $est s, elapsed $((now - T0)) s)"; continue; fi
  out=""; [ "$ref" != "-" ] && out=/tmp/g13d_$tag.out
  base="$envs ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 RNS_VERBOSE=1"
  wd=$((3 * est + 120))   # watchdog: a run hung after init once (a_S4_lo_770e8, 2 threads in kfd_wait_on_events)
  t1=$(date +%s)
  if [ "$np" = 1 ]; then
    srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd ~/ntt-G13d/ecalc; rm -f /tmp/g13d_*.out*; env $base timeout -s TERM $wd /usr/bin/time -v ./ecalc $dg $out" > $D/$tag.log 2>&1
  else
    bash -lc "module load rocm >/dev/null 2>&1; cd ~/ntt-G13d/ecalc; srun --jobid=$J -N1 --overlap bash -c 'rm -f /tmp/g13d_*.out*'; SLURM_JOB_ID=$J env $base timeout -s TERM $wd ./mnrun.sh $np ./ecalc $dg $out" > $D/$tag.log 2>&1
  fi
  rc=$?; t2=$(date +%s)
  cmpres=""
  if [ -n "$out" ] && [ $rc = 0 ]; then
    if [ "$np" = 1 ]; then
      h=$(srun --jobid=$J -N1 --overlap bash -c "sha1sum $out | cut -c1-40")
    else
      h=$(srun --jobid=$J -N1 --overlap bash -c "ls $out.part* >/dev/null 2>&1 && cat \$(ls $out.part* | sort) | sha1sum | cut -c1-40 || sha1sum $out | cut -c1-40")
    fi
    r=$(cut -c1-40 $ref)
    [ "$h" = "$r" ] && cmpres="IDENTICAL(sha1)" || cmpres="DIFFER($h vs $r)"
    srun --jobid=$J -N1 --overlap bash -c "rm -f /tmp/g13d_*.out*"
  fi
  v=$(grep -a -c '^VERIFY OK\|^mn: all .* VERIFY OK' $D/$tag.log)
  tot=$(grep -a '^total' $D/$tag.log | tail -1 | cut -c1-110)
  pcs=$(grep -a -o '[0-9]* x [0-9]* pieces' $D/$tag.log | sort | uniq -c | sort -k2n | awk '{printf "%s*%sx%s ", $1, $2, $4}')
  log "$tag np=$np D=$dg [$envs]: rc $rc, VERIFY OK x$v, $cmpres wall $((t2 - t1)) s | $tot | pieces: $pcs"
done 3< $L
log "batch done, elapsed $(( $(date +%s) - T0 )) s"

#!/bin/bash
# k8b_probe_driver.sh (K8b) - aac6 fire-and-forget: the seed-transfer micro-probe (tools/k8b_probe.cpp), 1 node, PPAC_MI300A_SPX.
#   arm:  cd ~ && setsid nohup bash ~/k8b/k8b_probe_driver.sh > ~/k8b/probe_driver.log 2>&1 < /dev/null &
#   env:  DEADLINE_ET (give up waiting for the hold), TL_MIN (30), VARIANTS, PROBE (~/k8b/k8b_probe, built by the armer)
# Output ~/k8b/probe/<variant>.log, ~/k8b/probe_summary.txt (one RESULT line per variant), marker ~/k8b/PROBE_DONE.
# Cancels only its own hold; kills nothing else.
O=$HOME/k8b; P=$O/probe; mkdir -p $P; S=$O/probe_summary.txt; PROBE=${PROBE:-$O/k8b_probe}
DEADLINE_ET=${DEADLINE_ET:-"2026-10-12 23:00"}; TL_MIN=${TL_MIN:-30}
VARIANTS=${VARIANTS:-"none kern kern0 masync memcpy pull pull_late masyncm cpustore kern:REG_LATE=1 kern:NOMAP=1 pull:NOMAP=1 masync:NOMAP=1 none:MAP_PAR=1 kern:MAP_PAR=1"}
VERDICT="FAILED: driver ended early"; J=
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $S; }
fin() { if [ -n "$J" ]; then scancel $J 2>/dev/null; sleep 2; left=$(squeue -s -j $J -h 2>/dev/null | wc -l); say "hold $J cancelled (own job); steps left $left"; fi
        echo "$VERDICT" > $O/PROBE_DONE; say "$VERDICT"; }
trap fin EXIT
rm -f $O/PROBE_DONE; : > $S
[ -x $PROBE ] || { VERDICT="FAILED: $PROBE not built"; exit 2; }
DL=$(TZ=America/New_York date -d "$DEADLINE_ET" +%s)
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:$TL_MIN:00 -J k8bp --parsable -o /dev/null --wrap "sleep $((TL_MIN*60))" < /dev/null) || { VERDICT="FAILED: sbatch"; exit 2; }
J=${J%%;*}; say "hold $J submitted"
while [ "$(squeue -j $J -h -o %T)" != RUNNING ]; do
  [ -z "$(squeue -j $J -h)" ] && { VERDICT="FAILED: hold $J vanished"; J=; exit 1; }
  [ $(date +%s) -gt $DL ] && { VERDICT="FAILED: hold did not start before $DEADLINE_ET"; exit 1; }
  sleep 30
done
T0=$(date +%s); say "hold $J RUNNING on node 0"
NOK=0; NRUN=0
for V in $VARIANTS; do
  var=${V%%:*}; envs=; [ "$V" != "$var" ] && envs=${V#*:}
  tag=$(echo $V | tr ':=' '__')
  left=$(( TL_MIN*60 - ($(date +%s) - T0) )); [ $left -lt 240 ] && { say "SKIP $tag (left ${left}s)"; continue; }
  NRUN=$((NRUN+1))
  timeout 300 srun --jobid=$J -N1 -n1 --mem=0 --gpus=4 -c 192 --overlap bash -lc "module load rocm >/dev/null 2>&1; env OMP_NUM_THREADS=96 $envs $PROBE $var" > $P/$tag.log 2>&1 < /dev/null
  rc=$?; r=$(grep -a -m1 '^RESULT' $P/$tag.log); [ $rc -eq 0 ] && [ -n "$r" ] && NOK=$((NOK+1))
  say "$tag rc $rc | ${r:-no RESULT line: $(grep -a -m1 -E 'FAIL|error|Error' $P/$tag.log | cut -c1-120)}"
done
VERDICT="SUCCESS: $NOK of $NRUN variants gave a RESULT line in $S"; [ $NOK -gt 0 ] || VERDICT="FAILED: no variant finished (see $S, $P)"

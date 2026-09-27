#!/bin/bash
# R4 (Phase 15): one unattended batch.  usage: job.sh <name> <node|any> <begin> <steps file>
# steps: "run <tag> <digits> [VAR=val ...]" (digits to /tmp, sha1 of the output) | "runnf <tag> <digits> [VAR=val ...]" (no digit file)
#        | "gsh <tag> <cmd...>" (on the node with GPUs) | "sh <tag> <cmd...>" (node shell) | "mn <tag> <procs> <digits> [VAR=val ...]"
#        (mnrun, cmp to ref) | "cmp <tag> <digits> <reffile> [VAR=val ...]" (size 1, cmp to a reference file) | "acc <only> [VAR=val ...]"
NAME=$1; NODE=$2; W=; [ $NODE = any ] || W="-w $NODE"; BEGIN=$3; STEPS=$4
D=$HOME/R415/$NAME; mkdir -p $D; cd $HOME/ntt-R415/ecalc || exit 1
exec > $D/driver.log 2>&1
J=$(sbatch -p PPAC_MI300A_SPX -N1 $W --gpus=4 -t 0:45:00 -J R4 --begin=$BEGIN --parsable --wrap "sleep 2700")
echo "job $J node $NODE $(date)"
trap 'scancel $J' EXIT
while [ "$(squeue -j $J -h -o %T)" != RUNNING ]; do [ -z "$(squeue -j $J -h -o %T)" ] && { echo gone; exit 1; }; sleep 10; done
echo "running $(date) on $(squeue -j $J -h -o %N)"; T0=$(date +%s)
N() { srun --jobid=$J -N1 --overlap bash -c "$*" < /dev/null; }
N "hostname; df -h /tmp | tail -1; free -g | head -2" > $D/node.txt 2>&1
C=$(cd $HOME/ntt-R415 && git log --oneline -1); echo "commit $C"
case "$C" in "$(cat $HOME/R415/expect_commit)"*) ;; *) echo "WRONG COMMIT (expected $(cat $HOME/R415/expect_commit))"; exit 1;; esac
REF=$HOME/ntt/ecalc/ref
while read -r kind tag dg rest; do
  [ -z "$kind" ] && continue; case "$kind" in \#*) continue;; esac
  el=$(( $(date +%s) - T0 )); [ $el -gt 2350 ] && { echo "SKIP $kind $tag (time $el)"; continue; }
  echo "== $kind $tag $dg $rest  $(date +%T)"
  t1=$(date +%s.%N)
  case $kind in
  run) timeout 1200 srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $HOME/ntt-R415/ecalc; env $rest ECALC_VERBOSE=2 RNS_VERBOSE=1 ./ecalc $dg /tmp/r4_$tag.txt" > $D/$tag.log 2>&1 < /dev/null; echo "rc $?";;
  runnf) timeout 1200 srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $HOME/ntt-R415/ecalc; env $rest ECALC_VERBOSE=2 RNS_VERBOSE=1 ./ecalc $dg" > $D/$tag.log 2>&1 < /dev/null; echo "rc $?";;
  cmp) set -- $rest; rf=$1; shift; timeout 1200 srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $HOME/ntt-R415/ecalc; env $* ECALC_VERBOSE=2 RNS_VERBOSE=1 ./ecalc $dg /tmp/r4_$tag.txt" > $D/$tag.log 2>&1 < /dev/null; echo "rc $?"
      N "cmp -s /tmp/r4_$tag.txt $rf && echo identical || echo DIFFERS; rm -rf /tmp/r4_$tag.*" > $D/$tag.cmp 2>&1; echo "   cmp $(cat $D/$tag.cmp)";;
  mn) p=$dg; set -- $rest; dd=$1; shift; SLURM_JOB_ID=$J timeout 900 ./mnrun.sh $p env "$@" ECALC_VERBOSE=2 ./ecalc $dd /tmp/r4_$tag.txt > $D/$tag.log 2>&1 < /dev/null; echo "rc $? $(grep -a 'mn: all' $D/$tag.log | tail -1)"
      N "cat /tmp/r4_$tag.txt.part* > /tmp/r4_$tag.all 2>/dev/null || cp /tmp/r4_$tag.txt /tmp/r4_$tag.all; cmp -s /tmp/r4_$tag.all $REF/e_$dd.txt && echo identical || echo DIFFERS; rm -rf /tmp/r4_$tag.*" > $D/$tag.cmp 2>&1; echo "   cmp $(cat $D/$tag.cmp)";;
  acc) env $dg $rest ./mnaccept.sh $J --only $tag > $D/acc_$tag.log 2>&1 < /dev/null; echo "rc $?"; tail -12 $D/acc_$tag.log;;
  sh) N "$dg $rest" > $D/$tag.log 2>&1; echo "rc $?"; tail -2 $D/$tag.log;;
  gsh) timeout 1200 srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $HOME/ntt-R415/ecalc; $dg $rest" > $D/$tag.log 2>&1 < /dev/null; echo "rc $?"; tail -2 $D/$tag.log;;
  esac
  echo "   wall $(awk "BEGIN{printf \"%.1f\", $(date +%s.%N) - $t1}") s; $(grep -a '^total' $D/$tag.log 2>/dev/null | tail -1 | cut -c1-200)"
  if [ $kind = run ]; then
    N "ls -la /tmp/r4_$tag.txt* | head -4; P=\$(ls /tmp/r4_$tag.txt.part* 2>/dev/null | sort); [ -n \"\$P\" ] || P=/tmp/r4_$tag.txt; for p in \$P; do dd if=\$p iflag=direct bs=64M status=none; done | sha1sum; rm -rf /tmp/r4_$tag.txt* /tmp/r4_$tag.ck" > $D/$tag.sha 2>&1
    echo "   sha $(tail -1 $D/$tag.sha)"
  fi
done < $STEPS
echo "done $(date), $(( $(date +%s) - T0 )) s"

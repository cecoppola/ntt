#!/bin/bash
# IO15 (Phase 15, agent IO): one unattended batch.  usage: job.sh <name> <node|any> <batch file>
# The batch file is bash, sourced here, using G (a shell on the node with rocm, in the clone's ecalc/), N (a plain shell on the
# node), M <procs> <cmd> (mnrun.sh), step <tag> <cmd...> (logged to $D/<tag>.log, skipped past TMAX s of the job), $J, $D.
NAME=$1; NODE=$2; BATCH=$3
D=$HOME/IO15/$NAME; mkdir -p $D; cd $HOME/ntt-IO15/ecalc || exit 1
exec > $D/driver.log 2>&1
W=; [ "$NODE" = any ] || W="-w $NODE"
J=$(sbatch -p PPAC_MI300A_SPX -N${NN:-1} $W --gpus=4 -t 0:45:00 -J IO --parsable --wrap "sleep 2700")
echo "job $J $(date)"
trap 'scancel $J' EXIT
while [ "$(squeue -j $J -h -o %T)" != RUNNING ]; do [ -z "$(squeue -j $J -h -o %T)" ] && { echo gone; exit 1; }; sleep 10; done
echo "running $(date) on $(squeue -j $J -h -o %N)"; T0=$(date +%s)
echo "commit $(git -C $HOME/ntt-IO15 log --oneline -1)"
U=$HOME/ntt-IO15/tools/unpack_digits
G() { timeout ${STO:-1500} srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $HOME/ntt-IO15/ecalc; $*" < /dev/null; }
N() { timeout ${STO:-1500} srun --jobid=$J -N1 --overlap bash -c "cd $HOME/ntt-IO15/ecalc; $*" < /dev/null; }
M() { local p=$1; shift; SLURM_JOB_ID=$J timeout ${STO:-1500} ./mnrun.sh $p "$@" < /dev/null; }
step() { local tag=$1; shift; local el=$(( $(date +%s) - T0 )); [ $el -gt ${TMAX:-2400} ] && { echo "SKIP $tag (time $el)"; return; }
         echo "== $tag $(date +%T)"; local t1=$(date +%s); "$@" > $D/$tag.log 2>&1; local rc=$?
         echo "   rc $rc, $(( $(date +%s) - t1 )) s; $(grep -a '^total\|VERIFY\|IDENTICAL\|identical\|differ\|RECHECK OK\|RECHECK FAILED\|FATAL\|^rc=\|^wrote\|unpack_digits:\|^wall' $D/$tag.log | head -8 | cut -c1-220 | tr '\n' '|')"; }
source $BATCH
echo "done $(date), $(( $(date +%s) - T0 )) s"

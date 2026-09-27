# IO15: the helpers with the time limit inside (timeout cannot run a shell function: job.sh's first step() ran nothing, rc 127)
G() { timeout ${STO:-1500} srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $HOME/ntt-IO15/ecalc; $*" < /dev/null; }
N() { timeout ${STO:-1500} srun --jobid=$J -N1 --overlap bash -c "cd $HOME/ntt-IO15/ecalc; $*" < /dev/null; }
M() { local p=$1; shift; SLURM_JOB_ID=$J timeout ${STO:-1500} ./mnrun.sh $p "$@" < /dev/null; }
step() { local tag=$1; shift; local el=$(( $(date +%s) - T0 )); [ $el -gt ${TMAX:-2400} ] && { echo "SKIP $tag (time $el)"; return; }
         echo "== $tag $(date +%T)"; local t1=$(date +%s); "$@" > $D/$tag.log 2>&1; local rc=$?
         echo "   rc $rc, $(( $(date +%s) - t1 )) s; $(grep -a '^total\|VERIFY\|IDENTICAL\|identical\|differ\|RECHECK OK\|RECHECK FAILED\|FATAL\|^rc=\|^wrote\|unpack_digits:\|^wall' $D/$tag.log | head -8 | cut -c1-220 | tr '\n' '|')"; }

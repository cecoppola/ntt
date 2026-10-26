#!/bin/bash
# cor05 (TQ22) - what each srun launch shape gives a task on one node: CPUs (nproc, Cpus_allowed_list), visible GPUs, ROCR_VISIBLE_DEVICES, NUMA
# mems allowed, SLURM_* task vars.  Arms: 1 task default; 1 task -c ncpu; 4 tasks -c ncpu/4 (cpu-bind none, gpu-bind none); 4 tasks map_ldom
# + mem-bind local; 4 tasks --gpus-per-task=1; plus mnrun.sh's own computed launch (MNRUN_PRINT_ENV=1).
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
PN=1; ecalc_have 1 || PN=2
H=$(ecalc_hosts 0 1)
cat > raw/lp.sh <<'LP'
g=$(command -v rocm-smi >/dev/null && rocm-smi --showid 2>/dev/null | grep -c 'GPU\[')
echo "TASK host=$(hostname -s) rank=${SLURM_PROCID:-} local=${SLURM_LOCALID:-} nproc=$(nproc) cpus=$(grep Cpus_allowed_list /proc/self/status | cut -f2) mems=$(grep Mems_allowed_list /proc/self/status | cut -f2) rocr=${ROCR_VISIBLE_DEVICES-unset} hip=${HIP_VISIBLE_DEVICES-unset} gpubind=${SLURM_GPU_BIND-unset} cpubind=${SLURM_CPU_BIND_TYPE-unset} cpt=${SLURM_CPUS_PER_TASK-unset} smi_gpus=${g:-NA}"
LP
NCPU=$(srun --jobid="$JOBID" -N1 -n1 -w "$H" --overlap nproc --all 2>/dev/null | head -1); NCPU=${NCPU:-96}
PRE=$(_ec_path $PN >/dev/null 2>&1; _ec_pre)
run() {   # TAG NTASKS srun-options...
    local tag=$1 nt=$2; shift 2
    timeout 120 srun --jobid="$JOBID" --export=ALL -N1 -w "$H" -n "$nt" --overlap "$@" bash -lc "$PRE"'bash '"$OUT"'/raw/lp.sh' > raw/arm_$tag.log 2>&1 < /dev/null
    echo "rc=$?" >> raw/arm_$tag.log
    local n m; n=$(grep -a -c '^TASK' raw/arm_$tag.log)
    local cp; cp=$(grep -a '^TASK' raw/arm_$tag.log | sed -n 's/.* nproc=\([0-9]*\).*/\1/p' | sort -n | head -1)
    local gs; gs=$(grep -a '^TASK' raw/arm_$tag.log | sed -n 's/.* rocr=\([^ ]*\).*/\1/p' | sort -u | paste -sd'/' -)
    tgt_kv m.${tag}_tasks "$n tasks"; tgt_kv m.${tag}_min_nproc "${cp:-NA} cpus"; tgt_kv e.${tag}_rocr "${gs:-NA}"
    SUMQ="$SUMQ; $tag: $n tasks, min nproc ${cp:-NA}, ROCR $gs"
}
SUMQ=
run default1 1
run c_all 1 -c "$NCPU" --gpus-per-node=4
run four_c 4 -c $((NCPU / 4)) --cpu-bind=none --gpu-bind=none --gpus-per-node=4
run four_ldom 4 --cpu-bind=map_ldom:0,1,2,3 --mem-bind=local --gpus-per-node=4
run four_gpt 4 --gpus-per-task=1 -c $((NCPU / 4))
run four_default 4 --gpus-per-node=4
scontrol show node "$H" 2>/dev/null | tr ' ' '\n' | grep -E '^(CPUTot|CoresPerSocket|Sockets|ThreadsPerCore|Gres|RealMemory|CfgTRES|Boards|OS)=' > raw/scontrol_node.txt
tgt_kv e.cpus_per_node "$NCPU"; tgt_kv e.gres "$(sed -n 's/^Gres=//p' raw/scontrol_node.txt | head -1)"
if ecalc_have $PN; then
    D=$(ecalc_digits 1e9); S=D; [ $PN = 2 ] && S=C
    MN=$(ecalc_tool $PN mnrun.sh)
    SLURM_JOB_ID=$JOBID MNRUN_NODELIST=$H MNRUN_PRINT_ENV=1 MNRUN_MODULES="$EC_MODS" MNRUN_UNLOAD="$EC_UNLOAD" bash "$MN" 1 env ECALC_PE_PER_GPU=1 "$EC_BIN" "$D" > raw/mnrun_print_env.log 2>&1
    tgt_kv e.mnrun_cpus_per_task "$(grep -a -m1 -io 'cpus.per.task[^ ]*[ =]*[^ ]*' raw/mnrun_print_env.log | head -1)"
fi
ecalc_q TQ22 "node has $NCPU cpus; launch shapes (1 node):${SUMQ#;}; mnrun's computed launch in raw/mnrun_print_env.log"
[ "$(grep -a -c '^TASK' raw/arm_four_c.log)" = 4 ] && ecalc_end PASS || ecalc_end PARTIAL

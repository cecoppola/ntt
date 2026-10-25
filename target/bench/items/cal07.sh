#!/bin/bash
# cal07 - launch and PMI bootstrap against node count (TQ76), 2..64 nodes: wall of srun hostname (4 tasks per node) and wall of t_xshmem basic (launcher + library init + first exchange).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
PN=2; ecalc_have 2 || PN=1; need_path $PN
for n in 2 4 8 16 32 64; do
    [ "$(hosts_n)" -ge $n ] || continue; tleft_ok 0.8 || { note "stopped before n=$n"; break; }
    PPN=4 ecalc_srun "host$n" 0 $n /bin/hostname || true; H=$E_SRUN_WALL_S
    XT_TMO=3 xt_run "basic$n" $PN $n basic XT_MAXMB=1; B=$XT_WALL_S
    tgt_kv m.n${n}_srun_hostname_s "$H s"; tgt_kv m.n${n}_xshmem_basic_wall_s "${B:-NA} s"; R="$R n$n:srun ${H}s/basic ${B:-NA}s;"
done
tgt_kv q.TQ76 "srun hostname / t_xshmem basic wall by node count:$R"
OPT_NO_RUNS_OK=1; finish

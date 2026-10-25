#!/bin/bash
# prm08 - MN_NONPOW2_CAP=1 vs 0 at a non-power-of-two node count (Q119): time and plane memory difference.
# 24 allocated nodes = two groups of 12; both arms run at the same time on disjoint groups, once each (PRM08_ROUNDS=2 repeats with the groups swapped).
# Digits per node: the largest the planner accepts for the CAP=1 arm (the larger one); both arms use the same digits.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
N=${PRM08_NODES:-12}
[ "$(hosts_n)" -ge $((2 * N)) ] || skip "needs $((2 * N)) hosts, have $(hosts_n)"
SH=C; ecalc_have 2 || { SH=P; note "path 2 absent: shape P"; ecalc_have 1 || skip "no binary"; }
ARMV[N1]="MN_NONPOW2_CAP=1"; ARMV[N0]="MN_NONPOW2_CAP=0"
D0=$(dig "${PRM08_PER_NODE:-5e10}" "$N")
D=$(fitd "$N" "$SH" "$D0" MN_NONPOW2_CAP=1)
ecalc_plan "$N" "$D" "$SH" MN_NONPOW2_CAP=1; P1=$P_DEV_GB; N1G=$P_NODE_GB; F1=$P_FIT
ecalc_plan "$N" "$D" "$SH" MN_NONPOW2_CAP=0; P0=$P_DEV_GB; N0G=$P_NODE_GB; F0=$P_FIT
tgt_kv m.plan_dev_cap1_gb "$P1 GB"; tgt_kv m.plan_dev_cap0_gb "$P0 GB"; tgt_kv m.plan_node_cap1_gb "$N1G GB"; tgt_kv m.plan_node_cap0_gb "$N0G GB"
tgt_kv e.digits "$D"; tgt_kv e.nodes_per_arm "$N"; tgt_kv e.shape "$SH"; tgt_kv e.plan_fit "cap1=$F1 cap0=$F0"
R=1; ORD="N1 N0"
while [ "$R" -le "${PRM08_ROUNDS:-1}" ]; do
    conc "$SH" "$N" "$D" "$R" "$ORD"; ORD="N0 N1"; R=$((R + 1))
    tleft_ok 0.6 || break
done
arms_report N0
ecalc_load n1_r1; DP1=$E_DEVPEAK_GB; ecalc_load n0_r1; DP0=$E_DEVPEAK_GB
tgt_kv m.devpeak_cap1_gb "${DP1:-NA} GB"; tgt_kv m.devpeak_cap0_gb "${DP0:-NA} GB"
tgt_kv q.TQ81 "${AB_LINE:-no counted run} (cap1 minus cap0; plane memory per node planner ${P1:-?} vs ${P0:-?} GB, measured ${DP1:-?} vs ${DP0:-?} GB; $N nodes, $D digits, shape $SH)"
finish

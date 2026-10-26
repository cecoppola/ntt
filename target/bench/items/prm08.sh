#!/bin/bash
# prm08 - MN_NONPOW2_CAP=1 vs 0 at a non-power-of-two node count (Q119, TQ81): time and plane memory difference.
# 36 allocated nodes = THREE groups of 12 running at the same time on disjoint groups, ONE concurrent round: N1 = MN_NONPOW2_CAP=1,
# N0 = MN_NONPOW2_CAP=0, N0B = MN_NONPOW2_CAP=0 again (A/A control: its difference to N0 is the group-to-group spread, the yardstick for the
# CAP effect N1 - N0).  PRM08_ROUNDS=2 repeats with the groups rotated.
# Digits per node: the largest the planner accepts for the CAP=1 arm (the larger one) and for CAP=0; all three arms use the same digits.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
N=${PRM08_NODES:-12}
[ "$(hosts_n)" -ge $((3 * N)) ] || skip "needs $((3 * N)) hosts, have $(hosts_n)"
SH=C; ecalc_have 2 || { SH=P; note "path 2 absent: shape P"; ecalc_have 1 || skip "no binary"; }
ARMV[N1]="MN_NONPOW2_CAP=1"; ARMV[N0]="MN_NONPOW2_CAP=0"; ARMV[N0B]="MN_NONPOW2_CAP=0"
D0=$(dig "${PRM08_PER_NODE:-5e10}" "$N")
D=$(fitd "$N" "$SH" "$D0" MN_NONPOW2_CAP=1)
D=$(fitd "$N" "$SH" "$D" MN_NONPOW2_CAP=0)       # planner check for every arm's setting before any node time
ecalc_plan "$N" "$D" "$SH" MN_NONPOW2_CAP=1; P1=$P_DEV_GB; N1G=$P_NODE_GB; F1=$P_FIT
ecalc_plan "$N" "$D" "$SH" MN_NONPOW2_CAP=0; P0=$P_DEV_GB; N0G=$P_NODE_GB; F0=$P_FIT
[ -n "$P1" ] && { tgt_kv m.plan_dev_cap1_gb "$P1 GB"; tgt_kv m.plan_dev_cap0_gb "$P0 GB"; tgt_kv m.plan_node_cap1_gb "$N1G GB"; tgt_kv m.plan_node_cap0_gb "$N0G GB"; }
tgt_kv e.digits "$D"; tgt_kv e.nodes_per_arm "$N"; tgt_kv e.shape "$SH"; tgt_kv e.plan_fit "cap1=$F1 cap0=$F0"
R=1; ORD="N1 N0 N0B"
while [ "$R" -le "${PRM08_ROUNDS:-1}" ]; do
    conc "$SH" "$N" "$D" "$R" "$ORD"; ORD=$(echo "$ORD" | awk '{ print $2, $3, $1 }'); R=$((R + 1))
    tleft_ok 0.6 || break
done
arms_report N0
ecalc_load n1_r1; DP1=$E_DEVPEAK_GB; T1=$E_TOTAL
ecalc_load n0_r1; DP0=$E_DEVPEAK_GB; T0=$E_TOTAL
ecalc_load n0b_r1; T0B=$E_TOTAL
tgt_kv m.devpeak_cap1_gb "${DP1:-NA} GB"; tgt_kv m.devpeak_cap0_gb "${DP0:-NA} GB"
EFF=NA; AA=NA
[ -n "$T1" ] && [ -n "$T0" ] && EFF=$(awk -v a="$T1" -v b="$T0" 'BEGIN { printf "%+.2f", a - b }')
[ -n "$T0B" ] && [ -n "$T0" ] && AA=$(awk -v a="$T0B" -v b="$T0" 'BEGIN { printf "%+.2f", a - b }')
tgt_kv m.cap_effect_round1_s "$EFF s"; tgt_kv m.aa_spread_round1_s "$AA s"
tgt_kv q.TQ81 "round 1: cap1 minus cap0 = $EFF s, A/A control (cap0 again minus cap0) = $AA s: the CAP effect counts only beyond that spread; all rounds: ${AB_LINE:-no counted run} (plane memory per node planner ${P1:-?} vs ${P0:-?} GB, measured ${DP1:-?} vs ${DP0:-?} GB; $N nodes per group, $D digits, shape $SH)"
finish

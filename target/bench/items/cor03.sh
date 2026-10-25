#!/bin/bash
# cor03 (TQ09) - the planner at 576 nodes on the login node (MN_PLAN_ONLY, no device touched): 3.82e13 / 3.71e13 / 3.76e13 with
# MN_NONPOW2_CAP=0 and =auto, layouts P and D (path 1) and C (path 2); then a bisection of the largest digit count that fits (P and C,
# both caps).  Reproduces (or not) the class-1 discrepancy: 407.41 GB refused vs 358.96 GB fit for 3.82e13:576 (auto vs cap 0).
# Digit counts are not scaled: the planner touches no memory.
# Q120 block (added): the same checks at TGT_SHARE x {1,8,64,576} nodes for D, P (ECALC1 + ECALC_PE_PER_GPU=1) and C (ECALC2), on the
# target's real limits: fit and margins per point (q.TQ09 text, m.q120_*, e.q120_fit_*), table q120_table.tsv.
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
ecalc_have 1 || ecalc_have 2 || { tgt_kv note "no binary (bld01/bld02 absent)"; ecalc_end SKIP; }
ecalc_refresh 600 >/dev/null 2>&1
N=${COR03_NODES:-576}
printf 'digits\tnodes\tshape\tcap\tfit\tdev_gb\tdev_limit_gb\tnode_gb\tnode_limit_gb\twrap\tpool_mb\n' > plan_table.tsv
DISC=; SUMQ=
plan() {   # DIGITS SHAPE CAP -> P_*; row in plan_table.tsv
    ecalc_plan "$N" "$1" "$2" MN_NONPOW2_CAP="$3" > /dev/null; local rc=$?
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$N" "$2" "$3" "$P_FIT" "$P_DEV_GB" "$P_DEV_LIMIT_GB" "$P_NODE_GB" "$P_NODE_LIMIT_GB" "$P_WRAP" "$P_POOL_MB" >> plan_table.tsv
    # keep one log per call
    return $rc
}
SHAPES="P D"; ecalc_have 2 && SHAPES="P D C"
for d in 38200000000000 37100000000000 37600000000000; do
  for sh in $SHAPES; do
    ecalc_have $([ $sh = C ] && echo 2 || echo 1) || continue
    plan $d $sh 0; f0=$P_FIT; n0=$P_NODE_GB; d0=$P_DEV_GB; l0=$P_LINE
    plan $d $sh auto; fa=$P_FIT; na=$P_NODE_GB; da=$P_DEV_GB; la=$P_LINE
    t=${d%????????????}; k=${d}_${sh}
    tgt_kv m.node_gb_cap0_${k} "${n0:-NA} GB"; tgt_kv m.node_gb_capauto_${k} "${na:-NA} GB"
    tgt_kv m.dev_gb_cap0_${k} "${d0:-NA} GB"; tgt_kv m.dev_gb_capauto_${k} "${da:-NA} GB"
    tgt_kv e.fit_cap0_${k} "$f0"; tgt_kv e.fit_capauto_${k} "$fa"
    if [ "$f0" != "$fa" ] || { [ -n "$n0" ] && [ -n "$na" ] && awk -v a="$n0" -v b="$na" 'BEGIN { exit !(a > 0 && (b / a > 1.05 || a / b > 1.05)) }'; }; then DISC="$DISC $k"; fi
    if [ $d = 38200000000000 ]; then SUMQ="$SUMQ; 3.82e13 $sh: cap0 fit=$f0 node ${n0:-NA}/dev ${d0:-NA} GB, auto fit=$fa node ${na:-NA}/dev ${da:-NA} GB"; fi
  done
done
# Q120 plan: TGT_SHARE per node at 1/8/64/576 nodes (cap auto, the default)
printf 'digits\tnodes\tshape\tfit\tdev_gb\tdev_margin_gb\tnode_gb\tnode_margin_gb\n' > q120_table.tsv
QFAIL=; QOK=0; QN=0; QW=
for qn in 1 8 64 576; do
  qd=$(awk -v s="$TGT_SHARE" -v n=$qn 'BEGIN { printf "%.0f\n", s * n }')
  for sh in D P C; do
    if [ $sh = C ]; then ecalc_have 2 || continue; else ecalc_have 1 || continue; fi
    ecalc_plan $qn $qd $sh > /dev/null; k=${qn}n_${sh}; QN=$((QN + 1))
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$qd" $qn $sh "$P_FIT" "$P_DEV_GB" "$P_DEV_MARGIN_GB" "$P_NODE_GB" "$P_NODE_MARGIN_GB" >> q120_table.tsv
    tgt_kv e.q120_fit_$k "$P_FIT"; tgt_kv m.q120_dev_gb_$k "${P_DEV_GB:-NA} GB"; tgt_kv m.q120_dev_margin_gb_$k "${P_DEV_MARGIN_GB:-NA} GB"
    tgt_kv m.q120_node_gb_$k "${P_NODE_GB:-NA} GB"; tgt_kv m.q120_node_margin_gb_$k "${P_NODE_MARGIN_GB:-NA} GB"
    if [ "$P_FIT" = yes ]; then QOK=$((QOK + 1)); else QFAIL="$QFAIL $k"; fi
    if [ $qn = 576 ]; then QW="$QW; $sh at 576 n: fit=$P_FIT dev ${P_DEV_GB:-NA} (margin ${P_DEV_MARGIN_GB:-NA}) node ${P_NODE_GB:-NA} (margin ${P_NODE_MARGIN_GB:-NA}) GB"; fi
  done
done
Q120Q="Q120 plan $TGT_SHARE/node at 1/8/64/576 n: $QOK of $QN fit${QFAIL:+, NOT fitting:$QFAIL}$QW"
# the largest fitting digit count, bisection (60 s of budget per arm)
bis() {   # SHAPE CAP -> prints digits
    local lo=30000000000000 hi=45000000000000 mid t0=$(date +%s) i
    plan $lo $1 $2 >/dev/null || { echo "<3.0e13"; return; }
    for i in 1 2 3 4 5 6 7 8 9; do
        [ $(( $(date +%s) - t0 )) -gt 90 ] && break
        mid=$(( (lo + hi) / 2 / 1000000000 * 1000000000 ))
        if plan $mid $1 $2 >/dev/null; then lo=$mid; else hi=$mid; fi
    done; echo $lo
}
MAXQ=
for sh in $SHAPES; do for cap in 0 auto; do
    ecalc_have $([ $sh = C ] && echo 2 || echo 1) || continue
    v=$(bis $sh $cap); tgt_kv m.max_fit_digits_${sh}_cap$cap "$v digits"; MAXQ="$MAXQ $sh/cap$cap=$v"
done; done
if [ -n "$DISC" ]; then DQ="discrepancy between caps REPRODUCED (${DISC# })"; else DQ="no difference between cap 0 and auto"; fi
[ -s plan_table.tsv ] && [ "$(wc -l < plan_table.tsv)" -gt 1 ] || { tgt_kv note "planner printed nothing"; ecalc_end FAIL; }
ecalc_q TQ09 "$Q120Q; 576 n planner at 3.82e13: ${SUMQ#; }; $DQ; largest fit:${MAXQ}"
ecalc_end PASS

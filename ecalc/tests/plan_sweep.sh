#!/bin/bash
# tests/plan_sweep.sh <g> <from> <to> <step> [out] - Phase 13d L: MN_PLAN_ONLY over a range of total digits (login node, no device):
# one summary line per size (the pieces per phase from the code's own decisions, mn_plan.c), then every size where a count changes.
# Example (results/L13d_plan576.txt): tests/plan_sweep.sh 576 2.0e13 6.0e13 0.01e13 results/L13d_plan576.txt
g=$1; from=$2; to=$3; step=$4; out=${5:-/dev/stdout}
cd "$(dirname "$0")/.." || exit 2
n=$(python3 -c "print(round(($to - $from) / $step))")
{
echo "# MN_PLAN_ONLY sweep: g = $g, total digits $from .. $to step $step; $(git log --oneline -1 2>/dev/null); env: ${MN_GROUPS:+MN_GROUPS=$MN_GROUPS }${ECALC_PLANE_CAP:+ECALC_PLANE_CAP=$ECALC_PLANE_CAP }(defaults otherwise)"
echo "# columns: digits | tree (node 0's groups) | tree (each level's largest group) | recip | div | total (node 0) | total (largest groups) | grids tree/recip/div | levels node0/largest"
prev=""; changes=()
for i in $(seq 0 "$n"); do
  D=$(python3 -c "print('%.4e' % ($from + $i * $step))")
  s=$(MN_PLAN_ONLY=$D:$g MN_PLAN_QUIET=1 ./ecalc 2>&1 | grep '^plan summary')
  [ -n "$s" ] || { echo "# $D: no summary"; continue; }
  read -r t0 r v tot tmax totmax gt gr gd lv <<< "$(python3 - "$s" <<'EOF'
import re, sys
s = sys.argv[1]
m = re.search(r'pieces tree (\d+) recip (\d+) div (\d+) total (\d+) \| tree with each level.s largest group (\d+), total (\d+) \| grids tree (\d+) recip (\d+) div (\d+) .*levels \(node 0 / largest\) (.*)$', s)
print(*m.groups()[:9], m.group(10).replace(' ', ','))
EOF
)"
  printf "%s  tree %4s  tree_max %4s  recip %3s  div %3s  total %4s  total_max %4s  grids %s/%s/%s  levels %s\n" "$D" "$t0" "$tmax" "$r" "$v" "$tot" "$totmax" "$gt" "$gr" "$gd" "$lv"
  key="$t0 $tmax $r $v"
  if [ -n "$prev" ] && [ "$key" != "$prev" ]; then changes+=("$pD -> $D: tree $pt0 -> $t0, tree_max $ptmax -> $tmax, recip $pr -> $r, div $pv -> $v, total $ptot -> $tot, total_max $ptotmax -> $totmax"); fi
  prev=$key; pD=$D; pt0=$t0; ptmax=$tmax; pr=$r; pv=$v; ptot=$tot; ptotmax=$totmax
done
echo "# the steps (a count changes between two neighbouring sizes):"
for c in "${changes[@]}"; do echo "#   $c"; done
} > "$out"

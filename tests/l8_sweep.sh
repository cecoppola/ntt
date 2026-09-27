#!/bin/bash
# L8 (Phase 15, throwaway): the code's own product plan (MN_PLAN_ONLY, login node, no GPU) over one-node sizes and seed spans,
# for tests/l8_sweep.py.  Run on aac6 from the clone's ecalc directory:  bash ../tests/l8_sweep.sh  ->  ~/L815_sweep/*.txt
set -e
out=~/L815_sweep; mkdir -p $out
D=$(python3 -c "import math; print(' '.join('%.4g' % (4e10 * (1.4e11 / 4e10) ** (i / 39)) for i in range(40)))")
for d in $D; do
  for s in 160 176 192 208 224 240 256 272 288 320; do
    BS_SEED_TERMS=$s MN_PLAN_ONLY=$d:1 ./ecalc 2>&1 | grep -E '^(plan|== MN_PLAN_ONLY)' > $out/plan_${d}_${s}.txt
  done
done
ls $out | wc -l

#!/bin/bash
# INT3 (Phase 15 Batch 3): MN_PLAN_ONLY on the target's launch line with each combination of MN_P24 (0/1/2), ECALC_NP_AUTO_MIN (0/1)
# and NEWTON_DKM (0/1); login node.  Usage: ./int315_plans.sh <outdir> [ecalc binary] [digits ...]
out=${1:?outdir}; bin=${2:-./ecalc}; shift 2 2>/dev/null; ds=${*:-51000000000000 55500000000000}
mkdir -p "$out"
for d in $ds; do
  for p in 0 1 2; do for m in 0 1; do for k in 0 1; do
    f=$out/plan_${d}_p${p}_m${m}_k${k}.txt
    env ECALC_NP=auto RNS_DIST_CACHE_FIT=1 COMM_SHMEM_ROUND_MB=1024 MN_P24=$p ECALC_NP_AUTO_MIN=$m NEWTON_DKM=$k MN_PLAN_ONLY=$d:576 "$bin" > "$f" 2>&1
    rc=$?
    s=$(grep -E '^plan summary' "$f" | sed -E 's/.*pieces tree ([0-9]+) recip ([0-9]+) div ([0-9]+) total ([0-9]+) \| tree with each level.s largest group ([0-9]+), total ([0-9]+).*/node0 \4 (\1\/\2\/\3) largest \6/')
    c=$(grep -oE '^plan (check|REFUSED)[^:]*: (OK|[0-9]+ of)' "$f" | sed -E 's/.*: //')
    pr=$(grep -E '^plan primes' "$f" | sed -E 's/.*pieces at four primes tree ([0-9]+) of [0-9]+, recip ([0-9]+) of [0-9]+, div ([0-9]+) of [0-9]+.*/4p \1\/\2\/\3/')
    p24=$(grep -E '^plan p24' "$f" | sed -E 's/.*tree ([0-9]+) of [0-9]+, recip ([0-9]+) of [0-9]+, div ([0-9]+) of [0-9]+.*/p24 \1\/\2\/\3/')
    echo "$d MN_P24=$p MIN=$m DKM=$k rc $rc check $c | $s | $pr | ${p24:--}"
  done; done; done
done

#!/bin/bash
# INT3 (Phase 15 Batch 3): the estimate at 5.1e13 and 5.55e13 on 576 nodes, cache 0 slots, the write at 0.6 GB/s (estimate.py's default),
# for the switch combinations (MN_P24 1 and 2; ECALC_NP_AUTO_MIN=1 = MPB; MN_MODEL_DKM=1 = NEWTON_DKM=1), the launch line's ECALC_NP=auto.
# MN_MODEL_P24_F=1.025: P24's per-point surcharge, measured (results/P2415.md 3.3).  Run from ecalc/.
cd "$(dirname "$0")/../ecalc" || exit 1
for D in 8.854e10 9.635e10; do
  for combo in "none" "P24=1" "P24=2" "P24=1 MPB" "P24=2 MPB" "P24=1 DKM" "P24=2 DKM" "P24=1 MPB DKM" "P24=2 MPB DKM" "MPB" "DKM" "MPB DKM"; do
    e=(env -u MN_P24 -u ECALC_NP_AUTO_MIN -u MN_MODEL_DKM MN_MODEL_P24_F=1.025)
    for w in $combo; do
      case $w in
        P24=*) e+=(MN_P24=${w#P24=});;
        MPB) e+=(ECALC_NP_AUTO_MIN=1);;
        DKM) e+=(MN_MODEL_DKM=1);;
      esac
    done
    line=$("${e[@]}" python3 estimate.py --g 576 --D $D --cache-slots 0 2>&1 | grep -E '^576 ')
    printf '%-9s %-16s %s\n' "$D" "$combo" "$line"
  done
done

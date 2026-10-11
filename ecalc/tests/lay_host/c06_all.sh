#!/bin/bash
# C06: the uneven-last-group (cut group) shapes of the 576-k schedules, scaled down (REAL=1: only non-power-of-two group sizes run the layered communicator), in one SLURM
# allocation (MI250 CPU nodes).  Pass A: the switches as they are (X2 off / on, SHARE 0 / 1, the OFI and SHMEM pool emulations); pass B: COMM_LAYER_VSLOT_RELEASE=1 (M3: the pool slots freed after each
# batch of exchanges), X2 variants only.  Usage (in the allocation): c06_all.sh <t_lay> <outdir>.  Writes $OUT/ALL_DONE.
BIN=${1:-$HOME/c06/out/t_lay}; OUT=${2:-$HOME/c06/runs}; HERE=$(cd "$(dirname "$0")" && pwd); mkdir -p "$OUT"; export REAL=1
cfg() { sleep 70; echo "=== $* $(date +%T)"; timeout 600 "$HERE/run_matrix.sh" "$BIN" "$OUT" "$@" 2>&1 | tail -9; }
{
for pass in A B; do
  if [ $pass = B ]; then export COMM_LAYER_VSLOT_RELEASE=1 SUF=_rel; fi
  cfg 9000 23 2,4,8,23 16         # odd N, cuts 3 / 7 at levels 4 / 8 (layered), 92 ranks
  cfg 9000 47 2,4,8,16,47 16      # cuts 3 / 7 / 15 at levels 4 / 8 / 16 (the 575 shape: every level cut), 188 ranks
  cfg 9000 35 2,4,8,24,35 16      # a 3-fold level (24 = 3 x 8) with a cut group of 11, 140 ranks
  if [ $pass = A ]; then cfg 9000 34 2,4,8,24,34 16; else cfg 9000 36 2,4,12,36 16; fi   # A: cut groups of 2 (power of two, plain) and 10; B: the exact-group control (the 576 shape scaled)
done
} > "$OUT/summary.txt" 2>&1
echo done > "$OUT/ALL_DONE"

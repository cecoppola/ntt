#!/bin/bash
# S45: the batch run inside one SLURM allocation (MI250 partition, 3 nodes, CPU only): the configurations below, each the 4-6 variants of run_matrix.sh.
# Everything under one `timeout`; writes $OUT/ALL_DONE at the end.  Usage (in the allocation): s45_all.sh <t_lay> <outdir>
BIN=${1:-$HOME/s45out/lay/t_lay}; OUT=${2:-$HOME/s45out/runs}; HERE=$(cd "$(dirname "$0")" && pwd); mkdir -p "$OUT"
cfg() { echo "=== $* $(date +%T)"; timeout 1500 "$HERE/run_matrix.sh" "$BIN" "$OUT" "$@" 2>&1 | tail -12; }
{
cfg 9000 36 12,36 64          # 144 ranks: three equal groups of 12 then the whole (the 192 / 576 shape)
cfg 9000 39 12,39 64          # 156 ranks: groups 12,12,12,3 (the last group cut to 3), then 39
cfg 9000 72 24,72 32          # 288 ranks
cfg 9000 144 48,144 16        # 576 ranks: 3 groups of 48, then 144 (the shape 3 x 192 -> 576, 4 APUs per node)
cfg 9000 150 48,150 16        # 600 ranks: groups 48,48,48,6 (cut), then 150
cfg 9000 36 6,12,36 64        # three levels (three owners of the shared pair)
} > "$OUT/summary.txt" 2>&1
echo done > "$OUT/ALL_DONE"

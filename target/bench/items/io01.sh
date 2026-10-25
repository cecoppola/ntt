#!/bin/bash
# io01 - writer rate at 1 node (TQ54): tools/wbench, direct mode, 8 vs 32 threads, plus buffered 8 threads; 8 GB per run (4 GB on aac7).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
WB=$(wb_bin); [ "$WB" != 0 ] || skip "no tools/wbench"
DIR=$(wb_dir); SZ=8; site_is_aac7 && SZ=4
for a in "direct 8" "direct 32" "buffered 8"; do
    set -- $a; tleft_ok 0.85 || break; t=wb_$1_$2
    ecalc_srun "$t" 1 1 "$WB" -m "$1" -t "$2" -c 256 -s "$SZ" -r "$DIR/io01-%h-%r.dat" || note "$t rc $?"
    S=$(wb_kv "$OUT/raw/$t.log" "$t"); R="$R $1/$2 thr ${S} GB/s;"
done
tgt_kv q.TQ54 "write per node:$R (8 vs 32 threads direct, then buffered 8); 1 node, $SZ GB"
OPT_NO_RUNS_OK=1; finish

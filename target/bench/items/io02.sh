#!/bin/bash
# io02 - aggregate write rate at 64 nodes (TQ55): per-node files vs one shared file name (all ranks write the same file: a lock/contention bound, not a striped partitioned write), direct, 8 threads.  IO02_SZ (GB per rank, 4), IO02_NODES (64).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
WB=$(wb_bin); [ "$WB" != 0 ] || skip "no tools/wbench"
N=${IO02_NODES:-64}; [ "$(hosts_n)" -ge $N ] || N=$(hosts_n); DIR=$(wb_dir); SZ=${IO02_SZ:-4}; site_is_aac7 && SZ=2
ecalc_srun wb_pernode 1 $N "$WB" -m direct -t 8 -c 256 -s "$SZ" "$DIR/io02-%h-%r.dat" || note "per-node rc $?"
A=$(wb_kv "$OUT/raw/wb_pernode.log" wb_pernode)
tleft_ok 0.7 && { ecalc_srun wb_shared 1 $N "$WB" -m direct -t 8 -c 256 -s "$SZ" -k "$DIR/io02-shared.dat" || note "shared rc $?"; B=$(wb_kv "$OUT/raw/wb_shared.log" wb_shared); }
rm -f "$DIR/io02-shared.dat"
tgt_kv q.TQ55 "aggregate write at $N nodes: per-node files ${A:-NA} GB/s, one shared file ${B:-NA} GB/s ($SZ GB per rank, direct)"
OPT_NO_RUNS_OK=1; finish

#!/bin/bash
# io03 - striping (TQ56): wbench at 8 nodes into directories with stripe count 1, 4, 16, all OSTs (lfs setstripe when present), per-node files, direct, 8 threads; the file system type and lfs getstripe of the base dir go to the summary.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
WB=$(wb_bin); [ "$WB" != 0 ] || skip "no tools/wbench"
N=${IO03_NODES:-8}; [ "$(hosts_n)" -ge $N ] || N=$(hosts_n); DIR=$(wb_dir); SZ=4; site_is_aac7 && SZ=2
tgt_kv e.fs "$(stat -f -c %T "$DIR")"
command -v lfs >/dev/null 2>&1 && { tgt_kv e.lfs_getstripe "$(lfs getstripe -d "$DIR" 2>&1 | tr '\n' ' ' | cut -c1-200)"; HAVE_LFS=1; } || note "no lfs: stripe arms collapse to the default layout"
for c in 1 4 16 -1; do
    tleft_ok 0.85 || break
    D2=$DIR/s$c; mkdir -p "$D2"; [ -n "${HAVE_LFS:-}" ] && lfs setstripe -c "$c" "$D2" 2>/dev/null
    ecalc_srun wb_s$c 1 $N "$WB" -m direct -t 8 -c 256 -s "$SZ" "$D2/io03-%h-%r.dat" || note "stripe $c rc $?"
    S=$(wb_kv "$OUT/raw/wb_s$c.log" "wb_s$c"); R="$R c$c=${S}GB/s"
done
tgt_kv q.TQ56 "aggregate write at $N nodes by stripe count:$R"
OPT_NO_RUNS_OK=1; finish

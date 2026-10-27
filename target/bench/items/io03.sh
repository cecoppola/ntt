#!/bin/bash
# io03 - striping (TQ56): wbench at 8 nodes into directories with stripe count 1, 4, 16, all OSTs (lfs setstripe when present), per-node files, direct, 8 threads; the file system type and lfs getstripe of the base dir go to the summary.  IO03_SZ (GB per rank, 4), IO03_NODES (8), IO03_ARMS ("1 4 16 -1"; default | COUNT | COUNT:SIZE).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
WB=$(wb_bin); [ "$WB" != 0 ] || skip "no tools/wbench"
N=${IO03_NODES:-8}; [ "$(hosts_n)" -ge $N ] || N=$(hosts_n); DIR=$(wb_dir); SZ=${IO03_SZ:-4}; site_is_aac7 && SZ=2
tgt_kv e.fs "$(stat -f -c %T "$DIR")"
command -v lfs >/dev/null 2>&1 && { tgt_kv e.lfs_getstripe "$(lfs getstripe -d "$DIR" 2>&1 | tr '\n' ' ' | cut -c1-200)"; HAVE_LFS=1; } || note "no lfs: stripe arms collapse to the default layout"
for c in ${IO03_ARMS:-1 4 16 -1}; do      # arm = default (no setstripe: the PFL layout) | COUNT | COUNT:SIZE (e.g. 4:32M); the tag swaps ":" for "_" and "-" for "all"
    tleft_ok 0.85 || break
    ct=${c//:/_}; ct=${ct//-1/all}; cc=${c%%:*}; cs=; [ "$cc" != "$c" ] && cs=${c#*:}
    D2=$DIR/s$ct; mkdir -p "$D2"; [ -n "${HAVE_LFS:-}" ] && [ "$c" != default ] && lfs setstripe -c "$cc" ${cs:+-S "$cs"} "$D2" 2>/dev/null   # arm "default": the directory keeps the file system's own (PFL) layout
    ecalc_srun wb_s$ct 1 $N "$WB" -m direct -t 8 -c 256 -s "$SZ" "$D2/io03-%h-%r.dat" || note "stripe $c rc $?"
    S=$(wb_kv "$OUT/raw/wb_s$ct.log" "wb_s$ct"); R="$R c$ct=${S}GB/s"; [ -n "${HAVE_LFS:-}" ] && tgt_kv e.getstripe_$ct "$(lfs getstripe -d "$D2" 2>&1 | tr '\n' ' ' | cut -c1-160)"
done
tgt_kv q.TQ56 "aggregate write at $N nodes by stripe count:$R"
OPT_NO_RUNS_OK=1; finish

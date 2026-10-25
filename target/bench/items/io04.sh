#!/bin/bash
# io04 - read-back rates (TQ57): wbench write+read (direct and buffered) of a 4 GB file on 1 node, and tools/unpack_digits over the digit parts of an ecalc 1e9 run (WRITE=1, KEEP=1).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
WB=$(wb_bin); [ "$WB" != 0 ] || skip "no tools/wbench"
DIR=$(wb_dir); SZ=4
for m in direct buffered; do
    ecalc_srun wb_$m 1 1 "$WB" -m $m -t 8 -c 256 -s "$SZ" -r "$DIR/io04-%h.dat" || note "$m rc $?"
    wb_kv "$OUT/raw/wb_$m.log" "wb_$m" > /dev/null
done
UN=$(tool 1 tools/unpack_digits)
if [ "$UN" != 0 ] && need_path 1; then
    WRITE=1 KEEP=1 ecalc_run w1e9 D 1 "$(ecalc_digits 1e9)" || true; rec w1e9 W 1
    SD=${TGT_SCRATCH:-${ECALC_OUT_DIR:-$TGT_ROOT/build/scratch}}/${ID:-item}-$$-w1e9; tgt_kv e.kept_parts_dir "$SD"
    P=$(ls "$SD"/w1e9.out.part* "$SD"/w1e9.out 2>/dev/null | sort)
    if [ -n "$P" ]; then
        BY=$(du -cb $P | tail -1 | cut -f1); T1=$(date +%s.%N)
        ecalc_srun unpack 1 1 bash -c "$UN -q $P | wc -c"; T2=$(date +%s.%N)
        tgt_kv m.unpack_s "$(awk -v a=$T1 -v b=$T2 'BEGIN { printf "%.2f", b - a }') s"; tgt_kv m.unpack_bytes "$BY B"
        tgt_kv m.unpack_gbs "$(awk -v a=$T1 -v b=$T2 -v y=$BY 'BEGIN { printf "%.3f", y / (b - a) / 1e9 }') GB/s"
    else note "no digit parts found for unpack_digits"; fi
fi
g() { grep -h "^m.$1=" "$TGT_SUM_DIR/summary.txt" | tail -1 | cut -d= -f2; }
tgt_kv q.TQ57 "wbench read-back direct $(g wb_direct.read_mean_gbs), buffered $(g wb_buffered.read_mean_gbs); unpack_digits $(g unpack_gbs) over the written parts (1 node)"
OPT_NO_RUNS_OK=1; finish

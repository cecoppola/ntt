#!/bin/bash
# gpu02 - kernel and clock lines (TQ31 TQ32 TQ33): one ecalc 1e10 run with clock logging, verbose kernel lines and DIST_STATS, then t_ntt rates at 2^31.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 1
D=$(dig 1e10)
ecalc_run gpu02 D 1 "$D" ECALC_LOG_CLOCKS=5 RNS_VERBOSE=1 DIST_STATS=1 || true
rec gpu02 A 1
clocks_kv "$OUT/raw/gpu02.log" clk
_cat "$OUT/raw/gpu02.log" | grep -a -E 'DIST_STATS|xgmi|exchange' | head -40 > "$OUT/raw/gpu02.xgmi_lines" || true
L=$(grep -a -m1 DIST_STATS "$OUT/raw/gpu02.xgmi_lines")
[ -n "$L" ] && tgt_kv e.dist_stats_first "$L"
_cat "$OUT/raw/gpu02.log" | grep -a -i -E 'kernel|modmul|Top/s|TB/s|GB/s' | head -60 > "$OUT/raw/gpu02.kernel_lines" || true
LG=31; site_is_aac7 && LG=28
if build_test 1 t_ntt; then
    TMO=${GPU02_NTT_MIN:-6} ecalc_srun tntt 1 1 "$(tool 1 tests/t_ntt)" "$LG" || note "t_ntt rc $? (killed or failed; rates need section 5)"
    ntt_rates_kv "$OUT/raw/tntt.log" ntt
else note "t_ntt build failed"; fi
kg() { grep -h "^m.$1=" "$TGT_SUM_DIR/summary.txt" | tail -1 | cut -d= -f2; }
tgt_kv q.TQ31 "2^$LG fwd $(kg ntt.fwd_ms_mean) ($(kg ntt.fwd_tbs_mean)) per APU (t_ntt, aac7 117 ms); ecalc 1e10 total ${E_TOTAL:-NA} s"
tgt_kv q.TQ32 "xGMI stage lines in raw/gpu02.xgmi_lines: ${L:-none printed}"
tgt_kv q.TQ33 "sclk mean $(kg clk.sclk_mean_mhz) min $(kg clk.sclk_min_mhz) max $(kg clk.sclk_max_mhz), fraction below 95 % of max $(kg clk.sclk_below95pct_frac); mclk mean $(kg clk.mclk_mean_mhz); fclk mean $(kg clk.fclk_mean_mhz)"
finish

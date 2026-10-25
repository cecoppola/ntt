#!/bin/bash
# cal01 - kernel rate tables for the offline model (TQ70): t_ntt rates at 2^24 and 2^28 (2^31 is gpu02), t_alloc bandwidth of the allocation forms, t_dist over xGMI up to 2^31 with DIST_STATS (T_PIECE_31, T_XGMI_31, HIDDEN_XGMI).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 1
build_test 1 t_ntt t_alloc t_dist || note "some test build failed"
NT=$(tool 1 tests/t_ntt); AL=$(tool 1 tests/t_alloc); DI=$(tool 1 tests/t_dist)
for lg in 24 28; do [ "$NT" != 0 ] && { TMO=4 ecalc_srun ntt$lg 1 1 "$NT" $lg || note "t_ntt $lg rc $?"; ntt_rates_kv "$OUT/raw/ntt$lg.log" ntt$lg; }; done
GB=20; site_is_aac7 && GB=10
[ "$AL" != 0 ] && { T_ALLOC_SEED=0 TMO=5 ecalc_srun alloc 1 1 "$AL" $GB hipmalloc mmap hostnuma || note "t_alloc rc $?"
    _cat "$OUT/raw/alloc.log" | awk -F'|' '$1 ~ /^(hipmalloc|mmap|hostnuma) / { f = $1; gsub(/ /, "", f); split($2, a, " "); split($4, b, " "); printf "m.alloc.%s.alloc_s=%s s\nm.alloc.%s.s_per_gb=%s s/GB\nm.alloc.%s.bw_gbs=%s GB/s\n", f, a[1], f, a[2], f, b[1] }' | while IFS='=' read -r k v; do tgt_kv "$k" "$v"; done; }
LG=31; site_is_aac7 && LG=28
[ "$DI" != 0 ] && { DIST_XGMI=1 DIST_STATS=1 TMO=${CAL01_DIST_MIN:-6} ecalc_srun dist 1 1 "$DI" $LG || note "t_dist rc $? (killed or failed)"
    _cat "$OUT/raw/dist.log" | grep -a -E 'xgmi 2\^|DIST_STATS' > "$OUT/raw/dist.xgmi_lines" || true
    awk '/xgmi 2\^[0-9]+ \(/ { n = $2; sub(/2\^/, "", n); f = $6; sub(/,/, "", f); lg = n; fw = f } /DIST_STATS \(four ranks/ { for (i = 1; i <= NF; i++) { if ($i == "rows") r = $(i + 1); if ($i == "exchange") ex = $(i + 1); if ($i == "total") tot = $(i + 1); if ($i == "hidden)" || $i == "%") { } } if (match($0, /\([0-9]+ % hidden/)) h = substr($0, RSTART + 1, RLENGTH - 9); lastex = ex; lasttot = tot; lasthid = h }
        END { if (lg != "") printf "m.dist.xgmi_log2=%s\nm.dist.xgmi_fwd_s=%s s\nm.dist.stats_exchange_s=%s s\nm.dist.stats_total_s=%s s\nm.dist.hidden_pct=%s %%\n", lg, fw, lastex, lasttot, lasthid }' "$OUT/raw/dist.xgmi_lines" | while IFS='=' read -r k v; do tgt_kv "$k" "$v"; done; }
tgt_kv q.TQ70 "t_ntt 2^24/2^28 per APU in m.ntt24/ntt28.*, t_dist xGMI 2^$LG fwd in m.dist.xgmi_fwd_s, hidden m.dist.hidden_pct, alloc forms in m.alloc.*; tables in raw/*.log"
OPT_NO_RUNS_OK=1; finish

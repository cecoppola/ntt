#!/bin/bash
# ex_summary.sh <log> [tag]: one line from a t_comm --ex launch log (mnrun output, one "t_comm ex:" line per PE):
#   tag mean_ms p50_ms p95_ms node_gbs lat_us roff_wait_s sig_wait_s busy_gbs pes
# means over the PEs; roff_wait_s / sig_wait_s = sums of the XSTATS lines (COMM_XSTATS=1; kind layeq, summed over PEs and threads; NA without);
# busy_gbs = mean of the proxies' "GB/s while busy" (comm_shmem, PE_PER_APU=1 only).
log=$1; tag=${2:-$(basename "$log" .log)}
sed -E 's/^ *[0-9]+: //' "$log" | awk -v T="$tag" '
  /t_comm ex: pe [0-9]+ n [0-9]+ threads/ { for (i = 1; i < NF; i++) { if ($i == "mean_ms") a += $(i+1); if ($i == "p50_ms") b += $(i+1); if ($i == "p95_ms") c += $(i+1); if ($i == "node_gbs") d += $(i+1); if ($i == "lat_us") l += $(i+1) } n++ }
  /^xstats pe .* kind layeq: n / { for (i = 1; i < NF; i++) { if ($i == "roff_wait") ro += $(i+1); if ($i == "sig_wait") sg += $(i+1) } nx++ }
  /GB\/s while busy/ { if (match($0, /= [0-9.]+ GB\/s while busy/)) { v = substr($0, RSTART + 2, RLENGTH); sub(/ GB.*/, "", v); bs += v; nb++ } }
  END { if (!n) { printf "%s\tNO_RESULT\n", T; exit 1 }
        printf "%s\t%.3f\t%.3f\t%.3f\t%.2f\t%.1f\t%s\t%s\t%s\t%d\n", T, a/n, b/n, c/n, d/n, l/n, nx ? sprintf("%.3f", ro) : "NA", nx ? sprintf("%.3f", sg) : "NA", nb ? sprintf("%.2f", bs/nb) : "NA", n }'

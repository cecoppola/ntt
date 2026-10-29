#!/bin/bash
# monitor.sh SEC MAXSEC [STOPFILE] - timeline CSV of THIS node on stdout, one row every SEC seconds, for at most MAXSEC seconds (started by s5_run.sh --monitor, one task
# per node; it stops when STOPFILE exists (checked every second; the run dir is on the shared file system), when the parent kills its srun, or at MAXSEC at the latest).  Columns: epoch, MemAvailable / MemFree /
# Cached / Dirty (kB), load1, per GPU: power (W), hotspot (C), memory temperature (C), used vram (MB), used gtt (MB), then per NIC the packets sent / received
# (hni_pkts_*_by_tc sums) and the estimated bytes sent / received (the hni_tx_ok_* / hni_rx_ok_* size buckets x the bucket midpoint; the NIC has no byte counter).
SEC=${1:-30}; MAX=${2:-86400}; STOP=${3:-/nonexistent/stop}; HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
NICS=$(ls -d /sys/class/cxi/cxi* 2>/dev/null | xargs -n1 basename 2>/dev/null | tr '\n' ' ')
ng=0; command -v amd-smi >/dev/null && ng=$(timeout 20 amd-smi metric --csv 2>/dev/null | awk 'NR > 1 && NF { n++ } END { print n + 0 }')
h="epoch,host,memavail_kb,memfree_kb,cached_kb,dirty_kb,load1"
for ((g = 0; g < ng; g++)); do h="$h,gpu${g}_power_w,gpu${g}_hot_c,gpu${g}_memtemp_c,gpu${g}_vram_mb,gpu${g}_gtt_mb"; done
for n in $NICS; do h="$h,${n}_tx_pkts,${n}_rx_pkts,${n}_tx_bytes_est,${n}_rx_bytes_est"; done
echo "$h"; host=$(hostname); T0=$SECONDS
while [ $((SECONDS - T0)) -lt "$MAX" ] && [ ! -e "$STOP" ]; do
    s0=$SECONDS
    row="$(date +%s.%N),$host,$(awk '/^MemAvailable:/ { a = $2 } /^MemFree:/ { f = $2 } /^Cached:/ { c = $2 } /^Dirty:/ { d = $2 } END { printf "%s,%s,%s,%s", a, f, c, d }' /proc/meminfo),$(cut -d' ' -f1 /proc/loadavg)"
    if [ "$ng" -gt 0 ]; then
        row="$row,$(timeout 20 amd-smi metric -p -t -m --csv 2>/dev/null | awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next } NF { printf "%s%s,%s,%s,%s,%s", (n++ ? "," : ""), $c["socket_power"], $c["hotspot"], $c["mem"], $c["used_vram"], $c["used_gtt"] }')"
    fi
    if [ -n "$NICS" ]; then
        row="$row,$("$HERE/nodesnap.sh" nic-sel | awk -v nics="$NICS" 'BEGIN { split(nics, N, " ") }
            { n = $2; k = $3; v = $4 + 0
              if (k ~ /^hni_pkts_sent_by_tc_/) tp[n] += v; else if (k ~ /^hni_pkts_recv_by_tc_/) rp[n] += v
              else if (k ~ /^hni_(tx|rx)_ok_[0-9]/) { m = k; sub(/^hni_(tx|rx)_ok_/, "", m); lo = m + 0; hi = (m ~ /to_max/) ? 9000 : m; sub(/^[0-9]+_to_/, "", hi); hi += 0
                  mid = (m ~ /_to_/) ? (lo + hi) / 2 : lo; if (k ~ /^hni_tx/) tb[n] += v * mid; else rb[n] += v * mid } }
            END { for (i = 1; N[i] != ""; i++) { n = N[i]; printf "%s%.0f,%.0f,%.0f,%.0f", (i > 1 ? "," : ""), tp[n], rp[n], tb[n], rb[n] } }')"
    fi
    echo "$row"
    d=$((SECONDS - s0)); while [ "$d" -lt "$SEC" ] && [ ! -e "$STOP" ]; do sleep 1; d=$((SECONDS - s0)); done
done

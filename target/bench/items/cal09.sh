#!/bin/bash
# cal09 - per-NIC counters over a 2-node ecalc 1e10 run (TQ78): all cxi telemetry files of every node read before and after (value before the @ timestamp), deltas per NIC; path 2, shape C.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 2
snap() { ecalc_srun "$1" 0 2 bash -c 'for d in /sys/class/cxi/cxi*; do n=${d##*/}; for f in $d/device/telemetry/*; do [ -f "$f" ] && echo "$(hostname -s) $n ${f##*/} $(cut -d@ -f1 "$f" 2>/dev/null)"; done; done' || true; }
snap before_snap
one C 1 C 2 "$(dig 1e10 2)"
snap after_snap
cp "$OUT/raw/before_snap.log" "$OUT/raw/cxi_before.txt" 2>/dev/null; cp "$OUT/raw/after_snap.log" "$OUT/raw/cxi_after.txt" 2>/dev/null
awk 'NR == FNR { b[$1 " " $2 " " $3] = $4; next } { k = $1 " " $2 " " $3; if (k in b && $4 + 0 >= b[k] + 0) { d = $4 - b[k]; if (d > 0) { nz[$1 " " $2]++; if ($3 ~ /tx_ok|tx_bytes|tx_octets/) tx[$1 " " $2] += d } } }
    END { for (k in nz) { g = k; gsub(/ /, "_", g); printf "m.nic.%s.nonzero_counters=%d\nm.nic.%s.tx_ok_sum=%.0f\n", g, nz[k], g, tx[k] } }' "$OUT/raw/cxi_before.txt" "$OUT/raw/cxi_after.txt" | while IFS='=' read -r k v; do tgt_kv "$k" "$v"; done
tgt_kv q.TQ78 "counters per NIC and node in m.nic.*; tx_ok_sum balance over the 8 NICs per node; full before/after dumps raw/cxi_before.txt, cxi_after.txt (deltas by counter name offline); run total ${E_TOTAL:-NA} s"
finish

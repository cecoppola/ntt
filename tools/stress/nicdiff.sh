#!/bin/bash
# nicdiff.sh - sourced by tools/stress/stress.sh: nicdiff() turns the node snapshots (tools/nodesnap.sh) before and after a run into a NIC delta.
nicdiff() {      # -> $RUN/snap/nic_summary.csv from snap/before and snap/after; sets NIC_NODES NIC_TX_GB NIC_RX_GB NIC_ERR NIC_PAUSE (means per node; bytes are estimates)
    NIC_TOP=; NIC_NODES=0; NIC_TX_GB=NA; NIC_RX_GB=NA; NIC_ERR=NA; NIC_PAUSE=NA
    local b=$RUN/snap/before a=$RUN/snap/after o=$RUN/snap/nic_summary.csv
    ls "$b"/*.txt > /dev/null 2>&1 && ls "$a"/*.txt > /dev/null 2>&1 || return 0
    grep -a -l '^nic ' "$b"/*.txt "$a"/*.txt > /dev/null 2>&1 || return 0
    awk -v out="$o" -v topf="$RUN/snap/.nic_top" 'BEGIN { print "host,nic,tx_pkts,rx_pkts,tx_bytes_est,rx_bytes_est,tx_paused,rx_paused,tx_stall_llr,error_like_sum,fec,pause_evt,stall,drop" > out }
        function mid(k,  m, lo, hi) { m = k; sub(/^hni_(tx|rx)_ok_/, "", m); lo = m + 0; hi = (m ~ /to_max/) ? 9000 : m; sub(/^[0-9]+_to_/, "", hi); hi += 0; return (m ~ /_to_/) ? (lo + hi) / 2 : lo }
        FNR == 1 { f = FILENAME; isb = (f ~ /\/snap\/before\//); h = f; sub(/.*\//, "", h); sub(/\.txt$/, "", h) }
        $1 == "nic" { key = h SUBSEP $2 SUBSEP $3; if (isb) bv[key] = $4 + 0; else { av[key] = $4 + 0; seen[h SUBSEP $2] = 1 } ; names[key] = 1 }
        END { for (key in names) { split(key, K, SUBSEP); h = K[1]; n = K[2]; k = K[3]; d = av[key] - bv[key]; if (d < 0) d = 0
                if (k ~ /^hni_pkts_sent_by_tc_/) tp[h, n] += d; else if (k ~ /^hni_pkts_recv_by_tc_/) rp[h, n] += d
                else if (k ~ /^hni_(tx|rx)_ok_[0-9]/) { if (k ~ /^hni_tx/) tb[h, n] += d * mid(k); else rb[h, n] += d * mid(k) }
                else if (k ~ /^hni_tx_paused_/) tz[h, n] += d; else if (k ~ /^hni_rx_paused_/) rz[h, n] += d; else if (k == "hni_tx_stall_llr") sl[h, n] += d
                if (k ~ /(err|drop|nack|retr|timeout|discard|fail|ovf|poison|bad|jabber)/ && k !~ /_ok_/) er[h, n] += d
                if (k ~ /^hni_pcs_(corrected_cw|uncorrected_cw|fecl_errors_|pml_[ms]be_error_cnt)/) fe[h, n] += d          # FEC: corrected / uncorrected codewords, per-lane FEC-layer errors
                if (k ~ /^hni_pause_(sent|recv_|xoff_sent_|refresh)/) pe[h, n] += d                                          # pause frames sent / received (events)
                if (k == "hni_tx_stall_llr" || k ~ /^hni_rx_stall_/) st[h, n] += d                                           # link-layer-retry / rx stalls
                if (k ~ /^(hni|ixe|pct)_/ && k ~ /(drop|discard|nack|poison|bad|replay|duplicate|unexpected_seq)/ && k !~ /_ok_/) dr[h, n] += d }   # dropped / discarded / nacked / replayed packets
            for (hn in seen) { split(hn, K, SUBSEP); h = K[1]; n = K[2]
                printf "%s,%s,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n", h, n, tp[h, n], rp[h, n], tb[h, n], rb[h, n], tz[h, n], rz[h, n], sl[h, n], er[h, n], fe[h, n], pe[h, n], st[h, n], dr[h, n] >> out
                H[h] = 1; TB[h] += tb[h, n]; RB[h] += rb[h, n]; ER[h] += er[h, n]; PZ[h] += tz[h, n] + rz[h, n]; FE[h] += fe[h, n]; PE[h] += pe[h, n]; ST[h] += st[h, n]; DR[h] += dr[h, n] }
            for (h in H) sc[h] = FE[h] + DR[h]
            for (i = 1; i <= 5; i++) { b = ""; for (h in H) if (!(h in used) && (b == "" || sc[h] > sc[b])) b = h
                if (b == "") break; used[b] = 1; nu++; printf "%s%s fec=%.0f pause=%.0f stall=%.0f drop=%.0f", (i > 1 ? "; " : ""), b, FE[b], PE[b], ST[b], DR[b] > topf }
            if (nu) printf "\n" > topf
            for (h in H) { nn++; t += TB[h]; r += RB[h]; e += ER[h]; z += PZ[h] }
            if (nn) printf "%d %.3f %.3f %.0f %.0f\n", nn, t / nn / 1e9, r / nn / 1e9, e / nn, z / nn }' "$b"/*.txt "$a"/*.txt > "$RUN/snap/.nic_mean" 2>/dev/null
    { sort -t, -k1,2 -o "$o.sorted" <(tail -n +2 "$o") && { head -1 "$o"; cat "$o.sorted"; } > "$o.new" && mv "$o.new" "$o"; rm -f "$o.sorted"; } 2>/dev/null
    read -r NIC_NODES NIC_TX_GB NIC_RX_GB NIC_ERR NIC_PAUSE < "$RUN/snap/.nic_mean" 2>/dev/null || true; rm -f "$RUN/snap/.nic_mean"
    NIC_TOP=$(head -1 "$RUN/snap/.nic_top" 2>/dev/null); rm -f "$RUN/snap/.nic_top"
    NIC_NODES=${NIC_NODES:-0}; NIC_TX_GB=${NIC_TX_GB:-NA}; NIC_RX_GB=${NIC_RX_GB:-NA}; NIC_ERR=${NIC_ERR:-NA}; NIC_PAUSE=${NIC_PAUSE:-NA}; return 0
}

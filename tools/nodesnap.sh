#!/bin/bash
# nodesnap.sh - state of THIS node on stdout, in "## name" sections (run one task per node by tools/stress/stress.sh --nic; also usable by hand).
#   nodesnap.sh [static]       static adds the once-per-run facts (kernel, numastat, amd-smi static, cxi_stat); every call: time, uptime, /proc/meminfo,
#                              amd-smi metric (rocm-smi when there is no amd-smi), "## nic" = the Slingshot NIC counters, one "nic <cxiN> <name> <value>" line per
#                              NONZERO counter of /sys/class/cxi/*/device/telemetry (that file's "value@time" read).  Every tool is optional; a missing one is skipped.
#                              "## health" = tools/nodescreen.sh --health: EDAC uncorrectable / correctable totals and the boot time (read before / after: per-run deltas, a reboot
#                              in between; NIC error classes are in "## nic": hni_pcs_* FEC, hni_pause_*, hni_*_paused_*, hni_*stall*, hni_llr_*, drops).
#   nodesnap.sh nic-sel        only the traffic / pause counters read by a monitor (about 55 per NIC, ~0.5 s for 8 NICs)
t() { timeout 20 "$@" 2>&1; }
nic_read() {    # [sel] -> nic lines; one background reader per NIC (the 1890 files of a NIC take ~3 s: mostly system time)
    local d n f v
    for d in /sys/class/cxi/cxi*/device/telemetry; do
        [ -d "$d" ] || continue; n=$(basename "$(dirname "$(dirname "$d")")")
        if [ "${1:-}" = sel ]; then
            ( for f in "$d"/hni_tx_ok_* "$d"/hni_rx_ok_* "$d"/hni_pkts_sent_by_tc_* "$d"/hni_pkts_recv_by_tc_* "$d"/hni_tx_paused_* "$d"/hni_rx_paused_* "$d"/hni_tx_stall_llr; do
                  [ -r "$f" ] && { v=$(cat "$f" 2>/dev/null); echo "nic $n ${f##*/} ${v%%@*}"; }; done ) &
        else
            ( for f in "$d"/*; do v=$(cat "$f" 2>/dev/null); v=${v%%@*}; case $v in ''|0) ;; *) echo "nic $n ${f##*/} $v";; esac; done ) &
        fi
    done
    wait
}
case ${1:-} in
  nic-sel) nic_read sel; exit 0;;
esac
echo "## time"; date +%s.%N; date '+%F %T %Z'
echo "## host"; hostname; uptime
echo "## health"; bash "$(dirname "${BASH_SOURCE[0]}")/nodescreen.sh" --health 2>&1 | head -3   # EDAC ue/ce totals + boot time (C4)
if [ "${1:-}" = static ]; then
    echo "## kernel"; uname -a; head -4 /etc/os-release 2>/dev/null
    echo "## cmdline"; cat /proc/cmdline 2>/dev/null
    echo "## lscpu"; t lscpu | head -30
fi
echo "## meminfo"; cat /proc/meminfo
echo "## vmstat"; grep -a -E '^(thp_|pgfault|pgmajfault|numa_|nr_free_pages|nr_dirty|compact_stall)' /proc/vmstat 2>/dev/null | head -40
if [ "${1:-}" = static ]; then
    echo "## numastat"; command -v numastat >/dev/null && t numastat -m | head -60
    echo "## numactl"; command -v numactl >/dev/null && t numactl -H | head -20
    echo "## amd-smi static"; if command -v amd-smi >/dev/null; then t amd-smi static --csv; else command -v rocm-smi >/dev/null && t rocm-smi --showallinfo | head -120; fi
    echo "## cxi_stat"; command -v cxi_stat >/dev/null && t cxi_stat
    echo "## hugepages"; cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null
fi
echo "## amd-smi metric"
if command -v amd-smi >/dev/null; then t amd-smi metric --csv
elif command -v rocm-smi >/dev/null; then t rocm-smi --showpower --showtemp --showmemuse --showuse; fi
echo "## nic"; nic_read
echo "## end"

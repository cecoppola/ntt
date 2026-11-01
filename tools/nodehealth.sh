#!/bin/bash
# tools/nodehealth.sh - one line on stdout: this node's memory-error totals and boot time (C4, results/HEALTH.md).
#   nodehealth.sh            ->  NS health host=<h> ue=<n> ce=<n> boot=<epoch> boot_at=<YYYY-MM-DDTHH:MM:SSZ> up_s=<s> mc=<mcN:ue/ce,...>
# ue / ce = the sums of /sys/devices/system/edac/mc/mc*/ue_count and ce_count (uncorrectable / correctable errors since boot, no timestamps), mc = the
# same per memory controller.  ue=NA ce=NA when the node has no EDAC controller.  The boot time comes from /proc/uptime.
# Test hook: NODEHEALTH_SYSFS_ROOT=<dir> reads <dir>/sys/devices/system/edac/mc/... instead of /sys (a fake tree proves the refusal without a broken node).
# Used by tools/nodescreen.sh (node_local.sh) and remote/run/nodesnap.sh.  No credentials, no target values.
R=${NODEHEALTH_SYSFS_ROOT:-}
ue=0 ce=0 n=0 mcs=
for m in "$R"/sys/devices/system/edac/mc/mc[0-9]*; do
    [ -r "$m/ue_count" ] || continue
    u=$(cat "$m/ue_count" 2>/dev/null); c=$(cat "$m/ce_count" 2>/dev/null)
    case $u$c in *[!0-9]*|'') continue;; esac
    ue=$((ue + u)); ce=$((ce + c)); n=$((n + 1)); mcs="$mcs${mcs:+,}${m##*/}:$u/$c"
done
[ "$n" = 0 ] && { ue=NA; ce=NA; mcs=NA; }
up=$(cut -d' ' -f1 /proc/uptime 2>/dev/null); up=${up%%.*}; up=${up:-0}
now=$(date +%s); boot=$((now - up))
echo "NS health host=$(hostname -s) ue=$ue ce=$ce boot=$boot boot_at=$(date -u -d "@$boot" +%FT%TZ) up_s=$up mc=$mcs"

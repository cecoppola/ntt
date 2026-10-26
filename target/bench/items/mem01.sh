#!/bin/bash
# mem01 (TQ04, TQ05) - the node's memory edges on one node, each form ALONE: t_edge dev / vmm (device pools), host / malloc (host side),
# then one ecalc 1e9 (layout D) with DB_POOL_VERBOSE=1 for the VMM map rate; snapshots of meminfo, ulimit, THP.
# Metrics: m.edge_<form>_gb (node total, touched), m.map_s_per_gb; suggestion e.suggest_ECALC_DEVICE_GB / _NODE_GB.
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
PN=1; ecalc_have 1 || PN=2
T=$(ecalc_tool $PN tests/t_edge)
[ -x "$T" ] || { tgt_kv note "no t_edge in the build tree (bld01/bld02 absent)"; ecalc_end SKIP; }
STEP_D=${MEM01_STEP_DEV:-4}; CAP_D=${MEM01_CAP_DEV:-520}; STEP_H=${MEM01_STEP_HOST:-16}; CAP_H=${MEM01_CAP_HOST:-520}
{ echo "== meminfo"; grep -E '^(MemTotal|MemFree|MemAvailable|Cached|HugePages_Total|Hugepagesize|Shmem|SwapTotal)' /proc/meminfo
  echo "== ulimit"; ulimit -a; echo "== THP"; cat /sys/kernel/mm/transparent_hugepage/enabled 2>&1; echo "== overcommit"; cat /proc/sys/vm/overcommit_memory /proc/sys/vm/overcommit_ratio 2>&1
  echo "== numa"; command -v numactl >/dev/null && numactl -H 2>&1 | head -20; } > raw/snapshot_login.txt 2>&1
srun --jobid="$JOBID" -N1 -n1 -w "$(ecalc_hosts 0 1)" --overlap bash -c 'grep -E "^(MemTotal|MemFree|MemAvailable|HugePages_Total)" /proc/meminfo; ulimit -l; cat /sys/kernel/mm/transparent_hugepage/enabled' > raw/snapshot_node.txt 2>&1
edge() {   # FORM STEP CAP
    TMO=${MEM01_TMO:-6} ecalc_srun edge_$1 $PN 1 "$T" "$1" "$2" "$3"
    local lg=$OUT/raw/edge_$1.log v fl av
    v=$(sed -n "s/^t_edge: the edge in form $1: \([0-9.]*\) GB.*/\1/p" "$lg" | tail -1)
    fl=$(grep -a -m1 'failed' "$lg" | cut -c1-200); av=$(sed -n 's/.*MemAvailable \([0-9.]*\) GB at the start.*/\1/p' "$lg" | head -1)
    if [ -z "$v" ]; then   # the kernel's OOM killer (host / malloc forms) ends the program before it reports: the last committed step is a lower bound of the edge
        v=$(sed -n 's/^t_edge: *\([0-9.]*\) GB committed in [0-9]* steps.*/\1/p' "$lg" | tail -1)
        [ -n "$v" ] && fl="killed by the kernel (OOM) after the step that committed $v GB; the edge lies within one step above"
    fi
    if [ -n "$v" ]; then tgt_kv m.edge_$1_gb "$v GB"; [ -n "$fl" ] && tgt_kv e.edge_$1_stop "$fl" || tgt_kv e.edge_$1_stop "cap $3 GB reached"
    else tgt_kv e.edge_$1_stop "no result: $(tail -1 "$lg" | cut -c1-160)"; fi
    [ -n "$av" ] && tgt_kv m.memavail_before_$1_gb "$av GB"
    eval "EDGE_$1=\${v:-}"
}
edge dev "$STEP_D" "$CAP_D"; edge vmm "$STEP_D" "$CAP_D"; edge host "$STEP_H" "$CAP_H"; edge malloc "$STEP_H" "$CAP_H"
# the VMM map rate as ecalc sees it (a small run, layout D, 1 node)
ecalc_have 1 && { TMO=3 ecalc_run map D 1 "$(ecalc_digits 1e9)" DB_POOL_VERBOSE=1; ecalc_kv map
  r=$(ecalc_grab "$E_LOG" 'dbig pool: APU[0-9]+ VMM arena' | sed -nE 's/.*\(([0-9.]+) s\/GB\).*/\1/p')
  all=$( { zcat -f "$E_LOG" 2>/dev/null || cat "$E_LOG"; } | sed -E 's/^ *[0-9]+: //' | sed -nE '/dbig pool: APU[0-9]+ VMM arena/ s/.*\(([0-9.]+) s\/GB\).*/\1/p' | tr '\n' ' ')
  [ -n "$all" ] && tgt_kv m.map_s_per_gb "$(ecalc_mean $all) s/GB"; }
ecalc_q TQ04 "device edge per node (4 APUs): hipMalloc ${EDGE_dev:-NA} GB, VMM ${EDGE_vmm:-NA} GB (steps ${STEP_D} GB, cap ${CAP_D}); per APU ~$(awk -v v="${EDGE_vmm:-0}" 'BEGIN { printf "%.1f", v / 4 }') GB; ECALC_DEVICE_GB suggestion = the VMM edge minus the guard"
ecalc_q TQ05 "host edge per node: hipHostMalloc ${EDGE_host:-NA} GB, malloc ${EDGE_malloc:-NA} GB (steps ${STEP_H} GB, cap ${CAP_H}); MemAvailable at start $(sed -n 's/^MemAvailable:[ ]*//p' raw/snapshot_login.txt | head -1); ECALC_NODE_GB suggestion = the smaller edge minus the guard"
[ -n "${EDGE_vmm:-}" ] && tgt_kv e.suggest_ECALC_DEVICE_GB "$EDGE_vmm"
{ [ -n "${EDGE_host:-}" ] || [ -n "${EDGE_malloc:-}" ]; } && tgt_kv e.suggest_ECALC_NODE_GB "$(ecalc_stat "${EDGE_host:-${EDGE_malloc}}" "${EDGE_malloc:-${EDGE_host}}" | awk '{print ($1<$2)?$1:$2}')"
if [ -n "${EDGE_vmm:-}" ] && [ -n "${EDGE_host:-}" ]; then ecalc_end PASS; elif [ -n "${EDGE_vmm:-}${EDGE_dev:-}${EDGE_host:-}${EDGE_malloc:-}" ]; then ecalc_end PARTIAL; else ecalc_end FAIL; fi

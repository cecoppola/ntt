#!/bin/bash
# net03 (TQ10) - NIC map of one node: sysfs cxi devices (numa node, link), fi_info -p cxi domains, the APU numa nodes, and the PE -> NIC / numa
# map that t_xshmem prints (MAP lines) for 4 PEs with the default NIC choice (COMM_OFI_NICS unset) and with an explicit COMM_OFI_NICS pairing.
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
H=$(ecalc_hosts 0 1)
cat > raw/nic_probe.sh <<'PROBE'
echo "== host $(hostname -s)"
for d in /sys/class/cxi/* /sys/class/cxi_user/*; do [ -e "$d" ] || continue; printf '%s numa=%s' "$d" "$(cat $d/device/numa_node 2>/dev/null)"; for f in $d/device/net/*/operstate $d/device/net/*/speed; do [ -e "$f" ] && printf ' %s=%s' "$(basename $(dirname $f))/$(basename $f)" "$(cat $f 2>/dev/null)"; done; echo; done
echo "== net numa"; for n in /sys/class/net/*; do [ -e "$n/device/numa_node" ] && echo "$(basename $n) numa=$(cat $n/device/numa_node) state=$(cat $n/operstate 2>/dev/null) speed=$(cat $n/speed 2>/dev/null)"; done
echo "== drm numa"; for c in /sys/class/drm/card[0-9]*; do [ -e "$c/device/numa_node" ] && echo "$(basename $c) numa=$(cat $c/device/numa_node)"; done
echo "== fi_info"; command -v fi_info >/dev/null && fi_info -p cxi 2>&1 | grep -E 'domain:|provider:|version:' | sort | uniq -c | head -30
echo "== numactl"; command -v numactl >/dev/null && numactl -H 2>&1 | head -14
PROBE
ecalc_have 1 && PN=1 || PN=2
ecalc_srun probe $PN 1 bash raw/nic_probe.sh
NSYS=$(grep -c '^/sys/class/cxi/' raw/probe.log); NDOM=$(grep -a -c 'domain: cxi' raw/probe.log); NSYS=${NSYS:-0}; NDOM=${NDOM:-0}
NUMAS=$(grep -a '^/sys/class/cxi/' raw/probe.log | sed -n 's/.*numa=\([-0-9]*\).*/\1/p' | tr '\n' ' ')
tgt_kv m.cxi_sysfs_devices "$NSYS devices"; tgt_kv m.fi_info_cxi_domains "$NDOM domains"; tgt_kv e.cxi_numa_nodes "${NUMAS:-NA}"
MAPQ=
map() {   # TAG PATH [VAR=VALUE...]
    local tag=$1 pn=$2; shift 2
    TMO=3 XT_QUICK=1 ecalc_xt "$tag" "$pn" 1 basic "$@"
    [ "$X_SKIP" = 1 ] && { tgt_kv e.$tag "skipped (no t_xshmem for path $pn)"; return; }
    grep -a 'MAP pe' "$OUT/raw/$tag.log" | sed -E 's/^ *[0-9]+: //' | sort -u > "raw/$tag.map"
    local pes npe; npe=$(grep -c . raw/$tag.map)
    tgt_kv m.${tag}_map_lines "$npe lines"; tgt_kv e.${tag}_xt_result "$X_RESULT"
    MAPQ="$MAPQ; $tag: $(sed -E 's/^MAP pe ([0-9]+).* gpu ([0-9]+) nics ([0-9]+) \[([^]]*)\] numa ([-0-9]*).*/pe\1 gpu\2 nics\3[\4] numa\5/' raw/$tag.map | head -4 | tr '\n' ' ')"
}
ecalc_have 1 && { map map_default 1; map map_explicit 1 COMM_OFI_NICS="0,4;1,5;2,6;3,7"; }
ecalc_refresh 600 && map map_path2 2
SUMQ=$(grep -a '^/sys/class/cxi/' raw/probe.log | head -8 | sed -n 's/.*numa=\([-0-9]*\).*/\1/p' | paste -sd, -)
ecalc_q TQ10 "$NSYS cxi sysfs devices, $NDOM fi_info cxi domains, device numa nodes [${SUMQ:-NA}]; PE map (default NIC choice)${MAPQ:- not taken}"
if [ "$NSYS" -gt 0 ] || [ "$NDOM" -gt 0 ]; then ecalc_end PASS; else tgt_kv note "no cxi device seen on the node"; ecalc_end PARTIAL; fi

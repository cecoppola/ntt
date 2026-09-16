#!/bin/bash
# Per-node memory/compute partition survey (PLAN.md Phase 1b, D6/D9).
[ -z "${MODULEPATH:-}" ] && source /etc/profile.d/lmod.sh 2>/dev/null
module load rocm >/dev/null 2>&1
echo "=== $(hostname) $(date -Is) kernel $(uname -r)"
echo "--- amd-smi static --partition"; amd-smi static --partition 2>&1 | grep -vE "^\s*$" | head -40
echo "--- rocm-smi partitions"; rocm-smi --showcomputepartition --showmemorypartition 2>&1 | grep -iE "partition|GPU\[" | head -12
echo "--- amd-smi static --limit (GPU0)"; amd-smi static --gpu 0 --limit 2>&1 | grep -vE "^\s*$" | head -20
echo "--- numa"; numactl -H | grep -E "available|size|distances" -A0; numactl -H | tail -5
echo "--- kfd nodes: $(ls /sys/class/kfd/kfd/topology/nodes/ | wc -l); gpus per rocminfo: $(rocminfo | grep -c gfx942)"
echo "--- amdgpu/amdttm params"; for p in /sys/module/amdgpu/parameters/{sched_policy,noretry,vm_fragment_size} /sys/module/amdttm/parameters/pages_limit; do printf "%s=%s " $(basename $p) "$(cat $p 2>/dev/null)"; done; echo
echo "--- hbm per gpu"; rocm-smi --showmeminfo vram 2>&1 | grep "Total Memory" | head -4
echo "--- firmware/bios"; cat /sys/class/dmi/id/bios_version /sys/class/dmi/id/product_name 2>/dev/null | tr '\n' ' '; echo; cat /sys/class/drm/card*/device/vbios_version 2>/dev/null | sort -u | head -2
echo "--- power cap"; rocm-smi --showmaxpower 2>&1 | grep -i "power" | head -4

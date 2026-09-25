#!/bin/bash
# N414 A5: a node's properties (run on the node).  Usage: props.sh <tag>
echo "== $(hostname) $(date +%H:%M:%S) $1"
echo "-- uname / uptime"; uname -r; uptime; cat /proc/cmdline
echo "-- numactl -H"; numactl -H
echo "-- meminfo"; cat /proc/meminfo
echo "-- buddyinfo"; cat /proc/buddyinfo
echo "-- pagetypeinfo (head)"; head -n 30 /proc/pagetypeinfo 2>&1
echo "-- THP"; for f in /sys/kernel/mm/transparent_hugepage/enabled /sys/kernel/mm/transparent_hugepage/defrag /sys/kernel/mm/transparent_hugepage/shmem_enabled /sys/kernel/mm/transparent_hugepage/khugepaged/defrag /sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan; do echo "$f: $(cat $f 2>&1)"; done
echo "-- hugepages"; grep -H . /sys/kernel/mm/hugepages/*/nr_hugepages 2>&1
echo "-- vm"; for f in compaction_proactiveness extfrag_threshold min_free_kbytes zone_reclaim_mode watermark_scale_factor watermark_boost_factor swappiness overcommit_memory numa_balancing; do echo "$f: $(cat /proc/sys/vm/$f 2>/dev/null)"; done; echo "numa_balancing(kernel): $(cat /proc/sys/kernel/numa_balancing 2>/dev/null)"
echo "-- vmstat (compact/thp/pgmigrate)"; grep -E "compact|thp_|migrate|numa_" /proc/vmstat
echo "-- swap"; swapon --show 2>&1; cat /proc/swaps
echo "-- env"; env | grep -E "HSA_|HIP_|ROCR|AMD_|GPU_" | sort
echo "-- amdgpu params"; for f in /sys/module/amdgpu/parameters/*; do printf "%s=%s " "$(basename $f)" "$(cat $f 2>/dev/null)"; done; echo
echo "-- drm mem_info"; for c in /sys/class/drm/card*/device; do [ -f $c/mem_info_vram_total ] && echo "$c vram_total $(cat $c/mem_info_vram_total) used $(cat $c/mem_info_vram_used) gtt_total $(cat $c/mem_info_gtt_total 2>/dev/null) gtt_used $(cat $c/mem_info_gtt_used 2>/dev/null) xgmi_hive $(cat $c/xgmi_hive_info/xgmi_hive_id 2>/dev/null)"; done
echo "-- ttm"; grep -H . /sys/kernel/mm/ttm/* /sys/kernel/debug/ttm/* 2>/dev/null | head
echo "-- rocm-smi"; bash -lc 'module load rocm >/dev/null 2>&1; rocm-smi --showclocks --showperflevel --showmemuse --showpower 2>&1 | grep -v "^$" | head -n 60; rocm-smi --showxgmierr 2>&1 | head -n 12; amd-smi static -b 2>/dev/null | head -n 20'
echo "-- kfd"; cat /sys/class/kfd/kfd/topology/nodes/*/properties 2>/dev/null | grep -E "^(simd_count|max_engine_clk_fcompute|capability|local_mem_size|fw_version|drm_render_minor)" | paste -sd' ' | head -c 2000; echo
echo "-- df"; df -h 2>/dev/null | grep -v tmpfs | head -n 20; df -h /tmp /dev/shm /var/tmp 2>/dev/null
echo "-- top mem users"; ps -eo pid,user,rss,comm --sort=-rss | head -n 8

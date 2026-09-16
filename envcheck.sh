#!/bin/bash
# Phase 0 environment check, run on a compute node via srun.
source /etc/profile.d/lmod.sh 2>/dev/null || source /usr/share/lmod/lmod/init/bash 2>/dev/null
module load rocm 2>&1
echo "=== host/date"; hostname; date -Is; uname -r
echo "=== rocm"; echo ROCM_PATH=$ROCM_PATH; hipcc --version 2>&1 | head -3; cat $ROCM_PATH/.info/version 2>/dev/null
echo "=== lscpu"; lscpu | grep -E "Model name|^CPU\(s\)|Thread|Core|Socket|NUMA|L1d|L2|L3"
echo "=== numactl"; numactl -H 2>&1
echo "=== free"; free -g
echo "=== ulimit -l"; ulimit -l
echo "=== ulimit -a"; ulimit -a
echo "=== hugepages"; cat /sys/kernel/mm/transparent_hugepage/enabled; grep -i huge /proc/meminfo
echo "=== amdttm/amdgpu params"; for p in /sys/module/amdttm/parameters/pages_limit /sys/module/amdttm/parameters/page_pool_size /sys/module/amdgpu/parameters/vm_fragment_size; do echo -n "$p: "; cat $p 2>/dev/null || echo n/a; done
echo "=== rocm-smi"; rocm-smi --showproductname --showclocks --showmeminfo vram --showpower --showtemp 2>&1 | grep -vE "^=+$|^$" | head -80
echo "=== amd-smi version"; amd-smi version 2>&1 | head -3
echo "=== rocminfo agents"; rocminfo 2>/dev/null | grep -E "^\s+Name:|Compute Unit|Max Clock|Uuid" | head -40
echo "=== xnack"; rocminfo 2>/dev/null | grep -m1 -i "xnack"
echo "=== kfd topology"; ls /sys/class/kfd/kfd/topology/nodes/ 2>/dev/null

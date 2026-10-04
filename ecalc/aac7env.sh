#!/bin/bash
# aac7env.sh - the aac7 (HPE Cray EX, 4 x MI300A + 4 x Slingshot-11 per node, Cray OpenSHMEMX 11.8.0, ROCm 7.0.3) environment
# of ecalc (Phase 16 A, results/A16.md).  Two uses:
#   source ecalc/aac7env.sh         the modules (cray-dsmml cray-openshmemx rocm) and the variables the scripts read on aac7:
#                                   MNRUN_MODULES, MNRUN_CPUS_PER_TASK=auto (srun confines a task to 2 CPUs otherwise), MNACCEPT_TMP
#                                   under $HOME (the nodes' /tmp is RAM), AAC7_PART; plus the helpers aac7_alloc / aac7_wait / aac7_log.
#   bash ecalc/aac7env.sh --log     the log block of TARGET_HW_REVIEW N3, printed once per node by the task that runs it (mnrun.sh's
#                                   wrappers call it when ECALC_LOG_CLOCKS is set): the node, the task's CPU set, rocm-smi's current
#                                   sclk / mclk / fclk per APU, and the NUMA map APU i -> NUMA node -> cxi<j> (one Cassini per socket), so
#                                   a run's per-APU dist_mn times can be read against the clocks and the NIC each APU thread (pinned to
#                                   its NUMA node by mem.c) reaches.  ECALC_LOG_CLOCKS=<seconds >= 2> adds a sampler: the clocks line
#                                   again every that many seconds while the command runs (the binning tail under load, not at idle).
# Every variable here is a switch of the scripts with its aac7 value; nothing changes for aac6 (the defaults stay).

# ---- the log block (a task on a node) ----------------------------------------------------------------------------------------
aac7_log() {
    local h; h=$(hostname -s)
    echo "aac7env: node $h task ${SLURM_PROCID:-?} of ${SLURM_NTASKS:-?} $(date '+%Y-%m-%dT%H:%M:%S%z') cpus $(grep Cpus_allowed_list /proc/self/status | awk '{print $2}')"
    # the clocks: one line per APU, sclk / mclk / fclk as rocm-smi --showclocks prints them ("GPU[i] : sclk clock level: n: (fMhz)")
    aac7_clocks() { rocm-smi --showclocks 2>/dev/null | awk -v h="$h" -v t="$1" '
        match($0, /^GPU\[[0-9]+\]/) { g = substr($0, 1, RLENGTH); if (!(g in seen)) { seen[g] = 1; order[++n] = g } 
            if (match($0, /(sclk|mclk|fclk) clock level: [^(]*\(([0-9]+)Mhz\)/)) { split(substr($0, RSTART, RLENGTH), a, / /); v = substr($0, RSTART, RLENGTH); sub(/.*\(/, "", v); sub(/Mhz\)/, "", v); c[g] = c[g] " " a[1] "=" v } }
        END { for (i = 1; i <= n; i++) print "aac7env: node " h " clocks" (t != "" ? " t=" t "s" : "") " " order[i] c[order[i]] }'; }
    command -v rocm-smi > /dev/null 2>&1 && aac7_clocks ""
    # ECALC_LOG_CLOCKS=<seconds >= 2>: a sampler every that many seconds while the wrapper's process (the command, after its exec) lives
    local every=${ECALC_LOG_CLOCKS:-0}
    if [ "$every" -ge 2 ] 2>/dev/null && command -v rocm-smi > /dev/null 2>&1; then
        ( local t0=$SECONDS p=$PPID; while kill -0 "$p" 2>/dev/null; do sleep "$every"; kill -0 "$p" 2>/dev/null || break; aac7_clocks "$((SECONDS - t0))"; done ) 2>/dev/null &
        disown 2>/dev/null
    fi
    # the NUMA map: APU i (the KFD order, by PCI bus like HIP's device order) -> its numa_node; cxi<j> -> its numa_node
    local i=0 d nn; local -a gpu_numa
    for d in $(ls -d /sys/bus/pci/devices/*/ 2>/dev/null); do
        case "$(cat "$d"/class 2>/dev/null)" in 0x030000|0x038000|0x120000) ;; *) continue;; esac   # VGA, display, processing accelerator (MI300A)
        [ "$(cat "$d"/vendor 2>/dev/null)" = 0x1002 ] || continue
        gpu_numa[$i]="$(basename "$d"):numa$(cat "$d"/numa_node 2>/dev/null)"; i=$((i + 1))
    done
    local j nic=""
    for j in 0 1 2 3; do [ -e /sys/class/cxi/cxi$j/device/numa_node ] && nic="$nic cxi$j:numa$(cat /sys/class/cxi/cxi$j/device/numa_node)"; done
    echo "aac7env: node $h apus ${gpu_numa[*]} nics$nic"
    # the SHMEM library's own NIC choice, if it was asked to print it (SHMEM_OFI_NIC_POLICY, SHMEM_OFI_NUM_NICS are the caller's)
    [ -n "${SHMEM_OFI_NIC_POLICY:-}${SHMEM_OFI_NUM_NICS:-}" ] && echo "aac7env: node $h SHMEM_OFI_NIC_POLICY=${SHMEM_OFI_NIC_POLICY:-} SHMEM_OFI_NUM_NICS=${SHMEM_OFI_NUM_NICS:-}"
    return 0
}
if [ "${1:-}" = --log ]; then aac7_log; exit 0; fi

# ---- sourced: modules and variables ---------------------------------------------------------------------------------------
# Phase 16 V (results/V16.md): the fastest stack measured on aac7 becomes the default — rocm/7.2.4 (identical digits to
# 7.0.3, 168.5 vs 205.9 s at 1e11, C16 §2.7) instead of the system default rocm/7.0.3.  AAC7_ROCM overrides it
# (AAC7_ROCM=rocm/7.0.3 reverts to the old stack, e.g. to match the target's TARGET_HW_REVIEW ROCm version).
export AAC7_ROCM=${AAC7_ROCM:-rocm/7.2.4}
if [ -d /opt/cray/pe/sma ]; then
    export MNRUN_MODULES="cray-dsmml cray-openshmemx $AAC7_ROCM"
    # the login/compute default shell profile already has rocm/7.0.3 loaded; "rocm" is a conflict-marked family
    # (module-whatis "conflict rocm" on every rocm/* and rocm-new/* modulefile) so a later "module load rocm/X" is a
    # silent no-op (prints ERROR:150 to stderr, swallowed by 2>&1, and leaves 7.0.3 active) unless 7.0.3 is unloaded
    # first by its exact name.  Measured on aac7 2026-10-04 (results/V16.md step 1).
    module unload rocm/7.0.3 > /dev/null 2>&1 || true
    module load $MNRUN_MODULES > /dev/null 2>&1 || true
fi
export MNRUN_CPUS_PER_TASK=${MNRUN_CPUS_PER_TASK:-auto}
export MNACCEPT_TMP=${MNACCEPT_TMP:-$HOME/p16/mnaccept_tmp}
export AAC7_PART=${AAC7_PART:-192C4G1H_MI300A_RHEL9_A1}
# AAC7_FI_TUNE=1: libfabric/CXI tunables that an A/B on 2 nodes (results/V16.md §3c) found worth keeping for large SHMEM puts.
# Off by default — none of this changes the default stack, only an opt-in override.
if [ "${AAC7_FI_TUNE:-0}" != 0 ]; then
    : # results/V16.md §3c: no knob beat the > 5% wall / > 10% GB/s bar in the brief A/B; placeholder for a later pass.
fi
[ -d "$HOME/gmp/lib" ] && export GMP_HOME=${GMP_HOME:-$HOME/gmp}
# aac7_alloc <nodes> <h:mm:00> [name] [sbatch options...]: an exclusive allocation of whole nodes, prints the job id
aac7_alloc() { local n=$1 t=$2 nm=${3:-p16}; shift 3 2>/dev/null; local s; s=$(echo "$t" | awk -F: '{ print $1 * 3600 + $2 * 60 + $3 }')
    sbatch -p "$AAC7_PART" -N "$n" --exclusive -c 192 --gpus-per-node=4 -t "$t" -J "$nm" --parsable "$@" --wrap "sleep $s"; }
# aac7_wait <jobid> [seconds]: until the job runs (prints its nodes) or the wait is over
aac7_wait() { local j=$1 w=${2:-600} st; while [ "$w" -gt 0 ]; do st=$(squeue -j "$j" -h -o %T 2>/dev/null); [ "$st" = RUNNING ] && { squeue -j "$j" -h -o %N; return 0; }; [ -z "$st" ] && return 1; sleep 10; w=$((w - 10)); done; return 1; }

#!/bin/bash
# mnrun.sh - run <procs> node-processes of ecalc (or any command) connected by the inter-node
# communicator (Phase 11 S: SHMEM, COMM_TRANSPORT=shmem; xshmem is the default for <procs> > 1 where the engine is linked, Q73 2026-10-21; the Phase 8 TCP transport was removed
# 2026-10-12, so a build without SHMEM runs one process only).  The processes are spread over the nodes of the allocation, several per node when there are
# fewer nodes (they share the node's four APUs); the node count used is the largest that divides
# <procs> (Slurm places tasks by blocks otherwise), listed explicitly.
#   mnrun.sh <procs> <command...>          e.g.  SLURM_JOB_ID=<id> ./mnrun.sh 2 ./ecalc 1000000000 /tmp/e.txt
# SHMEM (COMM_TRANSPORT=shmem): the same placement, launched by plain srun (Cray's PMI), rank = PE.  COMM_SHMEM_POOL_MB sizes the
# transport's symmetric pool (unset: the run's modelled need from MN_PLAN_ONLY for ecalc, else 8192; Phase 14 V1); the host heap is set
# 512 MiB above it.  Only Cray OpenSHMEMX is launched (D14, 2026-10-12: the SOS and OpenMPI OSHMEM launch forms were removed;
# the archived forms are in the local archive removed_2026-10-12/).  A COMM_SHMEM_POOL_MB word of the command wins over the plan (the heap follows it).
# Phase 16 A (archive:results_merged_2026-10-24/AAC7_PORT.md §A16): `cray` (Cray OpenSHMEMX on aac7 / the target: libsma under /opt/cray/pe/sma,
# COMM_SHMEM_IMPL=cray): plain srun (Cray's PMI), the heap SHMEM_SYMMETRIC_SIZE = XT_SYMMETRIC_HEAP_SIZE = pool + 512 MiB (its heap is
# host hugepages; COMM_SHMEM_DEVHEAP=1 falls back to the registered host pool there).  MNRUN_MODULES is the module line of every
# wrapper (default `rocm`; `cray-dsmml cray-openshmemx rocm` where /opt/cray/pe/sma exists); MNRUN_CPUS_PER_TASK adds srun -c
# (unset: as before; `auto`: the node's CPUs / tasks per node -- aac7's srun confines a task to 2 CPUs otherwise);
# ECALC_LOG_CLOCKS=1 runs aac7env.sh --log (rocm-smi clocks, the APU / NIC NUMA map) on every node before the command.
# Phase 16 D: MNRUN_UNLOAD=<modules> (unset by default) is unloaded before MNRUN_MODULES in every wrapper and the plan call -- a versioned
# rocm module (rocm/7.2.4) conflicts with the default rocm/7.0.3 of the login shell (aac7env.sh sets both from AAC7_ROCM).
# Phase 16 P (results/OFI_FABRIC.md §P16): MNRUN_NODES=<n> (unset by default) uses at most the first n nodes of the allocation -- 4 PEs on one node
# (`MNRUN_NODES=1 mnrun.sh 4`) or a 2-node A/B inside a 4-node job.
# T27 (results/TOOLS_PROFILES.md §T27): MNRUN_PRINT_ENV=1 (default 0) computes everything as for a launch -- the plan's pool, the heap, the libfabric
# defaults, the SHMEM implementation -- but prints the computed variables as `export K=V` lines on stdout and exits 0 instead of
# calling srun (no allocation needed; mnrun's own messages go to stderr).  tools/plan_values.py uses it, so the size-dependent
# launch values have one source.  Unset, nothing changes.
# FIT (D26, results/LAUNCH_DEFAULTS.md §FIT): before srun, the planner's memory fit of the command's ecalc run is printed and a run that does not fit
# (device over ECALC_DEVICE_GB, node over ECALC_NODE_GB) is refused with exit 4; MNRUN_FIT=0 skips it (see the FIT block below).
# S2a (TARGET12, ECALC_PE_PER_GPU=1, exported or a VAR=value word of the command; default off = this script is unchanged): ONE PROCESS PER GPU.  <procs> is the node count (one
# group per node); each node runs PPN tasks (MNRUN_PPG_PPN; default the node's GPU count from scontrol Gres, else 4), --distribution=block, every task keeps its stdout, COMM_RANK / COMM_SIZE
# count nodes (as for PPN), srun --kill-on-bad-exit=1 even on one node.  Needs COMM_TRANSPORT=xshmem (make XSHMEM=ofi); XSHMEM_LOOPBACK_PES=1 (K = 1; the engine refuses another value).
# Launch arms (the S2a probe decides the default): MNRUN_PPG_GPUBIND=none (default: --gpus-per-node=4 --gpu-bind=none, the PE's device = pe_geo's: its local id) | task (--gpus-per-task=1, one
# visible device 0); MNRUN_PPG_CPUBIND=none (default: --cpu-bind=none, the process restricts itself to its NUMA node's cpus, vpe_pe_affinity) | ldom (--cpu-bind=map_ldom:0..PPN-1 --mem-bind=local).
set -e
P=$1; shift
PRINT_ENV=${MNRUN_PRINT_ENV:-0}
if [ "$PRINT_ENV" != 0 ]; then exec 3>&1 1>&2; fi
print_env_exit() {   # T27: the variables mnrun.sh sets or computes for the launch, as export lines on the saved stdout (fd 3)
    local v
    echo "# mnrun.sh MNRUN_PRINT_ENV: $P process(es), transport ${COMM_TRANSPORT:-none}, SHMEM implementation ${impl:-none}" >&3
    for v in COMM_TRANSPORT XSHMEM_LOOPBACK_PES XSHMEM_HEAP_MB COMM_SHMEM_POOL_MB COMM_OFI_PLAN_CXI SHMEM_SYMMETRIC_SIZE XT_SYMMETRIC_HEAP_SIZE \
             FI_UNIVERSE_SIZE FI_LOG_LEVEL \
             FI_MR_CACHE_MONITOR MNRUN_MODULES MNRUN_UNLOAD; do
        [ -n "${!v+x}" ] && printf 'export %s=%q\n' "$v" "${!v}" >&3
    done
    exit 0
}
# S46 §4.6: FI_MR_CACHE_MONITOR=disabled gave wrong exchanges silently (2097152 of 2097152 points differed, VERIFY FAILED); refuse it.
__fmc=${FI_MR_CACHE_MONITOR-}; __fmc=${__fmc,,}
if [ "$__fmc" = disabled ]; then echo "mnrun.sh: refusing FI_MR_CACHE_MONITOR=$FI_MR_CACHE_MONITOR: it corrupts exchanges silently (S46 section 4.6). Unset it (memhooks, the default) or use userfaultfd." >&2; exit 1; fi
if [ -n "${COMM_SHMEM_PE_PER_APU+x}" ]; then echo "mnrun.sh: refusing COMM_SHMEM_PE_PER_APU: the SHMEM proxy mode was removed in P7 (Q78); COMM_OFI=0 is plain SHMEM, XSHMEM=cray the SHMEM engine. Unset it." >&2; exit 1; fi
export MNRUN_DIR=$(cd "$(dirname "$0")" && pwd)
# Phase 16 V: the Cray default module picks up AAC7_ROCM (default rocm/7.2.4, results/AAC7_PORT.md §V16) instead of the bare "rocm" alias;
# rocm/7.0.3 (the login default) must be unloaded first (module conflict).
case "${AAC7_ROCM:-}" in ""|*/*) ;; *) AAC7_ROCM=rocm/$AAC7_ROCM;; esac   # merge V+D: a bare version (D: 7.2.4) means rocm/<version>
if [ -z "${MNRUN_MODULES+x}" ]; then if [ -d /opt/cray/pe/sma ]; then MNRUN_MODULES="cray-dsmml cray-openshmemx ${AAC7_ROCM:-rocm/7.2.4}"; else MNRUN_MODULES=rocm; fi; fi
if [ -d /opt/cray/pe/sma ]; then MNRUN_UNLOAD=${MNRUN_UNLOAD-rocm/7.0.3}; fi
export MNRUN_MODULES; export MNRUN_UNLOAD=${MNRUN_UNLOAD:-}
if [ "$PRINT_ENV" != 0 ]; then nodes=$(seq -f "print-env-node%g" 1 "$P"); else   # T27: no allocation needed to print the values
[ -n "$SLURM_JOB_ID" ] || { echo "set SLURM_JOB_ID to the allocation"; exit 1; }
nodes=$(scontrol show hostnames "$(squeue -j "$SLURM_JOB_ID" -h -o %N)")
fi
[ -n "${MNRUN_NODELIST:-}" ] && nodes=$(echo "$MNRUN_NODELIST" | tr , "\n")   # s29: run on exactly these nodes of the allocation (unset: all, in squeue order)
# S35 wrap-up: MNRUN_EXCLUDE_HOSTS="h1,h2" (default empty = off) names slow hosts (aac7: aac7-n01, a ~20 s host->device upload stall, results/SESSIONS.md §S31).
# If one is in the node list: default MNRUN_EXCLUDE_MODE=warn warns on stderr and changes nothing; MNRUN_EXCLUDE_MODE=drop removes it (the run then uses the remaining nodes;
# ranks shift, so it needs >= the nodes asked for, else fewer nodes are used as with MNRUN_NODES).
if [ -n "${MNRUN_EXCLUDE_HOSTS:-}" ]; then
    __hit=$(echo "$nodes" | grep -Fxf <(echo "$MNRUN_EXCLUDE_HOSTS" | tr , "\n") || true)
    if [ -n "$__hit" ]; then
        if [ "${MNRUN_EXCLUDE_MODE:-warn}" = drop ]; then
            echo "mnrun.sh: MNRUN_EXCLUDE_MODE=drop: removing excluded host(s) from the node list: $(echo $__hit)" >&2
            nodes=$(echo "$nodes" | grep -Fvxf <(echo "$MNRUN_EXCLUDE_HOSTS" | tr , "\n") || true)
            [ -n "$nodes" ] || { echo "mnrun.sh: no node left after MNRUN_EXCLUDE_HOSTS"; exit 1; }
        else
            echo "mnrun.sh: WARNING: excluded-list host(s) in the allocation: $(echo $__hit) (MNRUN_EXCLUDE_MODE=warn: kept; =drop removes them)" >&2
        fi
    fi
fi
nn=$(echo "$nodes" | wc -l); [ "$nn" -gt "$P" ] && nn=$P
[ -n "${MNRUN_NODES:-}" ] && [ "$nn" -gt "$MNRUN_NODES" ] && nn=$MNRUN_NODES   # Phase 16 P: at most this many nodes of the allocation (unset: all)
while [ $((P % nn)) -ne 0 ]; do nn=$((nn - 1)); done
per=$((P / nn))
case "${MNRUN_NODE_SKIP:-0}" in ''|*[!0-9]*) echo "mnrun.sh: MNRUN_NODE_SKIP=${MNRUN_NODE_SKIP}: a non-negative integer" >&2; exit 1;; esac
[ $((nn + ${MNRUN_NODE_SKIP:-0})) -le "$(echo "$nodes" | wc -l)" ] || { echo "mnrun.sh: MNRUN_NODE_SKIP=${MNRUN_NODE_SKIP}: nodes ($nn) + skip exceed the allocation ($(echo "$nodes" | wc -l))" >&2; exit 1; }
use=$(echo "$nodes" | tail -n +$((${MNRUN_NODE_SKIP:-0} + 1)) | head -n "$nn")   # MNRUN_NODE_SKIP=k: leave the first k nodes of the allocation out (diagnosis: which node a stall follows)
copt=()
# S32: MNRUN_LABEL=1 adds srun --label (every output line prefixed "<rank>: ").  (MNRUN_ARBITRARY, unused, was removed 2026-10-12.)
[ "${MNRUN_ARBITRARY:-0}" = 0 ] || { echo "mnrun.sh: MNRUN_ARBITRARY was removed (2026-10-12, unused); unset it" >&2; exit 1; }
[ "${MNRUN_LABEL:-0}" != 0 ] && copt+=(--label)
case "${MNRUN_CPUS_PER_TASK:-}" in
    "") ;;
    auto) ncpu=$(scontrol show node "$(echo "$use" | head -1)" -o 2>/dev/null | sed -n 's/.*CPUTot=\([0-9]*\).*/\1/p'); [ -n "$ncpu" ] && [ "$ncpu" -ge "$per" ] && copt+=(-c $((ncpu / per)));;
    *) copt+=(-c "$MNRUN_CPUS_PER_TASK");;
esac
list=$(echo "$use" | paste -sd,)
# Phase 16 A: COMM_TRANSPORT given as a leading VAR=value word of the command (`mnrun.sh 2 env COMM_TRANSPORT=shmem ... ./ecalc`, the
# launch line's form) counts as well -- without this the non-SHMEM path launched it (no heap variable: the library's default heap);
# the same for ECALC_LOG_CLOCKS, which the wrappers test before the command runs
for a in "$@"; do case "$a" in env|-*) continue;; COMM_TRANSPORT=*) COMM_TRANSPORT=${a#*=};; ECALC_LOG_CLOCKS=*) export ECALC_LOG_CLOCKS=${a#*=};; COMM_OFI=*) COMM_OFI=${a#*=};; ECALC_PE_PER_GPU=*) ECALC_PE_PER_GPU=${a#*=};; XSHMEM_HEAP_MB=*) XSHMEM_HEAP_MB=${a#*=};; MNRUN_SYMHEAP_XSHMEM=*) MNRUN_SYMHEAP_XSHMEM=${a#*=};; XSHMEM_HEAP_SLACK_MB=*) XSHMEM_HEAP_SLACK_MB=${a#*=};; XSHMEM_PES_PER_GPU=*) XSHMEM_PES_PER_GPU=${a#*=};; *=*) continue;; *) break;; esac; done   # (ECALC_LOG_CLOCKS: the wrapper reads it)
# 2026-10-12 (TCP removed): SHMEM is the only multi-process transport -- the default for <procs> > 1; any other value is refused
# 2026-10-21 (Q73): the default transport for <procs> > 1 is xshmem (XSHMEM_ROUND_MB=128 and XSHMEM_ORDER=rot are its defaults, in the engine and the planner)
# when the launched executable has the engine linked (make XSHMEM=ofi, the default build where libfabric is found); otherwise (another command, a build
# without libfabric) shmem as before.  An explicit COMM_TRANSPORT=shmem (with or without COMM_OFI=1) is the fallback and the reference, unchanged.
# (S2a: ECALC_PE_PER_GPU=1 runs one process per GPU, so it picks the transport the same way on one node.)
if { [ "$P" -gt 1 ] || [ "${ECALC_PE_PER_GPU:-0}" = 1 ]; } && [ -z "${COMM_TRANSPORT:-}" ]; then
    COMM_TRANSPORT=shmem
    for a in "$@"; do case "$a" in env|-*|*=*) continue;; esac
        f=$(command -v -- "$a" 2>/dev/null) || continue; [ -f "$f" ] && [ -x "$f" ] || continue
        head -c 4 "$f" 2>/dev/null | grep -q ELF || continue
        grep -aq comm_xshmem_init "$f" 2>/dev/null && COMM_TRANSPORT=xshmem; break
    done
fi
if [ "$P" -gt 1 ] && [ "$COMM_TRANSPORT" != shmem ] && [ "$COMM_TRANSPORT" != xshmem ]; then echo "mnrun.sh: COMM_TRANSPORT=$COMM_TRANSPORT: only shmem (and xshmem, make XSHMEM=ofi) runs more than one process (the TCP transport was removed)" >&2; exit 1; fi
[ -n "${COMM_TRANSPORT:-}" ] && export COMM_TRANSPORT
PPN=1   # PEs per node (ECALC_PE_PER_GPU=1 sets it below)
# S2a: one process per GPU (ECALC_PE_PER_GPU=1): PPN tasks per node, the tasks are the PEs
MNRUN_PPG=0; gopt=(--gpus-per-node=4)
case "${ECALC_PE_PER_GPU:-0}" in 0|"") ;; 1)
    [ "${COMM_TRANSPORT:-}" = xshmem ] || { echo "mnrun.sh: ECALC_PE_PER_GPU=1 needs COMM_TRANSPORT=xshmem (make XSHMEM=ofi)" >&2; exit 1; }
    [ "$per" = 1 ] || { echo "mnrun.sh: ECALC_PE_PER_GPU=1 needs <procs> = nodes (one group of tasks per node; here $per per node)" >&2; exit 1; }
    MNRUN_PPG=1; export ECALC_PE_PER_GPU=1
    PPN=${MNRUN_PPG_PPN:-}
    if [ -z "$PPN" ] && [ "$PRINT_ENV" = 0 ]; then PPN=$(scontrol show node "$(echo "$use" | head -1)" -o 2>/dev/null | grep -o 'Gres=gpu[^ ]*' | head -1 | sed 's/(.*//; s/.*://'); fi
    case "$PPN" in ""|*[!0-9]*|0) PPN=4;; esac
    export XSHMEM_LOOPBACK_PES=${XSHMEM_LOOPBACK_PES:-1}
    PPG2=${XSHMEM_PES_PER_GPU:-1}
    case "$PPG2" in ""|*[!0-9]*|0) echo "mnrun.sh: XSHMEM_PES_PER_GPU=$PPG2: a positive integer" >&2; exit 1;; esac
    if [ $((PPN % PPG2)) != 0 ] || [ $((PPN / PPG2)) -lt 1 ]; then echo "mnrun.sh: $PPN processes per node are not a multiple of XSHMEM_PES_PER_GPU=$PPG2" >&2; exit 1; fi
    case "${MNRUN_PPG_GPUBIND:-none}" in none) gopt=(--gpus-per-node="$((PPN / PPG2))" --gpu-bind=none);;   # (XSHMEM_PES_PER_GPU=2: PPN PEs share PPN/2 GPUs)
    task) gopt=(--gpus-per-task=1);; *) echo "mnrun.sh: MNRUN_PPG_GPUBIND=$MNRUN_PPG_GPUBIND: none or task" >&2; exit 1;; esac
    # Cray build (an ELF in the command carrying the XSHMEM=cray engine), one process per GPU: NUMA placement is the default (measured: stall-free, -4 s at 3 n 1e10; the Cray topo has no cpu list, so
    # without it every process runs on all cpus and first-touches its heap anywhere): srun map_ldom + mem-bind local, and the process pins itself (XSHMEM_CRAY_NUMA=1).  Explicit values win; ofi builds unchanged.
    if [ -z "${MNRUN_PPG_CPUBIND:-}" ] || [ -z "${XSHMEM_CRAY_NUMA+x}" ]; then
        for a in "$@"; do case "$a" in -*|[A-Za-z_]*=*) continue;; esac
            f=$(command -v -- "$a" 2>/dev/null) || continue; [ -f "$f" ] && [ -x "$f" ] || continue
            head -c 4 "$f" 2>/dev/null | grep -q ELF || continue
            if grep -qa 'xshmem_cray: ' "$f"; then   # the XSHMEM=cray engine only (an ofi build made with SHMEM_CRAY=1 also links libsma: unchanged)
                [ -n "${MNRUN_PPG_CPUBIND:-}" ] || MNRUN_PPG_CPUBIND=ldom; [ -n "${XSHMEM_CRAY_NUMA+x}" ] || export XSHMEM_CRAY_NUMA=1
                echo "mnrun.sh: Cray build under ECALC_PE_PER_GPU=1: MNRUN_PPG_CPUBIND=$MNRUN_PPG_CPUBIND XSHMEM_CRAY_NUMA=$XSHMEM_CRAY_NUMA (defaults for the Cray build)"; break; fi
        done
    fi
    case "${MNRUN_PPG_CPUBIND:-none}" in none|ldom) ;; *) echo "mnrun.sh: MNRUN_PPG_CPUBIND=$MNRUN_PPG_CPUBIND: none or ldom" >&2; exit 1;; esac
    echo "mnrun.sh: ECALC_PE_PER_GPU=1: $PPN processes per node ($([ "$PPG2" = 1 ] && echo "one per GPU" || echo "$PPG2 per GPU")), gpu ${MNRUN_PPG_GPUBIND:-none}, cpu ${MNRUN_PPG_CPUBIND:-none}, XSHMEM_LOOPBACK_PES=$XSHMEM_LOOPBACK_PES";;
  *) echo "mnrun.sh: ECALC_PE_PER_GPU=$ECALC_PE_PER_GPU: 0 or 1" >&2; exit 1;; esac
export MNRUN_PPG
export MNRUN_PPN=$PPN
if [ "$PPN" -gt 1 ]; then   # the CPUs: -c the node's CPUs / PEs per node (auto) and no per-task binding, so the node-process's threads use the whole step
    for i in "${!copt[@]}"; do [ "${copt[$i]}" = -c ] && unset 'copt[i]' 'copt[i+1]'; done; copt=("${copt[@]}")
    ncpu=$(scontrol show node "$(echo "$use" | head -1)" -o 2>/dev/null | sed -n 's/.*CPUTot=\([0-9]*\).*/\1/p'); [ -n "$ncpu" ] && copt+=(-c $((ncpu / PPN)))
    copt+=(--cpu-bind=none)
    if [ "$MNRUN_PPG" = 1 ] && [ "${MNRUN_PPG_CPUBIND:-none}" = ldom ]; then
        for i in "${!copt[@]}"; do [ "${copt[$i]}" = --cpu-bind=none ] && copt[$i]="--cpu-bind=map_ldom:$(seq -s, 0 $((PPN - 1)))"; done
        copt+=(--mem-bind=local)
    fi
fi
# COMM_TRANSPORT=xshmem (TARGET1 S3a, make XSHMEM=ofi): the placement, the FI_* block, the plan and the plain srun launch are the shmem block's; in addition
# XSHMEM_LOOPBACK_PES (default = the GPUs per node-process, 4: one xshmem PE per APU thread, each on its own GPU and NIC) and XSHMEM_HEAP_MB (the plan's value
# below, or the caller's) are exported.  The libsma heap below is still sized (the plan counts the pool conservatively; xshmem does not initialise libsma).
[ "$COMM_TRANSPORT" = xshmem ] && export XSHMEM_LOOPBACK_PES=${XSHMEM_LOOPBACK_PES:-4}   # (S2a: ECALC_PE_PER_GPU=1 set it to 1 above)
if [ "$COMM_TRANSPORT" = shmem ] || [ "$COMM_TRANSPORT" = xshmem ]; then
    # s18-target Part 2 (TGTBENCH2 L2, internal target notes §4; the user: "use the flags that optimize performance but allow us to
    # adjust to a different system later"): two libfabric knobs, overridable per system by exporting them before the call.
    # FI_UNIVERSE_SIZE: comm_ofi opens its AVs at the provider's default count (comm_ofi.c:157) and inserts every
    # communicator's member -- the user's target rule ">= 4 x ntasks" (576 -> 2304; their largest tested value was 4096, kept
    # as the floor so a small run still gets their tested size). FI_LOG_LEVEL=warn: cuts libfabric's output ~1000x (the
    # user's L2). Both default with ${VAR:-...}, so a site's own exported value (or a different system's) is never shadowed.
    # MNRUN_FI_DEFAULTS=0 (default 1) leaves both unset -- the pre-s18 launch (S18AB: run (a) of 2026-10-06 segfaulted with them set).
    if [ "${MNRUN_FI_DEFAULTS:-1}" != 0 ]; then
        fip=$P; [ "$MNRUN_PPG" = 1 ] && fip=$((P * PPN))   # (S2a: the tasks, not the nodes, are the PEs)
        export FI_UNIVERSE_SIZE=${FI_UNIVERSE_SIZE:-$((4 * fip > 4096 ? 4 * fip : 4096))}
        export FI_LOG_LEVEL=${FI_LOG_LEVEL:-warn}
    fi
    # S46 (adopted 2026-10-11): FI_MR_CACHE_MONITOR=userfaultfd by default (memhooks deadlocks ROCr <-> libfabric: base 22/105 hangs vs uffd 0/104,
    # results/CRASHES_HANGS.md §S46). An explicit value (e.g. memhooks) is respected; `disabled` was refused above. Independent of MNRUN_FI_DEFAULTS.
    export FI_MR_CACHE_MONITOR=${FI_MR_CACHE_MONITOR:-userfaultfd}
    echo "mnrun.sh: FI_MR_CACHE_MONITOR=$FI_MR_CACHE_MONITOR"
    echo "mnrun.sh: FI_UNIVERSE_SIZE=${FI_UNIVERSE_SIZE:-<unset>} FI_LOG_LEVEL=${FI_LOG_LEVEL:-<unset>} (MNRUN_FI_DEFAULTS=${MNRUN_FI_DEFAULTS:-1})"
    # Phase 14 V1: the pool (and with it the heap below) from the run's own model when COMM_SHMEM_POOL_MB is not set by hand: the
    # command's SHMEM-linked executable followed by a digit count (ecalc <digits> ...) is asked first, on this host, with the
    # command's VAR=value words and MN_PLAN_ONLY=<digits>:<procs> (no device is touched), and its `plan pool` line gives
    # COMM_SHMEM_POOL_MB -- so ecalc's pool rule (binsplit_shmem_pool_rule) finds the pool already at the need, and a host heap
    # (SHMEM_SYMMETRIC_SIZE = pool + 512 MiB) holds it.  Other commands: 8192 as before.
    # MNRUN_PLAN_POOL=0 skips the plan.
    if [ -z "${COMM_SHMEM_POOL_MB:-}" ] && [ "${MNRUN_PLAN_POOL:-1}" != 0 ]; then
        pbin=; pd=; pas=()
        for a in "$@"; do
            if [ -n "$pbin" ]; then case "$a" in -*) continue;; esac; case "$a" in *[!0-9]*|"") ;; *) pd=$a;; esac; break; fi
            case "$a" in [A-Za-z_]*=*) pas+=("$a"); continue;; -*) continue;; esac
            f=$(command -v -- "$a" 2>/dev/null) || continue; [ -f "$f" ] && [ -x "$f" ] || continue
            head -c 4 "$f" 2>/dev/null | grep -q ELF || continue
            readelf -d "$f" 2>/dev/null | grep NEEDED | grep -q libsma && pbin=$f
        done
        if [ -n "$pbin" ] && [ -n "$pd" ]; then
            # Phase 17 OFIMEM: comm_ofi (on by default where a cxi NIC is present) moves the device staging out of the SHMEM pool, and
            # the plan sizes the pool by it -- but the plan runs here, on a login node that may have no cxi (aac7's aac7-login): ask the
            # first compute node once (COMM_OFI unset, no cxi here) and hand the answer to the plan as COMM_OFI_PLAN_CXI
            if [ -z "${COMM_OFI:-}" ] && [ -z "${COMM_OFI_PLAN_CXI:-}" ] && [ ! -e /sys/class/cxi/cxi0 ] && [ -n "${SLURM_JOB_ID:-}" ] && [ "$PRINT_ENV" = 0 ]; then
                if timeout 120 srun --jobid="$SLURM_JOB_ID" -N 1 -n 1 -w "$(echo "$use" | head -1)" --overlap test -e /sys/class/cxi/cxi0 2>/dev/null; then export COMM_OFI_PLAN_CXI=1; else export COMM_OFI_PLAN_CXI=0; fi
                echo "mnrun.sh: COMM_OFI_PLAN_CXI=$COMM_OFI_PLAN_CXI (a cxi NIC on the compute node: comm_ofi's default; the plan counts its pools)"
            fi
            pppg=(); [ "$MNRUN_PPG" = 1 ] && pppg=(SLURM_NTASKS_PER_NODE="$PPN")   # (S2a: as in the FIT plan below; without it the plan dies and the pool stays at 8192)
            pline=$(bash -lc '[ -n "${MNRUN_UNLOAD:-}" ] && module unload $MNRUN_UNLOAD > /dev/null 2>&1; module load $MNRUN_MODULES > /dev/null 2>&1; exec "$@"' _ env "${pas[@]}" "${pppg[@]}" COMM_TRANSPORT="$COMM_TRANSPORT" MN_PLAN_ONLY="$pd:$P" "$pbin" 2>/dev/null | grep '^plan pool' || true)
            pmb=$(echo "$pline" | sed -n 's/.*COMM_SHMEM_POOL_MB=\([0-9][0-9]*\).*/\1/p')
            xmb=$(echo "$pline" | sed -n 's/.*XSHMEM_HEAP_MB=\([0-9][0-9]*\).*/\1/p')
            if [ "$COMM_TRANSPORT" = xshmem ] && [ -n "$xmb" ] && [ -z "${XSHMEM_HEAP_MB:-}" ]; then export XSHMEM_HEAP_MB=$xmb; echo "mnrun.sh: XSHMEM_HEAP_MB=$xmb (the plan's DATA heap per loopback PE)"; fi
            # The Cray backend of the xshmem engine (make XSHMEM=cray; the binary carries the string "xshmem_cray:") has ONE DATA heap per process (K = 1), inside the libsma heap:
            # the plan's figure is for one PE, so a process of 4 APU threads (no ECALC_PE_PER_GPU) needs 4 x it, one PE per process 1 x it; + XSHMEM_HEAP_SLACK_MB (192) for
            # the control area and alignment.  Automatic when MNRUN_SYMHEAP_XSHMEM is unset and XSHMEM_HEAP_MB is not given (results/XSHMEM.md, target shape); MNRUN_SYMHEAP_XSHMEM=0 leaves it off.
            if [ "$COMM_TRANSPORT" = xshmem ] && [ -n "$xmb" ] && [ -z "${MNRUN_SYMHEAP_XSHMEM:-}" ] && grep -q -a 'xshmem_cray: ' "$pbin" 2>/dev/null; then
                export MNRUN_SYMHEAP_XSHMEM=1 MNRUN_SYMHEAP_PES=1 XSHMEM_HEAP_SLACK_MB=${XSHMEM_HEAP_SLACK_MB:-192}
                if [ -z "${XSHMEM_HEAP_MB:-}" ] || [ "${XSHMEM_HEAP_MB:-}" = "$xmb" ]; then
                    hm=$xmb; [ "$MNRUN_PPG" = 1 ] || hm=$((xmb * ${XSHMEM_LOOPBACK_PES:-4})); export XSHMEM_HEAP_MB=$hm
                    echo "mnrun.sh: Cray backend: XSHMEM_HEAP_MB=$hm (the plan's $xmb x $([ "$MNRUN_PPG" = 1 ] && echo 1 || echo "${XSHMEM_LOOPBACK_PES:-4} APU threads sharing the one heap")) + XSHMEM_HEAP_SLACK_MB=$XSHMEM_HEAP_SLACK_MB; libsma heap sized to match"
                fi
            fi
            if [ -n "$pmb" ]; then export COMM_SHMEM_POOL_MB=$pmb; echo "mnrun.sh: COMM_SHMEM_POOL_MB=$pmb from MN_PLAN_ONLY=$pd:$P ($(echo "$pline" | sed 's/^plan pool *//'))"
            else echo "mnrun.sh: MN_PLAN_ONLY=$pd:$P of $pbin gave no 'plan pool' line: the pool stays at COMM_SHMEM_POOL_MB=8192" >&2; fi
        fi
    fi
    for a in "$@"; do case "$a" in COMM_SHMEM_POOL_MB=*) COMM_SHMEM_POOL_MB=${a#*=};; esac; done   # a VAR=value word of the command wins over the plan (the heap follows it)
    POOL=${COMM_SHMEM_POOL_MB:-8192}
    export COMM_SHMEM_POOL_MB=$POOL
    # The implementation the binary was built against: only Cray OpenSHMEMX is launched (D14, 2026-10-12).  The detection looks
    # through wrappers (env, stdbuf, timeout, numactl, ...): every word of the command that resolves to an executable ELF file is
    # checked for libsma / liboshmem in its dynamic section, the first that links either decides (the wrappers link neither).
    # A libsma that is not Cray's, or liboshmem, is refused; none found: cray, with a note.  COMM_SHMEM_IMPL=cray is accepted.
    impl=${COMM_SHMEM_IMPL:-}
    case "$impl" in ""|cray) ;; *) echo "mnrun.sh: COMM_SHMEM_IMPL=$impl: only cray is supported (the sos and oshmem launch forms were removed 2026-10-12)"; exit 1;; esac
    if [ -z "$impl" ]; then
        for a in "$@"; do
            case "$a" in *=*|-*) continue;; esac                                 # env assignments, wrapper options
            f=$(command -v -- "$a" 2>/dev/null) || continue; [ -f "$f" ] && [ -x "$f" ] || continue
            head -c 4 "$f" 2>/dev/null | grep -q ELF || continue                  # scripts: their interpreter is not the binary
            need=$(readelf -d "$f" 2>/dev/null | grep NEEDED; ldd "$f" 2>/dev/null)
            if echo "$need" | grep -q libsma; then
                echo "$need" | grep -q /opt/cray/pe || { echo "mnrun.sh: $f links a libsma that is not Cray's: only Cray OpenSHMEMX is launched (sos removed 2026-10-12)" >&2; exit 1; }
                impl=cray; break
            fi
            if echo "$need" | grep -q liboshmem; then echo "mnrun.sh: $f links liboshmem: only Cray OpenSHMEMX is launched (oshmem removed 2026-10-12)" >&2; exit 1; fi
        done
        [ -n "$impl" ] || { impl=cray; echo "mnrun.sh: no libsma found in the command's executables; assuming cray" >&2; }
    fi
    [ -n "${MNRUN_SHOW_IMPL:-}" ] && { echo "mnrun.sh: SHMEM implementation $impl"; [ "$MNRUN_SHOW_IMPL" = only ] && exit 0; }   # (test hook: MNRUN_SHOW_IMPL=only prints and stops)
    mpi=(); pre=                                                              # the srun --mpi option and the exec prefix (none for Cray)
    # Cray OpenSHMEMX: srun's PMI (no --mpi option), the host hugepage heap at pool + 512 MiB under both of its names;
    # the libfabric provider is cxi on aac7 (nothing to set); SHMEM_OFI_* knobs (NIC policy, progress) are the caller's.
    # MNRUN_SYMHEAP_XSHMEM=1 (default off): the Cray backend of the xshmem engine (make XSHMEM=cray) keeps its DATA heaps in this heap too: + XSHMEM_LOOPBACK_PES x (XSHMEM_HEAP_MB + XSHMEM_HEAP_SLACK_MB)
    [ "${MNRUN_SYMHEAP_XSHMEM:-0}" = 1 ] && [ "${COMM_TRANSPORT:-}" = xshmem ] && case "${XSHMEM_HEAP_MB:-0}" in *[!0-9]*|"") false;; *) [ "${XSHMEM_HEAP_MB:-0}" -gt 0 ];; esac && POOL=$((POOL + ${MNRUN_SYMHEAP_PES:-${XSHMEM_LOOPBACK_PES:-1}} * (XSHMEM_HEAP_MB + ${XSHMEM_HEAP_SLACK_MB:-0})))
    export SHMEM_SYMMETRIC_SIZE=${SHMEM_SYMMETRIC_SIZE:-$((POOL + 512))M}
    export XT_SYMMETRIC_HEAP_SIZE=${XT_SYMMETRIC_HEAP_SIZE:-$SHMEM_SYMMETRIC_SIZE}
    echo "mnrun.sh: SHMEM_SYMMETRIC_SIZE=$SHMEM_SYMMETRIC_SIZE XT_SYMMETRIC_HEAP_SIZE=$XT_SYMMETRIC_HEAP_SIZE (pool $POOL MiB + 512)"
fi
[ "$PRINT_ENV" = 0 ] || print_env_exit
# ---- FIT (user decision D26, results/LAUNCH_DEFAULTS.md §FIT): the pre-launch memory fit.  A block of its own (the pool block above belongs to the
# SHMEM pool sizing; nothing here changes the pool).  The command's ecalc (the first executable whose name starts with `ecalc`, followed by
# a digit count) is planned on this host with the command's VAR=value words and MN_PLAN_ONLY=<digits>:<procs> (no device touched); its
# `plan fit` lines (device per node and per APU vs ECALC_DEVICE_GB, node vs ECALC_NODE_GB, the margins, the largest consumers, the
# DM_MN_WRAP decision) are printed, and a run that does not fit -- or any `plan REFUSED` -- is refused HERE, before srun: exit 4.
# MNRUN_FIT=0 (default 1) skips the check (deliberate edge tests only).  The plan counts the comm_ofi pools when a cxi NIC is on the
# compute node (asked once, as the pool block does, when COMM_OFI / COMM_OFI_PLAN_CXI are unset and this host has none).
if [ "${MNRUN_FIT:-1}" != 0 ]; then
    fbin=; fd=; fas=()
    for a in "$@"; do
        if [ -n "$fbin" ]; then case "$a" in -*) continue;; esac; case "$a" in *[!0-9]*|"") ;; *) fd=$a;; esac; break; fi
        case "$a" in [A-Za-z_]*=*) fas+=("$a"); continue;; -*) continue;; esac
        f=$(command -v -- "$a" 2>/dev/null) || continue; [ -f "$f" ] && [ -x "$f" ] || continue
        case "$(basename -- "$f")" in ecalc*) head -c 4 "$f" 2>/dev/null | grep -q ELF && fbin=$f;; esac
    done
    if [ -n "$fbin" ] && [ -n "$fd" ]; then
        if [ "$P" -gt 1 ] && [ -z "${COMM_OFI:-}" ] && [ -z "${COMM_OFI_PLAN_CXI:-}" ] && [ ! -e /sys/class/cxi/cxi0 ]; then
            if timeout 120 srun --jobid="$SLURM_JOB_ID" -N 1 -n 1 -w "$(echo "$use" | head -1)" --overlap test -e /sys/class/cxi/cxi0 2>/dev/null; then export COMM_OFI_PLAN_CXI=1; else export COMM_OFI_PLAN_CXI=0; fi
            echo "mnrun.sh: FIT: COMM_OFI_PLAN_CXI=$COMM_OFI_PLAN_CXI (a cxi NIC on the compute node: the plan counts the comm_ofi pools)"
        fi
        ftr=(); { [ "$P" -gt 1 ] || [ "$MNRUN_PPG" = 1 ]; } && ftr=(COMM_TRANSPORT="$COMM_TRANSPORT")
        [ "$MNRUN_PPG" = 1 ] && ftr+=(SLURM_NTASKS_PER_NODE="$PPN")   # (S2a: the engine reads the PEs per node from srun; the plan has no srun)
        fout=$(bash -lc '[ -n "${MNRUN_UNLOAD:-}" ] && module unload $MNRUN_UNLOAD > /dev/null 2>&1; module load $MNRUN_MODULES > /dev/null 2>&1; exec "$@"' _ env "${fas[@]}" "${ftr[@]}" MN_PLAN_ONLY="$fd:$P" "$fbin" 2>&1) || true
        flines=$(echo "$fout" | grep -aE '^plan (fit|REFUSED)' || true)
        if [ -z "$flines" ]; then
            echo "mnrun.sh: FIT: MN_PLAN_ONLY=$fd:$P of $fbin printed no fit report (a binary before FIT?): the memory fit is not checked" >&2
            echo "$fout" | grep -a -m1 FATAL | cut -c1-240 | sed 's/^/mnrun.sh: FIT: /' >&2
        else
            echo "$flines" | sed 's/^/mnrun.sh: /'
            if echo "$flines" | grep -q '^plan REFUSED'; then
                echo "mnrun.sh: REFUSED before launch (no node time used): $(echo "$flines" | grep '^plan REFUSED' | head -1 | cut -c1-240)" >&2
                exit 4
            fi
        fi
    fi
fi
# D34 (user 2026-10-15): multi-node runs launch with srun --kill-on-bad-exit=1 (a dead rank ends the step in seconds, not at the timeout);
# COMM_KILL_ON_BAD_EXIT=0 is the only override.
kopt=(); { [ "$nn" -gt 1 ] || [ "$MNRUN_PPG" = 1 ]; } && [ "${COMM_KILL_ON_BAD_EXIT:-1}" != 0 ] && kopt=(--kill-on-bad-exit=1)   # (S2a: also on one node: a dead PE must end the step)
# the one launch of every path: the tasks block-placed on the listed nodes; each task loads the modules, sets its rank and runs the command
launch=(srun --jobid="$SLURM_JOB_ID" "${kopt[@]}" "${mpi[@]}" -N "$nn" -w "$list" --ntasks="$((P * PPN))" --ntasks-per-node="$((per * PPN))" "${copt[@]}" --distribution=block "${gopt[@]}" --overlap --export=ALL \
     bash -lc '[ -n "${MNRUN_UNLOAD:-}" ] && module unload $MNRUN_UNLOAD > /dev/null 2>&1; module load $MNRUN_MODULES; k=${MNRUN_PPN:-1}; export COMM_RANK=$((SLURM_PROCID / k)) COMM_SIZE=$((SLURM_NTASKS / k)); [ "${ECALC_LOG_CLOCKS:-0}" != 0 ] && bash "$MNRUN_DIR/aac7env.sh" --log; exec '"$pre"'"$@"' _ "$@")
# MNRUN_NICGRP=1 (default off; results/NODE_SLOWMODE.md §A14): log which NIC launch group (A = oxe_mcu_meas 18-25, B = 28-33) carried this
# launch's traffic: per node, the counter deltas around the srun, one line "mnrun.sh: nicgrp <host> A=<bytes> B=<bytes> -> <A|B|none>" and a
# summary "nicgrp: <A|B|mixed|none>"; log only, the exit status is the run's.  The group is per launch (§A13) and sets the 4-node rate, so
# this lets a multi-node timing be split by group (the layv factor 0.92 of TIME_MODEL.md §L4 assumed B).
if [ "${MNRUN_NICGRP:-0}" = 1 ]; then
    nicsnap() { timeout 120 srun --jobid="$SLURM_JOB_ID" -N "$nn" -w "$list" --ntasks="$nn" --ntasks-per-node=1 --overlap bash -c 'a=0; b=0; for d in /sys/class/cxi/cxi[0-9]*/device/telemetry; do for k in 18 19 20 21 22 23 24 25; do v=$(grep -m1 -o "^[0-9]*" $d/oxe_mcu_meas_$k 2>/dev/null); a=$((a + ${v:-0})); done; for k in 28 29 30 31 32 33; do v=$(grep -m1 -o "^[0-9]*" $d/oxe_mcu_meas_$k 2>/dev/null); b=$((b + ${v:-0})); done; done; echo "$(hostname -s) $a $b"' 2>/dev/null | sort; }
    snap0=$(nicsnap); "${launch[@]}"; rc=$?; snap1=$(nicsnap)
    { echo "$snap0" | sed 's/^/P /'; echo "$snap1" | sed 's/^/Q /'; } | awk '
        $1=="P"{pa[$2]=$3; pb[$2]=$4; next}
        {h=$2; da=$3-pa[h]; db=$4-pb[h]; g=(da+db<1e6)?"none":(da>db)?"A":"B"; printf "mnrun.sh: nicgrp %s A=%d B=%d -> %s\n", h, da, db, g; seen[g]=1; n++}
        END{m=""; for (g in seen) m=(m==""?g:"mixed"); printf "mnrun.sh: nicgrp: %s\n", (n? (m ~ /^(A|B|none)$/ ? m : "mixed") : "none")}'
    exit $rc
fi
exec "${launch[@]}"

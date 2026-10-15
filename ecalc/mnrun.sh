#!/bin/bash
# mnrun.sh - run <procs> node-processes of ecalc (or any command) connected by the inter-node
# communicator (Phase 11 S: SHMEM, COMM_TRANSPORT=shmem -- the default for <procs> > 1; the Phase 8 TCP transport was removed
# 2026-10-12, so a build without SHMEM runs one process only).  The processes are spread over the nodes of the allocation, several per node when there are
# fewer nodes (they share the node's four APUs); the node count used is the largest that divides
# <procs> (Slurm places tasks by blocks otherwise), listed explicitly.
#   mnrun.sh <procs> <command...>          e.g.  SLURM_JOB_ID=<id> ./mnrun.sh 2 ./ecalc 1000000000 /tmp/e.txt
# SHMEM (COMM_TRANSPORT=shmem): the same placement, launched by plain srun (Cray's PMI), rank = PE.  COMM_SHMEM_POOL_MB sizes the
# transport's symmetric pool (unset: the run's modelled need from MN_PLAN_ONLY for ecalc, else 8192; Phase 14 V1); the host heap is set
# 512 MiB above it.  Only Cray OpenSHMEMX is launched (D14, 2026-10-12: the SOS and OpenMPI OSHMEM launch forms were removed;
# the archived forms are in the local archive removed_2026-10-12/).
# Phase 16 A (results/A16.md): `cray` (Cray OpenSHMEMX on aac7 / the target: libsma under /opt/cray/pe/sma,
# COMM_SHMEM_IMPL=cray): plain srun (Cray's PMI), the heap SHMEM_SYMMETRIC_SIZE = XT_SYMMETRIC_HEAP_SIZE = pool + 512 MiB (its heap is
# host hugepages; COMM_SHMEM_DEVHEAP=1 falls back to the registered host pool there).  MNRUN_MODULES is the module line of every
# wrapper (default `rocm`; `cray-dsmml cray-openshmemx rocm` where /opt/cray/pe/sma exists); MNRUN_CPUS_PER_TASK adds srun -c
# (unset: as before; `auto`: the node's CPUs / tasks per node -- aac7's srun confines a task to 2 CPUs otherwise);
# ECALC_LOG_CLOCKS=1 runs aac7env.sh --log (rocm-smi clocks, the APU / NIC NUMA map) on every node before the command.
# Phase 16 D: MNRUN_UNLOAD=<modules> (unset by default) is unloaded before MNRUN_MODULES in every wrapper and the plan call -- a versioned
# rocm module (rocm/7.2.4) conflicts with the default rocm/7.0.3 of the login shell (aac7env.sh sets both from AAC7_ROCM).
# Phase 16 P (results/P16.md): MNRUN_NODES=<n> (unset by default) uses at most the first n nodes of the allocation -- 4 PEs on one node
# (`MNRUN_NODES=1 mnrun.sh 4`) or a 2-node A/B inside a 4-node job.
# T27 (results/T27.md): MNRUN_PRINT_ENV=1 (default 0) computes everything as for a launch -- the plan's pool, the heap, the libfabric
# defaults, the SHMEM implementation -- but prints the computed variables as `export K=V` lines on stdout and exits 0 instead of
# calling srun (no allocation needed; mnrun's own messages go to stderr).  tools/plan_values.py uses it, so the size-dependent
# launch values have one source.  Unset, nothing changes.
# FIT (D26, results/FIT.md): before srun, the planner's memory fit of the command's ecalc run is printed and a run that does not fit
# (device over ECALC_DEVICE_GB, node over ECALC_NODE_GB) is refused with exit 4; MNRUN_FIT=0 skips it (see the FIT block below).
# N7b (value 2: N7E, 1 + 8 PEs per node, local PEs 2d and 2d + 1 are APU d's helpers, NICs round robin over 8): COMM_SHMEM_PE_PER_APU=1 (exported or a VAR=value word of the command; default 1 under an explicit COMM_OFI=0 (D31), else 0) launches 1 + 4 PEs per node under
# COMM_TRANSPORT=shmem: local PEs 0..3 are comm_shmem's proxies (one per APU and NIC: SHMEM_OFI_NIC_POLICY=ROUND-ROBIN and
# SHMEM_OFI_NUM_NICS=4 unless set), the last one the node-process; <procs> is still the node count (one node-process per node), COMM_RANK
# and COMM_SIZE count nodes, the proxies' stdout goes to /dev/null, every task may use all of the step's CPUs (--cpu-bind=none).
# It needs COMM_OFI=0 (set here when unset; COMM_OFI=1 is refused).
set -e
P=$1; shift
PRINT_ENV=${MNRUN_PRINT_ENV:-0}
if [ "$PRINT_ENV" != 0 ]; then exec 3>&1 1>&2; fi
print_env_exit() {   # T27: the variables mnrun.sh sets or computes for the launch, as export lines on the saved stdout (fd 3)
    local v
    echo "# mnrun.sh MNRUN_PRINT_ENV: $P process(es), transport ${COMM_TRANSPORT:-none}, SHMEM implementation ${impl:-none}" >&3
    for v in COMM_TRANSPORT COMM_SHMEM_POOL_MB COMM_OFI_PLAN_CXI SHMEM_SYMMETRIC_SIZE XT_SYMMETRIC_HEAP_SIZE \
             FI_UNIVERSE_SIZE FI_LOG_LEVEL \
             FI_MR_CACHE_MONITOR MNRUN_MODULES MNRUN_UNLOAD; do
        [ -n "${!v+x}" ] && printf 'export %s=%q\n' "$v" "${!v}" >&3
    done
    exit 0
}
# S46 §4.6: FI_MR_CACHE_MONITOR=disabled gave wrong exchanges silently (2097152 of 2097152 points differed, VERIFY FAILED); refuse it.
__fmc=${FI_MR_CACHE_MONITOR-}; __fmc=${__fmc,,}
if [ "$__fmc" = disabled ]; then echo "mnrun.sh: refusing FI_MR_CACHE_MONITOR=$FI_MR_CACHE_MONITOR: it corrupts exchanges silently (S46 section 4.6). Unset it (memhooks, the default) or use userfaultfd." >&2; exit 1; fi
export MNRUN_DIR=$(cd "$(dirname "$0")" && pwd)
# Phase 16 V: the Cray default module picks up AAC7_ROCM (default rocm/7.2.4, results/V16.md) instead of the bare "rocm" alias;
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
# S35 wrap-up: MNRUN_EXCLUDE_HOSTS="h1,h2" (default empty = off) names slow hosts (aac7: aac7-n01, a ~20 s host->device upload stall, results/S31.md).
# If one is in the node list: default MNRUN_EXCLUDE_MODE=warn warns on stderr and changes nothing; MNRUN_EXCLUDE_MODE=drop removes it (the run then uses the remaining nodes;
# ranks shift, so it needs >= the nodes asked for, else fewer nodes are used as with MNRUN_NODES).  Shared with tools/rundriver.sh rd_exclude_hosts.
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
use=$(echo "$nodes" | head -n "$nn")
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
for a in "$@"; do case "$a" in env|-*) continue;; COMM_TRANSPORT=*) COMM_TRANSPORT=${a#*=};; ECALC_LOG_CLOCKS=*) export ECALC_LOG_CLOCKS=${a#*=};; COMM_SHMEM_PE_PER_APU=*) COMM_SHMEM_PE_PER_APU=${a#*=};; COMM_OFI=*) COMM_OFI=${a#*=};; *=*) continue;; *) break;; esac; done   # (ECALC_LOG_CLOCKS: the wrapper reads it)
# 2026-10-12 (TCP removed): SHMEM is the only multi-process transport -- the default for <procs> > 1; any other value is refused
[ "$P" -gt 1 ] && COMM_TRANSPORT=${COMM_TRANSPORT:-shmem}
if [ "$P" -gt 1 ] && [ "$COMM_TRANSPORT" != shmem ]; then echo "mnrun.sh: COMM_TRANSPORT=$COMM_TRANSPORT: only shmem runs more than one process (the TCP transport was removed)" >&2; exit 1; fi
[ -n "${COMM_TRANSPORT:-}" ] && export COMM_TRANSPORT
PPN=1   # N7b: PEs per node (1 + 4 COMM_SHMEM_PE_PER_APU under SHMEM)
# D31: an explicit COMM_OFI=0 makes the proxy mode the default (COMM_SHMEM_PE_PER_APU=1) for a multi-node SHMEM run of one node-process
# per node; an explicit COMM_SHMEM_PE_PER_APU (exported or a word of the command) wins; comm_ofi runs (COMM_OFI unset or 1) are unchanged
if [ -z "${COMM_SHMEM_PE_PER_APU+x}" ] && [ "${COMM_OFI:-}" = 0 ] && [ "${COMM_TRANSPORT:-}" = shmem ] && [ "$P" -gt 1 ] && [ "$per" = 1 ]; then
    COMM_SHMEM_PE_PER_APU=1; echo "mnrun.sh: COMM_OFI=0: COMM_SHMEM_PE_PER_APU=1 (the default under COMM_OFI=0, D31)"; fi
case "${COMM_SHMEM_PE_PER_APU:-0}" in 0|"") ;; 1|2)
    if [ "${COMM_TRANSPORT:-}" = shmem ] && [ "$P" -gt 1 ]; then   # (one process: no SHMEM PEs at all -- ecalc runs single-node, the setting has no effect)
        [ "$per" = 1 ] || { echo "mnrun.sh: COMM_SHMEM_PE_PER_APU=1 needs one node-process per node (<procs> = nodes; here $per per node)" >&2; exit 1; }
        case "${COMM_OFI:-}" in 0) ;; "") export COMM_OFI=0; echo "mnrun.sh: COMM_SHMEM_PE_PER_APU=1: COMM_OFI=0 (the proxies carry the data)";; *) echo "mnrun.sh: COMM_SHMEM_PE_PER_APU=1 needs COMM_OFI=0 (got COMM_OFI=$COMM_OFI)" >&2; exit 1;; esac
        export COMM_SHMEM_PE_PER_APU; PPN=$((1 + 4 * COMM_SHMEM_PE_PER_APU))
        export SHMEM_OFI_NIC_POLICY=${SHMEM_OFI_NIC_POLICY:-ROUND-ROBIN} SHMEM_OFI_NUM_NICS=${SHMEM_OFI_NUM_NICS:-$((4 * COMM_SHMEM_PE_PER_APU))}
        echo "mnrun.sh: COMM_SHMEM_PE_PER_APU=$COMM_SHMEM_PE_PER_APU: $PPN PEs per node ($((PPN - 1)) proxies + the node-process), SHMEM_OFI_NIC_POLICY=$SHMEM_OFI_NIC_POLICY SHMEM_OFI_NUM_NICS=$SHMEM_OFI_NUM_NICS"
    fi;;
  *) echo "mnrun.sh: COMM_SHMEM_PE_PER_APU=$COMM_SHMEM_PE_PER_APU: 0, 1 or 2" >&2; exit 1;; esac
export MNRUN_PPN=$PPN
if [ "$PPN" -gt 1 ]; then   # the CPUs: -c the node's CPUs / PEs per node (auto) and no per-task binding, so the node-process's threads use the whole step
    for i in "${!copt[@]}"; do [ "${copt[$i]}" = -c ] && unset 'copt[i]' 'copt[i+1]'; done; copt=("${copt[@]}")
    ncpu=$(scontrol show node "$(echo "$use" | head -1)" -o 2>/dev/null | sed -n 's/.*CPUTot=\([0-9]*\).*/\1/p'); [ -n "$ncpu" ] && copt+=(-c $((ncpu / PPN)))
    copt+=(--cpu-bind=none)
fi
if [ "$COMM_TRANSPORT" = shmem ]; then
    # s18-target Part 2 (TGTBENCH2 L2, internal target notes §4; the user: "use the flags that optimize performance but allow us to
    # adjust to a different system later"): two libfabric knobs, overridable per system by exporting them before the call.
    # FI_UNIVERSE_SIZE: comm_ofi opens its AVs at the provider's default count (comm_ofi.c:157) and inserts every
    # communicator's member -- the user's target rule ">= 4 x ntasks" (576 -> 2304; their largest tested value was 4096, kept
    # as the floor so a small run still gets their tested size). FI_LOG_LEVEL=warn: cuts libfabric's output ~1000x (the
    # user's L2). Both default with ${VAR:-...}, so a site's own exported value (or a different system's) is never shadowed.
    # MNRUN_FI_DEFAULTS=0 (default 1) leaves both unset -- the pre-s18 launch (S18AB: run (a) of 2026-10-06 segfaulted with them set).
    if [ "${MNRUN_FI_DEFAULTS:-1}" != 0 ]; then
        export FI_UNIVERSE_SIZE=${FI_UNIVERSE_SIZE:-$((4 * P > 4096 ? 4 * P : 4096))}
        export FI_LOG_LEVEL=${FI_LOG_LEVEL:-warn}
    fi
    # S46 (adopted 2026-10-11): FI_MR_CACHE_MONITOR=userfaultfd by default (memhooks deadlocks ROCr <-> libfabric: base 22/105 hangs vs uffd 0/104,
    # results/S46.md). An explicit value (e.g. memhooks) is respected; `disabled` was refused above. Independent of MNRUN_FI_DEFAULTS.
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
            pline=$(bash -lc '[ -n "${MNRUN_UNLOAD:-}" ] && module unload $MNRUN_UNLOAD > /dev/null 2>&1; module load $MNRUN_MODULES > /dev/null 2>&1; exec "$@"' _ env "${pas[@]}" COMM_TRANSPORT=shmem MN_PLAN_ONLY="$pd:$P" "$pbin" 2>/dev/null | grep '^plan pool' || true)
            pmb=$(echo "$pline" | sed -n 's/.*COMM_SHMEM_POOL_MB=\([0-9][0-9]*\).*/\1/p')
            if [ -n "$pmb" ]; then export COMM_SHMEM_POOL_MB=$pmb; echo "mnrun.sh: COMM_SHMEM_POOL_MB=$pmb from MN_PLAN_ONLY=$pd:$P ($(echo "$pline" | sed 's/^plan pool *//'))"
            else echo "mnrun.sh: MN_PLAN_ONLY=$pd:$P of $pbin gave no 'plan pool' line: the pool stays at COMM_SHMEM_POOL_MB=8192" >&2; fi
        fi
    fi
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
    export SHMEM_SYMMETRIC_SIZE=${SHMEM_SYMMETRIC_SIZE:-$((POOL + 512))M}
    export XT_SYMMETRIC_HEAP_SIZE=${XT_SYMMETRIC_HEAP_SIZE:-$SHMEM_SYMMETRIC_SIZE}
    echo "mnrun.sh: SHMEM_SYMMETRIC_SIZE=$SHMEM_SYMMETRIC_SIZE XT_SYMMETRIC_HEAP_SIZE=$XT_SYMMETRIC_HEAP_SIZE (pool $POOL MiB + 512)"
fi
[ "$PRINT_ENV" = 0 ] || print_env_exit
# ---- FIT (user decision D26, results/FIT.md): the pre-launch memory fit.  A block of its own (the pool block above belongs to the
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
        ftr=(); [ "$P" -gt 1 ] && ftr=(COMM_TRANSPORT=shmem)
        fout=$(bash -lc '[ -n "${MNRUN_UNLOAD:-}" ] && module unload $MNRUN_UNLOAD > /dev/null 2>&1; module load $MNRUN_MODULES > /dev/null 2>&1; exec "$@"' _ env "${fas[@]}" "${ftr[@]}" MN_PLAN_ONLY="$fd:$P" "$fbin" 2>&1) || true
        flines=$(echo "$fout" | grep -aE '^plan (fit|REFUSED)' || true)
        if [ -z "$flines" ]; then
            echo "mnrun.sh: FIT: MN_PLAN_ONLY=$fd:$P of $fbin printed no fit report (a binary before FIT?): the memory fit is not checked" >&2
        else
            echo "$flines" | sed 's/^/mnrun.sh: /'
            if echo "$flines" | grep -q '^plan REFUSED'; then
                echo "mnrun.sh: REFUSED before launch (no node time used): $(echo "$flines" | grep '^plan REFUSED' | head -1 | cut -c1-240)" >&2
                exit 4
            fi
        fi
    fi
fi
# the one launch of every path: the tasks block-placed on the listed nodes; each task loads the modules, sets its rank and runs the command
exec srun --jobid="$SLURM_JOB_ID" "${mpi[@]}" -N "$nn" -w "$list" --ntasks="$((P * PPN))" --ntasks-per-node="$((per * PPN))" "${copt[@]}" --distribution=block --gpus-per-node=4 --overlap --export=ALL \
     bash -lc '[ -n "${MNRUN_UNLOAD:-}" ] && module unload $MNRUN_UNLOAD > /dev/null 2>&1; module load $MNRUN_MODULES; k=${MNRUN_PPN:-1}; export COMM_RANK=$((SLURM_PROCID / k)) COMM_SIZE=$((SLURM_NTASKS / k)); if [ "$k" -gt 1 ] && [ $((SLURM_PROCID % k)) -ne $((k - 1)) ]; then export COMM_SHMEM_PROXY=1 ECALC_LOG_CLOCKS=0; exec > /dev/null; fi; [ "${ECALC_LOG_CLOCKS:-0}" != 0 ] && bash "$MNRUN_DIR/aac7env.sh" --log; exec '"$pre"'"$@"' _ "$@"

#!/bin/bash
# mnrun.sh - run <procs> node-processes of ecalc (or any command) connected by the inter-node
# communicator (Phase 8 M1: TCP; Phase 11 S: SHMEM with COMM_TRANSPORT=shmem).  Correctness only on
# aac6.  The processes are spread over the nodes of the allocation, several per node when there are
# fewer nodes (they share the node's four APUs); the node count used is the largest that divides
# <procs> (Slurm places tasks by blocks otherwise, and COMM_HOSTS must match the placement), listed explicitly.
#   mnrun.sh <procs> <command...>          e.g.  SLURM_JOB_ID=<id> ./mnrun.sh 2 ./ecalc 1000000000 /tmp/e.txt
# SHMEM (COMM_TRANSPORT=shmem): the same placement, launched by srun's PMIx (OpenMPI 4.1.6 OSHMEM on aac6; the
# target's srun likewise), rank = PE.  COMM_SHMEM_POOL_MB sizes the transport's symmetric pool (unset: the run's modelled
# need from MN_PLAN_ONLY for ecalc, else 8192; Phase 14 V1); a host heap (OSHMEM, SOS without COMM_SHMEM_DEVHEAP) is set
# 512 MiB above it.  setarch -L (the legacy bottom-up mmap layout) makes OSHMEM's scan of the static
# data segments deterministic across the PEs -- without it every anonymous mapping below liboshmem (thread stacks,
# malloc arenas) is registered and their count differs per PE, and shmem_init crashes in the key exchange about
# half the time (results/S.md).
# Phase 16 A (results/A16.md): a third implementation, `cray` (Cray OpenSHMEMX on aac7 / the target: libsma under /opt/cray/pe/sma,
# COMM_SHMEM_IMPL=cray): plain srun (Cray's PMI), the heap SHMEM_SYMMETRIC_SIZE = XT_SYMMETRIC_HEAP_SIZE = pool + 512 MiB (its heap is
# host hugepages; COMM_SHMEM_DEVHEAP=1 falls back to the registered host pool there).  MNRUN_MODULES is the module line of every
# wrapper (default `rocm`; `cray-dsmml cray-openshmemx rocm` where /opt/cray/pe/sma exists); MNRUN_CPUS_PER_TASK adds srun -c
# (unset: as before; `auto`: the node's CPUs / tasks per node -- aac7's srun confines a task to 2 CPUs otherwise);
# ECALC_LOG_CLOCKS=1 runs aac7env.sh --log (rocm-smi clocks, the APU / NIC NUMA map) on every node before the command.
# Phase 16 D: MNRUN_UNLOAD=<modules> (unset by default) is unloaded before MNRUN_MODULES in every wrapper and the plan call -- a versioned
# rocm module (rocm/7.2.4) conflicts with the default rocm/7.0.3 of the login shell (aac7env.sh sets both from AAC7_ROCM).
# Phase 16 P (results/P16.md): MNRUN_NODES=<n> (unset by default) uses at most the first n nodes of the allocation -- 4 PEs on one node
# (`MNRUN_NODES=1 mnrun.sh 4`) or a 2-node A/B inside a 4-node job.
set -e
P=$1; shift
export MNRUN_DIR=$(cd "$(dirname "$0")" && pwd)
# Phase 16 V: the Cray default module picks up AAC7_ROCM (default rocm/7.2.4, results/V16.md) instead of the bare "rocm" alias;
# rocm/7.0.3 (the login default) must be unloaded first (module conflict).
case "${AAC7_ROCM:-}" in ""|*/*) ;; *) AAC7_ROCM=rocm/$AAC7_ROCM;; esac   # merge V+D: a bare version (D: 7.2.4) means rocm/<version>
if [ -z "${MNRUN_MODULES+x}" ]; then if [ -d /opt/cray/pe/sma ]; then MNRUN_MODULES="cray-dsmml cray-openshmemx ${AAC7_ROCM:-rocm/7.2.4}"; else MNRUN_MODULES=rocm; fi; fi
if [ -d /opt/cray/pe/sma ]; then MNRUN_UNLOAD=${MNRUN_UNLOAD-rocm/7.0.3}; fi
export MNRUN_MODULES; export MNRUN_UNLOAD=${MNRUN_UNLOAD:-}
[ -n "$SLURM_JOB_ID" ] || { echo "set SLURM_JOB_ID to the allocation"; exit 1; }
nodes=$(scontrol show hostnames "$(squeue -j "$SLURM_JOB_ID" -h -o %N)")
[ -n "${MNRUN_NODELIST:-}" ] && nodes=$(echo "$MNRUN_NODELIST" | tr , "\n")   # s29: run on exactly these nodes of the allocation (unset: all, in squeue order)
# S35 wrap-up: MNRUN_EXCLUDE_HOSTS="h1,h2" (default empty = off) names slow hosts (aac7: x9000c1s0b1n0, a ~20 s host->device upload stall, results/S31.md).
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
# S32: MNRUN_LABEL=1 adds srun --label (every output line prefixed "<rank>: "); MNRUN_ARBITRARY=1 places rank i on the i-th host of the use-list
# (srun --distribution=arbitrary + SLURM_HOSTFILE; plain block distribution follows Slurm's sorted node order, not the -w order), one task per node only
dist=block; tpn=(--ntasks-per-node="$per"); nopt=(-N "$nn")
[ "${MNRUN_LABEL:-0}" != 0 ] && copt+=(--label)
if [ "${MNRUN_ARBITRARY:-0}" != 0 ]; then
    [ "$per" = 1 ] || { echo "mnrun.sh: MNRUN_ARBITRARY needs one task per node"; exit 1; }
    export SLURM_HOSTFILE=$(mktemp "${TMPDIR:-/tmp}/mnrun_hf.XXXXXX"); echo "$use" > "$SLURM_HOSTFILE"; dist=arbitrary; tpn=(); nopt=()   # (srun: --nodes is incompatible with arbitrary)
fi
case "${MNRUN_CPUS_PER_TASK:-}" in
    "") ;;
    auto) ncpu=$(scontrol show node "$(echo "$use" | head -1)" -o 2>/dev/null | sed -n 's/.*CPUTot=\([0-9]*\).*/\1/p'); [ -n "$ncpu" ] && [ "$ncpu" -ge "$per" ] && copt=(-c $((ncpu / per)));;
    *) copt=(-c "$MNRUN_CPUS_PER_TASK");;
esac
hosts=$(echo "$use" | awk -v per=$per '{ for (i = 0; i < per; i++) printf "%s%s", (NR > 1 || i) ? "," : "", $1 }')
list=$(echo "$use" | paste -sd,)
export COMM_HOSTS="$hosts" COMM_PORT=${COMM_PORT:-$((20000 + RANDOM % 6000))}    # a per-run port base: a straggler of a failed run must not catch the next run's connections (M3 uses base .. base + 6656); below the ephemeral range 32768-60999, where a listener collides with any outgoing connection now and then (S: "bind: Address already in use" once in ~4 runs at 8 processes)
# Phase 16 A: COMM_TRANSPORT given as a leading VAR=value word of the command (`mnrun.sh 2 env COMM_TRANSPORT=shmem ... ./ecalc`, the
# launch line's form) counts as well -- without this the non-SHMEM path launched it (no heap variable: the library's default heap);
# the same for ECALC_LOG_CLOCKS, which the wrappers test before the command runs
for a in "$@"; do case "$a" in env|-*) continue;; COMM_TRANSPORT=*) COMM_TRANSPORT=${a#*=};; ECALC_LOG_CLOCKS=*) export ECALC_LOG_CLOCKS=${a#*=};; *=*) continue;; *) break;; esac; done   # (ECALC_LOG_CLOCKS: the wrapper reads it)
if [ "$COMM_TRANSPORT" = shmem ]; then
    # s18-target Part 2 (TGTBENCH2 L2, docs/TARGET.md §4; the user: "use the flags that optimize performance but allow us to
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
    echo "mnrun.sh: FI_UNIVERSE_SIZE=${FI_UNIVERSE_SIZE:-<unset>} FI_LOG_LEVEL=${FI_LOG_LEVEL:-<unset>} (MNRUN_FI_DEFAULTS=${MNRUN_FI_DEFAULTS:-1})"
    # Phase 14 V1: the pool (and with it the heap below) from the run's own model when COMM_SHMEM_POOL_MB is not set by hand: the
    # command's SHMEM-linked executable followed by a digit count (ecalc <digits> ...) is asked first, on this host, with the
    # command's VAR=value words and MN_PLAN_ONLY=<digits>:<procs> (no device is touched), and its `plan pool` line gives
    # COMM_SHMEM_POOL_MB -- so COMM_SHMEM_POOL_AUTO (on in ecalc by default) finds the pool already at the need, and a host heap
    # (SHMEM_SYMMETRIC_SIZE / SHMEM_SYMMETRIC_HEAP_SIZE = pool + 512 MiB) holds it.  Other commands: 8192 as before.
    # MNRUN_PLAN_POOL=0 skips the plan.
    if [ -z "${COMM_SHMEM_POOL_MB:-}" ] && [ "${MNRUN_PLAN_POOL:-1}" != 0 ]; then
        pbin=; pd=; pas=()
        for a in "$@"; do
            if [ -n "$pbin" ]; then case "$a" in -*) continue;; esac; case "$a" in *[!0-9]*|"") ;; *) pd=$a;; esac; break; fi
            case "$a" in [A-Za-z_]*=*) pas+=("$a"); continue;; -*) continue;; esac
            f=$(command -v -- "$a" 2>/dev/null) || continue; [ -f "$f" ] && [ -x "$f" ] || continue
            head -c 4 "$f" 2>/dev/null | grep -q ELF || continue
            readelf -d "$f" 2>/dev/null | grep NEEDED | grep -q -e libsma -e liboshmem && pbin=$f
        done
        if [ -n "$pbin" ] && [ -n "$pd" ]; then
            # Phase 17 OFIMEM: comm_ofi (on by default where a cxi NIC is present) moves the device staging out of the SHMEM pool, and
            # the plan sizes the pool by it -- but the plan runs here, on a login node that may have no cxi (aac7's uan1): ask the
            # first compute node once (COMM_OFI unset, no cxi here) and hand the answer to the plan as COMM_OFI_PLAN_CXI
            if [ -z "${COMM_OFI:-}" ] && [ -z "${COMM_OFI_PLAN_CXI:-}" ] && [ ! -e /sys/class/cxi/cxi0 ] && [ -n "${SLURM_JOB_ID:-}" ]; then
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
    # Phase 12 S: the implementation the binary was built against: SOS (make SHMEM_HOME=~/sos; libsma) or OSHMEM (oshcc).
    # COMM_SHMEM_IMPL=sos|oshmem overrides the detection.
    # Phase 14 N4 (A4): the detection looks through wrappers (env, stdbuf, timeout, numactl, setarch, nice, ...): every word of
    # the command that resolves to an executable ELF file is checked for libsma / liboshmem in its dynamic section, the
    # first that links either decides (the wrappers link neither); none found: oshmem, as before, with a note.
    impl=${COMM_SHMEM_IMPL:-}
    case "$impl" in ""|sos|oshmem|cray) ;; *) echo "mnrun.sh: COMM_SHMEM_IMPL=$impl: use sos, oshmem or cray"; exit 1;; esac
    if [ -z "$impl" ]; then
        for a in "$@"; do
            case "$a" in *=*|-*) continue;; esac                                 # env assignments, wrapper options
            f=$(command -v -- "$a" 2>/dev/null) || continue; [ -f "$f" ] && [ -x "$f" ] || continue
            head -c 4 "$f" 2>/dev/null | grep -q ELF || continue                  # scripts: their interpreter is not the binary
            need=$(readelf -d "$f" 2>/dev/null | grep NEEDED; ldd "$f" 2>/dev/null)
            if echo "$need" | grep -q libsma; then if echo "$need" | grep -q /opt/cray/pe; then impl=cray; else impl=sos; fi; break; fi   # Phase 16 A: Cray's libsma by its rpath / resolved path
            if echo "$need" | grep -q liboshmem; then impl=oshmem; break; fi
        done
        [ -n "$impl" ] || { impl=oshmem; echo "mnrun.sh: no libsma/liboshmem found in the command's executables; assuming oshmem (set COMM_SHMEM_IMPL)" >&2; }
    fi
    [ -n "${MNRUN_SHOW_IMPL:-}" ] && { echo "mnrun.sh: SHMEM implementation $impl"; [ "$MNRUN_SHOW_IMPL" = only ] && exit 0; }   # (test hook: MNRUN_SHOW_IMPL=only prints and stops)
    if [ "$impl" = sos ]; then
        # SOS: PMI-1 (simple PMI) under srun's pmi2 plugin; the sockets provider of libfabric (the tcp provider lacks what SOS asks
        # for); the pool is the transport's own shmem_malloc (SHMEM_SYMMETRIC_SIZE) or, with COMM_SHMEM_DEVHEAP=1, a HIP buffer
        # registered as SOS's external heap (results/S12.md).  SOS's internal heap is only its own bookkeeping then.
        if [ "${COMM_SHMEM_DEVHEAP:-0}" != 0 ]; then export SHMEM_SYMMETRIC_SIZE=${SHMEM_SYMMETRIC_SIZE:-64M}; else export SHMEM_SYMMETRIC_SIZE=${SHMEM_SYMMETRIC_SIZE:-$((POOL + 512))M}; fi
        export FI_PROVIDER=${FI_PROVIDER:-sockets} SHMEM_OFI_PROVIDER=${SHMEM_OFI_PROVIDER:-sockets} SHMEM_DISABLE_ASLR_CHECK=1
        echo "mnrun.sh: SHMEM_SYMMETRIC_SIZE=$SHMEM_SYMMETRIC_SIZE (pool $POOL MiB + 512, unless COMM_SHMEM_DEVHEAP)"
        exec srun --jobid="$SLURM_JOB_ID" --mpi=pmi2 "${nopt[@]}" -w "$list" --ntasks="$P" "${tpn[@]}" "${copt[@]}" --distribution=$dist --gpus-per-node=4 --overlap --export=ALL \
             bash -lc '[ -n "${MNRUN_UNLOAD:-}" ] && module unload $MNRUN_UNLOAD > /dev/null 2>&1; module load $MNRUN_MODULES; export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; [ "${ECALC_LOG_CLOCKS:-0}" != 0 ] && bash "$MNRUN_DIR/aac7env.sh" --log; exec "$@"' _ "$@"
    fi
    if [ "$impl" = cray ]; then
        # Phase 16 A: Cray OpenSHMEMX: srun's PMI (no --mpi option), the host hugepage heap at pool + 512 MiB under both of its names;
        # the libfabric provider is cxi on aac7 (nothing to set); SHMEM_OFI_* knobs (NIC policy, progress) are the caller's.
        export SHMEM_SYMMETRIC_SIZE=${SHMEM_SYMMETRIC_SIZE:-$((POOL + 512))M}
        export XT_SYMMETRIC_HEAP_SIZE=${XT_SYMMETRIC_HEAP_SIZE:-$SHMEM_SYMMETRIC_SIZE}
        echo "mnrun.sh: SHMEM_SYMMETRIC_SIZE=$SHMEM_SYMMETRIC_SIZE XT_SYMMETRIC_HEAP_SIZE=$XT_SYMMETRIC_HEAP_SIZE (pool $POOL MiB + 512)"
        exec srun --jobid="$SLURM_JOB_ID" "${nopt[@]}" -w "$list" --ntasks="$P" "${tpn[@]}" "${copt[@]}" --distribution=$dist --gpus-per-node=4 --overlap --export=ALL \
             bash -lc '[ -n "${MNRUN_UNLOAD:-}" ] && module unload $MNRUN_UNLOAD > /dev/null 2>&1; module load $MNRUN_MODULES; export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; [ "${ECALC_LOG_CLOCKS:-0}" != 0 ] && bash "$MNRUN_DIR/aac7env.sh" --log; exec "$@"' _ "$@"
    fi
    export SHMEM_SYMMETRIC_HEAP_SIZE=${SHMEM_SYMMETRIC_HEAP_SIZE:-$((POOL + 512))M}
    export OMPI_MCA_memheap_base_max_segments=${OMPI_MCA_memheap_base_max_segments:-64}
    echo "mnrun.sh: SHMEM_SYMMETRIC_HEAP_SIZE=$SHMEM_SYMMETRIC_HEAP_SIZE (pool $POOL MiB + 512)"
    exec srun --jobid="$SLURM_JOB_ID" --mpi=pmix "${nopt[@]}" -w "$list" --ntasks="$P" "${tpn[@]}" "${copt[@]}" --distribution=$dist --gpus-per-node=4 --overlap --export=ALL \
         bash -lc '[ -n "${MNRUN_UNLOAD:-}" ] && module unload $MNRUN_UNLOAD > /dev/null 2>&1; module load $MNRUN_MODULES; export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; [ "${ECALC_LOG_CLOCKS:-0}" != 0 ] && bash "$MNRUN_DIR/aac7env.sh" --log; exec setarch x86_64 -L "$@"' _ "$@"
fi
exec srun --jobid="$SLURM_JOB_ID" "${nopt[@]}" -w "$list" --ntasks="$P" "${tpn[@]}" "${copt[@]}" --distribution=$dist --gpus-per-node=4 --overlap --export=ALL \
     bash -lc '[ -n "${MNRUN_UNLOAD:-}" ] && module unload $MNRUN_UNLOAD > /dev/null 2>&1; module load $MNRUN_MODULES; export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; [ "${ECALC_LOG_CLOCKS:-0}" != 0 ] && bash "$MNRUN_DIR/aac7env.sh" --log; exec "$@"' _ "$@"

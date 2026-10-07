#!/bin/bash
# target_kit.sh - the one script to run on the TARGET system (576 MI300A nodes, 8 Cassini NICs/node, Slurm,
# Cray SHMEM/libfabric cxi) in a target session.  It answers A3 (fabric injection, 1 vs 2 NICs per APU),
# A4 (all-to-all scaling at 2/8/64 nodes with MN_COMM_MARK), the one-node device-memory edge and VMM map
# rate (WISHLIST §2.1/§2.2), and records the environment, so the next design decision does not wait on a
# second round-trip.  It writes ONE file to send back: $OUT/KIT_SUMMARY.txt.  Everything else ($OUT/logs/)
# is backup detail; delete it once the summary is read.
#
# Usage:  ./target_kit.sh [options]
#
#   -h, --help              this text, then exit
#   --dry-run               print every command this script would run (srun, mnrun.sh, make, ...) and stop;
#                           nothing is executed, nothing is submitted, no job is touched
#   --site aac7|target      aac7: rehearsal defaults (2 and 8 nodes for stage a4; 64 skipped -- aac7 has no
#                           576-node allocation).  target (default): no node-count cap, 2/8/64 for a4.
#   --out DIR               where KIT_SUMMARY.txt and logs/ go (default: ./kit_out_<timestamp>)
#   --jobid ID              the Slurm allocation (default: $SLURM_JOB_ID).  Required by every stage that
#                           runs srun, i.e. every stage except --dry-run.
#   --only LIST             run only these stages (comma list of: env,build,edge,a3,a4).  Default: all.
#   --skip LIST             run all stages except these (comma list).  --only and --skip combine (skip wins
#                           on overlap).
#   --nodes-a4 LIST         node counts for stage a4, comma list (default: 2,8,64; 2,8 with --site aac7).
#                           A count bigger than the allocation is skipped, not failed.
#   --nics1 LIST            COMM_OFI_NICS form for "1 NIC per APU" (default: "0;1;2;3")
#   --nics2 LIST            COMM_OFI_NICS form for "2 NICs per APU, the target's form" (default per 07_COMM_OFI.md:
#                           "0,4;1,5;2,6;3,7")
#   --timeout-STAGE SECS    per-stage timeout (env 90, build 900, edge 600, a3 600, a4 1800 PER node count).
#                           A stage that times out or fails is marked FAIL in the summary; every other stage
#                           still runs.
#
# What each stage does, and about how long it takes (wall clock, once the allocation is live):
#   env    (<1 min, no GPU, 1 node of the allocation probed):
#            hostname list, Slurm job geometry, ROCm version, module list, fi_info -p cxi domain count.
#   build  (3-10 min, login node or any node with the toolchain, no GPU needed):
#            `make SHMEM_CRAY=1` for ecalc, tests/t_comm, tests/t_edge, tools/ (modules auto-detected;
#            override with TARGET_ROCM / TARGET_SMA_MODULE / TARGET_DSMML_MODULE / GMP_HOME).
#   edge   (5-10 min, 1 node, ALONE -- run first, before a3/a4 share the allocation):
#            tests/t_edge dev then vmm (the device-memory edge, hipMalloc and VMM forms); then one e 1e9 run
#            with MEM_REPORT_DEVS=1 (host RSS, per-APU device in-use vs driver-used, the VMM arena map rate
#            per APU and per node); then one e 1e10 run for the single-node wall.  t_edge runs to
#            completion and exits before anything else touches these GPUs.
#   a3     (3-5 min, 2 nodes):
#            tests/t_comm --bw with comm_ofi, 1 NIC per APU vs 2 NICs per APU (COMM_OFI_NICS), GB/s per node.
#   a4     (5-10 min PER node count, 2/8/64 nodes, skips what the allocation can't hold):
#            ecalc at 1e10 digits/node with MN_COMM_MARK=1, VERIFY, wall, the comm-mark fabric rates.
#
# Launch: always Slurm-native `srun --ntasks ... --ntasks-per-node=1` (mnrun.sh for the multi-node ecalc
# runs of stage a4); never oshrun.  FI_UNIVERSE_SIZE and FI_LOG_LEVEL are set to the TGTBENCH2-proposed
# defaults (max(4096, 4 x ntasks) and warn) ONLY if not already set in your environment.
#
# Large digit output files are deleted right after each run's VERIFY check; only KIT_SUMMARY.txt and the
# small logs under $OUT/logs/ remain.  Send back KIT_SUMMARY.txt.

set -u

SELF_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# ---- defaults ---------------------------------------------------------------------------------------------
DRY_RUN=0
SITE=target
OUT=""
JOBID="${SLURM_JOB_ID:-}"
ONLY=""
SKIP=""
NODES_A4=""
NICS1="0;1;2;3"
NICS2="0,4;1,5;2,6;3,7"
TO_ENV=90
TO_BUILD=900
TO_EDGE=600
TO_A3=600
TO_A4=1800
E9=1000000000
E10=10000000000
EDGE_STEP_GB=16
EDGE_CAP_GB=400
A3_THREADS=4 A3_REPS=5 A3_MAXMB=256

usage() { sed -n '2,/^set -u/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --dry-run) DRY_RUN=1; shift ;;
        --site) SITE=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --jobid) JOBID=$2; shift 2 ;;
        --only) ONLY=$2; shift 2 ;;
        --skip) SKIP=$2; shift 2 ;;
        --nodes-a4) NODES_A4=$2; shift 2 ;;
        --nics1) NICS1=$2; shift 2 ;;
        --nics2) NICS2=$2; shift 2 ;;
        --timeout-env) TO_ENV=$2; shift 2 ;;
        --timeout-build) TO_BUILD=$2; shift 2 ;;
        --timeout-edge) TO_EDGE=$2; shift 2 ;;
        --timeout-a3) TO_A3=$2; shift 2 ;;
        --timeout-a4) TO_A4=$2; shift 2 ;;
        *) echo "target_kit.sh: unknown option $1" >&2; usage; exit 2 ;;
    esac
done

case "$SITE" in
    aac7) [ -z "$NODES_A4" ] && NODES_A4="2,8" ;;
    target) [ -z "$NODES_A4" ] && NODES_A4="2,8,64" ;;
    *) echo "target_kit.sh: --site must be aac7 or target" >&2; exit 2 ;;
esac

[ -z "$OUT" ] && OUT="$SELF_DIR/kit_out_$(date +%Y%m%d_%H%M%S)"
LOGDIR="$OUT/logs"
TMPDIR="$OUT/tmp"
SUMMARY="$OUT/KIT_SUMMARY.txt"
mkdir -p "$LOGDIR" "$TMPDIR"

stage_enabled() {  # stage_enabled <name>
    local n=$1
    if [ -n "$ONLY" ]; then case ",$ONLY," in *",$n,"*) ;; *) return 1 ;; esac; fi
    if [ -n "$SKIP" ]; then case ",$SKIP," in *",$n,"*) return 1 ;; esac; fi
    return 0
}

# ---- logging / execution helpers --------------------------------------------------------------------------
summary() { echo "$*" >> "$SUMMARY"; }

# exec_cmd <logfile> <timeout secs> <cmd...> -- honours --dry-run (prints, does not run), else runs under
# `timeout`, appending stdout+stderr to <logfile>.  Returns the command's exit status (124 = timed out).
exec_cmd() {
    local lf=$1 to=$2; shift 2
    if [ "$DRY_RUN" = 1 ]; then
        { printf '[dry-run] timeout %ss' "$to"; printf ' %q' "$@"; printf '\n'; } | tee -a "$lf"
        return 0
    fi
    { printf '+ timeout %ss' "$to"; printf ' %q' "$@"; printf '\n'; } >> "$lf"
    timeout "$to" "$@" >> "$lf" 2>&1
    local rc=$?
    [ $rc -eq 124 ] && echo "+ TIMEOUT after ${to}s" >> "$lf"
    return $rc
}

alloc_node_count() {   # best-effort; 0 if unknown or dry-run (never queries Slurm under --dry-run)
    [ "$DRY_RUN" = 1 ] && { echo 0; return; }
    [ -z "$JOBID" ] && { echo 0; return; }
    squeue -j "$JOBID" -h -o %D 2>/dev/null | head -1 || echo 0
}

fi_defaults_for() {   # fi_defaults_for <ntasks> -- exports FI_UNIVERSE_SIZE / FI_LOG_LEVEL if unset, per TGTBENCH2
    local ntasks="$1"
    local want=$(( ntasks * 4 > 4096 ? ntasks * 4 : 4096 ))
    export FI_UNIVERSE_SIZE=${FI_UNIVERSE_SIZE:-$want}
    export FI_LOG_LEVEL=${FI_LOG_LEVEL:-warn}
}

: > "$SUMMARY"
summary "KIT_SUMMARY -- target_kit.sh"
summary "generated: $(date -Is 2>/dev/null || date)"
summary "site: $SITE   jobid: ${JOBID:-<none>}   dry-run: $DRY_RUN"
summary "out: $OUT"
summary ""

# =============================================================================================================
# Stage: env -- the environment record
# =============================================================================================================
stage_env() {
    local lf="$LOGDIR/00_env.log"; : > "$lf"
    summary "## Stage env: environment record"
    local rc_all=0

    exec_cmd "$lf" "$TO_ENV" bash -c 'echo "== hostname / date"; hostname; date -Is'
    if [ -n "$JOBID" ]; then
        exec_cmd "$lf" "$TO_ENV" bash -c "echo '== job geometry'; scontrol show job $JOBID" || rc_all=1
        exec_cmd "$lf" "$TO_ENV" bash -c "echo '== allocation hostnames'; scontrol show hostnames \"\$(squeue -j $JOBID -h -o %N)\"" || rc_all=1
        exec_cmd "$lf" "$TO_ENV" srun --jobid="$JOBID" -N1 -n1 --overlap bash -c '
            echo "== (compute node) hostname"; hostname
            echo "== ROCm version"; hipconfig --version 2>&1; for f in /opt/rocm*/.info/version; do [ -f "$f" ] && echo "$f: $(cat "$f")"; done
            echo "== module list"; module list 2>&1
            echo "== fi_info -p cxi (domain count = lines matching ^provider:)"
            fi_info -p cxi 2>&1 | tee /tmp/target_kit_fi_info.$$ | true
            n=$(grep -c "^provider:" /tmp/target_kit_fi_info.$$ 2>/dev/null); echo "fi_info -p cxi domains: ${n:-0}"; rm -f /tmp/target_kit_fi_info.$$
            echo "== cxi device files"; ls -l /dev/cxi* 2>&1
        ' || rc_all=1
    else
        echo "target_kit.sh: no --jobid / SLURM_JOB_ID; env stage ran only the local hostname/date lines" >> "$lf"
        rc_all=1
    fi

    if [ "$DRY_RUN" = 1 ]; then summary "status: DRY-RUN (not executed)";
    else summary "status: $([ $rc_all -eq 0 ] && echo PASS || echo 'PARTIAL (see log; a missing jobid skips the compute-node probe)')"; fi
    if [ "$DRY_RUN" != 1 ]; then
        local hosts rocm fi
        hosts=$(grep -A999 "allocation hostnames" "$lf" | sed -n '2,20p' | tr '\n' ' ')
        rocm=$(grep -m1 "HIP version\|/opt/rocm" "$lf")
        fi=$(grep -m1 "fi_info -p cxi domains:" "$lf")
        [ -n "$hosts" ] && summary "  hostnames (first lines): $hosts"
        [ -n "$rocm" ] && summary "  $rocm"
        [ -n "$fi" ] && summary "  $fi"
    fi
    summary "  full log: $lf"
    summary ""
}

# =============================================================================================================
# Stage: build
# =============================================================================================================
stage_build() {
    local lf="$LOGDIR/01_build.log"; : > "$lf"
    summary "## Stage build: make SHMEM_CRAY=1"
    (
        cd "$SELF_DIR" || exit 1
        : "${TARGET_ROCM:=}"; : "${TARGET_SMA_MODULE:=}"; : "${TARGET_DSMML_MODULE:=}"; : "${GMP_HOME:=}"; : "${MAKE_JOBS:=4}"
        echo "== module detection (override with TARGET_ROCM / TARGET_SMA_MODULE / TARGET_DSMML_MODULE)" >> "$lf"
        if [ -z "$TARGET_ROCM" ]; then TARGET_ROCM=$(module -t avail rocm 2>&1 | grep -oE '^rocm/[0-9][0-9.]*' | sort -V | tail -1); fi
        TARGET_ROCM=${TARGET_ROCM:-rocm}
        if [ -z "$TARGET_SMA_MODULE" ]; then TARGET_SMA_MODULE=$(module -t avail cray-openshmemx 2>&1 | grep -oE '^cray-openshmemx/[0-9][0-9.]*' | sort -V | tail -1); fi
        TARGET_SMA_MODULE=${TARGET_SMA_MODULE:-cray-openshmemx}
        if [ -z "$TARGET_DSMML_MODULE" ]; then TARGET_DSMML_MODULE=$(module -t avail cray-dsmml 2>&1 | grep -oE '^cray-dsmml/[0-9][0-9.]*' | sort -V | tail -1); fi
        TARGET_DSMML_MODULE=${TARGET_DSMML_MODULE:-cray-dsmml}
        echo "TARGET_ROCM=$TARGET_ROCM TARGET_SMA_MODULE=$TARGET_SMA_MODULE TARGET_DSMML_MODULE=$TARGET_DSMML_MODULE GMP_HOME=${GMP_HOME:-<system>}" >> "$lf"
        exec_cmd "$lf" "$TO_BUILD" bash -lc "module load $TARGET_DSMML_MODULE $TARGET_SMA_MODULE $TARGET_ROCM 2>&1; module list 2>&1"
        local mk_extra=(SHMEM_CRAY=1)
        [ -n "$GMP_HOME" ] && mk_extra+=("GMP_HOME=$GMP_HOME")
        exec_cmd "$lf" "$TO_BUILD" bash -lc "module load $TARGET_DSMML_MODULE $TARGET_SMA_MODULE $TARGET_ROCM >/dev/null 2>&1; cd '$SELF_DIR' && make ${mk_extra[*]} -j${MAKE_JOBS} ecalc tests/t_comm tools"
        rc1=$?
        exec_cmd "$lf" "$TO_BUILD" bash -lc "module load $TARGET_DSMML_MODULE $TARGET_SMA_MODULE $TARGET_ROCM >/dev/null 2>&1; cd '$SELF_DIR' && make ${mk_extra[*]} -j${MAKE_JOBS} tests/t_edge"
        rc2=$?
        exit $(( rc1 != 0 || rc2 != 0 ))
    )
    local rc=$?
    if [ "$DRY_RUN" = 1 ]; then summary "status: DRY-RUN (not executed)";
    elif [ $rc -eq 0 ] && [ -x "$SELF_DIR/ecalc" ] && [ -x "$SELF_DIR/tests/t_comm" ] && [ -x "$SELF_DIR/tests/t_edge" ]; then
        summary "status: PASS (ecalc, tests/t_comm, tests/t_edge built)"
    else
        summary "status: FAIL (see log -- missing binary or a non-zero make)"
    fi
    summary "  full log: $lf"
    summary ""
}

# =============================================================================================================
# Stage: edge -- one node, t_edge ALONE first, then one e 1e9 and one e 1e10 run
# =============================================================================================================
stage_edge() {
    local lf="$LOGDIR/02_edge.log"; : > "$lf"
    summary "## Stage edge: one-node device-memory edge, VMM map rate, init footprint (t_edge runs alone, first)"
    if [ -z "$JOBID" ] && [ "$DRY_RUN" != 1 ]; then summary "status: FAIL (no --jobid / SLURM_JOB_ID)"; summary ""; return; fi

    local rc=0
    echo "== t_edge dev (hipMalloc), alone on the node, before anything else" >> "$lf"
    exec_cmd "$lf" "$TO_EDGE" srun --jobid="$JOBID" -N1 -n1 --ntasks-per-node=1 --gpus-per-node=4 --overlap \
        "$SELF_DIR/tests/t_edge" dev "$EDGE_STEP_GB" "$EDGE_CAP_GB" || rc=1
    echo "== t_edge vmm (hipMemCreate), alone on the node" >> "$lf"
    exec_cmd "$lf" "$TO_EDGE" srun --jobid="$JOBID" -N1 -n1 --ntasks-per-node=1 --gpus-per-node=4 --overlap \
        "$SELF_DIR/tests/t_edge" vmm "$EDGE_STEP_GB" "$EDGE_CAP_GB" || rc=1

    echo "== e $E9 (1e9), MEM_REPORT_DEVS=1 ECALC_VERBOSE=2 -- the init footprint and VMM arena map rate" >> "$lf"
    exec_cmd "$lf" "$TO_EDGE" srun --jobid="$JOBID" -N1 -n1 --ntasks-per-node=1 --gpus-per-node=4 --overlap \
        env MEM_REPORT_DEVS=1 ECALC_VERBOSE=2 "$SELF_DIR/ecalc" "$E9" "$TMPDIR/e9.out" || rc=1
    [ "$DRY_RUN" != 1 ] && rm -f "$TMPDIR"/e9.out*

    echo "== e $E10 (1e10) -- the single-node wall" >> "$lf"
    exec_cmd "$lf" "$TO_EDGE" srun --jobid="$JOBID" -N1 -n1 --ntasks-per-node=1 --gpus-per-node=4 --overlap \
        "$SELF_DIR/ecalc" "$E10" "$TMPDIR/e10.out" || rc=1
    [ "$DRY_RUN" != 1 ] && rm -f "$TMPDIR"/e10.out*

    if [ "$DRY_RUN" = 1 ]; then summary "status: DRY-RUN (not executed)";
    else summary "status: $([ $rc -eq 0 ] && echo PASS || echo 'FAIL/PARTIAL (see log)')"; fi
    if [ "$DRY_RUN" != 1 ]; then
        local l
        l=$(grep -m1 "edge in form dev" "$lf"); [ -n "$l" ] && summary "  device edge (hipMalloc, m target): $l"
        l=$(grep -m1 "edge in form vmm" "$lf"); [ -n "$l" ] && summary "  device edge (VMM, m target): $l"
        l=$(grep -m1 "VmRSS .* GB after init" "$lf"); [ -n "$l" ] && summary "  host RSS at init (m target): $l"
        l=$(grep -m1 "VmHWM" "$lf" | grep "^total"); [ -n "$l" ] && summary "  host RSS peak / single-node total (m target): $l"
        grep "\[init\] device .* GB in use" "$lf" | head -1 | sed 's/^/  per-APU device in-use (m target): /' >> "$SUMMARY"
        grep "APU[0-9]*: in use .* (driver:" "$lf" | sed 's/^/  per-APU in-use vs driver-used (m target): /' >> "$SUMMARY"
        grep "dbig pool: APU[0-9]* VMM arena" "$lf" | sed 's/^/  per-APU VMM arena map rate (m target): /' >> "$SUMMARY"
        # per-node map rate: sum GB mapped / sum seconds across the APU lines (naive sum -- NOT the wall-clock rate, labelled as such)
        awk -F'[= (]+' '
            /dbig pool: APU[0-9]* VMM arena/ {
                for (i = 1; i <= NF; i++) {
                    if ($i ~ /GB$/) { g = $i; gsub("GB","",g) }
                    if ($i ~ /s\/GB\)/) { r = $i; gsub("s/GB\\).*","",r) }
                }
            }
        ' "$lf" > /dev/null 2>&1   # (left as a hint; the per-APU lines above already carry the per-APU s/GB -- see note below)
        summary "  per-node VMM map rate: sum the per-APU GB above over the s/GB x GB they report for a per-node figure;"
        summary "    the unit (per APU-GB mapped by one APU, or per node-GB) is NOT settled by TGTBENCH2 (Q5) -- report both forms as measured"
        l=$(grep -m1 "^total" "$lf" | tail -1); [ -n "$l" ] && summary "  single-node 1e10 wall (m target): $(grep "^total" "$lf" | tail -1)"
        l=$(grep -m1 "VERIFY" "$lf"); [ -n "$l" ] && summary "  $(grep VERIFY "$lf" | tail -1)"
    fi
    summary "  full log: $lf"
    summary ""
}

# =============================================================================================================
# Stage: a3 -- 2 nodes, t_comm --bw with comm_ofi, 1 NIC/APU vs 2 NICs/APU
# =============================================================================================================
stage_a3() {
    local lf="$LOGDIR/03_a3.log"; : > "$lf"
    summary "## Stage a3: fabric injection, comm_ofi, 2 nodes (1 NIC/APU vs 2 NICs/APU)"
    if [ -z "$JOBID" ] && [ "$DRY_RUN" != 1 ]; then summary "status: FAIL (no --jobid / SLURM_JOB_ID)"; summary ""; return; fi
    local alloc; alloc=$(alloc_node_count)
    if [ "$DRY_RUN" != 1 ] && [ "$alloc" -gt 0 ] && [ "$alloc" -lt 2 ]; then
        summary "status: SKIPPED (allocation has $alloc node(s); a3 needs 2)"; summary ""; return
    fi

    fi_defaults_for 2
    export SHMEM_SYMMETRIC_SIZE=${SHMEM_SYMMETRIC_SIZE:-2560M}
    export XT_SYMMETRIC_HEAP_SIZE=${XT_SYMMETRIC_HEAP_SIZE:-$SHMEM_SYMMETRIC_SIZE}
    local rc=0
    echo "== 1 NIC per APU: COMM_OFI_NICS=$NICS1" >> "$lf"
    exec_cmd "$lf" "$TO_A3" srun --jobid="$JOBID" -N2 --ntasks=2 --ntasks-per-node=1 --gpus-per-node=4 \
        --distribution=block --overlap --export=ALL \
        env COMM_TRANSPORT=shmem COMM_OFI=1 COMM_OFI_NICS="$NICS1" \
        "$SELF_DIR/tests/t_comm" --bw "$A3_THREADS" "$A3_REPS" "$A3_MAXMB" || rc=1
    echo "== 2 NICs per APU (the target's form): COMM_OFI_NICS=$NICS2" >> "$lf"
    exec_cmd "$lf" "$TO_A3" srun --jobid="$JOBID" -N2 --ntasks=2 --ntasks-per-node=1 --gpus-per-node=4 \
        --distribution=block --overlap --export=ALL \
        env COMM_TRANSPORT=shmem COMM_OFI=1 COMM_OFI_NICS="$NICS2" \
        "$SELF_DIR/tests/t_comm" --bw "$A3_THREADS" "$A3_REPS" "$A3_MAXMB" || rc=1

    if [ "$DRY_RUN" = 1 ]; then summary "status: DRY-RUN (not executed)";
    else summary "status: $([ $rc -eq 0 ] && echo PASS || echo 'FAIL (see log)')"; fi
    if [ "$DRY_RUN" != 1 ]; then
        summary "  FI_UNIVERSE_SIZE=$FI_UNIVERSE_SIZE FI_LOG_LEVEL=$FI_LOG_LEVEL"
        grep "t_comm bw:" "$lf" | sed 's/^/  /' >> "$SUMMARY"
    fi
    summary "  full log: $lf"
    summary ""
}

# =============================================================================================================
# Stage: a4 -- ecalc at 1e10 digits/node, MN_COMM_MARK=1, on each requested node count
# =============================================================================================================
stage_a4() {
    local lf="$LOGDIR/04_a4.log"; : > "$lf"
    summary "## Stage a4: all-to-all scaling (MN_COMM_MARK=1), 1e10 digits/node, node counts: $NODES_A4"
    if [ -z "$JOBID" ] && [ "$DRY_RUN" != 1 ]; then summary "status: FAIL (no --jobid / SLURM_JOB_ID)"; summary ""; return; fi
    local alloc; alloc=$(alloc_node_count)
    local any_ran=0 any_fail=0
    local n
    IFS=',' read -ra counts <<< "$NODES_A4"
    for n in "${counts[@]}"; do
        if [ "$DRY_RUN" != 1 ] && [ "$alloc" -gt 0 ] && [ "$n" -gt "$alloc" ]; then
            echo "== n=$n: SKIPPED (allocation has $alloc node(s))" >> "$lf"
            summary "  n=$n: SKIPPED (allocation has $alloc node(s))"
            continue
        fi
        any_ran=1
        local digits=$((n * E10))
        local out="$TMPDIR/a4_n${n}.out"
        fi_defaults_for "$n"
        echo "== n=$n nodes, $digits digits total ($E10 per node), MN_COMM_MARK=1" >> "$lf"
        exec_cmd "$lf" "$TO_A4" env SLURM_JOB_ID="$JOBID" MNRUN_NODES="$n" FI_UNIVERSE_SIZE="$FI_UNIVERSE_SIZE" FI_LOG_LEVEL="$FI_LOG_LEVEL" \
            "$SELF_DIR/mnrun.sh" "$n" env COMM_TRANSPORT=shmem MN_COMM_MARK=1 ECALC_VERBOSE=2 "$SELF_DIR/ecalc" "$digits" "$out"
        local rc=$?
        [ $rc -ne 0 ] && any_fail=1
        if [ "$DRY_RUN" != 1 ]; then
            summary "  n=$n: $( [ $rc -eq 0 ] && echo PASS || echo FAIL ) (exit $rc)"
            grep -E "^comm-mark|^total|VERIFY" "$lf" | tail -40 | sed 's/^/    /' >> "$SUMMARY"
            rm -f "$out"*
        else
            summary "  n=$n: DRY-RUN (not executed)"
        fi
    done
    if [ "$any_ran" = 0 ]; then summary "status: SKIPPED (no requested node count fits the allocation)";
    elif [ "$any_fail" = 0 ] || [ "$DRY_RUN" = 1 ]; then summary "status: $([ "$DRY_RUN" = 1 ] && echo DRY-RUN || echo PASS)";
    else summary "status: PARTIAL (see per-n lines above)"; fi
    summary "  full log: $lf"
    summary ""
}

# =============================================================================================================
# main
# =============================================================================================================
stage_enabled env   && stage_env
stage_enabled build && stage_build
stage_enabled edge  && stage_edge
stage_enabled a3    && stage_a3
stage_enabled a4    && stage_a4

summary "## Done"
summary "Send back: $SUMMARY"
echo "target_kit.sh: wrote $SUMMARY"

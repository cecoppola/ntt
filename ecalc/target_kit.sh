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
#   --only LIST             run only these stages (comma list of: env,build,edge,a3,a4,s2chk).  Default: env,build,edge,a3,a4
#                           (s2chk is OPT-IN: it runs only when named here, never by default).
#   --skip LIST             run all stages except these (comma list).  --only and --skip combine (skip wins
#                           on overlap).
#   --nodes-a4 LIST         node counts for stage a4, comma list (default: 2,8,64; 2,8 with --site aac7).
#                           A count bigger than the allocation is skipped, not failed.
#   --nics1 LIST            COMM_OFI_NICS form for "1 NIC per APU" (default: "0;1;2;3")
#   --nics2 LIST            COMM_OFI_NICS form for "2 NICs per APU, the target's form" (default per 07_COMM_OFI.md:
#                           "0,4;1,5;2,6;3,7")
#   --nodes-s2chk LIST      node counts for the opt-in stage s2chk (default: 2,8 = the smallest multi-node count a4 uses
#                           plus its larger one).  A count bigger than the allocation is skipped, not failed.
#   --s2chk-per-node N      digits per node for s2chk (default 64410000000 = 6.441e10, the largest node share of the
#                           3.71e13 target on 576 nodes).  KIT_S2CHK_LINE overrides the launch-line env words.
#   --timeout-STAGE SECS    per-stage timeout (env 90, build 900, edge 600, a3 600, a4 1800 PER node count, s2chk 1800 PER node count).
#                           A stage that times out or fails is marked FAIL in the summary; every other stage
#                           still runs.
#
# What each stage does, and about how long it takes (wall clock, once the allocation is live):
#   env    (<1 min, no GPU, 1 node of the allocation probed):
#            hostname list, Slurm job geometry, ROCm version, module list, fi_info -p cxi domain count.
#   build  (3-10 min, login node or any node with the toolchain, no GPU needed):
#            `make SHMEM_CRAY=1` for ecalc, tests/t_comm, tests/t_edge, tools/.  ROCm version is unknown ahead
#            of time on the target, so this tries candidates in order and keeps the first that links: just
#            $TARGET_ROCM if set, else rocm/7.2.4, rocm/7.2.3, rocm/7.0.3 (whichever are available), then any
#            other rocm/<version> module newest first (picking blindly-newest once chose rocm/7.14.0 on aac7,
#            which fails to link -- results/V16.md).  SMA/DSMML modules auto-detect as before; override any of
#            the three with TARGET_ROCM / TARGET_SMA_MODULE / TARGET_DSMML_MODULE / GMP_HOME.  The chosen set is
#            written to $OUT/kit_modules.env; every later stage sources it and loads the same modules inside its
#            own srun (not whatever the caller's shell happened to have), and records the ROCm actually seen on
#            the compute node.
#   edge   (5-10 min, 1 node, ALONE -- run first, before a3/a4 share the allocation):
#            tests/t_edge dev then vmm (the device-memory edge, hipMalloc and VMM forms); then one e 1e9 run
#            with MEM_REPORT_DEVS=1 (host RSS, per-APU device in-use vs driver-used, the VMM arena map rate
#            per APU and per node); then one e 1e10 run for the single-node wall.  t_edge runs to
#            completion and exits before anything else touches these GPUs.
#   a3     (3-5 min, 2 nodes):
#            tests/t_comm --bw with comm_ofi, 1 NIC per APU vs 2 NICs per APU (COMM_OFI_NICS), GB/s per node.
#   a4     (5-10 min PER node count, 2/8/64 nodes, skips what the allocation can't hold):
#            ecalc at 1e10 digits/node with MN_COMM_MARK=1, VERIFY, wall, the comm-mark fabric rates.
#   s2chk  (OPT-IN, off unless named in --only; about 5-15 min PER node count, default 2 and 8 nodes; needs stage build's
#           kit_modules.env, so run `--only build,s2chk` or point --out at a directory where build already ran):
#            the TARGET per-node share (6.441e10 digits/node) on the target launch line (docs/TARGET.md 4 without the
#            576-only MN_GROUPS/POOL words; DM_MN_LEAN=1, MN_T_CHUNK_MB=1024) with ECALC_INIT_TL=1 MN_WAIT_STATS=1,
#            nothing written.  Saves the full logs (logs/05_s2chk_n<N>.log) and prints a verdict helper:
#             A37-R1 (GPU seed): "seed thread ends" vs the last "background mapping done" (tl lines, seconds since
#               rns_init).  Build the GPU seed ONLY if the seed ends later than about 18 s AND more than 2 s after the
#               last mapping; seed late but mapping within 2 s = mapping binds (marginal, no build); else NO-GO.
#             S-2 (division overlap): MN_WAIT_STATS "dm" (= reciprocal + division) wait, per APU thread (node sum / 4),
#               max node; reciprocal and division split; spread max-min (the skew bound).  Rule: build only if the
#               exposed wait is above about 30 s.  CAVEAT printed with it: the literal wait includes the transfer itself
#               (results/S22.md 2.3), and the 576-node value is modelled at 34 s; judge the 8-node figure against
#               S22's aac7 table (n=2: recip 10 + division 32; n=8: 22 + 73 s per thread).
#            Needs an ecalc built WITH MN_WAIT_STATS (the instrument is not in every branch; the stage says so if no
#            wait-stats line appears).  The command for the target team:
#              ./target_kit.sh --jobid $SLURM_JOB_ID --out kit_out_s2chk --only build,s2chk
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
TO_S2=1800
NODES_S2="2,8"
S2_PER_NODE=64410000000
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
        --timeout-s2chk) TO_S2=$2; shift 2 ;;
        --nodes-s2chk) NODES_S2=$2; shift 2 ;;
        --s2chk-per-node) S2_PER_NODE=$2; shift 2 ;;
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

stage_optin() {  # stage_optin <name> -- an opt-in stage: runs ONLY when named in --only (and not in --skip)
    local n=$1
    [ -n "$ONLY" ] || return 1
    case ",$ONLY," in *",$n,"*) ;; *) return 1 ;; esac
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

# ---- module handling: the chosen stack is decided once (stage_build), recorded in $OUT/kit_modules.env, and
# every later run stage (edge, a3, a4 -- not env, which records the target's as-found default) sources it so the
# srun'd binaries run under the same ROCm/SHMEM the build used, not whatever the caller's login shell happens to have.
KIT_MODULES_ENV="$OUT/kit_modules.env"

require_kit_modules() {   # require_kit_modules <stage> <logfile> -- sources $KIT_MODULES_ENV; 1 (stage must FAIL) if missing
    local stage=$1 lf=$2
    if [ -f "$KIT_MODULES_ENV" ]; then
        # shellcheck disable=SC1090
        . "$KIT_MODULES_ENV"
        echo "== kit_modules.env: TARGET_ROCM=$TARGET_ROCM TARGET_SMA_MODULE=$TARGET_SMA_MODULE TARGET_DSMML_MODULE=$TARGET_DSMML_MODULE TARGET_ROCM_UNLOAD=${TARGET_ROCM_UNLOAD:-}" >> "$lf"
        return 0
    fi
    echo "target_kit.sh: stage $stage: $KIT_MODULES_ENV not found -- run stage 'build' first (it writes the chosen module set) or point --out at a directory where build already ran" >> "$lf"
    return 1
}

module_preamble() {   # the module unload/load lines to prepend inside a `bash -lc "$(module_preamble)"'...'` srun stage
    # NB: ends with "; " (not a bare newline) -- $(...) strips trailing newlines, which would otherwise glue this
    # straight onto the next literal word with no separator.
    printf 'for __m in %s; do module unload "$__m" >/dev/null 2>&1; done; module load %s %s %s >/dev/null 2>&1; ' \
        "${TARGET_ROCM_UNLOAD:-}" "$TARGET_DSMML_MODULE" "$TARGET_SMA_MODULE" "$TARGET_ROCM"
}

fi_defaults_for() {   # fi_defaults_for <ntasks> -- exports FI_UNIVERSE_SIZE / FI_LOG_LEVEL if unset, per TGTBENCH2
    local ntasks="$1"
    [ "${MNRUN_FI_DEFAULTS:-1}" = 0 ] && return 0   # the same opt-out as mnrun.sh: leave both unset
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
        if [ -z "$TARGET_SMA_MODULE" ]; then TARGET_SMA_MODULE=$(module -t avail cray-openshmemx 2>&1 | grep -oE '^cray-openshmemx/[0-9][0-9.]*' | sort -V | tail -1); fi
        TARGET_SMA_MODULE=${TARGET_SMA_MODULE:-cray-openshmemx}
        if [ -z "$TARGET_DSMML_MODULE" ]; then TARGET_DSMML_MODULE=$(module -t avail cray-dsmml 2>&1 | grep -oE '^cray-dsmml/[0-9][0-9.]*' | sort -V | tail -1); fi
        TARGET_DSMML_MODULE=${TARGET_DSMML_MODULE:-cray-dsmml}

        # ROCm choice (Phase S18 fix): picking the newest rocm/x.y.z (sort -V | tail -1) used to pick rocm/7.14.0 on
        # aac7, which FAILS to link ecalc (results/V16.md: 7.12/7.13/7.14 and rocm-new/10.0.0 all fail the same
        # -fPIC device-link error; 7.2.4 is the newest that links).  The target's ROCm is unknown ahead of time, so
        # try candidates in order and keep the first that actually builds: $TARGET_ROCM alone if the caller set it;
        # else the validated list (rocm/7.2.4, rocm/7.2.3, rocm/7.0.3) in that order, filtered to what's available,
        # then any other rocm/<version> module, newest first.
        default_rocm=$(module -t list 2>&1 | grep -oE '^rocm/[0-9][0-9.]*|^rocm$' | tr '\n' ' ')
        local -a rocm_candidates=()
        if [ -n "$TARGET_ROCM" ]; then
            rocm_candidates=("$TARGET_ROCM")
        else
            avail=$(module -t avail rocm 2>&1 | grep -oE '^rocm/[0-9][0-9.]*' | sort -Vu)
            local -a validated=(rocm/7.2.4 rocm/7.2.3 rocm/7.0.3)
            for v in "${validated[@]}"; do
                echo "$avail" | grep -qx "$v" && rocm_candidates+=("$v")
            done
            for o in $(echo "$avail" | sort -Vr); do
                already=0
                for v in "${rocm_candidates[@]}"; do [ "$o" = "$v" ] && already=1 && break; done
                [ $already -eq 0 ] && rocm_candidates+=("$o")
            done
        fi
        [ ${#rocm_candidates[@]} -eq 0 ] && rocm_candidates=(rocm)
        echo "TARGET_SMA_MODULE=$TARGET_SMA_MODULE TARGET_DSMML_MODULE=$TARGET_DSMML_MODULE GMP_HOME=${GMP_HOME:-<system>} default_rocm(unload list)=${default_rocm:-<none>}" >> "$lf"
        echo "ROCm candidates (in try order): ${rocm_candidates[*]}" | tee -a "$lf"

        local mk_extra=(SHMEM_CRAY=1)
        [ -n "$GMP_HOME" ] && mk_extra+=("GMP_HOME=$GMP_HOME")

        unload_load_cmd() {   # unload_load_cmd <rocm-candidate> -- the module unload/load line, as plain text (no nested quoting)
            printf 'for x in %s; do module unload "$x" >/dev/null 2>&1; done; module load %s %s %s' \
                "$default_rocm" "$TARGET_DSMML_MODULE" "$TARGET_SMA_MODULE" "$1"
        }

        local -a tried=() failed=()
        chosen=""
        for cand in "${rocm_candidates[@]}"; do
            tried+=("$cand")
            echo "== trying ROCm candidate: $cand" >> "$lf"
            if [ ${#tried[@]} -gt 1 ]; then
                exec_cmd "$lf" "$TO_BUILD" bash -lc "cd '$SELF_DIR' && make clean"
            fi
            ul=$(unload_load_cmd "$cand")
            exec_cmd "$lf" "$TO_BUILD" bash -lc "$ul; module list 2>&1"
            exec_cmd "$lf" "$TO_BUILD" bash -lc "$ul >/dev/null 2>&1; cd '$SELF_DIR' && make ${mk_extra[*]} -j${MAKE_JOBS} ecalc tests/t_comm tools"
            rc1=$?
            exec_cmd "$lf" "$TO_BUILD" bash -lc "$ul >/dev/null 2>&1; cd '$SELF_DIR' && make ${mk_extra[*]} -j${MAKE_JOBS} tests/t_edge"
            rc2=$?
            if [ "$DRY_RUN" = 1 ]; then
                chosen="$cand"
                echo "[dry-run] assuming the first candidate ($chosen) builds; the rest are not tried" | tee -a "$lf"
                break
            fi
            if [ $rc1 -eq 0 ] && [ $rc2 -eq 0 ] && [ -x "$SELF_DIR/ecalc" ] && [ -x "$SELF_DIR/tests/t_comm" ] && [ -x "$SELF_DIR/tests/t_edge" ]; then
                chosen="$cand"
                echo "== ROCm candidate $cand: build OK (ecalc, tests/t_comm, tests/t_edge, tools)" >> "$lf"
                break
            else
                failed+=("$cand")
                echo "== ROCm candidate $cand: FAILED (rc1=$rc1 rc2=$rc2 or a missing binary) -- trying the next candidate" >> "$lf"
            fi
        done

        if [ -n "$chosen" ]; then
            {
                echo "TARGET_ROCM=$chosen"
                echo "TARGET_SMA_MODULE=$TARGET_SMA_MODULE"
                echo "TARGET_DSMML_MODULE=$TARGET_DSMML_MODULE"
                echo "TARGET_ROCM_UNLOAD=\"$default_rocm\""
            } > "$KIT_MODULES_ENV"
            summary "  ROCm used: $chosen$([ ${#failed[@]} -gt 0 ] && echo "  (failed first: ${failed[*]})")"
        else
            summary "  ROCm used: none -- every candidate failed (tried: ${tried[*]})"
        fi
        [ -n "$chosen" ]
    )
    local rc=$?
    if [ "$DRY_RUN" = 1 ]; then summary "status: DRY-RUN (not executed)";
    elif [ $rc -eq 0 ] && [ -x "$SELF_DIR/ecalc" ] && [ -x "$SELF_DIR/tests/t_comm" ] && [ -x "$SELF_DIR/tests/t_edge" ]; then
        summary "status: PASS (ecalc, tests/t_comm, tests/t_edge built)"
    else
        summary "status: FAIL (see log -- no ROCm candidate linked, or a missing binary)"
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
    if ! require_kit_modules edge "$lf"; then summary "status: FAIL ($KIT_MODULES_ENV missing -- run stage build first)"; summary ""; return; fi

    local pre; pre=$(module_preamble)
    local rc=0
    echo "== t_edge dev (hipMalloc), alone on the node, before anything else" >> "$lf"
    exec_cmd "$lf" "$TO_EDGE" srun --jobid="$JOBID" -N1 -n1 --ntasks-per-node=1 --gpus-per-node=4 --overlap \
        bash -lc "$pre"'echo "== runtime ROCm (compute node): $(hipconfig --version 2>&1)"; exec "$@"' _ \
        "$SELF_DIR/tests/t_edge" dev "$EDGE_STEP_GB" "$EDGE_CAP_GB" || rc=1
    echo "== t_edge vmm (hipMemCreate), alone on the node" >> "$lf"
    exec_cmd "$lf" "$TO_EDGE" srun --jobid="$JOBID" -N1 -n1 --ntasks-per-node=1 --gpus-per-node=4 --overlap \
        bash -lc "$pre"'echo "== runtime ROCm (compute node): $(hipconfig --version 2>&1)"; exec "$@"' _ \
        "$SELF_DIR/tests/t_edge" vmm "$EDGE_STEP_GB" "$EDGE_CAP_GB" || rc=1

    # Phase S18 kit fix (results/S18KITFIX.md, bug 3): dbig.c's "VMM arena ... s/GB" line (the only place a map
    # rate is printed) is gated by vmm_vb(), which is off unless DB_POOL_VERBOSE (or RNS_VERBOSE) is set -- so
    # this run used to print the "report both forms" text with no number behind it.  DB_POOL_VERBOSE=1 turns
    # on just that line (narrower than RNS_VERBOSE, which adds a lot of unrelated chatter).
    echo "== e $E9 (1e9), MEM_REPORT_DEVS=1 ECALC_VERBOSE=2 DB_POOL_VERBOSE=1 -- the init footprint and VMM arena map rate" >> "$lf"
    exec_cmd "$lf" "$TO_EDGE" srun --jobid="$JOBID" -N1 -n1 --ntasks-per-node=1 --gpus-per-node=4 --overlap \
        bash -lc "$pre"'echo "== runtime ROCm (compute node): $(hipconfig --version 2>&1)"; exec "$@"' _ \
        env MEM_REPORT_DEVS=1 ECALC_VERBOSE=2 DB_POOL_VERBOSE=1 "$SELF_DIR/ecalc" "$E9" "$TMPDIR/e9.out" || rc=1
    [ "$DRY_RUN" != 1 ] && rm -f "$TMPDIR"/e9.out*

    echo "== e $E10 (1e10) -- the single-node wall" >> "$lf"
    exec_cmd "$lf" "$TO_EDGE" srun --jobid="$JOBID" -N1 -n1 --ntasks-per-node=1 --gpus-per-node=4 --overlap \
        bash -lc "$pre"'echo "== runtime ROCm (compute node): $(hipconfig --version 2>&1)"; exec "$@"' _ \
        "$SELF_DIR/ecalc" "$E10" "$TMPDIR/e10.out" || rc=1
    [ "$DRY_RUN" != 1 ] && rm -f "$TMPDIR"/e10.out*

    if [ "$DRY_RUN" = 1 ]; then summary "status: DRY-RUN (not executed)";
    else summary "status: $([ $rc -eq 0 ] && echo PASS || echo 'FAIL/PARTIAL (see log)')"; fi
    if [ "$DRY_RUN" != 1 ]; then
        local l
        l=$(grep -m1 "runtime ROCm" "$lf"); [ -n "$l" ] && summary "  $l (kit chose: $TARGET_ROCM)"
        l=$(grep -m1 "edge in form dev" "$lf"); [ -n "$l" ] && summary "  device edge (hipMalloc, m target): $l"
        l=$(grep -m1 "edge in form vmm" "$lf"); [ -n "$l" ] && summary "  device edge (VMM, m target): $l"
        l=$(grep -m1 "VmRSS .* GB after init" "$lf"); [ -n "$l" ] && summary "  host RSS at init (m target): $l"
        l=$(grep -m1 "VmHWM" "$lf" | grep "^total"); [ -n "$l" ] && summary "  host RSS peak / single-node total (m target): $l"
        grep "\[init\] device .* GB in use" "$lf" | head -1 | sed 's/^/  per-APU device in-use (m target): /' >> "$SUMMARY"
        grep "APU[0-9]*: in use .* (driver:" "$lf" | sed 's/^/  per-APU in-use vs driver-used (m target): /' >> "$SUMMARY"
        grep "dbig pool: APU[0-9]* VMM arena" "$lf" | sed 's/^/  per-APU VMM arena map rate (measured): /' >> "$SUMMARY"
        # Phase S18 kit fix (bug 3): compute the map rate instead of leaving a text-only note. Each
        # "dbig pool: APU<n> VMM arena <GB> GB = ... the first <m> mapped in <s> s (<rate> s/GB) ..." line
        # (dbig.c:327) already gives one APU's own GB mapped, seconds, and s/GB; TGTBENCH2 (Q5) never settled
        # whether the target figure should be "per APU-GB" (one APU's own GB) or "per node-GB" (the node's GB
        # mapped in the node's time), so both are computed here, labelled measured:
        #   per APU-GB form: the mean of the per-APU s/GB values above (each APU's own rate)
        #   per node-GB form: sum(seconds) / sum(GB) across the node's APUs (their GB mapped, summed; their
        #     seconds, summed -- a naive serialized-equivalent total, NOT the parallel wall-clock time)
        grep -oE 'dbig pool: APU[0-9]+ VMM arena [0-9.]+ GB.*mapped in [0-9.]+ s \([0-9.]+ s/GB\)' "$lf" | \
            sed -E 's/dbig pool: APU([0-9]+) VMM arena ([0-9.]+) GB.*mapped in ([0-9.]+) s \(([0-9.]+) s\/GB\).*/\1 \2 \3 \4/' | \
            awk '
                { tgb += $2; tsec += $3; trate += $4; n++ }
                END {
                    if (n > 0) {
                        printf "  per-node VMM map rate (measured, per APU-GB form): %.4f s/GB (mean of %d per-APU rates above)\n", trate / n, n
                        printf "  per-node VMM map rate (measured, per node-GB form): %.4f s/GB (%.2f GB total / %.2f s summed per-APU time across %d APUs -- a serialized-equivalent sum, not wall-clock)\n", (tgb > 0 ? tsec / tgb : 0), tgb, tsec, n
                    } else {
                        print "  per-node VMM map rate: no \"dbig pool: APU<n> VMM arena\" lines found (DB_POOL_VERBOSE=1 did not produce them -- see log)"
                    }
                }
            ' >> "$SUMMARY"
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
    if ! require_kit_modules a3 "$lf"; then summary "status: FAIL ($KIT_MODULES_ENV missing -- run stage build first)"; summary ""; return; fi
    local alloc; alloc=$(alloc_node_count)
    if [ "$DRY_RUN" != 1 ] && [ "$alloc" -gt 0 ] && [ "$alloc" -lt 2 ]; then
        summary "status: SKIPPED (allocation has $alloc node(s); a3 needs 2)"; summary ""; return
    fi

    fi_defaults_for 2
    # Phase S18 kit fix (results/S18KITFIX.md, bug 1): bare srun left the SHMEM heap at its library default,
    # far below t_comm --bw's own symmetric-buffer request at A3_MAXMB -- every launch failed with
    # "comm_shmem: pe N: shmem_malloc of 8192 MiB failed: the SHMEM heap ... must be >= 8704 MiB (the pool +
    # 512)".  t_comm fits mnrun.sh's <procs> <command...> interface exactly (as results/OFI17.md ยง3's and
    # results/TUNE17.md's own bw() driver functions launch it), so launch through mnrun.sh instead and give it
    # the pool t_comm's --bw actually needs, from the same formula those drivers used: t_comm allocates 2
    # symmetric buffers (sb, rb) per thread, each A3_MAXMB MiB x the node count (the all-to-all peer count);
    # with A3_THREADS threads that is 2 x A3_THREADS x A3_MAXMB x nodes MiB (OFI17.md/TUNE17.md's
    # "8 * g * mx + 512" at their default 4 threads).  mnrun.sh then sizes SHMEM_SYMMETRIC_SIZE /
    # XT_SYMMETRIC_HEAP_SIZE itself at pool + 512 MiB; COMM_OFI_POOL_MB sizes comm_ofi's device staging pool
    # the same way OFI17/TUNE17 size it (2 x nodes x A3_MAXMB + 256).
    local g=2
    local pool=$((2 * A3_THREADS * A3_MAXMB * g + 512))
    local ofipool=$((2 * g * A3_MAXMB + 256))
    export MNRUN_MODULES="${MNRUN_MODULES:-$TARGET_DSMML_MODULE $TARGET_SMA_MODULE $TARGET_ROCM}"
    export MNRUN_UNLOAD="${MNRUN_UNLOAD-${TARGET_ROCM_UNLOAD:-}}"
    local rc=0
    echo "== 1 NIC per APU: COMM_OFI_NICS=$NICS1 (COMM_SHMEM_POOL_MB=$pool COMM_OFI_POOL_MB=$ofipool via mnrun.sh)" >> "$lf"
    exec_cmd "$lf" "$TO_A3" env SLURM_JOB_ID="$JOBID" MNRUN_NODES="$g" FI_UNIVERSE_SIZE="$FI_UNIVERSE_SIZE" FI_LOG_LEVEL="$FI_LOG_LEVEL" \
        MNRUN_MODULES="$MNRUN_MODULES" MNRUN_UNLOAD="$MNRUN_UNLOAD" COMM_SHMEM_POOL_MB="$pool" COMM_OFI_POOL_MB="$ofipool" \
        "$SELF_DIR/mnrun.sh" "$g" env COMM_TRANSPORT=shmem COMM_OFI=1 COMM_OFI_NICS="$NICS1" \
        "$SELF_DIR/tests/t_comm" --bw "$A3_THREADS" "$A3_REPS" "$A3_MAXMB" || rc=1
    echo "== 2 NICs per APU (the target's form): COMM_OFI_NICS=$NICS2 (COMM_SHMEM_POOL_MB=$pool COMM_OFI_POOL_MB=$ofipool via mnrun.sh)" >> "$lf"
    exec_cmd "$lf" "$TO_A3" env SLURM_JOB_ID="$JOBID" MNRUN_NODES="$g" FI_UNIVERSE_SIZE="$FI_UNIVERSE_SIZE" FI_LOG_LEVEL="$FI_LOG_LEVEL" \
        MNRUN_MODULES="$MNRUN_MODULES" MNRUN_UNLOAD="$MNRUN_UNLOAD" COMM_SHMEM_POOL_MB="$pool" COMM_OFI_POOL_MB="$ofipool" \
        "$SELF_DIR/mnrun.sh" "$g" env COMM_TRANSPORT=shmem COMM_OFI=1 COMM_OFI_NICS="$NICS2" \
        "$SELF_DIR/tests/t_comm" --bw "$A3_THREADS" "$A3_REPS" "$A3_MAXMB" || rc=1

    if [ "$DRY_RUN" = 1 ]; then summary "status: DRY-RUN (not executed)";
    else summary "status: $([ $rc -eq 0 ] && echo PASS || echo 'FAIL (see log)')"; fi
    if [ "$DRY_RUN" != 1 ]; then
        summary "  FI_UNIVERSE_SIZE=$FI_UNIVERSE_SIZE FI_LOG_LEVEL=$FI_LOG_LEVEL COMM_SHMEM_POOL_MB=$pool COMM_OFI_POOL_MB=$ofipool (kit chose ROCm: $TARGET_ROCM)"
        grep "t_comm bw:" "$lf" | sed 's/^/  /' >> "$SUMMARY"
        grep -E ": pe [0-9]+ .*B per slab: .*aggregate [0-9.]+ GB/s" "$lf" | sed 's/^/  per-node (1 PE\/node) /' >> "$SUMMARY"
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
    if ! require_kit_modules a4 "$lf"; then summary "status: FAIL ($KIT_MODULES_ENV missing -- run stage build first)"; summary ""; return; fi
    local alloc; alloc=$(alloc_node_count)
    local any_ran=0 any_fail=0
    local n
    local pre; pre=$(module_preamble)
    # mnrun.sh's own module-loading convention (its header comments): MNRUN_MODULES / MNRUN_UNLOAD, exported here from
    # the build's chosen stack so the ecalc that mnrun.sh launches runs under the same ROCm/SHMEM as the build (a caller
    # override of MNRUN_MODULES/MNRUN_UNLOAD, if already set in the environment, is kept).
    export MNRUN_MODULES="${MNRUN_MODULES:-$TARGET_DSMML_MODULE $TARGET_SMA_MODULE $TARGET_ROCM}"
    export MNRUN_UNLOAD="${MNRUN_UNLOAD-${TARGET_ROCM_UNLOAD:-}}"
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
        # Phase S18 kit fix (results/S18KITFIX.md, bug 2): the ecalc run's own output used to go straight into the
        # shared, appended "$lf" -- by the time a later n's block was parsed, grep/tail over that same accumulated
        # file picked up every earlier n's "comm-mark"/"total"/VERIFY lines too (the n=8 block repeated n=2's
        # lines ahead of its own).  Give each n its own log file and parse only that file for this n's summary
        # lines; the per-n log is still appended into the combined "$lf" afterward so the full log on disk is
        # unchanged for a human reading it end to end.
        local nlf="$LOGDIR/04_a4_n${n}.log"; : > "$nlf"
        fi_defaults_for "$n"
        echo "== n=$n nodes, $digits digits total ($E10 per node), MN_COMM_MARK=1, MNRUN_MODULES=$MNRUN_MODULES MNRUN_UNLOAD=$MNRUN_UNLOAD" >> "$lf"
        exec_cmd "$lf" "$TO_ENV" srun --jobid="$JOBID" -N1 -n1 --overlap bash -lc "$pre"'echo "== runtime ROCm (compute node, n='"$n"'): $(hipconfig --version 2>&1)"'
        exec_cmd "$nlf" "$TO_A4" env SLURM_JOB_ID="$JOBID" MNRUN_NODES="$n" FI_UNIVERSE_SIZE="$FI_UNIVERSE_SIZE" FI_LOG_LEVEL="$FI_LOG_LEVEL" \
            MNRUN_MODULES="$MNRUN_MODULES" MNRUN_UNLOAD="$MNRUN_UNLOAD" \
            "$SELF_DIR/mnrun.sh" "$n" env COMM_TRANSPORT=shmem MN_COMM_MARK=1 ECALC_VERBOSE=2 "$SELF_DIR/ecalc" "$digits" "$out"
        local rc=$?
        cat "$nlf" >> "$lf"
        [ $rc -ne 0 ] && any_fail=1
        if [ "$DRY_RUN" != 1 ]; then
            summary "  n=$n: $( [ $rc -eq 0 ] && echo PASS || echo FAIL ) (exit $rc)"
            grep -m1 "runtime ROCm.*n=$n" "$lf" | sed 's/^/    /' >> "$SUMMARY"
            grep -E "^comm-mark|^total|VERIFY" "$nlf" | tail -40 | sed 's/^/    /' >> "$SUMMARY"
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
# Stage: s2chk (OPT-IN) -- the target's S-2 (division overlap) and A37-R1 (GPU seed) gates, one run per node count
# =============================================================================================================
# The gates (results/S22.md sections 2 and 3; TASKS.md S-2 / A37-R1, both SHELVED by the user 2026-10-08, kept as options):
#   A37-R1: build the GPU seed only if the seed thread ends later than ~18 s AND > 2 s after the last background mapping.
#   S-2:    build the division overlap only if the exposed division/reciprocal wait is above ~30 s per APU thread.
# The greps are the ones the S22/S23 drivers used: `tl` lines from ECALC_INIT_TL=1 ("seed thread ends (..)", "APU<d>
# background mapping done: ..") and the `wait-stats node R:` lines of MN_WAIT_STATS=1 (per phase: bs | dm | recip | other;
# dm contains recip; node sums over the 4 APU threads, so per thread = /4).
S2_SEED_LATE=18        # s: A37-R1 trigger (seed end)
S2_SEED_AFTER=2        # s: A37-R1 trigger (seed end minus the last mapping)
S2_WAIT_LIMIT=30       # s per APU thread: S-2 trigger
S2_LINE_DEFAULT="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 DM_MN_LEAN=1 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1"

s2chk_verdict() {   # s2chk_verdict <log> <n> -- appends the A37-R1 / S-2 verdict lines for one run to the summary
    local f=$1 n=$2
    local seed map
    seed=$(grep -a 'seed thread ends' "$f" | sed -nE 's/.*tl +([0-9.]+) +seed thread ends.*/\1/p')
    map=$(grep -a 'background mapping done' "$f" | sed -nE 's/.*tl +([0-9.]+) +APU[0-9]+ background mapping done.*/\1/p')
    if [ -z "$seed" ] || [ -z "$map" ]; then
        summary "    A37-R1: no 'seed thread ends' / 'background mapping done' tl lines (ECALC_INIT_TL=1 not effective, or the run failed before init ended) -- see $f"
    else
        { echo "$seed" | sed 's/^/S /'; echo "$map" | sed 's/^/M /'; } | awk -v late="$S2_SEED_LATE" -v aft="$S2_SEED_AFTER" -v n="$n" '
            $1=="S" { ns++; ss+=$2; if (ns==1||$2>smax) smax=$2; if (ns==1||$2<smin) smin=$2 }
            $1=="M" { nm++; if (nm==1||$2>mmax) mmax=$2 }
            END {
                d = smax - mmax
                printf "    A37-R1 (n=%d): seed thread ends %.2f s max (%.2f - %.2f over %d lines, mean %.2f); last background mapping %.2f s (%d lines); seed end - last mapping = %+.2f s\n", n, smax, smin, smax, ns, ss/ns, mmax, nm, d
                if (smax > late && d > aft) v = "BUILD-CANDIDATE (seed ends > " late " s and > " aft " s after the mapping: the seed binds init)"
                else if (smax > late) v = "MARGINAL (seed ends > " late " s but the mapping is within " aft " s: the mapping binds, a faster seed gains little) -- no build unless the user decides otherwise"
                else v = "NO-GO (seed ends <= " late " s; init is not seed-bound)"
                printf "    A37-R1 verdict (n=%d): %s\n", n, v
            }' >> "$SUMMARY"
    fi
    local ws; ws=$(grep -a 'wait-stats node [0-9]*:' "$f")
    if [ -z "$ws" ]; then
        summary "    S-2: no 'wait-stats node' lines -- this ecalc build lacks MN_WAIT_STATS (the instrument lives on branch s22 / d3-wait-stats; merge it into the build first) or the run failed -- see $f"
    else
        echo "$ws" | awk -v lim="$S2_WAIT_LIMIT" -v n="$n" '
            { d=-1; r=-1
              if (match($0, /[|] dm barrier [0-9.]+ s [(][0-9]+[)] wait [0-9.]+/)) { t=substr($0,RSTART,RLENGTH); sub(/.* wait /,"",t); d=t+0 }
              if (match($0, /[|] recip barrier [0-9.]+ s [(][0-9]+[)] wait [0-9.]+/)) { t=substr($0,RSTART,RLENGTH); sub(/.* wait /,"",t); r=t+0 }
              if (d < 0) next
              k++; if (k==1||d>dmax) { dmax=d; rmax=r } if (k==1||d<dmin) dmin=d
              dv = d - r; if (k==1||dv>vmax) vmax=dv }
            END {
                if (k==0) { printf "    S-2 (n=%d): wait-stats lines found but no dm field parsed\n", n; exit }
                printf "    S-2 (n=%d, %d nodes, per APU thread = node sum / 4): dm (reciprocal + division) wait %.1f s max node (min %.1f; spread/skew bound %.1f s); reciprocal %.1f s; division %.1f s (max node)\n", n, k, dmax/4, dmin/4, (dmax-dmin)/4, rmax/4, vmax/4
                printf "    S-2 verdict (n=%d): exposed dm wait %.1f s per thread is %s the %d s rule -- CAVEAT: the literal wait includes the transfer (S22 2.3); S22 aac7 reference per thread: n=2 recip 10 + division 32, n=8 recip 22 + division 73; model at 576 nodes: 34 s exposed, ceiling 25-34 s, realistic gain 6-17 s\n", n, dmax/4, (dmax/4 > lim ? "ABOVE" : "below"), lim
            }' >> "$SUMMARY"
    fi
}

stage_s2chk() {
    local lf="$LOGDIR/05_s2chk.log"; : > "$lf"
    summary "## Stage s2chk (opt-in): A37-R1 seed gate + S-2 wait gate, $S2_PER_NODE digits/node, ECALC_INIT_TL=1 MN_WAIT_STATS=1, node counts: $NODES_S2"
    if [ -z "$JOBID" ] && [ "$DRY_RUN" != 1 ]; then summary "status: FAIL (no --jobid / SLURM_JOB_ID)"; summary ""; return; fi
    if ! require_kit_modules s2chk "$lf"; then summary "status: FAIL ($KIT_MODULES_ENV missing -- run stage build first, e.g. --only build,s2chk)"; summary ""; return; fi
    local alloc; alloc=$(alloc_node_count)
    local any_ran=0 any_fail=0 n
    export MNRUN_MODULES="${MNRUN_MODULES:-$TARGET_DSMML_MODULE $TARGET_SMA_MODULE $TARGET_ROCM}"
    export MNRUN_UNLOAD="${MNRUN_UNLOAD-${TARGET_ROCM_UNLOAD:-}}"
    local line="${KIT_S2CHK_LINE:-$S2_LINE_DEFAULT}"
    summary "  launch line: $line ECALC_INIT_TL=1 MN_WAIT_STATS=1 (nothing written; no MN_GROUPS / pool words: mnrun.sh sizes the pool from MN_PLAN_ONLY)"
    IFS=',' read -ra counts <<< "$NODES_S2"
    for n in "${counts[@]}"; do
        if [ "$DRY_RUN" != 1 ] && [ "$alloc" -gt 0 ] && [ "$n" -gt "$alloc" ]; then
            echo "== n=$n: SKIPPED (allocation has $alloc node(s))" >> "$lf"
            summary "  n=$n: SKIPPED (allocation has $alloc node(s))"
            continue
        fi
        any_ran=1
        local digits=$((n * S2_PER_NODE))
        local nlf="$LOGDIR/05_s2chk_n${n}.log"; : > "$nlf"
        fi_defaults_for "$n"
        echo "== n=$n nodes, $digits digits total ($S2_PER_NODE per node), ECALC_INIT_TL=1 MN_WAIT_STATS=1" >> "$lf"
        # $line is deliberately unquoted: it is a list of VAR=value words for env(1)
        # shellcheck disable=SC2086
        exec_cmd "$nlf" "$TO_S2" env SLURM_JOB_ID="$JOBID" MNRUN_NODES="$n" FI_UNIVERSE_SIZE="$FI_UNIVERSE_SIZE" FI_LOG_LEVEL="$FI_LOG_LEVEL" \
            MNRUN_MODULES="$MNRUN_MODULES" MNRUN_UNLOAD="$MNRUN_UNLOAD" \
            "$SELF_DIR/mnrun.sh" "$n" env $line ECALC_INIT_TL=1 MN_WAIT_STATS=1 "$SELF_DIR/ecalc" "$digits"
        local rc=$?
        cat "$nlf" >> "$lf"
        [ $rc -ne 0 ] && any_fail=1
        if [ "$DRY_RUN" != 1 ]; then
            summary "  n=$n: $( [ $rc -eq 0 ] && echo PASS || echo FAIL ) (exit $rc); log $nlf"
            grep -aE "^total|VERIFY" "$nlf" | tail -3 | cut -c1-300 | sed 's/^/    /' >> "$SUMMARY"
            grep -a 'wait-stats max/min' "$nlf" | head -1 | cut -c1-600 | sed 's/^/    /' >> "$SUMMARY"
            s2chk_verdict "$nlf" "$n"
        else
            summary "  n=$n: DRY-RUN (not executed)"
        fi
    done
    if [ "$any_ran" = 0 ]; then summary "status: SKIPPED (no requested node count fits the allocation)";
    elif [ "$any_fail" = 0 ] || [ "$DRY_RUN" = 1 ]; then summary "status: $([ "$DRY_RUN" = 1 ] && echo DRY-RUN || echo PASS)";
    else summary "status: PARTIAL (see per-n lines above)"; fi
    summary "  full logs: $LOGDIR/05_s2chk_n<N>.log (kept; also appended to $lf)"
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
stage_optin s2chk   && stage_s2chk

summary "## Done"
summary "Send back: $SUMMARY"
echo "target_kit.sh: wrote $SUMMARY"

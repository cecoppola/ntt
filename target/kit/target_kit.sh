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
#   --out DIR               where KIT_SUMMARY.txt and logs/ go (default: target/kit/kit_out_<timestamp>)
#   --jobid ID              the Slurm allocation (default: $SLURM_JOB_ID).  Required by every stage that
#                           runs srun, i.e. every stage except --dry-run.
#   --only LIST             run only these stages (comma list of: env,build,uffd,plan,edge,a3,a4,initchk,s2chk,nodechk, or the
#                           shortcut `preflight` = env,build,uffd,plan,edge,nodechk).  Default: env,build,uffd,plan,edge,a3,a4,initchk
#                           (s2chk and nodechk are OPT-IN: they run only when named here, never by default).
#   --exclude-hosts LIST    comma list of hosts never to use (slow or failed nodes).  Passed to every multi-node run as
#                           MNRUN_EXCLUDE_HOSTS with MNRUN_EXCLUDE_MODE=drop (mnrun.sh removes them from its node list), to nodechk as
#                           srun -x, and counted out of the allocation size when a stage decides whether a node count fits.  Hosts
#                           flagged by stage nodechk in the same --out are added automatically (file $OUT/flagged_hosts.txt).
#                           MNRUN_EXCLUDE_HOSTS in the environment is taken as the default list.
#   --spares N              NOT USED (the policy of 2026-10-11 is exclude-only: ask for exactly the nodes you run, drop bad nodes
#                           with --exclude-hosts, no spare nodes).  The option is kept so old command lines still work: it only
#                           prints the allocation size (largest node count + N) and makes nodechk say STOP when more than N nodes
#                           are flagged.  Default: unset (no spare check); do not pass it in the recommended flow.
#   --plan-digits D --plan-procs G
#                           stage plan also checks this one point (D total digits on G node-processes), e.g. the run you are
#                           about to make.  Stage plan always checks every node count of stage a4.
#   --skip LIST             run all stages except these (comma list).  --only and --skip combine (skip wins
#                           on overlap).
#   --nodes-a4 LIST         node counts for stage a4, comma list (default: 2,8,64; 2,8 with --site aac7).
#                           A count bigger than the allocation is skipped, not failed.
#   --nics1 LIST            COMM_OFI_NICS form for "1 NIC per APU" (default: "0;1;2;3"; read by stage a3 only, which is disabled)
#   --nics2 LIST            COMM_OFI_NICS form for "2 NICs per APU, the target's form" (default "0,4;1,5;2,6;3,7"; stage a3 only, disabled)
#   --nodes-s2chk LIST      node counts for the opt-in stage s2chk (default: 2,8 = the smallest multi-node count a4 uses
#                           plus its larger one).  A count bigger than the allocation is skipped, not failed.
#   --s2chk-per-node N      digits per node for s2chk (default 64410000000 = 6.441e10, the largest node share of the
#                           3.71e13 target on 576 nodes).  KIT_S2CHK_LINE overrides the launch-line env words.
#   --nodechk-gb GB         upload size per APU for the opt-in stage nodechk (default 1).
#   --nodechk-cold-gb GB    T10 cold-path probe in stage nodechk (default 0 = off): per APU map GB of VMM arena + first-touch + one cold upload of it
#                           (the ecalc init path); extra NODECHK_COLD line per node (archive:results_merged_2026-10-24/NODE_SLOWMODE.md §T10).
#   --initchk-log FILE      log for stage initchk (default: the log of the smallest a4 node count in --out).
#   --timeout-STAGE SECS    per-stage timeout (env 90, build 900, uffd 300, plan 120 PER point, edge 600, a3 600, a4 1800 PER node count,
#                           s2chk 1800 PER node count, nodechk 300).
#                           A stage that times out or fails is marked FAIL in the summary; every other stage
#                           still runs.
#
# What each stage does, and about how long it takes (wall clock, once the allocation is live):
#   env    (<1 min, no GPU, 1 node of the allocation probed):
#            hostname list, Slurm job geometry, ROCm version, module list, fi_info -p cxi domain count.
#   build  (3-10 min, login node or any node with the toolchain, no GPU needed):
#            `make XSHMEM=cray` (path 2) for ecalc, tests/t_edge, tests/t_nodechk, tools/.  ROCm version is unknown ahead
#            of time on the target, so this tries candidates in order and keeps the first that links: just
#            $TARGET_ROCM if set, else rocm/7.2.4, rocm/7.2.3, rocm/7.0.3 (whichever are available), then any
#            other rocm/<version> module newest first (picking blindly-newest once chose rocm/7.14.0 on aac7,
#            which fails to link -- results/AAC7_PORT.md).  SMA/DSMML modules auto-detect as before; override any of
#            the three with TARGET_ROCM / TARGET_SMA_MODULE / TARGET_DSMML_MODULE / GMP_HOME.  The chosen set is
#            written to $OUT/kit_modules.env; every later stage sources it and loads the same modules inside its
#            own srun (not whatever the caller's shell happened to have), and records the ROCm actually seen on
#            the compute node.
#   edge   (5-10 min, 1 node, ALONE -- run first, before a3/a4 share the allocation):
#            tests/t_edge dev then vmm (the device-memory edge, hipMalloc and VMM forms); then one e 1e9 run
#            with MEM_REPORT_DEVS=1 (host RSS, per-APU device in-use vs driver-used, the VMM arena map rate
#            per APU and per node); then one e 1e10 run for the single-node wall.  t_edge runs to
#            completion and exits before anything else touches these GPUs.
#   uffd   (< 2 min, 2 nodes; default on): is libfabric's memory-cache monitor usable?  Launch default FI_MR_CACHE_MONITOR=userfaultfd
#            (the crash/hang fix, results/CRASHES_HANGS.md §S46; override by exporting it; `disabled` is refused).  The stage runs a tiny 2-node ecalc
#            with FI_LOG_LEVEL=info and looks for "memory monitor is: uffd"; it also tries the userfaultfd syscall on a compute node.
#            PASS = the log line is seen.  FAIL names the monitor libfabric chose instead; then set FI_MR_CACHE_MONITOR=memhooks.
#   plan   (< 1 min per point, no device; default on): MN_PLAN_ONLY=<digits>:<procs> for every a4 node count (and --plan-digits/--plan-procs).
#          FIT (D26): the plan includes the memory fit (`plan fit` lines: device vs ECALC_DEVICE_GB, node vs ECALC_NODE_GB, DM_MN_WRAP's
#          decision), shown in the summary; a point that does not fit is refused (`plan REFUSED`), like a prime-set refusal.
#            PASS only if the last plan line is `plan check ... OK`.  Stages a4 and s2chk run the same check before each long run and
#            refuse the run on `plan REFUSED`.  Run it before anything long.
#   initchk (seconds; default on after a4): runs tools/initchk.py (slow-host check: flag a node > 3x the median and >= 2 s above it; exit 0 ok, 3 flagged, 2 cannot parse)
#            the first a4 log (a4 sets ECALC_INIT_TL=1 NEWTON_MN_RECIP_TS=2 MNRUN_LABEL=1: extra timestamp lines and rank labels only; the
#            host-attributed upload line is where the known slow host stalls).  Post-run diagnosis: rerun without the host.  SKIPPED if absent.
#   a3     DISABLED (it ran tests/t_comm --bw, removed with the plain SHMEM transport, Q103): prints a notice and skips.
#   a4     (5-10 min PER node count, 2/8/64 nodes, skips what the allocation can't hold):
#            ecalc at 1e10 digits/node with MN_COMM_MARK=1, VERIFY, wall, the comm-mark fabric rates.
#   s2chk  (OPT-IN, off unless named in --only; about 5-15 min PER node count, default 2 and 8 nodes; needs stage build's
#           kit_modules.env, so run `--only build,s2chk` or point --out at a directory where build already ran):
#            the TARGET per-node share (6.441e10 digits/node) on the target launch line (internal target notes 4 without the
#            576-only MN_GROUPS/POOL words; MN_T_CHUNK_MB=2048, ECALC_SEGV_TRACE=1) with ECALC_INIT_TL=1 MN_WAIT_STATS=1,
#            nothing written.  Saves the full logs (logs/05_s2chk_n<N>.log) and prints a verdict helper:
#             A37-R1 (GPU seed): "seed thread ends" vs the last "background mapping done" (tl lines, seconds since
#               rns_init).  Build the GPU seed ONLY if the seed ends later than about 18 s AND more than 2 s after the
#               last mapping; seed late but mapping within 2 s = mapping binds (marginal, no build); else NO-GO.
#             S-2 (division overlap): MN_WAIT_STATS "dm" (= reciprocal + division) wait, per APU thread (node sum / 4),
#               max node; reciprocal and division split; spread max-min (the skew bound).  Rule: build only if the
#               exposed wait is above about 30 s.  CAVEAT printed with it: the literal wait includes the transfer itself
#               (results/SESSIONS.md 2.3), and the 576-node value is modelled at 34 s; judge the 8-node figure against
#               S22's aac7 table (n=2: recip 10 + division 32; n=8: 22 + 73 s per thread).
#            Needs an ecalc built WITH MN_WAIT_STATS (in main since D3, comm_util.c; the stage says so if no
#            wait-stats line appears).  The command for the target team:
#              ./target_kit.sh --jobid $SLURM_JOB_ID --out kit_out_s2chk --only build,s2chk
#   nodechk (OPT-IN, off unless named in --only; about 1-3 min, EVERY node of the allocation at once, one process per node; needs
#           stage build's kit_modules.env and the tests/t_nodechk it builds: `--only build,nodechk`; TARGET_TASKS T14):
#            per node: MemTotal / MemFree / MemAvailable / Cached / HugePages_Total / HugePages_Free from /proc/meminfo,
#            and the time of a 1 GB host->device hipMemcpy upload (pre-touched pageable buffer, median of 3) to each of the
#            4 APUs.  Output: a table per node in KIT_SUMMARY.txt (logs/06_nodechk.log keeps the raw NODECHK lines).  A node is
#            FLAGGED if its slowest-APU upload time is > 3x the median over nodes, or its MemAvailable is < 90 % of the median.
#            Why: on aac7 the node aac7-n01 stalled 20-25 s in the host->device upload of db_from_bi (about 1 s on the
#            other nodes, measured) and 7 of 13 nodes had ~441 GB MemAvailable against ~520 GB on the other 6 (measured,
#            2026-10-09): a slow or short node is a straggler for the whole run, so find it BEFORE the timed run and exclude it.
#            Run it alone on the nodes (nothing else may be using the GPUs or the fabric).  The command for the target team:
#              ./target_kit.sh --jobid $SLURM_JOB_ID --out kit_out_nodechk --only build,nodechk
#
# Launch: always Slurm-native `srun --ntasks ... --ntasks-per-node=1` (mnrun.sh for the multi-node ecalc
# runs of stage a4); never oshrun.  FI_UNIVERSE_SIZE and FI_LOG_LEVEL are set to the TGTBENCH2-proposed
# defaults (max(4096, 4 x ntasks) and warn) ONLY if not already set in your environment.
#
# Large digit output files are deleted right after each run's VERIFY check; only KIT_SUMMARY.txt and the
# small logs under $OUT/logs/ remain.  Send back KIT_SUMMARY.txt.

set -u

KIT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)      # target/kit
REPO_DIR=$(cd "$KIT_DIR/../.." && pwd)                      # repository root
. "$REPO_DIR/ecalc/gres.sh"                                 # MNRUN_GRES=auto|on|off
GRES_OPT=()
kit_gres() { GRES_OPT=(); mn_gres_on && GRES_OPT=(--gpus-per-node=4); return 0; }   # no GPU option where the nodes list no GRES
SELF_DIR=$REPO_DIR/ecalc                                    # mnrun.sh, Makefile, ecalc, tests/ live here (moved kit: formerly the kit's own dir)

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
TO_NC=300
TO_UFFD=300
TO_PLAN=120
EXCL_HOSTS="${MNRUN_EXCLUDE_HOSTS:-}"
SPARES=-1
PLAN_DIGITS=""
PLAN_PROCS=""
INITCHK_LOG=""
NC_GB=1
NC_COLD_GB=0
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
        --timeout-nodechk) TO_NC=$2; shift 2 ;;
        --nodechk-gb) NC_GB=$2; shift 2 ;;
        --exclude-hosts) EXCL_HOSTS=$2; shift 2 ;;
        --spares) SPARES=$2; shift 2 ;;
        --plan-digits) PLAN_DIGITS=$2; shift 2 ;;
        --plan-procs) PLAN_PROCS=$2; shift 2 ;;
        --initchk-log) INITCHK_LOG=$2; shift 2 ;;
        --timeout-uffd) TO_UFFD=$2; shift 2 ;;
        --timeout-plan) TO_PLAN=$2; shift 2 ;;
        --nodechk-cold-gb) NC_COLD_GB=$2; shift 2 ;;
        --nodes-s2chk) NODES_S2=$2; shift 2 ;;
        --s2chk-per-node) S2_PER_NODE=$2; shift 2 ;;
        *) echo "target_kit.sh: unknown option $1" >&2; usage; exit 2 ;;
    esac
done

ONLY=${ONLY//preflight/env,build,uffd,plan,edge,nodechk}
case "$SPARES" in -1|[0-9]*) ;; *) echo "target_kit.sh: --spares needs a number" >&2; exit 2 ;; esac
case "$SITE" in
    aac7) [ -z "$NODES_A4" ] && NODES_A4="2,8" ;;
    target) [ -z "$NODES_A4" ] && NODES_A4="2,8,64" ;;
    *) echo "target_kit.sh: --site must be aac7 or target" >&2; exit 2 ;;
esac

[ -z "$OUT" ] && OUT="$KIT_DIR/kit_out_$(date +%Y%m%d_%H%M%S)"
LOGDIR="$OUT/logs"
TMPDIR="$OUT/tmp"
SUMMARY="$OUT/KIT_SUMMARY.txt"
FLAGGED="$OUT/flagged_hosts.txt"   # written by stage nodechk; read by every later multi-node stage
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

alloc_hosts() {   # the allocation's host names, one per line (empty under --dry-run or without a job)
    [ "$DRY_RUN" = 1 ] && return 0
    [ -z "$JOBID" ] && return 0
    scontrol show hostnames "$(squeue -j "$JOBID" -h -o %N 2>/dev/null)" 2>/dev/null
}

excl_all() {   # the hosts to leave out: --exclude-hosts (or MNRUN_EXCLUDE_HOSTS), plus those nodechk flagged in this --out; sorted, one per line
    { echo "$EXCL_HOSTS" | tr , '\n'; [ -f "$FLAGGED" ] && cat "$FLAGGED"; } | grep -v '^$' | sort -u
}

excl_in_alloc() {   # excluded hosts that are really in the allocation
    local ex; ex=$(excl_all); [ -n "$ex" ] || return 0
    alloc_hosts | grep -Fxf <(echo "$ex") || true
}

alloc_usable_count() {   # allocation size minus excluded hosts; 0 if unknown or dry-run
    local tot ne; tot=$(alloc_node_count); [ "${tot:-0}" -gt 0 ] 2>/dev/null || { echo 0; return; }
    ne=$(excl_in_alloc | grep -c .); echo $(( tot - ne ))
}

export_exclude() {   # for the mnrun.sh launches of the calling stage: drop the excluded hosts from its node list
    local e; e=$(excl_all | paste -sd, -)
    if [ -n "$e" ]; then export MNRUN_EXCLUDE_HOSTS="$e" MNRUN_EXCLUDE_MODE=drop; fi
    return 0
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
    # the crash/hang fix (results/CRASHES_HANGS.md §S46 section 4.6, adopted as the launch default): libfabric's MR cache monitor = userfaultfd.
    # Overridable (export FI_MR_CACHE_MONITOR=memhooks to get the old behaviour); `disabled` is refused at the top of this script.
    export FI_MR_CACHE_MONITOR=${FI_MR_CACHE_MONITOR:-userfaultfd}
    [ "${MNRUN_FI_DEFAULTS:-1}" = 0 ] && return 0   # the same opt-out as mnrun.sh: leave both unset
    local want=$(( ntasks * 4 > 4096 ? ntasks * 4 : 4096 ))
    export FI_UNIVERSE_SIZE=${FI_UNIVERSE_SIZE:-$want}
    export FI_LOG_LEVEL=${FI_LOG_LEVEL:-warn}
}

plan_check() {   # plan_check <logfile> <digits> <procs> [VAR=val words...] -- MN_PLAN_ONLY on a compute node (no device touched).
    # 0 only if the LAST plan line is `plan check ... OK` (a `plan REFUSED` line, or no plan line, is a refusal: 1).  Dry-run: prints, 0.
    local lf=$1 d=$2 g=$3; shift 3
    local plf="$LOGDIR/plan_${d}_${g}.log"; : > "$plf"
    exec_cmd "$plf" "$TO_PLAN" srun --jobid="$JOBID" --export=ALL -N1 -n1 --overlap bash -lc "$(module_preamble)"'p=$1; shift; export MN_PLAN_ONLY=$p; exec env "$@"' _ "$d:$g" COMM_TRANSPORT=xshmem "$@" "$SELF_DIR/ecalc"
    [ "$DRY_RUN" = 1 ] && { cat "$plf" >> "$lf"; return 0; }
    cat "$plf" >> "$lf"
    local last; last=$(grep -aE '^plan (check|REFUSED)' "$plf" | tail -1)
    echo "plan point $d digits on $g node-process(es): ${last:-<no plan line>}" | cut -c1-220 >> "$lf"
    case "$last" in "plan check "*": OK"*) return 0 ;; esac
    return 1
}

# S46 §4.6: FI_MR_CACHE_MONITOR=disabled gave wrong exchanges silently (2097152 of 2097152 points differed, VERIFY FAILED); refuse it.
__fmc=${FI_MR_CACHE_MONITOR-}; __fmc=${__fmc,,}
if [ "$__fmc" = disabled ]; then
    echo "target_kit.sh: refusing FI_MR_CACHE_MONITOR=$FI_MR_CACHE_MONITOR: it corrupts exchanges silently (S46 section 4.6). Unset it (memhooks, the default) or use userfaultfd." >&2
    exit 1
fi
export FI_MR_CACHE_MONITOR=${FI_MR_CACHE_MONITOR:-userfaultfd}   # S46, adopted 2026-10-11 (the crash/hang fix); override: FI_MR_CACHE_MONITOR=memhooks
echo "target_kit.sh: FI_MR_CACHE_MONITOR=$FI_MR_CACHE_MONITOR"

: > "$SUMMARY"
summary "KIT_SUMMARY -- target_kit.sh"
summary "generated: $(date -Is 2>/dev/null || date)"
summary "site: $SITE   jobid: ${JOBID:-<none>}   dry-run: $DRY_RUN"
summary "out: $OUT"
if [ "$SPARES" -ge 0 ]; then
    need=0
    for c in $(echo "$NODES_A4,${PLAN_PROCS:-0}" | tr , ' '); do [ "$c" -gt "$need" ] 2>/dev/null && need=$c; done
    if stage_optin s2chk; then for c in $(echo "$NODES_S2" | tr , ' '); do [ "$c" -gt "$need" ] 2>/dev/null && need=$c; done; fi
    summary "spares (option NOT USED by the exclude-only policy): $SPARES -> request $((need + SPARES)) node(s) = largest node count asked for ($need) + $SPARES"
fi
[ -n "$(excl_all)" ] && summary "excluded hosts (--exclude-hosts / MNRUN_EXCLUDE_HOSTS): $(excl_all | paste -sd, -)"
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
        exec_cmd "$lf" "$TO_ENV" srun --jobid="$JOBID" --export=ALL -N1 -n1 --overlap bash -c '
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
    summary "## Stage build: make XSHMEM=cray"
    (
        cd "$SELF_DIR" || exit 1
        : "${TARGET_ROCM:=}"; : "${TARGET_SMA_MODULE:=}"; : "${TARGET_DSMML_MODULE:=}"; : "${GMP_HOME:=}"; : "${MAKE_JOBS:=4}"
        echo "== module detection (override with TARGET_ROCM / TARGET_SMA_MODULE / TARGET_DSMML_MODULE)" >> "$lf"
        if [ -z "$TARGET_SMA_MODULE" ]; then TARGET_SMA_MODULE=$(module -t avail cray-openshmemx 2>&1 | grep -oE '^cray-openshmemx/[0-9][0-9.]*' | sort -V | tail -1); fi
        TARGET_SMA_MODULE=${TARGET_SMA_MODULE:-cray-openshmemx}
        if [ -z "$TARGET_DSMML_MODULE" ]; then TARGET_DSMML_MODULE=$(module -t avail cray-dsmml 2>&1 | grep -oE '^cray-dsmml/[0-9][0-9.]*' | sort -V | tail -1); fi
        TARGET_DSMML_MODULE=${TARGET_DSMML_MODULE:-cray-dsmml}

        # ROCm choice (Phase S18 fix): picking the newest rocm/x.y.z (sort -V | tail -1) used to pick rocm/7.14.0 on
        # aac7, which FAILS to link ecalc (results/AAC7_PORT.md: 7.12/7.13/7.14 and rocm-new/10.0.0 all fail the same
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

        local mk_extra=(XSHMEM=cray)
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
            exec_cmd "$lf" "$TO_BUILD" bash -lc "$ul >/dev/null 2>&1; cd '$SELF_DIR' && make ${mk_extra[*]} -j${MAKE_JOBS} ecalc tools"
            rc1=$?
            exec_cmd "$lf" "$TO_BUILD" bash -lc "$ul >/dev/null 2>&1; cd '$SELF_DIR' && make ${mk_extra[*]} -j${MAKE_JOBS} tests/t_edge tests/t_nodechk"
            rc2=$?
            if [ "$DRY_RUN" = 1 ]; then
                chosen="$cand"
                echo "[dry-run] assuming the first candidate ($chosen) builds; the rest are not tried" | tee -a "$lf"
                break
            fi
            if [ $rc1 -eq 0 ] && [ $rc2 -eq 0 ] && [ -x "$SELF_DIR/ecalc" ] && [ -x "$SELF_DIR/tests/t_edge" ] && [ -x "$SELF_DIR/tests/t_nodechk" ]; then
                chosen="$cand"
                echo "== ROCm candidate $cand: build OK (ecalc, tests/t_edge, tests/t_nodechk, tools)" >> "$lf"
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
    elif [ $rc -eq 0 ] && [ -x "$SELF_DIR/ecalc" ] && [ -x "$SELF_DIR/tests/t_edge" ]; then
        summary "status: PASS (ecalc, tests/t_edge built)"
    else
        summary "status: FAIL (see log -- no ROCm candidate linked, or a missing binary)"
    fi
    summary "  full log: $lf"
    summary ""
}

# =============================================================================================================
# Stage: uffd -- is libfabric's userfaultfd memory monitor usable here?  (FI_MR_CACHE_MONITOR launch default, results/CRASHES_HANGS.md §S46)
# =============================================================================================================
stage_uffd() {
    local lf="$LOGDIR/01b_uffd.log"; : > "$lf"
    summary "## Stage uffd: libfabric memory monitor (FI_MR_CACHE_MONITOR=${FI_MR_CACHE_MONITOR:-userfaultfd})"
    if [ -z "$JOBID" ] && [ "$DRY_RUN" != 1 ]; then summary "status: FAIL (no --jobid / SLURM_JOB_ID)"; summary ""; return; fi
    if ! require_kit_modules uffd "$lf"; then summary "status: FAIL ($KIT_MODULES_ENV missing -- run stage build first)"; summary ""; return; fi
    local alloc; alloc=$(alloc_usable_count); export_exclude
    export MNRUN_MODULES="${MNRUN_MODULES:-$TARGET_DSMML_MODULE $TARGET_SMA_MODULE $TARGET_ROCM}"
    export MNRUN_UNLOAD="${MNRUN_UNLOAD-${TARGET_ROCM_UNLOAD:-}}"
    local n=2 pre; pre=$(module_preamble)
    [ "$DRY_RUN" != 1 ] && [ "$alloc" -gt 0 ] && [ "$alloc" -lt 2 ] && n=1
    fi_defaults_for "$n"
    # (1) the syscall on a compute node: permitted, EPERM (needs vm.unprivileged_userfaultfd=1 or a capability) or ENOSYS
    local slf="$LOGDIR/01b_uffd_syscall.log"; : > "$slf"
    exec_cmd "$slf" "$TO_ENV" srun --jobid="$JOBID" --export=ALL -N1 -n1 --overlap bash -c '
        echo "host $(hostname); vm.unprivileged_userfaultfd=$(cat /proc/sys/vm/unprivileged_userfaultfd 2>&1)"
        python3 -c "
import ctypes, os
libc = ctypes.CDLL(None, use_errno=True)
nr = {\"x86_64\": 323, \"aarch64\": 282}.get(os.uname().machine)
if nr is None: print(\"uffd syscall: unknown arch\"); raise SystemExit
fd = libc.syscall(nr, os.O_CLOEXEC | os.O_NONBLOCK)
print(\"uffd syscall: permitted\" if fd >= 0 else \"uffd syscall: refused, errno %d (%s)\" % (ctypes.get_errno(), os.strerror(ctypes.get_errno())))
" 2>&1'
    # (2) the real test: a tiny multi-node ecalc with libfabric info logging; libfabric prints the monitor it selected
    local tiny=$((n * E9 / 10))
    echo "== tiny run: $n node(s), $tiny digits, FI_LOG_LEVEL=info FI_MR_CACHE_MONITOR=$FI_MR_CACHE_MONITOR" >> "$lf"
    exec_cmd "$lf" "$TO_UFFD" env SLURM_JOB_ID="$JOBID" MNRUN_NODES="$n" FI_UNIVERSE_SIZE="$FI_UNIVERSE_SIZE" FI_LOG_LEVEL=info FI_MR_CACHE_MONITOR="$FI_MR_CACHE_MONITOR" \
        MNRUN_MODULES="$MNRUN_MODULES" MNRUN_UNLOAD="$MNRUN_UNLOAD" \
        "$SELF_DIR/mnrun.sh" "$n" env COMM_TRANSPORT=xshmem ECALC_SEGV_TRACE=1 "$SELF_DIR/ecalc" "$tiny" "$TMPDIR/uffd.out"
    local rc=$?
    rm -f "$TMPDIR"/uffd.out*
    if [ "$DRY_RUN" = 1 ]; then summary "status: DRY-RUN (not executed)"; summary ""; return; fi
    grep -a 'uffd syscall' "$slf" | head -1 | sed 's/^/  /' >> "$SUMMARY"
    local mon; mon=$(grep -aoE 'memory monitor is: [a-z_]+' "$lf" | sort | uniq -c | sort -rn | head -3 | tr '\n' ';')
    summary "  libfabric monitor lines: ${mon:-<none>}   (run exit $rc)"
    if grep -aq 'memory monitor is: uffd' "$lf"; then summary "status: PASS (libfabric selected uffd)"
    elif [ -n "$mon" ]; then summary "status: FAIL (libfabric selected another monitor: $mon; set FI_MR_CACHE_MONITOR=memhooks and tell the developers)"
    elif grep -aq 'syscall: permitted' "$slf"; then summary "status: PARTIAL (syscall permitted but no monitor line in the log: tiny run exit $rc, see $lf)"
    else summary "status: FAIL (no monitor line and no permitted syscall; see $lf and $slf)"; fi
    summary "  full log: $lf"
    summary ""
}

# =============================================================================================================
# Stage: plan -- MN_PLAN_ONLY before any long run (no device touched)
# =============================================================================================================
stage_plan() {
    local lf="$LOGDIR/01c_plan.log"; : > "$lf"
    summary "## Stage plan: MN_PLAN_ONLY=<digits>:<procs> must end in 'plan check ... OK' before any long run"
    if [ -z "$JOBID" ] && [ "$DRY_RUN" != 1 ]; then summary "status: FAIL (no --jobid / SLURM_JOB_ID)"; summary ""; return; fi
    if ! require_kit_modules plan "$lf"; then summary "status: FAIL ($KIT_MODULES_ENV missing -- run stage build first)"; summary ""; return; fi
    local n bad=0 pts=() c
    IFS=',' read -ra counts <<< "$NODES_A4"
    for c in "${counts[@]}"; do pts+=("$((c * E10)):$c"); done
    [ -n "$PLAN_DIGITS" ] && [ -n "$PLAN_PROCS" ] && pts+=("$PLAN_DIGITS:$PLAN_PROCS")
    for c in "${pts[@]}"; do
        if plan_check "$lf" "${c%%:*}" "${c##*:}"; then summary "  ${c%%:*} digits on ${c##*:}: plan check OK"
        else summary "  ${c%%:*} digits on ${c##*:}: NOT OK -- $(grep -aE '^plan (check|REFUSED)' "$LOGDIR/plan_${c%%:*}_${c##*:}.log" 2>/dev/null | tail -1 | cut -c1-160)"; bad=1; fi
        # FIT (D26, results/LAUNCH_DEFAULTS.md §FIT): the planner's memory fit (device vs ECALC_DEVICE_GB, node vs ECALC_NODE_GB, DM_MN_WRAP's decision);
        # a run that does not fit ends its plan in `plan REFUSED` (plan_check above fails it)
        grep -aE '^plan fit +(FITS|REFUSED|DM_MN_WRAP)' "$LOGDIR/plan_${c%%:*}_${c##*:}.log" 2>/dev/null | cut -c1-220 | sed 's/^/    /' >> "$SUMMARY"
    done
    if [ "$DRY_RUN" = 1 ]; then summary "status: DRY-RUN (not executed)"; elif [ $bad -eq 0 ]; then summary "status: PASS"; else summary "status: FAIL (a long run at a refused point must not be started)"; fi
    summary "  full log: $lf"
    summary ""
}

# =============================================================================================================
# Stage: initchk -- first-run init-time slow-host check (tools/initchk.py; policy: flag a node whose init is > 3x the median)
# =============================================================================================================
INITCHK_SCRIPT="$REPO_DIR/tools/initchk.py"
stage_initchk() {
    local lf="$LOGDIR/04b_initchk.log"; : > "$lf"
    summary "## Stage initchk: init-time slow-host check on the first multi-node run's log (flag > 3x median)"
    if [ ! -f "$INITCHK_SCRIPT" ]; then summary "status: SKIPPED ($INITCHK_SCRIPT not present in this checkout)"; summary ""; return; fi
    local log="$INITCHK_LOG"
    if [ -z "$log" ]; then   # the log of the smallest a4 node count that ran
        log=$(ls "$LOGDIR"/04_a4_n*.log 2>/dev/null | sort -t n -k3 -V | head -1)
    fi
    if [ "$DRY_RUN" = 1 ]; then
        printf '[dry-run] python3 %q %q\n' "$INITCHK_SCRIPT" "${log:-<first a4 log>}" | tee -a "$lf"; summary "status: DRY-RUN (not executed)"; summary ""; return
    fi
    if [ -z "$log" ] || [ ! -f "$log" ]; then summary "status: SKIPPED (no multi-node run log: run stage a4 in this --out or give --initchk-log FILE)"; summary ""; return; fi
    summary "  log checked: $log"
    timeout 120 python3 -I "$INITCHK_SCRIPT" ${KIT_INITCHK_ARGS:-} "$log" >> "$lf" 2>&1
    local rc=$?
    grep -aE '^(FLAG|node|metric|median)' "$lf" | head -30 | sed 's/^/    /' >> "$SUMMARY"
    case $rc in
        0) if grep -aq 'no median' "$lf"; then summary "status: PARTIAL (no flag, but fewer than 3 nodes report: no median, the check is vacuous)"
           else summary "status: PASS (no node above 3x the allocation median)"; fi ;;
        3) summary "status: FLAG (slow host(s), see FLAG lines / $lf). Policy: post-run diagnosis, not a pre-run gate -- rerun without the flagged host (--exclude-hosts, or sbatch -x)" ;;
        *) summary "status: PARTIAL (initchk could not parse the log, rc $rc: needs ECALC_INIT_TL=1, and >= 3 nodes for a median; see $lf)" ;;
    esac
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

    local pre; pre=$(module_preamble); kit_gres
    local rc=0
    echo "== t_edge dev (hipMalloc), alone on the node, before anything else" >> "$lf"
    exec_cmd "$lf" "$TO_EDGE" srun --jobid="$JOBID" --export=ALL -N1 -n1 --ntasks-per-node=1 ${GRES_OPT[@]+"${GRES_OPT[@]}"} --overlap \
        bash -lc "$pre"'echo "== runtime ROCm (compute node): $(hipconfig --version 2>&1)"; exec "$@"' _ \
        "$SELF_DIR/tests/t_edge" dev "$EDGE_STEP_GB" "$EDGE_CAP_GB" || rc=1
    echo "== t_edge vmm (hipMemCreate), alone on the node" >> "$lf"
    exec_cmd "$lf" "$TO_EDGE" srun --jobid="$JOBID" --export=ALL -N1 -n1 --ntasks-per-node=1 ${GRES_OPT[@]+"${GRES_OPT[@]}"} --overlap \
        bash -lc "$pre"'echo "== runtime ROCm (compute node): $(hipconfig --version 2>&1)"; exec "$@"' _ \
        "$SELF_DIR/tests/t_edge" vmm "$EDGE_STEP_GB" "$EDGE_CAP_GB" || rc=1

    # Phase S18 kit fix (archive:results_merged_2026-10-24/TOOLS_PROFILES.md §S18KITFIX, bug 3): dbig.c's "VMM arena ... s/GB" line (the only place a map
    # rate is printed) is gated by vmm_vb(), which is off unless DB_POOL_VERBOSE (or RNS_VERBOSE) is set -- so
    # this run used to print the "report both forms" text with no number behind it.  DB_POOL_VERBOSE=1 turns
    # on just that line (narrower than RNS_VERBOSE, which adds a lot of unrelated chatter).
    echo "== e $E9 (1e9), MEM_REPORT_DEVS=1 ECALC_VERBOSE=2 DB_POOL_VERBOSE=1 -- the init footprint and VMM arena map rate" >> "$lf"
    exec_cmd "$lf" "$TO_EDGE" srun --jobid="$JOBID" --export=ALL -N1 -n1 --ntasks-per-node=1 ${GRES_OPT[@]+"${GRES_OPT[@]}"} --overlap \
        bash -lc "$pre"'echo "== runtime ROCm (compute node): $(hipconfig --version 2>&1)"; exec "$@"' _ \
        env ECALC_SEGV_TRACE=1 MEM_REPORT_DEVS=1 ECALC_VERBOSE=2 DB_POOL_VERBOSE=1 "$SELF_DIR/ecalc" "$E9" "$TMPDIR/e9.out" || rc=1
    [ "$DRY_RUN" != 1 ] && rm -f "$TMPDIR"/e9.out*

    echo "== e $E10 (1e10) -- the single-node wall" >> "$lf"
    exec_cmd "$lf" "$TO_EDGE" srun --jobid="$JOBID" --export=ALL -N1 -n1 --ntasks-per-node=1 ${GRES_OPT[@]+"${GRES_OPT[@]}"} --overlap \
        bash -lc "$pre"'echo "== runtime ROCm (compute node): $(hipconfig --version 2>&1)"; exec "$@"' _ \
        env ECALC_SEGV_TRACE=1 "$SELF_DIR/ecalc" "$E10" "$TMPDIR/e10.out" || rc=1
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
# Stage: a3 -- DISABLED (it ran tests/t_comm --bw with COMM_TRANSPORT=shmem; both were removed, Q103)
# =============================================================================================================
stage_a3() {
    local lf="$LOGDIR/03_a3.log"; : > "$lf"
    summary "## Stage a3: DISABLED (its test t_comm and the plain SHMEM transport were removed in Q103; use tests/t_xshmem bw by hand)"
    echo "a3 disabled: t_comm and the plain SHMEM transport no longer exist (Q103)" >> "$lf"
    summary "status: SKIPPED (stage disabled)"; summary "  full log: $lf"; summary ""
}

# =============================================================================================================
# Stage: a4 -- ecalc at 1e10 digits/node, MN_COMM_MARK=1, on each requested node count
# =============================================================================================================
stage_a4() {
    local lf="$LOGDIR/04_a4.log"; : > "$lf"
    summary "## Stage a4: all-to-all scaling (MN_COMM_MARK=1), 1e10 digits/node, node counts: $NODES_A4"
    if [ -z "$JOBID" ] && [ "$DRY_RUN" != 1 ]; then summary "status: FAIL (no --jobid / SLURM_JOB_ID)"; summary ""; return; fi
    if ! require_kit_modules a4 "$lf"; then summary "status: FAIL ($KIT_MODULES_ENV missing -- run stage build first)"; summary ""; return; fi
    local alloc; alloc=$(alloc_usable_count); export_exclude
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
        # Phase S18 kit fix (archive:results_merged_2026-10-24/TOOLS_PROFILES.md §S18KITFIX, bug 2): the ecalc run's own output used to go straight into the
        # shared, appended "$lf" -- by the time a later n's block was parsed, grep/tail over that same accumulated
        # file picked up every earlier n's "comm-mark"/"total"/VERIFY lines too (the n=8 block repeated n=2's
        # lines ahead of its own).  Give each n its own log file and parse only that file for this n's summary
        # lines; the per-n log is still appended into the combined "$lf" afterward so the full log on disk is
        # unchanged for a human reading it end to end.
        local nlf="$LOGDIR/04_a4_n${n}.log"; : > "$nlf"
        fi_defaults_for "$n"
        echo "== n=$n nodes, $digits digits total ($E10 per node), MN_COMM_MARK=1, MNRUN_MODULES=$MNRUN_MODULES MNRUN_UNLOAD=$MNRUN_UNLOAD" >> "$lf"
        exec_cmd "$lf" "$TO_ENV" srun --jobid="$JOBID" --export=ALL -N1 -n1 --overlap bash -lc "$pre"'echo "== runtime ROCm (compute node, n='"$n"'): $(hipconfig --version 2>&1)"'
        if ! plan_check "$nlf" "$digits" "$n"; then
            any_fail=1; summary "  n=$n: REFUSED by the plan check (no run started; see $nlf)"; continue
        fi
        exec_cmd "$nlf" "$TO_A4" env SLURM_JOB_ID="$JOBID" MNRUN_NODES="$n" FI_UNIVERSE_SIZE="$FI_UNIVERSE_SIZE" FI_LOG_LEVEL="$FI_LOG_LEVEL" \
            MNRUN_MODULES="$MNRUN_MODULES" MNRUN_UNLOAD="$MNRUN_UNLOAD" \
            MNRUN_LABEL=1 "$SELF_DIR/mnrun.sh" "$n" env COMM_TRANSPORT=xshmem ECALC_SEGV_TRACE=1 MN_COMM_MARK=1 ECALC_VERBOSE=2 ECALC_INIT_TL=1 NEWTON_MN_RECIP_TS=2 "$SELF_DIR/ecalc" "$digits" "$out"
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
# The gates (results/SESSIONS.md sections 2 and 3; TASKS.md S-2 / A37-R1, both SHELVED by the user 2026-10-08, kept as options):
#   A37-R1: build the GPU seed only if the seed thread ends later than ~18 s AND > 2 s after the last background mapping.
#   S-2:    build the division overlap only if the exposed division/reciprocal wait is above ~30 s per APU thread.
# The greps are the ones the S22/S23 drivers used: `tl` lines from ECALC_INIT_TL=1 ("seed thread ends (..)", "APU<d>
# background mapping done: ..") and the `wait-stats node R:` lines of MN_WAIT_STATS=1 (per phase: bs | dm | recip | other;
# dm contains recip; node sums over the 4 APU threads, so per thread = /4).
S2_SEED_LATE=18        # s: A37-R1 trigger (seed end)
S2_SEED_AFTER=2        # s: A37-R1 trigger (seed end minus the last mapping)
S2_WAIT_LIMIT=30       # s per APU thread: S-2 trigger
S2_LINE_DEFAULT="COMM_TRANSPORT=xshmem ECALC_NP=auto MN_T_CHUNK_MB=2048 ECALC_SEGV_TRACE=1 ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1"

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
        summary "    S-2: no 'wait-stats node' lines -- this ecalc build lacks MN_WAIT_STATS (the instrument is in main since D3; a build from an older commit lacks it) or the run failed -- see $f"
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
    local alloc; alloc=$(alloc_usable_count); export_exclude
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
        if ! plan_check "$nlf" "$digits" "$n" $line; then
            any_fail=1; summary "  n=$n: REFUSED by the plan check (no run started; see $nlf)"; continue
        fi
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
# Stage: nodechk (OPT-IN) -- per-node memory and host->device upload health, a table per node, flagged outliers
# =============================================================================================================
NC_SLOW_X=3        # flag: slowest-APU upload time > this x the median over nodes
NC_AVAIL_PCT=90    # flag: MemAvailable < this % of the median over nodes

nodechk_table() {   # nodechk_table <raw log> -- appends the per-node table and the flags to the summary
    grep -a '^NODECHK ' "$1" | awk -v slowx="$NC_SLOW_X" -v apct="$NC_AVAIL_PCT" -v flagfile="$FLAGGED" '
        function med(a, n,   i, j, t, b) { for (i = 1; i <= n; i++) b[i] = a[i]; for (i = 2; i <= n; i++) { t = b[i]; for (j = i - 1; j >= 1 && b[j] > t; j--) b[j+1] = b[j]; b[j+1] = t } return (n % 2) ? b[(n+1)/2] : (b[n/2] + b[n/2+1]) / 2 }
        { n++; host[n] = ""; err[n] = ""; up[n] = -1; for (i = 1; i <= NF; i++) { split($i, kv, "="); k = kv[1]; v = kv[2]
            if (k == "host") host[n] = v; else if (k == "ERROR") err[n] = v; else if (k == "MemTotal_kB") tot[n] = v; else if (k == "MemFree_kB") fr[n] = v
            else if (k == "MemAvailable_kB") av[n] = v; else if (k == "Cached_kB") ca[n] = v; else if (k == "HugePages_Total") ht[n] = v; else if (k == "HugePages_Free") hf[n] = v
            else if (k ~ /^apu[0-9]+_s$/) { ap[n] = ap[n] " " v; if (v == "ERR") err[n] = "apu"; else if (v + 0 > up[n]) up[n] = v + 0 } } }
        END {
            if (n == 0) { print "    no NODECHK lines (the program failed to run: see the log)"; exit }
            m = 0; q = 0
            for (i = 1; i <= n; i++) { if (err[i] == "") { m++; U[m] = up[i]; A[m] = av[i] } }
            if (m == 0) { print "    every node reported an error"; for (i = 1; i <= n; i++) printf "    %s: %s\n", host[i], err[i]; exit }
            mu = med(U, m); ma = med(A, m)
            printf "    %-16s %9s %9s %9s %9s %7s %7s  %-24s %s\n", "node", "MemTot GB", "MemFree", "MemAvail", "Cached", "HugeTot", "HugeFr", "upload s per APU", "flag"
            nf = 0
            for (i = 1; i <= n; i++) {
                fl = ""
                if (err[i] != "") fl = "FLAG(error " err[i] ")"
                else { if (mu > 0 && up[i] > slowx * mu) fl = fl "FLAG(upload " sprintf("%.1f", up[i] / mu) "x median) "; if (av[i] < apct / 100.0 * ma) fl = fl "FLAG(MemAvailable " sprintf("%.0f", 100.0 * av[i] / ma) "% of median)" }
                if (fl != "") { nf++; if (host[i] != "") print host[i] >> flagfile }
                printf "    %-16s %9.1f %9.1f %9.1f %9.1f %7d %7d  %-24s %s\n", host[i], tot[i] / 1e6, fr[i] / 1e6, av[i] / 1e6, ca[i] / 1e6, ht[i], hf[i], ap[i], fl
            }
            printf "    median over %d node(s): slowest-APU upload %.3f s, MemAvailable %.1f GB; flag rules: upload > %dx median or MemAvailable < %d%% of median; %d node(s) flagged\n", m, mu, ma / 1e6, slowx, apct, nf
            if (nf > 0) print "    -> consider excluding the flagged node(s) from the timed run (aac7: MNRUN_EXCLUDE_HOSTS); a low MemAvailable alone is not a stall (aac7 2026-10-09: 7 of 13 nodes at 441 GB, 6 at 520 GB, only one node stalled)"
        }' >> "$SUMMARY"
}

stage_nodechk() {
    local lf="$LOGDIR/06_nodechk.log"; : > "$lf"
    summary "## Stage nodechk (opt-in): per-node meminfo + ${NC_GB} GB host->device upload per APU (flag: upload > ${NC_SLOW_X}x median, MemAvailable < ${NC_AVAIL_PCT}% of median)"
    if [ -z "$JOBID" ] && [ "$DRY_RUN" != 1 ]; then summary "status: FAIL (no --jobid / SLURM_JOB_ID)"; summary ""; return; fi
    if ! require_kit_modules nodechk "$lf"; then summary "status: FAIL ($KIT_MODULES_ENV missing -- run stage build first, e.g. --only build,nodechk)"; summary ""; return; fi
    if [ "$DRY_RUN" != 1 ] && [ ! -x "$SELF_DIR/tests/t_nodechk" ]; then summary "status: FAIL (tests/t_nodechk not built -- run stage build first)"; summary ""; return; fi
    local alloc pre rc xopt=(); alloc=$(alloc_node_count); [ "${alloc:-0}" -ge 1 ] 2>/dev/null || alloc=1
    : > "$FLAGGED"   # this run decides the flagged list afresh
    local exin; exin=$(excl_in_alloc | paste -sd, -)
    if [ -n "$exin" ]; then xopt=(-x "$exin"); alloc=$((alloc - $(echo "$exin" | tr , '\n' | grep -c .))); summary "  excluded from the probe (--exclude-hosts): $exin"; fi
    pre=$(module_preamble); kit_gres
    summary "  nodes probed: $alloc (every usable node of the allocation, one process each, run alone)"
    exec_cmd "$lf" "$TO_NC" srun --jobid="$JOBID" --export=ALL -N"$alloc" --ntasks="$alloc" --ntasks-per-node=1 ${GRES_OPT[@]+"${GRES_OPT[@]}"} --overlap ${xopt[@]+"${xopt[@]}"} \
        bash -lc "$pre"'exec "$@"' _ "$SELF_DIR/tests/t_nodechk" "$NC_GB" 3 "$NC_COLD_GB"
    rc=$?
    if [ "$DRY_RUN" = 1 ]; then summary "status: DRY-RUN (not executed)"; summary ""; return; fi
    nodechk_table "$lf"
    local nfl; nfl=$(grep -c . "$FLAGGED" 2>/dev/null); nfl=${nfl:-0}
    if [ "$nfl" -gt 0 ]; then
        summary "  flagged hosts ($nfl): $(paste -sd, - < "$FLAGGED")  -> later stages in this --out drop them automatically; for a separate launch use --exclude-hosts $(paste -sd, - < "$FLAGGED")"
        if [ "$SPARES" -ge 0 ]; then
            if [ "$nfl" -gt "$SPARES" ]; then summary "  SPARES: STOP -- $nfl flagged > $SPARES spare(s): ask for replacement nodes before the timed run"
            else summary "  SPARES: OK -- $nfl flagged <= $SPARES spare(s); exclude them and continue (the probe cannot see the first-touch stall: also run stage initchk on the first run)"; fi
        fi
    elif [ "$SPARES" -ge 0 ]; then summary "  SPARES: no node flagged (the known first-touch stall is invisible to this probe: stage initchk after the first run)"; fi
    summary "  status: $([ $rc -eq 0 ] && echo PASS || echo "FAIL (exit $rc)"); raw NODECHK lines: $lf"
    summary ""
}

# =============================================================================================================
# main
# =============================================================================================================
stage_enabled env   && stage_env
stage_enabled build && stage_build
stage_enabled uffd  && stage_uffd
stage_enabled plan  && stage_plan
stage_enabled edge  && stage_edge
stage_optin nodechk && stage_nodechk   # before a3/a4: the nodes it flags are dropped by every later stage in this --out
stage_enabled a3    && stage_a3
stage_enabled a4    && stage_a4
stage_enabled initchk && stage_initchk
stage_optin s2chk   && stage_s2chk

summary "## Done"
summary "Send back: $SUMMARY"
echo "target_kit.sh: wrote $SUMMARY"

#!/bin/bash
# rundriver.sh - one reviewed library for aac7 fire-and-forget drivers (s18w task 1). Source it from a small
# per-task script; it does not itself run anything until the per-task script calls its functions.
#
# Lessons folded in from 2026-10-06 (~/s18ab3/drive.sh on aac7, read via
# `sshpass -p <cluster-password> ssh -o ConnectTimeout=20 chcoppola@aac7.amd.com`, read-only there):
#   - a segfaulted rank left a step HUNG for 25 min unnoticed -- a wall timeout alone is not enough.
#     rd_run's watchdog scans the running step's own log for crash patterns every 20s, not just the clock.
#   - s18ab2's driver used a 1x-expected-wall timeout and killed a HEALTHY run that was just slow-writing
#     to a near-full NFS home. rd_run's timeout is max(2x expected, expected + 900s), the same rule s18ab3
#     adopted, and the watchdog also treats "no log growth for STALL_S (default 900s) while the step is
#     still running" as its own, separate stall condition -- the two give a real hang two independent ways
#     to be caught (crash text OR silence) without re-introducing a too-tight wall timeout.
#   - a false DIFFERS in s18ab's step1 crash test traced back to a hand-made reference file, not a real
#     digit mismatch. rd_verify() never substitutes or builds a reference of its own: it only reports what
#     the ecalc log itself already says (VERIFY OK / VERIFY FAILED / neither), so this library cannot be
#     the source of a false mismatch.
#   - the NFS home is 96% full. rd_disk() is a free-space guard the caller runs before anything that would
#     write a large file there (part files, checkpoints).
#
# Safety: rd_run NEVER calls scancel and NEVER touches a Slurm job as a whole -- it only ever signals the
# one PID it itself started (SIGTERM, 60s grace, then SIGKILL), exactly as ~/s18ab3/drive.sh's run_killable
# did. rd_health uses `srun --overlap` (never cancels, never allocates) to look at nodes already allocated
# to the job the caller names; it is read-only.
#
# ---------------------------------------------------------------------------------------------------------
# API
#   rd_init <outdir> <marker-name>
#       Creates <outdir>/log, a <outdir>/summary.txt, and an EXIT trap that writes <outdir>/<marker-name>
#       with the final verdict ("SUCCESS" or "FAILED: <reason>", set by the caller in $RD_VERDICT before
#       it exits, or left at the "driver ended unexpectedly" default if the script dies some other way).
#       All rd_say output (Eastern time, TZ=America/New_York) goes to both the summary file and stderr.
#
#   rd_run <label> <expected_s> <cmd...>
#       Runs <cmd...> in the background, its stdout+stderr to <outdir>/log/<label>.log. A watchdog loop
#       (every 20s) ends the step and returns non-zero the first time any of these is true:
#         - the log matches the crash regex (Segmentation fault, core dumped, an srun error exit/Killed,
#           CANCELLED, fatal, abort);
#         - the log has not grown for STALL_S seconds (env, default 900) while the command is still alive;
#         - the wall clock reaches max(2 * expected_s, expected_s + 900).
#       On any of those it kills the command's PID (SIGTERM, 60s grace, then SIGKILL -- never scancel,
#       never any other job's PID) and writes <outdir>/ALERT (label, reason, last 20 log lines). On a plain
#       exit (the usual case) it returns the command's own exit code and does not touch ALERT.
#       Sets RD_LAST_RC, RD_LAST_WALL, RD_LAST_PID, RD_LAST_LOG after every call.
#
#   rd_health <jobid> <node> [<node> ...]
#       For each node (already allocated to <jobid>): over `srun --jobid=<jobid> -N1 -w <node> --overlap`,
#       checks there is no leftover ecalc or tests/t_* process of ours and that MemAvailable can be read.
#       Returns non-zero (and says which node) if any node is unhealthy.
#
#   rd_exclude_hosts <jobid>
#       Opt-in (MNRUN_EXCLUDE_HOSTS="h1,h2", default empty = off): warns if a listed slow host is in the allocation; with
#       MNRUN_EXCLUDE_MODE=drop prints the remaining hosts (comma list, for MNRUN_NODELIST) and returns 3. mnrun.sh honours the same variables.
#
#   rd_disk <path> <min_GB>
#       Free-space guard: non-zero if <path>'s filesystem has fewer than <min_GB> GB available.
#
#   rd_verify <log>
#       Prints "VERIFY OK" and returns 0 if an ecalc log says so ("VERIFY OK" or "all N nodes: VERIFY OK");
#       otherwise prints "VERIFY FAILED: <first matching error line, or 'no VERIFY OK line found'>" and
#       returns 1. Never builds or compares against a reference of its own (the s18ab lesson above).
#
#   rd_peaks <log> [<layout-file>]
#       Prints tools/parse_mem.py's per-node peak-memory lines for <log> (MEM_REPORT_DEVS mem[] blocks);
#       if <layout-file> is given and has a `vslot:` line (a BS_LAYOUT_ONLY / MN_PLAN_ONLY run), prints
#       that too, for comparing the measured peak against the modelled layout.
# ---------------------------------------------------------------------------------------------------------

RD_SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RD_WATCHDOG_PERIOD=20
RD_CRASH_RE='Segmentation fault|core dumped|srun: error: .*(Exited with exit code [1-9]|Killed)|CANCELLED|fatal|abort'

RD_OUTDIR=""
RD_MARKER=""
RD_SUMMARY=""
RD_VERDICT="FAILED: driver ended unexpectedly"
RD_LAST_PID=""
RD_LAST_RC=""
RD_LAST_WALL=""
RD_LAST_LOG=""

rd_now() { TZ=America/New_York date '+%Y-%m-%d %H:%M:%S %Z'; }

rd_say() {
    local line="$(rd_now) $*"
    echo "$line" >&2
    [ -n "$RD_SUMMARY" ] && echo "$line" >> "$RD_SUMMARY"
}

rd_finish() {
    [ -n "$RD_MARKER" ] || return 0
    echo "$RD_VERDICT" > "$RD_MARKER"
    rd_say "rd_finish: $RD_VERDICT"
}

# rd_init <outdir> <marker-name>
rd_init() {
    local outdir=$1 marker=$2
    RD_OUTDIR="$outdir"
    RD_MARKER="$outdir/$marker"
    mkdir -p "$RD_OUTDIR/log"
    RD_SUMMARY="$RD_OUTDIR/summary.txt"
    : > "$RD_SUMMARY"
    RD_VERDICT="FAILED: driver ended unexpectedly"
    trap 'rd_finish' EXIT
    rd_say "rd_init: outdir=$RD_OUTDIR marker=$RD_MARKER"
}

# rd_run <label> <expected_s> <cmd...>
rd_run() {
    local label=$1 expected_s=$2; shift 2
    local log="$RD_OUTDIR/log/$label.log"
    RD_LAST_LOG="$log"
    local timeout_s=$(( expected_s * 2 > expected_s + 900 ? expected_s * 2 : expected_s + 900 ))
    : > "$log"

    ( exec "$@" ) >> "$log" 2>&1 &
    local pid=$!
    RD_LAST_PID=$pid
    local t0=$SECONDS waited=0 last_size=-1 stall=0 reason=""

    while kill -0 "$pid" 2>/dev/null; do
        sleep "$RD_WATCHDOG_PERIOD"
        waited=$((waited + RD_WATCHDOG_PERIOD))
        if ! kill -0 "$pid" 2>/dev/null; then break; fi

        if grep -aqE "$RD_CRASH_RE" "$log" 2>/dev/null; then
            reason="crash pattern matched in $label.log"
            break
        fi

        local sz
        sz=$(stat -c %s "$log" 2>/dev/null || wc -c < "$log" 2>/dev/null || echo 0)
        if [ "$sz" = "$last_size" ]; then
            stall=$((stall + RD_WATCHDOG_PERIOD))
        else
            stall=0
        fi
        last_size=$sz
        local stall_s=${STALL_S:-900}
        if [ "$stall" -ge "$stall_s" ]; then
            reason="no log growth for ${stall_s}s (STALL_S)"
            break
        fi

        if [ "$waited" -ge "$timeout_s" ]; then
            reason="hard timeout ${timeout_s}s (max(2x expected $expected_s, expected+900))"
            break
        fi
    done

    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid" 2>/dev/null
        RD_LAST_RC=$?
        RD_LAST_WALL=$((SECONDS - t0))
        rd_say "rd_run $label: finished rc=$RD_LAST_RC wall=${RD_LAST_WALL}s"
        return "$RD_LAST_RC"
    fi

    # the watchdog tripped (reason is set) and the command is still alive: kill it, never scancel, never
    # any PID but this one
    rd_say "rd_run $label: WATCHDOG: $reason -- killing PID $pid (SIGTERM, 60s grace, then SIGKILL)"
    kill -TERM "$pid" 2>/dev/null
    local w=0
    while kill -0 "$pid" 2>/dev/null && [ "$w" -lt 60 ]; do sleep 2; w=$((w + 2)); done
    if kill -0 "$pid" 2>/dev/null; then
        rd_say "rd_run $label: PID $pid still alive after 60s grace: SIGKILL"
        kill -KILL "$pid" 2>/dev/null
        sleep 1
    fi
    wait "$pid" 2>/dev/null
    RD_LAST_RC=$?
    RD_LAST_WALL=$((SECONDS - t0))
    {
        echo "label=$label reason=$reason pid=$pid rc=$RD_LAST_RC wall=${RD_LAST_WALL}s"
        echo "-- last 20 lines of $log --"
        tail -20 "$log" 2>/dev/null
    } > "$RD_OUTDIR/ALERT"
    rd_say "rd_run $label: ALERT written ($RD_OUTDIR/ALERT): $reason"
    return 1
}

# rd_exclude_hosts <jobid> -- opt-in slow-host check. MNRUN_EXCLUDE_HOSTS="h1,h2" (default empty: does nothing). If a listed host is in
# the allocation, rd_say-logs a WARNING (default MNRUN_EXCLUDE_MODE=warn) and returns 0; with MNRUN_EXCLUDE_MODE=drop it also prints the
# allocation's remaining hosts, comma-separated, on stdout for the caller to pass as MNRUN_NODELIST (rd_say goes to stderr/summary only) and
# returns 3. Returns 0 and prints nothing when the list is empty or no listed host is allocated. Read-only (squeue/scontrol).
rd_exclude_hosts() {
    local jid=$1 all hit
    [ -n "${MNRUN_EXCLUDE_HOSTS:-}" ] || return 0
    all=$(scontrol show hostnames "$(squeue -j "$jid" -h -o %N)")
    hit=$(echo "$all" | grep -Fxf <(echo "$MNRUN_EXCLUDE_HOSTS" | tr , "\n") || true)
    [ -n "$hit" ] || return 0
    if [ "${MNRUN_EXCLUDE_MODE:-warn}" = drop ]; then
        rd_say "MNRUN_EXCLUDE_HOSTS: dropping $(echo $hit) from the node list of job $jid"
        echo "$all" | grep -Fvxf <(echo "$MNRUN_EXCLUDE_HOSTS" | tr , "\n") | paste -sd, -
        return 3
    fi
    rd_say "WARNING: MNRUN_EXCLUDE_HOSTS host(s) in job $jid: $(echo $hit) (MNRUN_EXCLUDE_MODE=warn: kept; =drop removes them)"
    return 0
}

# rd_health <jobid> <node> [<node> ...]
rd_health() {
    local jobid=$1; shift
    local bad=0 nd
    for nd in "$@"; do
        local out
        out=$(timeout 60 srun --jobid="$jobid" -N1 -w "$nd" --overlap bash -c '
            n=$(pgrep -u "$USER" -f "(^|/)(ecalc|t_[A-Za-z0-9_]+)( |$)" | wc -l)
            avail=$(awk "/MemAvailable/{print \$2}" /proc/meminfo)
            echo "n=$n avail=$avail"
        ' 2>&1)
        rd_say "rd_health $nd: $out"
        local nproc avail
        nproc=$(echo "$out" | grep -oE 'n=[0-9]+' | cut -d= -f2)
        avail=$(echo "$out" | grep -oE 'avail=[0-9]+' | cut -d= -f2)
        if [ "${nproc:-1}" != "0" ] || [ -z "${avail:-}" ]; then
            rd_say "rd_health: $nd UNHEALTHY ($out)"
            bad=1
        fi
    done
    return $bad
}

# rd_disk <path> <min_GB>
rd_disk() {
    local path=$1 min_gb=$2 avail
    avail=$(df --output=avail -B1G "$path" 2>/dev/null | tail -1 | tr -d ' ')
    if [ -z "$avail" ] || [ "$avail" -lt "$min_gb" ]; then
        rd_say "rd_disk: $path has ${avail:-?}GB free, need >= ${min_gb}GB"
        return 1
    fi
    rd_say "rd_disk: $path has ${avail}GB free (>= ${min_gb}GB)"
    return 0
}

# rd_verify <log>
rd_verify() {
    local log=$1
    if grep -aqE 'VERIFY OK|all [0-9]+ nodes: VERIFY OK' "$log" 2>/dev/null; then
        echo "VERIFY OK"
        return 0
    fi
    local why
    why=$(grep -aE 'VERIFY FAILED|abort|error|Killed|Segmentation fault' "$log" 2>/dev/null | head -1)
    echo "VERIFY FAILED: ${why:-no VERIFY OK line found in $log}"
    return 1
}

# rd_peaks <log> [<layout-file>]
rd_peaks() {
    local log=$1 layout=${2:-}
    python3 "$RD_SELF_DIR/parse_mem.py" "$log"
    if [ -n "$layout" ] && [ -f "$layout" ]; then
        grep -m1 '^vslot:' "$layout" 2>/dev/null
    fi
}

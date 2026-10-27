#!/bin/bash
# alloc.sh - hold ONE session allocation (an sbatch sleep job) that every other stage script runs inside (no new queue waits).
#   alloc.sh --nodes N [--time H:MM:SS] [--partition P] [--account A] [--exclude LIST] [--wait SEC] [--dry-run]
#   alloc.sh --status            state of the recorded job (REMOTE_JOB in site.env) and its nodes
#   alloc.sh --release           cancel the recorded job (only a job this script created, checked by user and name)
# Writes REMOTE_JOB=<id> (and REMOTE_NODES, REMOTE_PARTITION, REMOTE_ACCOUNT) into $REMOTE_OUT/site.env.  --time default 4:00:00, --wait default 300 s:
# when the job has not started by then the verdict is PARTIAL (PENDING) and next= is the re-check command (alloc.sh --status).
# --time: Slurm forms MM, MM:SS, HH:MM:SS, D-HH, D-HH:MM, D-HH:MM:SS.  A recorded live job of ours is reused when it has the asked size; a
# PENDING one of another size is cancelled and replaced; a RUNNING one of another size is refused (alloc.sh --release first).
# Environment: REMOTE_OUT (default $HOME/remote_runs), REMOTE_SITE=aac7 (rehearsal), REMOTE_PARTITION, REMOTE_ACCOUNT.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
NODES= TLIM=4:00:00 PART= ACCT= EXCLUDE= WAIT=300 MODE=alloc
while [ $# -gt 0 ]; do case $1 in
  --nodes) NODES=${2:?}; shift;; --time) TLIM=${2:?}; shift;; --partition) PART=${2:?}; shift;; --account) ACCT=${2:?}; shift;;
  --exclude) EXCLUDE=${2-}; shift;; --wait) WAIT=${2:?}; shift;; --status) MODE=status;; --release) MODE=release;; --dry-run) DRY=1;;
  -h|--help) rl_help "$0"; exit 0;; *) echo "alloc.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
if [ "$MODE" = alloc ]; then
    rl_posint "$NODES" || { echo "alloc.sh: --nodes N (a positive integer) required (--help)" >&2; exit 2; }
    NODES=$((10#$NODES)); secs=$(rl_tlim_secs "$TLIM") || { echo "alloc.sh: --time $TLIM: use MM, MM:SS, HH:MM:SS, D-HH, D-HH:MM or D-HH:MM:SS" >&2; exit 2; }
    rl_posint "$WAIT" || [ "$WAIT" = 0 ] || { echo "alloc.sh: --wait SEC (an integer)" >&2; exit 2; }
fi
rl_init alloc
PART=${PART:-${REMOTE_PARTITION:-}}; ACCT=${ACCT:-${REMOTE_ACCOUNT:-}}
NEXT_HINT="$HERE/alloc.sh --status"
rl_unused() { :; }

case $MODE in
status)
    rl_resolve_job || rl_finish SKIP "no REMOTE_JOB recorded" "$HERE/alloc.sh --nodes <N>"
    st=$(rl_job_state "$JOB"); rl_kv job "$JOB"; rl_kv state "${st:-gone}"
    if [ "$st" = RUNNING ]; then
        h=$(rl_hosts_all "$JOB" | paste -sd,); rl_kv nodes "$(echo "$h" | tr ',' '\n' | grep -c .)"; rl_kv nodelist "$(echo "$h" | cut -c1-300)"
        rl_kv time_left "$(rl_tmo 30 squeue -h -j "$JOB" -o %L 2>/dev/null | head -1)"
        rl_finish PASS "$HERE/s0_check.sh"
    elif [ "$st" = PENDING ]; then
        rl_kv reason "$(rl_tmo 30 squeue -h -j "$JOB" -o '%R' 2>/dev/null | head -1)"; rl_kv start_est "$(rl_tmo 30 squeue -h -j "$JOB" -o %S --start 2>/dev/null | head -1)"
        rl_finish PARTIAL "job still pending" "$HERE/alloc.sh --status"
    else rl_finish FAIL "job $JOB is not in the queue (ended or never started)" "$HERE/alloc.sh --nodes <N>"; fi;;
release)
    rl_resolve_job || rl_finish SKIP "no REMOTE_JOB recorded" "$HERE/alloc.sh --nodes <N>"
    nm=; rl_job_alive "$JOB" && nm=$(rl_tmo 30 squeue -h -j "$JOB" -o %j 2>/dev/null | head -1)
    if [ -z "$nm" ]; then rl_site_unset REMOTE_JOB; rl_finish SKIP "job $JOB already gone" "$HERE/alloc.sh --nodes <N>"; fi
    if ! rl_job_mine "$JOB" || [ "$nm" != remote_hold ]; then rl_finish FAIL "job $JOB is not ours (name '$nm'): not touched" "$HERE/alloc.sh --status"; fi
    [ "$DRY" = 1 ] && { rl_would "scancel $JOB"; rl_finish SKIP "$HERE/alloc.sh --release"; }
    rl_tmo 60 scancel "$JOB" >> "$RUN/log.txt" 2>&1; rl_site_unset REMOTE_JOB; rl_kv released "$JOB"
    rl_finish PASS "$HERE/alloc.sh --nodes <N>";;
alloc)
    ex=$(rl_exclude_list)
    cmd=(sbatch --parsable -N "$NODES" --exclusive --gpus-per-node=4 -t "$TLIM" -J remote_hold --no-requeue -o "$RUN/slurm_hold.out"
         ${PART:+-p "$PART"} ${ACCT:+-A "$ACCT"} ${ex:+-x "$ex"} --wrap "sleep $((secs + 30))")
    rl_kv nodes_requested "$NODES"; rl_kv time "$TLIM"; rl_kv partition "${PART:-<default>}"; [ -n "$ex" ] && rl_kv exclude "$ex"
    if [ "$DRY" = 1 ]; then rl_would "${cmd[*]}"; rl_would "poll squeue -j <id> until RUNNING (<= ${WAIT}s)"; rl_would "site.env: REMOTE_JOB=<id>"; rl_finish SKIP "$HERE/alloc.sh --nodes $NODES"; fi
    for c in sbatch squeue scontrol; do rl_have $c || rl_die "$c not found (not on a login node?)"; done
    # a still-recorded job of ours is reused (same size) instead of stacking a second one
    reuse=0
    if rl_resolve_job && rl_job_alive "$JOB" && rl_job_mine "$JOB" && [ "$(rl_tmo 30 squeue -h -j "$JOB" -o %j | head -1)" = remote_hold ]; then
        jst=$(rl_job_state "$JOB"); jn=$(rl_tmo 30 squeue -h -j "$JOB" -o %D 2>/dev/null | head -1); jn=${jn:-${REMOTE_NODES:-$NODES}}
        if [ "$jn" = "$NODES" ]; then reuse=1
        elif [ "$jst" = PENDING ]; then
            rl_say "recorded job $JOB is PENDING with $jn node(s), $NODES asked: cancelling it and submitting the new size"
            rl_tmo 60 scancel "$JOB" >> "$RUN/log.txt" 2>&1; rl_site_unset REMOTE_JOB; rl_kv replaced_pending_job "$JOB ($jn node(s)) -> $NODES node(s)"
        else rl_finish FAIL "recorded job $JOB is $jst with $jn node(s), $NODES asked: not touched" "$HERE/alloc.sh --release && $HERE/alloc.sh --nodes $NODES"; fi
    fi
    if [ $reuse = 1 ]; then
        rl_say "recorded job $JOB is still alive ($NODES node(s)): reusing it (alloc.sh --release first to replace it)"
        rl_kv reused 1
    else
        out=$(rl_tmo 120 "${cmd[@]}" 2>"$RUN/raw/sbatch.err") || { cat "$RUN/raw/sbatch.err" >> "$RUN/log.txt"; RL_ERRFILES+=("$RUN/raw/sbatch.err"); rl_finish FAIL "sbatch refused" "$HERE/alloc.sh --nodes $NODES --partition <name>"; }
        JOB=${out%%;*}; rl_isnum "$JOB" || { RL_ERRFILES+=("$RUN/raw/sbatch.err"); rl_finish FAIL "no job id from sbatch: $out" "$HERE/s0_check.sh"; }
        rl_site_set REMOTE_JOB "$JOB"; rl_site_set REMOTE_NODES "$NODES"; [ -n "$PART" ] && rl_site_set REMOTE_PARTITION "$PART"; [ -n "$ACCT" ] && rl_site_set REMOTE_ACCOUNT "$ACCT"
        [ -n "$EXCLUDE" ] && rl_site_set REMOTE_EXCLUDE "$EXCLUDE"
        rl_say "submitted job $JOB, waiting up to ${WAIT}s for it to start"
    fi
    t0=$SECONDS; st=
    while :; do
        st=$(rl_job_state "$JOB"); [ "$st" = RUNNING ] && break
        case $st in PENDING|CONFIGURING) ;; *) break;; esac
        [ $((SECONDS - t0)) -ge "$WAIT" ] && break
        sleep 10
    done
    rl_kv job "$JOB"; rl_kv state "${st:-gone}"
    case $st in
    RUNNING) hs=$(rl_hosts_all "$JOB" | paste -sd,); rl_kv nodes "$(echo "$hs" | tr ',' '\n' | grep -c .)"; rl_kv nodelist "$(echo "$hs" | cut -c1-300)"
             rl_kv waited_s "$((SECONDS - t0))"; rl_kv time_left "$(rl_tmo 30 squeue -h -j "$JOB" -o %L | head -1)"
             rl_kv release "$HERE/alloc.sh --release"
             rl_finish PASS "$HERE/s0_check.sh";;
    PENDING) rl_kv reason "$(rl_tmo 30 squeue -h -j "$JOB" -o %R | head -1)"; rl_kv start_est "$(rl_tmo 30 squeue -h -j "$JOB" --start -h -o %S | head -1)"
             rl_kv PENDING "job $JOB has not started after ${WAIT}s (it stays queued; it is recorded as REMOTE_JOB)"
             rl_finish PARTIAL "PENDING" "$HERE/alloc.sh --status";;
    *)       rl_finish FAIL "job $JOB ended before it ran (see $RUN/slurm_hold.out)" "$HERE/alloc.sh --nodes $NODES";;
    esac;;
esac

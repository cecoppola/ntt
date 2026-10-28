#!/bin/bash
# chain.sh - run several stage scripts in ONE paste, in order; stops at the first FAIL, prints every summary produced so far.
#   chain.sh "<script> <args>" "<script> <args>" ... [--dry-run]
# Each quoted string is one call, resolved relative to remote/run/ ("s3_gate.sh --nodes 1,2", "s3 --nodes 1", "s3_gate" all work); quotes inside
# a string are honoured (s4_ab.sh --a "K=V L=W").  --dry-run (anywhere) is passed to every call.  Every script keeps its own bounded summary
# block; at the end a short chain table lists the verdicts.  Exit status: 0 when no call FAILED, 1 on the first FAIL, 2 usage.
#   example: chain.sh "alloc.sh --nodes 4 --time 3:00:00" "s1_facts.sh" "s2_build.sh" "s3_gate.sh --nodes 1,2"
# --brief: the calls' own output goes to $RUN/raw/call<N>_*.out only (a progress line per call on the terminal); the chain summary then carries,
#   per call, its verdict line and the key lines of its summary (all of it, capped, for a call that did not PASS): ONE compact block to paste back.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
CALLS=(); CDRY=0; BRIEF=0
for a in "$@"; do case $a in -h|--help) rl_help "$0"; exit 0;; --dry-run) CDRY=1;; --brief) BRIEF=1;; *) CALLS+=("$a");; esac; done
[ ${#CALLS[@]} -gt 0 ] || { echo "chain.sh: give at least one quoted \"script args\" (--help)" >&2; exit 2; }
[ $CDRY = 1 ] && DRY=1
resolve() {     # NAME -> script path
    local n=$1; [ -f "$HERE/$n" ] && { echo "$HERE/$n"; return 0; }
    [ -f "$HERE/$n.sh" ] && { echo "$HERE/$n.sh"; return 0; }
    local m; m=$(ls "$HERE/$n"*.sh 2>/dev/null | grep -v -E '/(lib|chain|show|mkmanifest)\.sh$' | head -1); [ -n "$m" ] && { echo "$m"; return 0; }; return 1
}
# validate every call before anything runs
i=0; for c in "${CALLS[@]}"; do
    eval "set -- $c" 2>/dev/null || { echo "chain.sh: cannot parse call $((i + 1)): $c" >&2; exit 2; }
    [ $# -gt 0 ] && resolve "$1" > /dev/null || { echo "chain.sh: call $((i + 1)): no script '${1:-}' in $HERE" >&2; exit 2; }
    i=$((i + 1)); done
rl_init chain
rows=(); worst=PASS; n=0; stop=; declare -A VERD
for c in "${CALLS[@]}"; do
    n=$((n + 1)); eval "set -- $c"; s=$(resolve "$1"); shift
    [ $CDRY = 1 ] && set -- "$@" --dry-run
    rl_say "call $n/${#CALLS[@]}: $(basename "$s") $*"
    of="$RUN/raw/call${n}_$(basename "$s" .sh).out"; c0=$SECONDS
    if [ $BRIEF = 1 ]; then bash "$s" "$@" > "$of" 2>&1 < /dev/null; rc=$?; else bash "$s" "$@" 2>&1 | tee "$of"; rc=${PIPESTATUS[0]}; fi
    v=$(grep -a -m1 -o 'verdict=[A-Z]*' "$of" | tail -1 | cut -d= -f2); [ -n "$v" ] || { [ $rc = 0 ] && v=PASS || v=FAIL; }
    rows+=("$(printf '%d %-14s %-8s rc=%s %ss' "$n" "$(basename "$s")" "$v" "$rc" "$((SECONDS - c0))")")
    [ $BRIEF = 1 ] && rl_say "call $n done: $(basename "$s") $v rc=$rc ($((SECONDS - c0)) s)"
    VERD[$n]=$v
    case $v in FAIL) worst=FAIL; stop=$n;; PARTIAL) [ $worst = PASS ] && worst=PARTIAL;; esac
    [ $rc != 0 ] && { worst=FAIL; stop=$n; }
    [ -n "$stop" ] && break
done
rl_line "calls (n script verdict rc time):"; for r in "${rows[@]}"; do rl_line "  $r"; done
if [ $BRIEF = 1 ]; then      # the key lines of each call's summary (budget: about 62 lines for all calls)
    per=$(( 54 / n )); [ $per -gt 13 ] && per=13; [ $per -lt 6 ] && per=6
    KEY='^(note|nodes|nodelist|job|state|time_left|waited_s|digits|path|rc|verify|total_s|phases_s|meas_over_model|device_peak_apu_gb|device_plan_apu_gb|fit|slow_hosts|screen|last_digits|digit_hash|gpu_count|nics_cxi|shmem_test_rc|manifest[a-z_]*|tools_missing[a-z_]*|carry_on|built[a-z_0-9]*|gate|userfaultfd[a-z_]*|partition|gres_gpu|advisory|PENDING|reason|errors|pass|fail)[=:]|^  [12] [0-9]+ |^  (1|2) +[0-9]+ '
    i=0; for c in "${CALLS[@]}"; do i=$((i + 1)); [ $i -le $n ] || break
        f=$(ls "$RUN"/raw/call${i}_*.out 2>/dev/null | head -1); [ -f "$f" ] || continue
        sf=$(sed -n 's/.* run dir //p' "$f" | head -1); sf=${sf/#\~/$HOME}/summary.txt; [ -f "$sf" ] || sf=$f
        rl_line "-- call $i: $(echo "$c" | cut -c1-110) -> ${VERD[$i]:-?}"
        if [ "${VERD[$i]:-}" = FAIL ]; then
            kl=$(sed -n '/^=== REMOTE/,/^=== END/p' "$sf" | grep -a -v -E '^(=== |stage=)' | cut -c1-170 | head -n $((per + 14)))
        else      # PASS / PARTIAL / SKIP: the key lines, then (not PASS) the lines that say what is off
            kl=$(sed -n '/^=== REMOTE/,/^=== END/p' "$sf" | grep -a -E "$KEY" | grep -a -v '^errors' | cut -c1-170 | head -n "$per")
            [ "${VERD[$i]:-}" = PASS ] || kl=$(printf '%s\n%s' "$kl" "$(sed -n '/^=== REMOTE/,/^=== END/p' "$sf" | grep -a -E 'note=|advisory|differs|_missing|=none|PENDING|reason=|exec_bits=|next=' | grep -a -v -E "$KEY" | cut -c1-170 | head -n 6)" | grep -a .)
        fi
        [ -n "$kl" ] && while IFS= read -r l; do rl_line "   $l"; done <<< "$kl"
    done
fi
[ $n -lt ${#CALLS[@]} ] && rl_kv not_run "$(( ${#CALLS[@]} - n )) call(s) after the failed one"
rl_finish "$worst" "$([ "$worst" = FAIL ] && echo "$HERE/show.sh latest errors" || echo "(chain done) $HERE/show.sh latest")"

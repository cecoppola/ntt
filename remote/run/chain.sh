#!/bin/bash
# chain.sh - run several stage scripts in ONE paste, in order; stops at the first FAIL, prints every summary produced so far.
#   chain.sh "<script> <args>" "<script> <args>" ... [--dry-run]
# Each quoted string is one call, resolved relative to remote/run/ ("s3_gate.sh --nodes 1,2", "s3 --nodes 1", "s3_gate" all work); quotes inside
# a string are honoured (s4_ab.sh --a "K=V L=W").  --dry-run (anywhere) is passed to every call.  Every script keeps its own bounded summary
# block; at the end a short chain table lists the verdicts.  Exit status: 0 when no call FAILED, 1 on the first FAIL, 2 usage.
#   example: chain.sh "alloc.sh --nodes 4 --time 3:00:00" "s1_facts.sh" "s2_build.sh" "s3_gate.sh --nodes 1,2"
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
CALLS=(); CDRY=0
for a in "$@"; do case $a in -h|--help) rl_help "$0"; exit 0;; --dry-run) CDRY=1;; *) CALLS+=("$a");; esac; done
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
rows=(); worst=PASS; n=0; stop=
for c in "${CALLS[@]}"; do
    n=$((n + 1)); eval "set -- $c"; s=$(resolve "$1"); shift
    [ $CDRY = 1 ] && set -- "$@" --dry-run
    rl_say "call $n/${#CALLS[@]}: $(basename "$s") $*"
    bash "$s" "$@" 2>&1 | tee "$RUN/raw/call${n}_$(basename "$s" .sh).out"; rc=${PIPESTATUS[0]}
    v=$(grep -a -m1 -o 'verdict=[A-Z]*' "$RUN/raw/call${n}_$(basename "$s" .sh).out" | tail -1 | cut -d= -f2); [ -n "$v" ] || { [ $rc = 0 ] && v=PASS || v=FAIL; }
    rows+=("$(printf '%d %-14s %-8s rc=%s' "$n" "$(basename "$s")" "$v" "$rc")")
    case $v in FAIL) worst=FAIL; stop=$n;; PARTIAL) [ $worst = PASS ] && worst=PARTIAL;; esac
    [ $rc != 0 ] && { worst=FAIL; stop=$n; }
    [ -n "$stop" ] && break
done
rl_line "calls (n script verdict rc):"; for r in "${rows[@]}"; do rl_line "  $r"; done
[ $n -lt ${#CALLS[@]} ] && rl_kv not_run "$(( ${#CALLS[@]} - n )) call(s) after the failed one"
rl_finish "$worst" "$([ "$worst" = FAIL ] && echo "$HERE/show.sh latest errors" || echo "(chain done) $HERE/show.sh latest")"

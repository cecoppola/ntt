#!/bin/bash
# arms.sh - several arms AT ONCE in one allocation: K arms of N nodes each run concurrently (each arm = one s5_run.sh on its own node subset).
#   arms.sh --nodes N --digits D --path 1|2 --arm "NAME:K=V ..." [--arm "NAME:K=V ..."]... [--tmo MIN] [--jobid J] [--exclude LIST]
#           [--stagger SEC] [--cmd "SHELL COMMAND"] [--dry-run]
# --arm NAME:WORDS  one arm: its label and its settings words (K=V, may be empty: --arm "A:").  The arms take consecutive blocks of N usable hosts
# of the allocation (so the allocation must have at least N x arms usable nodes); excluded hosts are left out as in s5_run.sh.  Every arm is
# `s5_run.sh --nodes N --digits D --path P --env WORDS --hosts <block>` started at once (--stagger SEC: SEC seconds apart); the script waits
# for all and prints ONE table: arm, nodes, settings, rc, total_s, init_s, verify, last_digits, run dir.  Verdict FAIL when an arm fails.
# --cmd runs a shell command per arm instead of ecalc (test mode for allocations without NICs; the command sees ARM_NAME ARM_NODES ARM_HOSTS
# ARM_WORDS, its output is the arm's row text, rc 0 = PASS).  Each arm runs in its own process group; INT/TERM/EXIT kill the arms still running
# (their srun steps too) and the table marks them KILLED.  The arms start their run dirs within the same second: `s5_latest`/`latest` point at
# whichever arm started last, so use the run dir printed in each table row (and raw/arm_<name>.out), never `latest`.  Nothing else changes: s5_run.sh behaves as before when called alone.
#   example (two 4-node arms side by side in an 8-node allocation, path 2):
#     arms.sh --nodes 4 --digits 1.6e11 --path 2 --arm "A:" --arm "B:XSHMEM_SPARSE=1"
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
NN= DIG= P= TMO= JOB= EXCLUDE= STAG=0 CMD=; ARMS=()
while [ $# -gt 0 ]; do case $1 in
  --nodes) NN=${2:?}; shift;; --digits) DIG=${2:?}; shift;; --path) P=${2:?}; shift;; --arm) ARMS+=("${2?}"); shift;; --tmo) TMO=${2:?}; shift;;
  --jobid) JOB=${2:?}; shift;; --exclude) EXCLUDE=${2-}; shift;; --stagger) STAG=${2:?}; shift;; --cmd) CMD=${2:?}; shift;; --dry-run) DRY=1;;
  -h|--help) rl_help "$0"; exit 0;; *) echo "arms.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
[ -n "$NN" ] && [ ${#ARMS[@]} -gt 0 ] || { echo "arms.sh: --nodes and at least one --arm are required (--help)" >&2; exit 2; }
[ -n "$CMD" ] || { [ -n "$DIG" ] && [ -n "$P" ] || { echo "arms.sh: --digits and --path are required (or --cmd) (--help)" >&2; exit 2; }; }
rl_posint "$NN" || { echo "arms.sh: --nodes a positive integer" >&2; exit 2; }
case $STAG in ''|*[!0-9]*) echo "arms.sh: --stagger whole seconds" >&2; exit 2;; esac
[ -n "$CMD" ] || { rl_isnum "$DIG" || { echo "arms.sh: --digits numeric" >&2; exit 2; }; case $P in 1|2) ;; *) echo "arms.sh: --path 1|2" >&2; exit 2;; esac; }
for a in "${ARMS[@]}"; do case $a in *:*) ;; *) echo "arms.sh: --arm needs NAME:WORDS (got '$a')" >&2; exit 2;; esac; done
NN=$((10#$NN)); NA=${#ARMS[@]}
[ "$(for a in "${ARMS[@]}"; do echo "${a%%:*}"; done | sort -u | wc -l)" = "$NA" ] || { echo "arms.sh: arm names must be distinct" >&2; exit 2; }
for a in "${ARMS[@]}"; do case ${a%%:*} in ''|*[!A-Za-z0-9_.-]*) echo "arms.sh: arm name '${a%%:*}': letters, digits, _ . - only" >&2; exit 2;; esac; done
rl_init arms
if [ "$DRY" = 1 ]; then
    i=0; for a in "${ARMS[@]}"; do i=$((i + 1))
        rl_would "arm ${a%%:*}: $([ -n "$CMD" ] && echo "cmd: $CMD" || echo "$HERE/s5_run.sh --nodes $NN --digits $DIG --path $P --env \"${a#*:}\" --hosts <hosts $(( (i - 1) * NN + 1 ))..$((i * NN)) of the allocation>${TMO:+ --tmo $TMO}") (concurrently with the other $((NA - 1)) arm(s))"; done
    rl_would "needs $((NN * NA)) usable nodes in the allocation"
    rl_finish SKIP "$HERE/arms.sh --nodes $NN --arms $NA"
fi
rl_conf; rl_resolve_job || rl_die "no allocation: run alloc.sh first (or pass --jobid J)"
rl_need_running "$JOB"
ALL=$(rl_usable_hosts "$JOB"); HN=$(echo "$ALL" | grep -c .)
[ "$HN" -ge $((NN * NA)) ] || rl_die "$NA arm(s) of $NN node(s) need $((NN * NA)) usable nodes, the allocation has $HN"
rl_kv job "$JOB"; rl_kv arms "$NA"; rl_kv nodes_per_arm "$NN"; rl_kv digits "${DIG:-NA}"; rl_kv path "${P:-NA}"
PIDS=(); i=0; KILLED=0
arms_kill() {   # kill every arm still running (own process group: the arm's s5_run, mnrun and srun steps), mark it
    local k n; for k in "${!PIDS[@]}"; do kill -0 "${PIDS[$k]}" 2>/dev/null || continue
        n=${ARMS[$k]%%:*}; : > "$RUN/raw/arm_$n.killed"; kill -TERM -- "-${PIDS[$k]}" 2>/dev/null; done
    [ -n "$(ls "$RUN"/raw/*.killed 2>/dev/null)" ] && { sleep 3; for k in "${!PIDS[@]}"; do kill -KILL -- "-${PIDS[$k]}" 2>/dev/null; done; }
    return 0
}
trap 'KILLED=1; arms_kill' INT TERM
trap 'arms_kill; rl_trap' EXIT
for a in "${ARMS[@]}"; do
    name=${a%%:*}; words=${a#*:}; blk=$(echo "$ALL" | sed -n "$((i * NN + 1)),$(((i + 1) * NN))p" | paste -sd,)
    out=$RUN/raw/arm_$name.out
    if [ -n "$CMD" ]; then
        ARM_NAME=$name ARM_NODES=$NN ARM_HOSTS=$blk ARM_WORDS=$words setsid bash -c 'bash -c "$1" > "$2" 2>&1; echo "rc=$?" >> "$2"' _ "$CMD" "$out" &
    else
        setsid bash "$HERE/s5_run.sh" --nodes "$NN" --digits "$DIG" --path "$P" --env "$words" --hosts "$blk" --jobid "$JOB" ${TMO:+--tmo "$TMO"} ${EXCLUDE:+--exclude "$EXCLUDE"} > "$out" 2>&1 < /dev/null &
    fi
    PIDS+=($!); rl_say "arm $name started on $blk"
    i=$((i + 1)); [ "$STAG" -gt 0 ] && [ $i -lt "$NA" ] && sleep "$STAG"
done
fails=0; i=0
rl_line "arm       nodes rc  total_s  init_s   verify  last_digits            bs/dm/dc_s        settings / run"
for a in "${ARMS[@]}"; do
    name=${a%%:*}; words=${a#*:}; out=$RUN/raw/arm_$name.out
    wait "${PIDS[$i]}"; rc=$?; wait "${PIDS[$i]}" 2>/dev/null; i=$((i + 1)); kd=; [ -f "$RUN/raw/arm_$name.killed" ] && { rc=KILLED; kd=" (killed)"; }
    if [ -n "$CMD" ]; then
        crc=$(sed -n 's/^rc=//p' "$out" | tail -1); rc=${crc:-$rc}
        rl_line "$(printf '%-9s %-5s %-3s %s | %s' "$name" "$NN" "$rc" "$(grep -av '^rc=' "$out" | head -1 | cut -c1-100)" "${words:-<defaults>}")"
    else
        rd=$(sed -n 's/.* run dir //p' "$out" | head -1); sf=$rd/summary.txt; [ -f "$sf" ] || sf=$out   # the arm's own summary.txt (the stdout copy is truncated for long summaries)
        kv() { grep -a -m1 "^$1=" "$sf" | cut -d= -f2-; }
        ph=$(grep -a -m1 '^phases_s:' "$sf" | sed -n 's/.*init=\([^ ]*\).*/\1/p')
        pp=$(grep -a -m1 '^phases_s:' "$sf" | sed -E 's/.*bs=([^ ]*) recip=[^ ]* dm=([^ ]*) dc=([^ ]*).*/\1 \2 \3/' | awk '{ if (NF == 3) printf "%s/%s/%s", $1, $2, $3; else printf "NA" }')
        rl_line "$(printf '%-9s %-5s %-3s %-8s %-8s %-7s %-22s %-17s %s | %s' "$name" "$NN" "$rc" "$(kv total_s)" "${ph:-NA}" "$(kv verify)" "$(kv last_digits | tr -d ' ' | cut -c1-22)" "${pp:-NA}" "${words:-<defaults>}${kd}" "${rd##*/}")"
        rl_kv "arm_$name" "rc=$rc total=$(kv total_s) init=${ph:-NA} bsdmdc=${pp:-NA} verify=$(kv verify) last=$(kv last_digits | tr -d ' ' | cut -c1-22) peak_apu_gb=$(kv device_peak_apu_gb | awk '{ print ($1 == "" ? "NA" : $1) }') run=${rd##*/}"
    fi
    [ "$rc" = 0 ] || fails=$((fails + 1))
done
rl_kv failed_arms "$fails of $NA"
if [ $fails = 0 ]; then rl_finish PASS "$HERE/show.sh $RUN"; else rl_finish FAIL "$fails of $NA arm(s) failed (see raw/arm_*.out)" "$HERE/show.sh $RUN"; fi

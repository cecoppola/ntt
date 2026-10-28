#!/bin/bash
# stair.sh - a staircase of runs in ONE paste: the steps in order, stop at the first failure, ONE table (<= 80 lines).
#   stair.sh --path 1|2 --step "NODES|DIGITS|ENV|EXTRA" [--step ...] [--budget MIN] [--min-margin GB] [--exclude LIST] [--jobid J] [--dry-run]
# Each step is one s5_run.sh call: NODES, DIGITS (64e9, 4.608e12 or an integer), ENV = settings words (K=V ..., may be empty), EXTRA = more s5_run.sh
# options (--screen, --write DIR, --tmo MIN; may be empty).  Example (the first record run):
#   stair.sh --path 1 --step "576|10e12|BS_MDEV_LOGL=29 XSHMEM_HANG_S=3600|"
# A step runs only while the allocation has time for it (its tmo + 5 min, from squeue) and the --budget (minutes for the whole call; default none)
# is not used up; a skipped step ends the staircase with verdict PARTIAL.  Any failure (planner refusal, rc, VERIFY, digits mismatch, timeout) stops at
# once: no automatic retry and no automatic smaller size (the agent reads the errors and picks the fallback; a refusal prints largest_fit_digits).  Per step:
# digits, verdict, total_s, init/bs/dm/dc, measured/model, per-APU device peak (measured) / plan, VERIFY, the last digits, the digit hash if taken.
# Memory rule (--min-margin GB, default 6.5, 0 = off): before a step the planner is asked (with the step's ENV, so ECALC_FIT_PER_APU=1 in it makes
# it print the busiest APU's margin to the 93.36 GB edge); a step whose margin is below the need is SKIPPED, not run.  The need is 6.5 GB (the largest
# excess of the measured per-APU peak over the plan seen on the target) until a step has been measured; after that it is that run's own excess
# (device_peak_apu_gb - plan, the largest so far) + 2 GB, but at least 3 GB: each passed step closes the memory gap for the next one.
# Verdict PASS all steps passed; PARTIAL some step skipped (budget, allocation time, memory rule) and the rest passed; FAIL a step failed.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
PATHN= STEPS=() BUDGET= JOB= EXCLUDE= MINM=6.5
while [ $# -gt 0 ]; do case $1 in
  --path) PATHN=${2:?}; shift;; --step) STEPS+=("${2:?}"); shift;; --budget) BUDGET=${2:?}; shift;; --min-margin) MINM=${2:?}; shift;; --jobid) JOB=${2:?}; shift;; --exclude) EXCLUDE=${2-}; shift;;
  --dry-run) DRY=1;; -h|--help) rl_help "$0"; exit 0;; *) echo "stair.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
case $PATHN in 1|2) ;; *) echo "stair.sh: --path 1|2 required (--help)" >&2; exit 2;; esac
[ ${#STEPS[@]} -gt 0 ] || { echo "stair.sh: give at least one --step \"NODES|DIGITS|ENV|EXTRA\" (--help)" >&2; exit 2; }
[ -z "$BUDGET" ] || rl_posint "$BUDGET" || { echo "stair.sh: --budget MIN (a positive integer)" >&2; exit 2; }
SN=() SD=() SE=() SX=()
for s in "${STEPS[@]}"; do
    IFS='|' read -r n d e x _ <<< "$s||||"
    rl_posint "$n" && rl_isnum "$d" || { echo "stair.sh: step '$s': NODES a positive integer and DIGITS numeric (NODES|DIGITS|ENV|EXTRA)" >&2; exit 2; }
    SN+=("$((10#$n))"); SD+=("$d"); SE+=("$e"); SX+=("$x")
done
rl_init stair
TOTAL=${#STEPS[@]}
rl_kv path "$PATHN"; rl_kv steps "$TOTAL"${BUDGET:+" budget_min=$BUDGET"}
if [ "$DRY" = 1 ]; then
    for i in "${!SN[@]}"; do rl_would "step $((i + 1)): s5_run.sh --nodes ${SN[$i]} --digits ${SD[$i]} --path $PATHN --env \"${SE[$i]}\" ${SX[$i]} ${EXCLUDE:+--exclude $EXCLUDE }(stop at the first failure)"; done
    rl_finish SKIP "$HERE/stair.sh --path $PATHN"
fi
[ -z "$JOB" ] && { rl_resolve_job && JOB=${JOB:-}; }
left_s() {     # seconds left of the allocation (empty = unknown)
    local l d=0 h=0 m=0 s=0; l=$(rl_tmo 30 squeue -h -j "${JOB:-0}" -o %L 2>/dev/null | head -1); [ -n "$l" ] || return 0
    case $l in UNLIMITED|INVALID|NOT_SET) echo 999999; return 0;; esac
    case $l in *-*) d=${l%%-*}; l=${l#*-};; esac
    IFS=: read -r a b c <<< "$l"
    if [ -n "${c:-}" ]; then h=$a; m=$b; s=$c; elif [ -n "${b:-}" ]; then m=$a; s=$b; else s=$a; fi
    echo $(( (10#$d * 24 + 10#$h) * 3600 + 10#$m * 60 + 10#$s ))
}
GAP=${REMOTE_STAIR_GAP:-}; GAPSRC=${REMOTE_STAIR_GAP:+" (from an earlier paste)"}; PMAX=; t0=$SECONDS; best=0; stop=; worst=PASS; skipped=0
rl_line "step nodes digits         verdict  total_s init/bs/dm/dc (s)       meas/model apu_peak/plan_max_GB verify last_digits(20)  hash"
run_step() {   # IDX NODES DIGITS ENV EXTRA TAG -> sets SV (verdict) and prints the table row
    local idx=$1 n=$2 d=$3 e=$4 x=$5 tag=$6 out rd sf rc v
    out=$RUN/raw/step${tag}_$n.out
    local tm; tm=$(echo " $x " | sed -n 's/.* --tmo \([0-9]*\) .*/\1/p'); tm=${tm:-60}
    rl_say "step $tag: $n node(s), $d digits, env: ${e:-<none>} ${x:+extra: $x}"
    local ex; ex=${EXCLUDE:+--exclude $EXCLUDE}
    # shellcheck disable=SC2086
    rl_tmo $(( (tm + 90) * 60 )) bash "$HERE/s5_run.sh" --nodes "$n" --digits "$d" --path "$PATHN" --env "$e" $x $ex ${JOB:+--jobid $JOB} > "$out" 2>&1 < /dev/null; rc=$?
    rd=$(sed -n 's/.* run dir //p' "$out" | head -1); sf=$rd/summary.txt; [ -f "$sf" ] || sf=$out
    kv() { grep -a -m1 "^$1=" "$sf" | cut -d= -f2-; }
    v=$(grep -a -m1 -o 'verdict=[A-Z]*' "$sf" | cut -d= -f2); [ -n "$v" ] || { [ $rc = 0 ] && v=PASS || v=FAIL; }
    [ $rc != 0 ] && v=FAIL
    if [ "$v" = FAIL ] && ! grep -aq '^=== REMOTE' "$sf"; then head -n 10 "$out" | cut -c1-170 | while IFS= read -r l; do rl_line "     raw: $l"; done; fi    # died before its summary
    local ph; ph=$(grep -a -m1 '^phases_s:' "$sf" | sed -E 's/.*init=([^ ]*) bs=([^ ]*) recip=[^ ]* dm=([^ ]*) dc=([^ ]*).*/\1 \2 \3 \4/' | awk '{ if (NF == 4) printf "%.1f/%.1f/%.1f/%.1f", $1, $2, $3, $4; else printf "NA" }')
    local pk pl; pk=$(kv device_peak_apu_gb | awk '{ print $1 }'); pl=${PMAX:-$(kv device_plan_apu_gb | awk '{ print $1 }')}
    rl_line "$(printf '%-4s %-5s %-13s %-8s %-7s %-24s %-10s %-15s %-6s %-20s %s' "$tag" "$n" "$d" "$v" "$(kv total_s)" "${ph:-NA}" "$(kv meas_over_model)" "${pk:-NA}/${pl:-NA}" "$(kv verify)" "$(kv last_digits | tr -d ' ' | cut -c1-20)" "$(kv digit_hash | awk '$1 ~ /^[0-9a-f]+$/ { print $1; next } { print "-" }')")"
    rl_line "     env: ${e:-<none>}${x:+ | extra: $x} | run ${rd##*/}"
    local sh sc; sh=$(kv slow_hosts); sc=$(kv screen)
    [ -n "$sc" ] && rl_line "     screen: $sc${sh:+; slow_hosts=$sh (left out of the later steps)}"
    [ -n "$sh" ] && EXCLUDE=${EXCLUDE:+$EXCLUDE,}$sh
    if [ "$v" = PASS ] && [ -n "${PMAX:-}" ] && [ -n "$pk" ] && [ "$pk" != NA ]; then     # measured busiest APU against the planner's busiest APU
        GAP=$(awk -v a="$pk" -v b="$PMAX" -v g="${GAP:-}" 'BEGIN { x = a - b; if (g == "" || x > g) g = x; printf "%.2f", g }'); rl_line "     measured peak - plan per APU: $(awk -v a="$pk" -v b="$PMAX" 'BEGIN { printf "%+.2f", a - b }') GB (largest so far $GAP; kept in site.env for the next paste)"; GAPSRC=; rl_site_set REMOTE_STAIR_GAP "$GAP"
    fi
    SV=$v; SRD=$rd; SSF=$sf; LFD=$(kv largest_fit_digits); [ -n "$LFD" ] && rl_line "     planner refused: largest fitting digits $LFD"
    [ "$v" = PASS ] || [ "$v" = PARTIAL ] || { RL_ERRFILES+=("$sf"); }
}
for i in "${!SN[@]}"; do
    PMAX=; idx=$((i + 1)); n=${SN[$i]}; d=${SD[$i]}; e=${SE[$i]}; x=${SX[$i]}
    if [ -n "$BUDGET" ] && [ $(( (SECONDS - t0) / 60 + 30 )) -gt "$BUDGET" ]; then
        rl_line "$idx $n $d SKIPPED (budget $BUDGET min: $(( (SECONDS - t0) / 60 )) min used, a step needs up to 30)"; skipped=$((skipped + 1)); stop=skip; break; fi
    ls=$(left_s)
    if [ -n "${JOB:-}" ] && [ -n "$ls" ]; then
        tmx=$(echo " $x " | sed -n 's/.* --tmo \([0-9]*\) .*/\1/p'); need=$(( ${tmx:-25} + 5 ))
        if [ "$ls" -lt $((need * 60)) ]; then
            rl_line "$idx $n $d SKIPPED (the allocation has $((ls / 60)) min left, the step needs $need = its --tmo ${tmx:-25} + 5; re-request a longer hold: alloc.sh --release, then alloc.sh --nodes <N> --time <H:MM:SS>)"; skipped=$((skipped + 1)); stop=skip; break; fi
    fi
    if [ "$MINM" != 0 ]; then       # memory rule: the planner's per-APU margin against what the measured runs showed
        D1=$(rl_digits_site "$d" "$n"); rl_fit "$PATHN" "$n" "$D1" $e; frc=$?; pline=$(grep -a 'per APU (max)' "$RUN/raw/fit_${PATHN}_${n}_$D1.log" 2>/dev/null | tail -1)
        mg=$(echo "$pline" | sed -n 's/.*margin \([-+][0-9.]*\) GB.*/\1/p'); PMAX=$(echo "$pline" | sed -n 's/.*per APU (max) \([0-9.]*\) GB.*/\1/p')
        need=$(awk -v g="${GAP:-}" -v m="$MINM" 'BEGIN { if (g == "") print m; else { x = g + 2; if (x < 3) x = 3; print x } }')
        why="assumed $MINM"; [ -n "${GAP:-}" ] && why="observed excess $GAP$GAPSRC + 2, at least 3"
        if [ $frc != 0 ]; then rl_line "$idx $n $d planner rc=$frc (refused or failed): the step runs into s5_run, which reports it"
        elif [ -z "$mg" ]; then rl_line "$idx $n $d memory rule not applied: the planner printed no per-APU margin (add ECALC_FIT_PER_APU=1 to the step's ENV)"
        elif awk -v a="$mg" -v b="$need" 'BEGIN { exit !(a + 0 < b + 0) }'; then
            rl_line "$idx $n $d SKIPPED (memory: planner margin $mg GB per APU is below the need $need GB = $why)"; skipped=$((skipped + 1)); stop=skip; break
        else rl_line "$idx $n $d memory rule ok: planner margin $mg GB per APU, need $need GB ($why)"; fi
    fi
    run_step "$idx" "$n" "$d" "$e" "$x" "$idx"
    if [ "$SV" = FAIL ]; then worst=FAIL; stop=fail; fail_idx=$idx; break; fi
    best=$(rl_num "$d")
done
[ "$stop" = skip ] && [ "$worst" = PASS ] && worst=PARTIAL
rl_kv best_passed_digits "$best"; rl_kv elapsed_min "$(( (SECONDS - t0) / 60 ))"
[ "$worst" = FAIL ] && rl_kv stopped "after step ${fail_idx:-?} (later steps not run); the errors are listed below"
rl_finish "$worst" "$([ "$worst" = FAIL ] && echo "$HERE/show.sh ${SRD:-latest} errors" || echo "$HERE/report.sh   (after the last step; or the next stair.sh)")"

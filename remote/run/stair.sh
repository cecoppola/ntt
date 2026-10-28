#!/bin/bash
# stair.sh - a staircase of runs in ONE paste: the steps in order, stop at the first failure, ONE table (<= 80 lines).
#   stair.sh --path 1|2 --step "NODES|DIGITS|ENV|EXTRA" [--step ...] [--budget MIN] [--exclude LIST] [--jobid J] [--dry-run]
# Each step is one s5_run.sh call: NODES, DIGITS (64e9, 4.608e12 or an integer), ENV = settings words (K=V ..., may be empty), EXTRA = more s5_run.sh
# options (--screen, --write DIR, --tmo MIN; may be empty).  Example (the first record run):
#   stair.sh --path 1 --step "576|10e12|BS_MDEV_LOGL=29 XSHMEM_HANG_S=3600|"
# A step runs only while the allocation has time for it (its tmo + 5 min, from squeue) and the --budget (minutes for the whole call; default none)
# is not used up; a skipped step ends the staircase with verdict PARTIAL.  A planner refusal (the step does not fit) is not a run failure: the step is
# run once at the largest fitting digit count when that is at least 3 % above the best passed step ("fitcap" row), then the staircase stops.
# Any other failure (rc, VERIFY, digits mismatch, timeout) stops at once: no automatic retry (the agent reads the errors and decides).  Per step:
# digits, verdict, total_s, init/bs/dm/dc, measured/model, per-APU device peak (measured) / plan, VERIFY, the last digits, the digit hash if taken.
# Verdict PASS all steps passed; PARTIAL some step skipped (budget, allocation time, fitcap) and the rest passed; FAIL a step failed.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
PATHN= STEPS=() BUDGET= JOB= EXCLUDE=
while [ $# -gt 0 ]; do case $1 in
  --path) PATHN=${2:?}; shift;; --step) STEPS+=("${2:?}"); shift;; --budget) BUDGET=${2:?}; shift;; --jobid) JOB=${2:?}; shift;; --exclude) EXCLUDE=${2-}; shift;;
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
t0=$SECONDS; best=0; stop=; worst=PASS; skipped=0; fitcap_done=0
rl_line "step nodes digits         verdict  total_s init/bs/dm/dc (s)       meas/model apu_peak/plan_GB verify last_digits(20)  hash"
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
    local ph; ph=$(grep -a -m1 '^phases_s:' "$sf" | sed -E 's/.*init=([^ ]*) bs=([^ ]*) recip=[^ ]* dm=([^ ]*) dc=([^ ]*).*/\1\/\2\/\3\/\4/')
    local pk pl; pk=$(kv device_peak_apu_gb | awk '{ print $1 }'); pl=$(kv device_plan_apu_gb | awk '{ print $1 }')
    rl_line "$(printf '%-4s %-5s %-13s %-8s %-7s %-24s %-10s %-15s %-6s %-20s %s' "$tag" "$n" "$d" "$v" "$(kv total_s)" "${ph:-NA}" "$(kv meas_over_model)" "${pk:-NA}/${pl:-NA}" "$(kv verify)" "$(kv last_digits | tr -d ' ' | cut -c1-20)" "$(kv digit_hash | cut -d' ' -f1)")"
    rl_line "     env: ${e:-<none>}${x:+ | extra: $x} | run ${rd##*/}"
    SV=$v; SRD=$rd; SSF=$sf; LFD=$(kv largest_fit_digits); SREF=$(kv fit | grep -c REFUSED)
    [ "$v" = PASS ] || [ "$v" = PARTIAL ] || { RL_ERRFILES+=("$sf"); }
}
for i in "${!SN[@]}"; do
    idx=$((i + 1)); n=${SN[$i]}; d=${SD[$i]}; e=${SE[$i]}; x=${SX[$i]}
    if [ -n "$BUDGET" ] && [ $(( (SECONDS - t0) / 60 + 30 )) -gt "$BUDGET" ]; then
        rl_line "$idx $n $d SKIPPED (budget $BUDGET min: $(( (SECONDS - t0) / 60 )) min used, a step needs up to 30)"; skipped=$((skipped + 1)); stop=skip; break; fi
    ls=$(left_s)
    if [ -n "${JOB:-}" ] && [ -n "$ls" ]; then
        tmx=$(echo " $x " | sed -n 's/.* --tmo \([0-9]*\) .*/\1/p'); need=$(( ${tmx:-25} + 5 ))
        if [ "$ls" -lt $((need * 60)) ]; then
            rl_line "$idx $n $d SKIPPED (the allocation has $((ls / 60)) min left, the step needs $need)"; skipped=$((skipped + 1)); stop=skip; break; fi
    fi
    run_step "$idx" "$n" "$d" "$e" "$x" "$idx"
    if [ "$SV" = FAIL ] && [ "$SREF" -ge 1 ] && [ -n "$LFD" ] && [ "$fitcap_done" = 0 ]; then
        # planner refusal: one run at the largest fitting digit count, if it is a real step up
        up=$(awk -v f="$LFD" -v b="$best" 'BEGIN { print (f > b * 1.03) ? 1 : 0 }')
        if [ "$up" = 1 ]; then
            fitcap_done=1; rl_say "step $idx refused by the planner; running the largest fitting size $LFD"
            run_step "$idx" "$n" "$LFD" "$e" "$x" "${idx}fc"
            [ "$SV" = PASS ] && { best=$LFD; worst=PARTIAL; stop=fitcap; break; }
        fi
    fi
    if [ "$SV" = FAIL ]; then worst=FAIL; stop=fail; fail_idx=$idx; break; fi
    best=$(rl_num "$d")
done
[ "$stop" = skip ] && [ "$worst" = PASS ] && worst=PARTIAL
[ "$stop" = fitcap ] && rl_kv note_fitcap "step refused by the planner; the fitting size ran instead (headline = that size)"
rl_kv best_passed_digits "$best"; rl_kv elapsed_min "$(( (SECONDS - t0) / 60 ))"
[ "$worst" = FAIL ] && rl_kv stopped "after step ${fail_idx:-?} (later steps not run); the errors are listed below"
rl_finish "$worst" "$([ "$worst" = FAIL ] && echo "$HERE/show.sh ${SRD:-latest} errors" || echo "$HERE/report.sh   (after the last step; or the next stair.sh)")"

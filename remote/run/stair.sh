#!/bin/bash
# stair.sh - a staircase of runs in ONE paste: the steps in order, stop at the first failure, ONE table (<= 80 lines).
#   stair.sh --path 1|2 --step "NODES|DIGITS|ENV|EXTRA" [--step ...] [--budget MIN] [--ab "NODES|DIGITS|WORDS"] [--min-margin GB] [--exclude LIST] [--jobid J] [--dry-run]
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
# --round-ab MB in a step's EXTRA (once; at 576 nodes): after that step (run at XSHMEM_ROUND_MB=128 with MN_WAIT_STATS=1 XSHMEM_ROUND_STATS=1, both
# diagnostics only) the same step runs again on the same hosts at XSHMEM_ROUND_MB=MB.  Rule printed: adopt MB when its total is lower by >= 3 % AND
# bs+dm are lower AND both runs are VERIFY OK with the same last digits; the winner is kept in site.env as REMOTE_ROUND_MB (unset when 128 wins) and every
# later step (also in later pastes) adds XSHMEM_ROUND_MB=$REMOTE_ROUND_MB unless its ENV has one.  A failed second run is not fatal (128 stays, PARTIAL);
# wrong digits are (FAIL).  Keys: round_test (the numbers), round_mb (the setting in force after the test).
# --ab "NODES|DIGITS|WORDS" (once): after step 1 passes, one arms.sh call of FOUR arms of NODES nodes at once (A B B A, one block each: ECALC_RESIDUE /
# thread team A/B; A = step 1's ENV, B = that plus WORDS).  Rule printed: adopt B when every arm is VERIFY OK with the same last digits, the largest dc of
# B is below the smallest dc of A (lower beyond the A-A / B-B spread) and B's mean total is no more than 2 % above A's; the WORDS are kept in site.env as
# REMOTE_RECORD_ENV and added to every later step (also in later pastes) for keys its ENV lacks.  An arm failure is not fatal (nothing adopted, PARTIAL);
# differing digits are (FAIL).  Needs 4 x NODES usable nodes.  Keys: ab_test, record_env.
# Verdict PASS all steps passed; PARTIAL some step skipped (budget, allocation time, memory rule) and the rest passed; FAIL a step failed.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
PATHN= STEPS=() BUDGET= JOB= EXCLUDE= MINM=6.5 AB=
while [ $# -gt 0 ]; do case $1 in
  --path) PATHN=${2:?}; shift;; --step) STEPS+=("${2:?}"); shift;; --budget) BUDGET=${2:?}; shift;; --min-margin) MINM=${2:?}; shift;; --ab) AB=${2:?}; shift;; --jobid) JOB=${2:?}; shift;; --exclude) EXCLUDE=${2-}; shift;;
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
AB_N= AB_D= AB_W=
if [ -n "$AB" ]; then
    IFS='|' read -r AB_N AB_D AB_W _ <<< "$AB||"
    rl_posint "$AB_N" && rl_isnum "$AB_D" && [ -n "${AB_W// /}" ] || { echo "stair.sh: --ab \"NODES|DIGITS|WORDS\" (words = the B arm's settings)" >&2; exit 2; }
    AB_N=$((10#$AB_N))
fi
for x in "${SX[@]}"; do case " $x " in *" --round-ab "*) echo " $x " | grep -q -E ' --round-ab [0-9.]+ ' || { echo "stair.sh: --round-ab MB (a number, in the step's EXTRA)" >&2; exit 2; };; esac; done
rl_init stair
TOTAL=${#STEPS[@]}
rl_kv path "$PATHN"; rl_kv steps "$TOTAL"${BUDGET:+" budget_min=$BUDGET"}
if [ "$DRY" = 1 ]; then
    for i in "${!SN[@]}"; do rl_would "step $((i + 1)): s5_run.sh --nodes ${SN[$i]} --digits ${SD[$i]} --path $PATHN --env \"${SE[$i]}\" ${SX[$i]} ${EXCLUDE:+--exclude $EXCLUDE }(stop at the first failure)"; done
    case " ${SX[*]} " in *" --round-ab "*) rl_would "round test: the --round-ab step runs twice on the same hosts (XSHMEM_ROUND_MB=128, then the value; MN_WAIT_STATS=1 XSHMEM_ROUND_STATS=1); adopt when total -3 % and bs+dm lower, digits identical -> site.env REMOTE_ROUND_MB";; esac
    [ -n "$AB" ] && rl_would "after step 1: arms.sh --nodes $AB_N --digits $AB_D --path $PATHN, arms A1 A2 (step 1 ENV) and B1 B2 (+ $AB_W) at once; adopt B when dc lower beyond the spread, digits identical -> site.env REMOTE_RECORD_ENV"
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
    ST_TOTAL=$(kv total_s | awk '{ print $1 }'); ST_PH=$(grep -a -m1 '^phases_s:' "$sf" | sed -E 's/.*init=([^ ]*) bs=([^ ]*) recip=[^ ]* dm=([^ ]*) dc=([^ ]*).*/\1 \2 \3 \4/'); ST_VERIFY=$(kv verify); ST_LAST=$(kv last_digits | tr -d ' ' | cut -c1-20); ST_RC=$(kv rc)
    SV=$v; SRD=$rd; SSF=$sf; LFD=$(kv largest_fit_digits); [ -n "$LFD" ] && rl_line "     planner refused: largest fitting digits $LFD"
    [ "$v" = PASS ] || [ "$v" = PARTIAL ] || { RL_ERRFILES+=("$sf"); }
}
addw() {       # ENV WORDS -> ENV with each K=V of WORDS added unless ENV already sets K
    local e=$1 w k; for w in $2; do k=${w%%=*}; case " $e " in *" $k="*) ;; *) e="${e:+$e }$w";; esac; done; echo "$e"
}
attempt() {    # IDX NODES DIGITS ENV EXTRA TAG -> rc 0 = the step ran (SV, ST_*), 1 = skipped (stop=skip: budget, allocation time or memory rule)
    local idx=$1 n=$2 d=$3 e=$4 x=$5 tag=$6 ls tmx need D1 frc pline mg why
    PMAX=
    if [ -n "$BUDGET" ] && [ $(( (SECONDS - t0) / 60 + 30 )) -gt "$BUDGET" ]; then
        rl_line "$tag $n $d SKIPPED (budget $BUDGET min: $(( (SECONDS - t0) / 60 )) min used, a step needs up to 30)"; skipped=$((skipped + 1)); stop=skip; return 1; fi
    ls=$(left_s)
    if [ -n "${JOB:-}" ] && [ -n "$ls" ]; then
        tmx=$(echo " $x " | sed -n 's/.* --tmo \([0-9]*\) .*/\1/p'); need=$(( ${tmx:-25} + 5 ))
        if [ "$ls" -lt $((need * 60)) ]; then
            rl_line "$tag $n $d SKIPPED (the allocation has $((ls / 60)) min left, the step needs $need = its --tmo ${tmx:-25} + 5; re-request a longer hold: alloc.sh --release, then alloc.sh --nodes <N> --time <H:MM:SS>)"; skipped=$((skipped + 1)); stop=skip; return 1; fi
    fi
    if [ "$MINM" != 0 ]; then       # memory rule: the planner's per-APU margin against what the measured runs showed
        D1=$(rl_digits_site "$d" "$n"); rl_fit "$PATHN" "$n" "$D1" $e; frc=$?; pline=$(grep -a 'per APU (max)' "$RUN/raw/fit_${PATHN}_${n}_$D1.log" 2>/dev/null | tail -1)
        mg=$(echo "$pline" | sed -n 's/.*margin \([-+][0-9.]*\) GB.*/\1/p'); PMAX=$(echo "$pline" | sed -n 's/.*per APU (max) \([0-9.]*\) GB.*/\1/p')
        need=$(awk -v g="${GAP:-}" -v m="$MINM" 'BEGIN { if (g == "") print m; else { x = g + 2; if (x < 3) x = 3; print x } }')
        why="assumed $MINM"; [ -n "${GAP:-}" ] && why="observed excess $GAP$GAPSRC + 2, at least 3"
        if [ $frc != 0 ]; then rl_line "$tag $n $d planner rc=$frc (refused or failed): the step runs into s5_run, which reports it"
        elif [ -z "$mg" ]; then rl_line "$tag $n $d memory rule not applied: the planner printed no per-APU margin (add ECALC_FIT_PER_APU=1 to the step's ENV)"
        elif awk -v a="$mg" -v b="$need" 'BEGIN { exit !(a + 0 < b + 0) }'; then
            rl_line "$tag $n $d SKIPPED (memory: planner margin $mg GB per APU is below the need $need GB = $why)"; skipped=$((skipped + 1)); stop=skip; return 1
        else rl_line "$tag $n $d memory rule ok: planner margin $mg GB per APU, need $need GB ($why)"; fi
    fi
    run_step "$idx" "$n" "$d" "$e" "$x" "$tag"
    return 0
}
run_ab() {     # BASE_ENV -> one arms.sh call of four arms (A B B A) of AB_N nodes; decision, site.env REMOTE_RECORD_ENV
    local base=$1 out rd sf rcs tm=25 nm ar arm_rc arm_tot arm_dc arm_v arm_l ok=1 amin amax bmin bmax at bt dig=1 first= d
    out=$RUN/raw/ab.out
    rl_say "ab: 4 arms of $AB_N nodes at once, $AB_D digits per arm (A: ${base:-<defaults>} | B: + $AB_W)"
    rl_tmo $(( (tm + 5) * 60 )) bash "$HERE/arms.sh" --nodes "$AB_N" --digits "$AB_D" --path "$PATHN" --tmo "$tm" ${JOB:+--jobid "$JOB"} ${EXCLUDE:+--exclude "$EXCLUDE"} \
        --arm "A1:$base" --arm "B1:$base $AB_W" --arm "B2:$base $AB_W" --arm "A2:$base" > "$out" 2>&1 < /dev/null; rcs=$?
    rd=$(sed -n 's/.* run dir //p' "$out" | head -1); sf=$rd/summary.txt; [ -f "$sf" ] || sf=$out
    if ! grep -aq '^arm_A1=' "$sf"; then
        rl_line "ab: not run (arms.sh rc $rcs): $(grep -a -m1 -E 'ERROR|need|usable|no allocation' "$out" | cut -c1-150)"; rl_kv ab_test "not run (rc $rcs)"; AB_SOFT=1; return 0; fi
    amin=; amax=; bmin=; bmax=; at=0; bt=0
    rl_line "     ab (4 arms of $AB_N nodes at once, positions A1 B1 B2 A2; B = + $AB_W):"
    for ar in A1 B1 B2 A2; do
        l=$(grep -a -m1 "^arm_$ar=" "$sf" | cut -d= -f2-)
        arm_rc=$(echo "$l" | sed -n 's/.*rc=\([^ ]*\).*/\1/p'); arm_tot=$(echo "$l" | sed -n 's/.* total=\([^ ]*\).*/\1/p'); arm_dc=$(echo "$l" | sed -n 's/.*bsdmdc=[^/ ]*\/[^/ ]*\/\([^ ]*\).*/\1/p')
        arm_v=$(echo "$l" | sed -n 's/.* verify=\([^ ]*\).*/\1/p'); arm_l=$(echo "$l" | sed -n 's/.* last=\([^ ]*\).*/\1/p')
        rl_line "$(printf '       %-3s rc=%-3s total_s %-7s bs/dm/dc %-18s verify %-5s last %s' "$ar" "$arm_rc" "$arm_tot" "$(echo "$l" | sed -n 's/.*bsdmdc=\([^ ]*\).*/\1/p')" "$arm_v" "$arm_l")"
        [ "$arm_rc" = 0 ] && [ "$arm_v" = OK ] || ok=0
        [ -n "$arm_l" ] && { [ -z "$first" ] && first=$arm_l; [ "$arm_l" = "$first" ] || dig=0; }
        case ${arm_dc:-x} in [0-9]*) ;; *) ok=0; continue;; esac
        case $ar in
            A*) at=$(awk -v a="$at" -v b="${arm_tot:-0}" 'BEGIN { print a + b }'); [ -z "$amin" ] || awk -v a="$arm_dc" -v b="$amin" 'BEGIN { exit !(a < b) }' && amin=$arm_dc; [ -z "$amax" ] || awk -v a="$arm_dc" -v b="$amax" 'BEGIN { exit !(a > b) }' && amax=$arm_dc;;
            B*) bt=$(awk -v a="$bt" -v b="${arm_tot:-0}" 'BEGIN { print a + b }'); [ -z "$bmin" ] || awk -v a="$arm_dc" -v b="$bmin" 'BEGIN { exit !(a < b) }' && bmin=$arm_dc; [ -z "$bmax" ] || awk -v a="$arm_dc" -v b="$bmax" 'BEGIN { exit !(a > b) }' && bmax=$arm_dc;;
        esac
    done
    if [ "$dig" = 0 ]; then rl_line "     ab: the last digits DIFFER between arms: not adopted; this is a correctness failure (do not go on; AGENT.md Failure handling)"; rl_kv ab_test "DIGITS DIFFER"; SV=FAIL; AB_HARD=1; RL_ERRFILES+=("$sf"); return 0; fi
    if [ "$ok" = 0 ] || [ -z "$amin" ] || [ -z "$bmin" ]; then rl_line "     ab: an arm failed or printed no dc: inconclusive, nothing adopted (raw/ab.out, arms run $rd)"; rl_kv ab_test "inconclusive"; AB_SOFT=1; return 0; fi
    d=$(awk -v at="$at" -v bt="$bt" -v bmax="$bmax" -v amin="$amin" 'BEGIN { ma = at / 2; mb = bt / 2; if (bmax + 0 < amin + 0 && mb <= ma * 1.02) print "adopt"; else print "keep" }')
    rl_line "     ab dc_s: A $amin..$amax, B $bmin..$bmax; mean total A $(awk -v a="$at" 'BEGIN { printf "%.1f", a / 2 }') B $(awk -v a="$bt" 'BEGIN { printf "%.1f", a / 2 }'); digits identical: yes"
    if [ "$d" = adopt ]; then
        rl_site_set REMOTE_RECORD_ENV "$AB_W"; rl_line "     ab decision: ADOPT B (dc lower beyond the spread, total not worse): REMOTE_RECORD_ENV=$AB_W kept in site.env, added to every later step"; rl_kv ab_test "adopt B dc A $amin..$amax B $bmin..$bmax"
    else rl_site_unset REMOTE_RECORD_ENV; rl_line "     ab decision: KEEP the defaults (B's dc is not below A's beyond the spread, or its total is more than 2 % higher)"; rl_kv ab_test "keep A dc A $amin..$amax B $bmin..$bmax"; fi
    rl_kv record_env "${REMOTE_RECORD_ENV:-<none>}"
}
round_decide() {   # RB: runs 1 (R1_*) and 2 (R2_*) of the round test -> decision, site.env REMOTE_ROUND_MB
    local rb=$1 res
    res=$(awk -v t1="$R1_TOTAL" -v t2="$R2_TOTAL" -v p1="$R1_PH" -v p2="$R2_PH" -v v1="$R1_VERIFY" -v v2="$R2_VERIFY" -v l1="$R1_LAST" -v l2="$R2_LAST" 'BEGIN {
        split(p1, a, " "); split(p2, b, " "); x1 = a[2] + a[3]; x2 = b[2] + b[3]
        g = (t1 > 0) ? (t1 - t2) / t1 * 100 : 0
        dig = (v1 == "OK" && v2 == "OK" && l1 != "" && l1 == l2) ? "yes" : "NO"
        r = (g >= 3 && x2 < x1 && dig == "yes") ? "ADOPT" : "KEEP"
        printf "%s total %s -> %s s (gain %.1f %%), bs+dm %.1f -> %.1f s, digits identical: %s", r, t1, t2, g, x1, x2, dig }')
    rl_line "     round test: XSHMEM_ROUND_MB 128 vs $rb at the same hosts: $res"
    rl_kv round_test "128 vs $rb: $res"
    case $res in
        ADOPT*) rl_site_set REMOTE_ROUND_MB "$rb"; rl_line "     round decision: ADOPT $rb (total lower by >= 3 % and bs+dm lower, digits identical): REMOTE_ROUND_MB=$rb kept in site.env, added to every later step (the planner margin is checked per step)";;
        *) rl_site_unset REMOTE_ROUND_MB; rl_line "     round decision: KEEP 128 (rule: $rb needs total lower by >= 3 % AND bs+dm lower AND identical digits); later steps use the default";;
    esac
    rl_kv round_mb "${REMOTE_ROUND_MB:-128}"
    case $res in *"identical: NO"*) [ "$R1_VERIFY" = OK ] && [ "$R2_VERIFY" = OK ] && { rl_line "     round test: both runs VERIFY OK but the last digits DIFFER: a correctness failure, stop"; return 1; };; esac
    return 0
}
AB_SOFT=0 AB_HARD=0 AB_DONE=0
for i in "${!SN[@]}"; do
    idx=$((i + 1)); n=${SN[$i]}; d=${SD[$i]}; e0=${SE[$i]}; x=${SX[$i]}; rb=
    case " $x " in *" --round-ab "*) rb=$(echo " $x " | sed -n 's/.* --round-ab \([0-9.]*\) .*/\1/p'); x=$(echo " $x " | sed -E 's/ --round-ab [0-9.]+ / /; s/^ +//; s/ +$//');; esac
    if [ -n "$rb" ]; then e=$(addw "$e0" "XSHMEM_ROUND_MB=128 MN_WAIT_STATS=1 XSHMEM_ROUND_STATS=1"); else e=$e0; fi
    e=$(addw "$e" "${REMOTE_RECORD_ENV:-}"); e=$(addw "$e" "${REMOTE_ROUND_MB:+XSHMEM_ROUND_MB=$REMOTE_ROUND_MB}")
    attempt "$idx" "$n" "$d" "$e" "$x" "$idx" || break
    if [ "$SV" = FAIL ]; then worst=FAIL; stop=fail; fail_idx=$idx; break; fi
    best=$(rl_num "$d")
    if [ -n "$rb" ]; then      # round test: the same step again on the same hosts at the other round size
        R1_TOTAL=$ST_TOTAL; R1_PH=$ST_PH; R1_VERIFY=$ST_VERIFY; R1_LAST=$ST_LAST
        e2=$(addw "$e0" "XSHMEM_ROUND_MB=$rb MN_WAIT_STATS=1 XSHMEM_ROUND_STATS=1"); e2=$(addw "$e2" "${REMOTE_RECORD_ENV:-}")
        attempt "$idx" "$n" "$d" "$e2" "$x" "${idx}b" || break
        if [ "$SV" = FAIL ]; then
            if [ "${ST_RC:-1}" = 0 ]; then worst=FAIL; stop=fail; fail_idx="${idx}b"; break; fi      # the run finished with wrong digits or a VERIFY failure: a correctness failure
            rl_line "     round test: the $rb run failed (rc ${ST_RC:-?}, verify ${ST_VERIFY:-NONE}); 128 stays; the staircase goes on (errors: show.sh ${SRD##*/} errors)"; rl_kv round_test "128 vs $rb: the $rb run failed (rc ${ST_RC:-?})"; rl_site_unset REMOTE_ROUND_MB; rl_kv round_mb 128; AB_SOFT=1
        else R2_TOTAL=$ST_TOTAL; R2_PH=$ST_PH; R2_VERIFY=$ST_VERIFY; R2_LAST=$ST_LAST; round_decide "$rb" || { worst=FAIL; stop=fail; fail_idx="${idx}b"; RL_ERRFILES+=("$SSF"); break; }; fi
    fi
    if [ -n "$AB" ] && [ $AB_DONE = 0 ] && [ "$idx" = 1 ]; then      # residue / thread team A/B on four blocks of the screened allocation
        AB_DONE=1; run_ab "${SE[0]}"
        if [ $AB_HARD = 1 ]; then worst=FAIL; stop=fail; fail_idx=ab; break; fi
    fi
done
[ $AB_SOFT = 1 ] && [ "$worst" = PASS ] && worst=PARTIAL
[ "$stop" = skip ] && [ "$worst" = PASS ] && worst=PARTIAL
rl_kv best_passed_digits "$best"; rl_kv elapsed_min "$(( (SECONDS - t0) / 60 ))"
[ "$worst" = FAIL ] && rl_kv stopped "after step ${fail_idx:-?} (later steps not run); the errors are listed below"
rl_finish "$worst" "$([ "$worst" = FAIL ] && echo "$HERE/show.sh ${SRD:-latest} errors" || echo "$HERE/report.sh   (after the last step; or the next stair.sh)")"

#!/bin/bash
# s4_ab.sh - stage 4: paired A/B timing in ABBA blocks inside the allocation (same nodes, alternating arms).
#   s4_ab.sh --nodes N --digits D --path 1|2 [--a "K=V ..."] [--b "K=V ..."] [--rounds R] [--aa] [--no-warmup] [--tmo MIN]
#            [--jobid J] [--exclude LIST] [--dry-run]
# Arm A and arm B are settings words (K=V, may be empty: --a ""), added to the launch line of the path.  One round = A B B A.  A discarded
# warm-up run (arm A) comes first unless --no-warmup (the first run of a fresh allocation is slow).  --aa: arm B uses A's words (an A/A control:
# the B-A difference must then be about zero).  Per arm: mean of total and of the phases init, bs, dm, dc (seconds); B-A: mean of the per-round
# differences with a 95 % t interval (needs >= 2 rounds), also in percent of A.  Every run must have rc 0 and VERIFY OK, and the last digits
# must be identical across all runs.  Default --rounds 2.  Plan fit is checked for both arms before any run.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
NN= DIG= P= AW= BW= ROUNDS=2 AA=0 WARM=1 TMO=45 JOB= EXCLUDE=
while [ $# -gt 0 ]; do case $1 in
  --nodes) NN=${2:?}; shift;; --digits) DIG=${2:?}; shift;; --path) P=${2:?}; shift;; --a) AW=${2-}; shift;; --b) BW=${2-}; shift;;
  --rounds) ROUNDS=${2:?}; shift;; --aa) AA=1;; --no-warmup) WARM=0;; --tmo) TMO=${2:?}; shift;; --jobid) JOB=${2:?}; shift;; --exclude) EXCLUDE=${2-}; shift;;
  --dry-run) DRY=1;; -h|--help) rl_help "$0"; exit 0;; *) echo "s4_ab.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
[ -n "$NN" ] && [ -n "$DIG" ] && [ -n "$P" ] || { echo "s4_ab.sh: --nodes, --digits and --path are required (--help)" >&2; exit 2; }
rl_isnum "$NN" && rl_isnum "$DIG" && rl_isnum "$ROUNDS" || { echo "s4_ab.sh: numeric --nodes/--digits/--rounds" >&2; exit 2; }
case $P in 1|2) ;; *) echo "s4_ab.sh: --path 1|2" >&2; exit 2;; esac
[ $AA = 1 ] && BW=$AW
rl_init s4
D=$(rl_digits_site "$DIG" "$NN")
if [ "$DRY" = 1 ]; then
    [ "$WARM" = 1 ] && rl_would "warm-up (discarded): $(rl_cmdline "$P" "$NN" "$D" $AW)"
    rl_would "arm A x$((ROUNDS * 2)): $(rl_cmdline "$P" "$NN" "$D" $AW)"
    rl_would "arm B x$((ROUNDS * 2)): $(rl_cmdline "$P" "$NN" "$D" $BW)"
    rl_would "order per round: A B B A; fit check: tools/plan_values.py --fit for A and B"
    rl_finish SKIP "$HERE/s4_ab.sh --nodes $NN --digits $DIG --path $P"
fi
export TMO
rl_prep "$P" "$NN"
[ "$NHOSTS" -ge "$NN" ] || rl_die "only $NHOSTS usable nodes in the allocation, $NN asked"
rl_kv path "$P"; rl_kv nodes "$NN"; rl_kv digits "$D"; rl_kv rounds "$ROUNDS"; rl_kv arm_A "${AW:-<defaults>}"; rl_kv arm_B "${BW:-<defaults>}$([ $AA = 1 ] && echo ' (A/A control)')"
[ "$D" != "$(rl_num "$DIG")" ] && rl_kv digits_requested "$DIG (capped for the rehearsal site)"
# ---- fit
for w in A B; do
    ws=$AW; [ $w = B ] && ws=$BW
    # shellcheck disable=SC2086
    rl_fit "$P" "$NN" "$D" $ws; frc=$?
    if [ $frc = 1 ]; then rl_errfiles "$RUN/raw/fit_${P}_${NN}_$D.log"; mx=$(rl_maxfit "$P" "$NN" "$D" $ws); rl_kv largest_fit_digits "$mx"; rl_finish FAIL "arm $w does not fit: $FIT_LINE" "$HERE/s4_ab.sh --nodes $NN --digits $mx --path $P"
    elif [ $frc = 2 ]; then rl_errfiles "$RUN/raw/fit_${P}_${NN}_$D.log"; rl_finish FAIL "planner failed for arm $w" "$HERE/show.sh $RUN errors"; fi
done
rl_kv fit "FITS ($FIT_LINE)"
# ---- runs
declare -A TOT INIT BS DM DC
tails=; nrun=0
run1() {    # TAG ARM(A|B) WORDS... ; sets LAST_OK; stores metrics under $TAG
    local tag=$1 arm=$2 rc; shift 2
    rl_say "run $tag (arm $arm)"; rl_log "$(rl_cmdline "$P" "$NN" "$D" "$@")"
    # shellcheck disable=SC2068
    rl_ecrun "$tag" "$P" "$NN" "$D" $@; rc=$?
    TOT[$tag]=${E_TOTAL:-}; INIT[$tag]=${E_INIT:-}; BS[$tag]=${E_BS:-}; DM[$tag]=${E_DM:-}; DC[$tag]=${E_DC:-}
    local t; t=$(rl_digits_tail "$RUN/raw/${tag,,}.log"); tails="$tails
$t"
    nrun=$((nrun + 1))
    LAST_OK=1; { [ $rc = 0 ] && [ "${E_VERIFY:-NONE}" = OK ] && [ -n "${E_TOTAL:-}" ]; } || { LAST_OK=0; rl_errfiles "$RUN/raw/${tag,,}.log" "$RUN/raw/${tag,,}.log.gz"; rl_kv failed_run "$tag rc=$rc verify=${E_VERIFY:-NONE} refused=${E_REFUSED:-0}"; }
}
abort() { rl_finish FAIL "run $1 failed: results so far are not conclusive" "$HERE/show.sh $RUN errors"; }
if [ $WARM = 1 ]; then run1 s4_warm A $AW; [ $LAST_OK = 1 ] || abort s4_warm; fi
dtot=(); pa=() pb=()
for r in $(seq 1 "$ROUNDS"); do
    for step in A1 B1 B2 A2; do
        arm=${step:0:1}; ws=$AW; [ $arm = B ] && ws=$BW
        run1 "s4_r${r}_$step" "$arm" $ws; [ $LAST_OK = 1 ] || abort "s4_r${r}_$step"
    done
    a=$(rl_mean "${TOT[s4_r${r}_A1]}" "${TOT[s4_r${r}_A2]}"); b=$(rl_mean "${TOT[s4_r${r}_B1]}" "${TOT[s4_r${r}_B2]}")
    pa+=("$a"); pb+=("$b"); dtot+=("$(awk -v a="$a" -v b="$b" 'BEGIN { printf "%.4f", b - a }')")
done
# ---- statistics
arm_vals() { local arm=$1 arr=$2 r s out=; for r in $(seq 1 "$ROUNDS"); do for s in 1 2; do eval "out=\"\$out \${$arr[s4_r${r}_$arm$s]}\""; done; done; echo "$out"; }
rl_line "arm   runs total_s  init_s   bs_s     dm_s     dc_s"
for arm in A B; do
    # shellcheck disable=SC2046
    rl_line "$(printf '%-5s %-4s %-8s %-8s %-8s %-8s %-8s' "$arm" "$((ROUNDS * 2))" "$(rl_mean $(arm_vals $arm TOT))" "$(rl_mean $(arm_vals $arm INIT))" "$(rl_mean $(arm_vals $arm BS))" "$(rl_mean $(arm_vals $arm DM))" "$(rl_mean $(arm_vals $arm DC))")"
done
read -r dm lo hi < <(rl_ci "${dtot[@]}")
ma=$(rl_mean "${pa[@]}")
pct=$(awk -v d="$dm" -v a="$ma" 'BEGIN { if (a > 0) printf "%+.2f", 100 * d / a; else print "NA" }')
rl_kv "B_minus_A_total_s" "$dm (95% interval $lo .. $hi, $ROUNDS round(s)); $pct % of A"
rl_kv per_round_diffs_s "${dtot[*]}"
if [ "$lo" != NA ]; then verdictw=$(awk -v lo="$lo" -v hi="$hi" 'BEGIN { if (lo > 0) print "B slower"; else if (hi < 0) print "B faster"; else print "no difference resolved" }'); else verdictw="one round: no interval"; fi
rl_kv reading "$verdictw$([ $AA = 1 ] && echo ' (A/A control: should be ~0)')"
ut=$(echo "$tails" | grep . | sort -u | paste -sd,)
if [ "$(echo "$ut" | tr ',' '\n' | grep -c .)" = 1 ]; then rl_kv digits "IDENTICAL across all $nrun runs (last digits $ut)"; dv=PASS
elif [ -z "$ut" ]; then rl_kv digits "NONE printed (VERIFY OK on every run)"; dv=PASS
else rl_kv digits "DIFFER: $ut"; dv=FAIL; fi
rl_kv verify "OK on all $nrun runs"
[ $dv = PASS ] || rl_finish FAIL "digits differ between runs" "$HERE/show.sh $RUN grep 'digits:'"
rl_finish PASS "$HERE/s5_run.sh --nodes $NN --digits <D> --path $P --env \"<winning words>\""

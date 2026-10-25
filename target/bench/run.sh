#!/bin/bash
# bench/run.sh - the worker loop of the bench (contract: target/FORMATS.md §4, §5).  Run it inside (or next to) a Slurm allocation:
#   run.sh --jobid J [--max-min M] [--once] [--only id,...] [--tier 1|2] [--worker NAME] [--wait-min M] [--dry-run]
#   run.sh --jobid none ...        a login-node worker: only items with nodes=0 (the builds, sys facts of the login node)
#   run.sh --status                table of all items: id, state (unclaimed / claimed / done-PASS|FAIL|...), worker, wall
#   run.sh --plan N [--max-nodes K]  how many workers of which allocation sizes drain the manifest fastest (list scheduling on est_min)
#   run.sh --reclaim ID [--force]  break a stale claim (older than 2*est_min+10 min, no DONE); --force also redoes an item that ended
#                                  FAIL/SKIP (the old dir is moved to results/bench/_old/)
#   run.sh --validate              check the manifest (ids, deps, columns, item scripts) and exit
# Each loop: take the eligible item with the lowest prio (deps DONE with PASS/PARTIAL; nodes <= hosts of J; est_min <= minutes left
# of J and <= --max-min; hosts in MNRUN_EXCLUDE_HOSTS or results/ladder/exclude.txt are dropped from the pool first), claim it (tgt_claim on results/bench/<id>/), give it the first NODES hosts of J in physical order, run it
# under tgt_timeout (2*est_min+2) with the §5 environment, finish summary.txt/DONE.  When nothing fits: wait up to --wait-min minutes (default 30) if items
# wait for dependencies that other workers are running, else exit with one line saying what is left and why.
# Item scripts get TGT_SUM_DIR=$OUT exported, so `tgt_kv key value` (after sourcing lib/common.sh) appends to summary.txt; the runner
# writes id/start/host/commit before and end/wall_s/status/rc after (status: the item's own status= line when rc=0, else FAIL).
# Also exported: MNRUN_DIR1/2 (dir of each binary, holds mnrun.sh), MNRUN_MODULES_P1/P2 (the module line each build linked with, bld summary e.MNRUN_MODULES).
# Binaries: ECALC1 / ECALC2 come from results/bench/bld01|bld02/summary.txt, key e.binary = path relative to target/ (build/<sha7>-p1/ecalc/ecalc).
# Self-test overrides: TGT_FAKE_HOSTS=h1,h2,.. (no squeue/scontrol), TGT_FAKE_REMAIN_MIN, TGT_BENCH_DIR, TGT_RES_DIR.

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib/common.sh"
tgt_init
[ -n "$TGT_RES_DIR" ] && TGT_RES=$TGT_RES_DIR
BENCH=${TGT_BENCH_DIR:-$TGT_ROOT/bench}
RB=$TGT_RES/bench
export TGT_BENCH_DIR=$BENCH TGT_RES_DIR=$TGT_RES
MP="python3 $here/manifest.py"

JOBID= MAXMIN=- ONCE=0 ONLY= TIER=0 WORKER= DRY=0 WAITMIN=30
while [ $# -gt 0 ]; do
    case $1 in
        --jobid) JOBID=$2; shift;;
        --max-min) MAXMIN=$2; shift;;
        --once) ONCE=1;;
        --only) ONLY=$2; shift;;
        --tier) TIER=$2; shift;;
        --worker) WORKER=$2; shift;;
        --wait-min) WAITMIN=$2; shift;;
        --dry-run) DRY=1;;
        --status) exec $MP status;;
        --validate) exec $MP validate;;
        --plan) shift; exec $MP plan "$@";;
        --reclaim) RECLAIM=$2; shift;;
        --force) FORCE=1;;
        -h|--help) sed -n 2,20p "$0"; exit 0;;
        *) echo "run.sh: unknown argument $1" >&2; exit 2;;
    esac
    shift
done

# ---- reclaim
if [ -n "$RECLAIM" ]; then
    d=$RB/$RECLAIM
    [ -e "$d/claim.lock" ] || { echo "no claim on $RECLAIM"; exit 1; }
    est=$($MP get "$RECLAIM" est_min) || { echo "unknown id $RECLAIM"; exit 1; }
    if [ -f "$d/DONE" ]; then
        st=$(grep '^status=' "$d/summary.txt" 2>/dev/null | tail -1 | cut -d= -f2)
        case "$st" in PASS|PARTIAL) echo "$RECLAIM is done ($st): nothing to reclaim"; exit 1;; esac
        [ -n "$FORCE" ] || { echo "$RECLAIM ended ${st:-?}; --force moves it to _old/ and releases it"; exit 1; }
    else
        age=$(( $(date +%s) - $(stat -c %Y "$d/claim.lock") ))
        lim=$(python3 -c "print(int((2*$est+10)*60))")
        if [ "$age" -le "$lim" ] && [ -z "$FORCE" ]; then echo "claim on $RECLAIM is ${age}s old (< ${lim}s): not stale"; exit 1; fi
    fi
    mkdir -p "$RB/_old"; mv "$d" "$RB/_old/$RECLAIM.$(date +%Y%m%d-%H%M%S)" && echo "reclaimed $RECLAIM (old dir in results/bench/_old/)"
    exit $?
fi

[ -n "$JOBID" ] || { echo "run.sh: --jobid J (or --jobid none) required" >&2; exit 2; }
export TGT_WORKER=${WORKER:-$(hostname -s)-$$}
python3 "$here/manifest.py" validate >&2 || { echo "run.sh: manifest invalid (run.sh --validate)" >&2; exit 2; }
mkdir -p "$RB"
tgt_conf

# ---- allocation facts
hosts_of() {        # all hosts of the allocation in physical order, one per line
    local raw
    if [ -n "$TGT_FAKE_HOSTS" ]; then raw=${TGT_FAKE_HOSTS//,/$'\n'}
    elif [ "$JOBID" = none ]; then raw=
    else raw=$(tgt_hosts "$JOBID"); fi
    [ -n "$raw" ] || return 0
    if [ -f "$TGT_ROOT/ladder/locality.py" ] && [ -z "$TGT_FAKE_HOSTS" ]; then
        local o; o=$(python3 "$TGT_ROOT/ladder/locality.py" order "$(echo "$raw" | paste -sd,)" 2>/dev/null) && [ -n "$o" ] && { echo "$o" | tr ', ' '\n\n' | grep .; return 0; }
    fi
    echo "$raw"
}
remaining_min() {   # minutes left of the allocation (squeue %L), "-" when unlimited / none
    local l d=0 h=0 m=0 s=0
    if [ -n "$TGT_FAKE_HOSTS" ]; then echo "${TGT_FAKE_REMAIN_MIN:--}"; return; fi
    [ "$JOBID" = none ] && { echo -; return; }
    l=$(squeue -h -j "$JOBID" -o %L 2>/dev/null | head -1)
    case "$l" in
        ""|UNLIMITED|NOT_SET|INVALID) echo -; return;;
        *-*) d=${l%%-*}; l=${l#*-};;
    esac
    IFS=: read -r a b c <<< "$l"
    if [ -n "$c" ]; then h=$a; m=$b; s=$c; elif [ -n "$b" ]; then m=$a; s=$b; else s=$a; fi
    echo $(( 10#$d * 1440 + 10#$h * 60 + 10#$m + (10#$s > 30 ? 1 : 0) ))
}
mapfile -t ALLH < <(hosts_of)
# excluded hosts (MNRUN_EXCLUDE_HOSTS, results/ladder/exclude.txt; one per line or comma separated) leave the pool BEFORE any item gets its hosts,
# so no item runs on one and mnrun never has to shrink a node count
EXH=$( { echo "${MNRUN_EXCLUDE_HOSTS:-}"; sed 's/#.*//' "$TGT_RES/ladder/exclude.txt" 2>/dev/null; } | tr ', \t' '\n\n\n' | grep . || true)
if [ -n "$EXH" ] && [ "${#ALLH[@]}" -gt 0 ]; then
    mapfile -t ALLH < <(printf '%s\n' "${ALLH[@]}" | grep -Fvxf <(echo "$EXH") || true)
    tgt_log "excluded hosts dropped from the pool: $(echo $EXH)"
fi
NH=${#ALLH[@]}
[ "$JOBID" = none ] && NH=0
tgt_log "run.sh worker $TGT_WORKER: job $JOBID, $NH host(s), commit $TGT_SHA${ONLY:+, only $ONLY}${TIER:+, tier $TIER}"

bin_of() {          # bld01|bld02 -> absolute binary path when that build is done with PASS/PARTIAL and the file exists
    local f=$RB/$1/summary.txt p
    [ -f "$RB/$1/DONE" ] && [ -f "$f" ] || return 0
    grep -qE '^status=(PASS|PARTIAL)' "$f" || return 0
    p=$(grep -E '^e\.binary=' "$f" | tail -1 | cut -d= -f2-)
    [ -n "$p" ] || p=$(grep -E '^m\.binary=' "$f" | tail -1 | cut -d= -f2- | awk '{print $1}')
    [ -n "$p" ] || return 0
    case "$p" in /*) ;; *) p=$TGT_ROOT/$p;; esac
    [ -x "$p" ] && echo "$p"
}

sum_of() {          # ID KEY -> value of KEY in that item's summary (last one)
    grep -E "^$2=" "$RB/$1/summary.txt" 2>/dev/null | tail -1 | cut -d= -f2-
}

run_item() {        # ID: the item is claimed by us
    local id=$1 d=$RB/$1 nodes est cmd tmo rc hl st
    nodes=$($MP get "$id" nodes); est=$($MP get "$id" est_min); cmd=$($MP get "$id" cmd)
    tmo=$(python3 -c "print(int(2*$est+2))")
    hl=$(printf '%s\n' "${ALLH[@]:0:$nodes}" | paste -sd, -); [ "$nodes" = 0 ] && hl=
    tgt_sum_init "$d" "$id"
    tgt_kv nodes "$nodes"; tgt_kv hosts "${hl:--}"; tgt_kv jobid "$JOBID"; tgt_kv worker "$TGT_WORKER"
    tgt_log "START $id nodes=$nodes est=${est}min timeout=${tmo}min hosts=${hl:--}"
    (
        cd "$d" || exit 99
        export OUT=$d ID=$id JOBID NODES=$nodes HOSTS=$hl TGT_SUM_DIR=$d TGT_ITEM_MIN=$(python3 -c "import math; print(max(1, math.ceil($est)))")
        export ECALC1=$(bin_of bld01) ECALC2=$(bin_of bld02)
        [ -n "$ECALC1" ] && export MNRUN_DIR1=$(dirname "$ECALC1")
        [ -n "$ECALC2" ] && export MNRUN_DIR2=$(dirname "$ECALC2")
        export MNRUN_MODULES_P1=$(sum_of bld01 e.MNRUN_MODULES) MNRUN_MODULES_P2=$(sum_of bld02 e.MNRUN_MODULES)
        tgt_timeout "$tmo" bash "$BENCH/items/${cmd%% *}" ${cmd#"${cmd%% *}"} > "$d/raw/item.out" 2>&1 < /dev/null
    )
    rc=$?
    [ "$rc" = 124 ] || [ "$rc" = 137 ] && tgt_kv note "killed by the item timeout (${tmo} min)"
    if [ ! -f "$d/DONE" ]; then
        st=$(grep '^status=' "$d/summary.txt" | tail -1 | cut -d= -f2)
        sed -i '/^status=/d' "$d/summary.txt"
        if [ "$rc" != 0 ]; then st=FAIL
        else case "$st" in PASS|PARTIAL|SKIP|FAIL) ;; *) st=PASS;; esac; fi
        tgt_sum_end "$st" "$rc"
    else
        st=$(grep '^status=' "$d/summary.txt" | tail -1 | cut -d= -f2)
    fi
    tgt_log "END $id status=$st rc=$rc"
    echo "$id $st"
}

# ---- the loop
WAITED=0 NRUN=0
while :; do
    rem=$(remaining_min)
    args=(pick --nhosts "$NH" --remaining "$rem" --max-min "$MAXMIN" --tier "$TIER" --assume-done "${ASSUME:-}")
    [ -n "$ONLY" ] && args+=(--only "$ONLY")
    cand=$($MP "${args[@]}" 2> "$RB/.pick.$$.err"); prc=$?
    why=$(cat "$RB/.pick.$$.err" 2>/dev/null); rm -f "$RB/.pick.$$.err"
    got=
    if [ "$prc" = 0 ]; then
        for id in $cand; do
            if [ "$DRY" = 1 ]; then
                echo "dry-run: would run $id (nodes $($MP get "$id" nodes), est $($MP get "$id" est_min) min)"; ASSUME="$ASSUME,$id"; got=$id; break
            fi
            mkdir -p "$RB"
            if tgt_claim "$RB/$id"; then got=$id; run_item "$id"; NRUN=$((NRUN + 1)); break; fi
        done
        if [ -n "$got" ]; then WAITED=0; [ "$ONCE" = 1 ] && break; continue; fi
        sleep 1; continue          # all candidates were claimed under our hands: re-pick
    fi
    if [ "$prc" = 3 ] && [ "$WAITED" -lt $((WAITMIN * 60)) ] && [ "$DRY" != 1 ]; then
        sleep 10; WAITED=$((WAITED + 10)); continue
    fi
    break
done
echo "run.sh worker $TGT_WORKER done: ran $NRUN item(s); ${why:-nothing left to do}"
exit 0

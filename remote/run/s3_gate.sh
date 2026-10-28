#!/bin/bash
# s3_gate.sh - stage 3: digit gates.  Runs ecalc with the digit file written and compares the digit hash with the known reference.
#   s3_gate.sh [--nodes 1,2] [--path 1|2|both] [--digits 1e9] [--tmo MIN] [--jobid J] [--exclude LIST] [--dry-run]
# For every (path, node count): one run through ecalc/mnrun.sh (path 1: one process per node; path 2: ECALC_PE_PER_GPU=1 XSHMEM_LANES=2),
# digit file written to $REMOTE_OUT/scratch (removed after the hash), hash = unpack_digits | sha1sum.  References (1e9 bf9f4b08758b40d9,
# 1e10 3fc025473586ed22, 1e11 578f5efb0ff2b9af); other sizes: all runs must agree with the first.  Also needs VERIFY OK and rc 0.
# Verdict PASS all good; FAIL any run wrong; PARTIAL some combinations could not run (too few nodes, no binary) and the rest are good.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
NODES=1,2 PATHS=both DIG=1e9 TMO=30 JOB= EXCLUDE=
while [ $# -gt 0 ]; do case $1 in
  --nodes) NODES=${2:?}; shift;; --path) PATHS=${2:?}; shift;; --digits) DIG=${2:?}; shift;; --tmo) TMO=${2:?}; shift;; --jobid) JOB=${2:?}; shift;;
  --exclude) EXCLUDE=${2-}; shift;; --dry-run) DRY=1;; -h|--help) rl_help "$0"; exit 0;; *) echo "s3_gate.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
case $PATHS in 1|2) pl=$PATHS;; both) pl="1 2";; *) echo "s3_gate.sh: --path 1|2|both" >&2; exit 2;; esac
rl_isnum "$DIG" || { echo "s3_gate.sh: --digits like 1e9" >&2; exit 2; }
nl=$(echo "$NODES" | tr ',' ' '); [ -n "$nl" ] || { echo "s3_gate.sh: --nodes 1,2" >&2; exit 2; }; for n in $nl; do rl_posint "$n" || { echo "s3_gate.sh: --nodes 1,2 (positive integers)" >&2; exit 2; }; done
nl=$(for n in $nl; do echo $((10#$n)); done | paste -sd" ")
maxn=$(echo "$nl" | tr ' ' '\n' | sort -n | tail -1)
rl_init s3
if [ "$DRY" = 1 ]; then
    for p in $pl; do for n in $nl; do d=$(rl_digits_site "$DIG" "$n"); rl_would "$(rl_cmdline "$p" "$n" "$d" ECALC_VERBOSE=2) <digit file> ; unpack_digits | sha1sum  (timeout ${TMO} min)"; done; done
    rl_finish SKIP "$HERE/s3_gate.sh --nodes $NODES --path $PATHS --digits $DIG"
fi
rl_prep "$pl" "$maxn"
export TMO WRITE=1
rl_line "gate: digits=$(rl_num "$DIG") (aac7 mode caps them per node)"
rl_line "  path nodes digits         rc verify hash             ref   total_s wall_s last_digits"
ref_hash=; bad=0 skipped=0 good=0
for p in $pl; do for n in $nl; do
    d=$(rl_digits_site "$DIG" "$n"); tag=g_p${p}_n${n}
    if [ "$n" -gt "$NHOSTS" ]; then rl_line "  $p $n $d SKIP (only $NHOSTS usable nodes in the allocation)"; skipped=$((skipped + 1)); continue; fi
    if [ ! -x "$(rl_bin "$p")" ]; then rl_line "  $p $n $d SKIP (no path $p binary)"; skipped=$((skipped + 1)); continue; fi
    rl_say "path $p, $n node(s), $d digits"
    rl_log "$(rl_cmdline "$p" "$n" "$d")"
    rl_ecrun "$tag" "$p" "$n" "$d"; rc=$?
    [ "$rc" = 98 ] || [ "$rc" = 99 ] && { rl_line "  $p $n $d SKIP (rc $rc: $(head -c 100 "$RUN/raw/$tag.log"))"; skipped=$((skipped + 1)); continue; }
    ref=$(rl_nou ecalc_ref_hash "$d"); match=-
    if [ -n "${E_HASH:-}" ]; then
        if [ -n "$ref" ]; then [ "$E_HASH" = "$ref" ] && match=yes || match=NO
        else [ -z "$ref_hash" ] && ref_hash=$E_HASH; [ "$E_HASH" = "$ref_hash" ] && match=same || match=NO; fi
    else match=NOHASH; fi
    okrun=1; { [ "$rc" = 0 ] && [ "${E_VERIFY:-NONE}" = OK ] && { [ "$match" = yes ] || [ "$match" = same ]; }; } || okrun=0
    [ $okrun = 1 ] && good=$((good + 1)) || { bad=$((bad + 1)); rl_errfiles "$RUN/raw/$tag.log" "$RUN/raw/$tag.log.gz"; }
    rl_line "$(printf '  %-4s %-5s %-14s %-2s %-6s %-16s %-5s %-7s %-6s %s' "$p" "$n" "$d" "$rc" "${E_VERIFY:-NONE}" "${E_HASH:--}" "$match" "${E_TOTAL:--}" "${E_WALL_S:--}" "$(rl_digits_tail "$RUN/raw/$tag.log" | cut -c1-24)")"
done; done
rl_kv runs_good "$good"; rl_kv runs_bad "$bad"; rl_kv runs_skipped "$skipped"
[ -n "$ref_hash" ] && rl_kv hash_first_run "$ref_hash"
if [ $bad -gt 0 ]; then rl_finish FAIL "$bad run(s) wrong (hash, VERIFY or rc); logs $RUN/raw/g_*.log" "$HERE/show.sh $RUN errors"; fi
if [ $good -eq 0 ]; then rl_finish FAIL "no gate run could be made" "$HERE/s2_build.sh; $HERE/alloc.sh --status"; fi
v=PASS; [ $skipped -gt 0 ] && v=PARTIAL
np=${PATHS/both/2}; [ "$PATHS" = both ] && [ ! -x "$(rl_bin 2)" ] && np=1
rl_finish "$v" "$HERE/s5_run.sh --nodes $maxn --digits <D> --path $np"

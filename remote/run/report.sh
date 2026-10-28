#!/bin/bash
# report.sh - the wrap-up paste: ONE table of every run of the session, the site facts, then (unless --keep) release the allocation.
#   report.sh [--keep] [--no-collect] [--dry-run]
# Reads the summaries under $REMOTE_OUT: every s5_run (one row each: nodes, digits, path, verdict, total_s, init/bs/dm/dc, measured/model, per-APU peak,
# VERIFY, last digits, the digit hash if taken, the settings), the newest s1 facts, s3 gate and s2 build verdicts.  Without --keep it then runs
# alloc.sh --release (cancels only the session job this toolkit created).  Before the release it runs collect.sh (the one bundle of all logs for the
# offline analysis: bundle= line below; --no-collect skips it).  Output <= 80 lines: the block the agent turns into the REPORT.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
KEEP=0 COLLECT=1
while [ $# -gt 0 ]; do case $1 in
  --keep) KEEP=1;; --no-collect) COLLECT=0;; --dry-run) DRY=1;; -h|--help) rl_help "$0"; exit 0;; *) echo "report.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
rl_init report
if [ "$DRY" = 1 ]; then rl_would "table of $REMOTE_OUT/s5_*/summary.txt, facts of the newest s1, then $([ $COLLECT = 1 ] && echo 'collect.sh (bundle of all logs), then ')$([ $KEEP = 1 ] && echo 'no release (--keep)' || echo 'alloc.sh --release')"; rl_finish SKIP "$HERE/report.sh"; fi
last() { ls -dt "$REMOTE_OUT/$1"_2* 2>/dev/null | head -1; }
kvf() { grep -a -m1 "^$2=" "$1" 2>/dev/null | cut -d= -f2-; }
s1=$(last s1)/summary.txt
if [ -f "$s1" ]; then
    rl_line "facts (s1): gpus=$(kvf "$s1" gpu_count) nics_cxi=$(kvf "$s1" nics_cxi) cpus=$(kvf "$s1" cpus) rocm=$(kvf "$s1" rocm) libfabric=$(kvf "$s1" libfabric)"
    rl_line "  sma=$(kvf "$s1" sma_versions) uffd=$(kvf "$s1" userfaultfd | cut -c1-70) shmem_test_rc=$(kvf "$s1" shmem_test_rc | cut -c1-60)"
else rl_line "facts: no s1 run under $REMOTE_OUT"; fi
for st in s2 s3 alloc; do f=$(last $st)/summary.txt; [ -f "$f" ] && rl_line "$st: $(grep -a -m1 -o 'verdict=[A-Z]*' "$f") ($(basename "$(dirname "$f")"))"; done
rl_line "runs (s5_run, oldest first): nodes digits path verdict total_s init/bs/dm/dc meas/model apu_peak/plan verify last20 hash | settings"
nr=0; ok=0; bestd=0; bests=
for d in $(ls -d "$REMOTE_OUT"/s5_2* 2>/dev/null | sort); do
    f=$d/summary.txt; [ -f "$f" ] || continue; grep -aq 'verdict=SKIP' "$f" && continue; [ -n "$(kvf "$f" nodes)" ] || continue
    v=$(grep -a -m1 -o 'verdict=[A-Z]*' "$f" | cut -d= -f2); dg=$(kvf "$f" digits); nr=$((nr + 1))
    ph=$(grep -a -m1 '^phases_s:' "$f" | sed -E 's/.*init=([^ ]*) bs=([^ ]*) recip=[^ ]* dm=([^ ]*) dc=([^ ]*).*/\1\/\2\/\3\/\4/')
    pk=$(kvf "$f" device_peak_apu_gb | awk '{ print $1 }'); pl=$(kvf "$f" device_plan_apu_gb | awk '{ print $1 }')
    rl_line "$(printf '%-3s %-5s %-14s p%-2s %-7s %-7s %-22s %-8s %-11s %-6s %-20s %s | %s' "$nr" "$(kvf "$f" nodes)" "$dg" "$(kvf "$f" path)" "$v" "$(kvf "$f" total_s)" "${ph:-NA}" "$(kvf "$f" meas_over_model)" "${pk:-NA}/${pl:-NA}" "$(kvf "$f" verify)" "$(kvf "$f" last_digits | tr -d ' ' | cut -c1-20)" "$(kvf "$f" digit_hash | awk '$1 ~ /^[0-9a-f]+$/ { print $1; next } { print "-" }')" "$(kvf "$f" settings | cut -c1-90)")"
    if [ "$v" = PASS ] && [ "$(kvf "$f" verify)" = OK ] && awk -v a="$dg" -v b="$bestd" 'BEGIN { exit !(a + 0 > b + 0) }'; then bestd=$dg; bests="$(kvf "$f" nodes) nodes, $(kvf "$f" total_s) s"; fi
    ok=$((ok + 1))
done
[ $nr -gt 0 ] || rl_line "(no s5_run summary under $REMOTE_OUT)"
rl_kv best_verified_digits "${bestd:-0} ($bests)"
rl_kv runs "$nr"
if [ "$COLLECT" = 1 ]; then
    cb=$(REMOTE_JOB=${REMOTE_JOB:-} bash "$HERE/collect.sh" 2>&1 | grep -a -E '^(bundle=|collect.sh:|left out)' | tr '\n' ' ' | cut -c1-420); rl_kv bundle "${cb:-collect.sh gave no output} (private: holds host names; the user transfers it, never GitHub)"
else rl_kv bundle "not made (--no-collect)"; fi
if [ "$KEEP" = 1 ]; then rl_kv allocation "kept (--keep)"
else
    ro=$(bash "$HERE/alloc.sh" --release 2>&1 | grep -a -E '^(verdict|stage=|released|note=)' | tr '\n' ' ' | cut -c1-200); rl_kv allocation "release: ${ro:-no output}"
fi
rl_finish PASS "(done) paste this block to the agent; the run directories under $REMOTE_OUT keep the full logs"

#!/bin/bash
# bench/selftest.sh - self-test of the runner without Slurm: a fake manifest with fake items (sleep/echo), TGT_FAKE_HOSTS=h1..h4
# instead of squeue/scontrol, 3 workers in parallel.  Checks: no item runs twice, dependencies respected, statuses right, item
# environment right, tier/only/dry-run, --reclaim, aggregate.py (METRICS, ANSWERS, idempotent), the real manifest validates.
# Pure bash + python3.  Usage: bench/selftest.sh [keep]      (keep: leave the temp dir)
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/tgtbench-selftest.XXXXXX")
[ "$1" = keep ] || trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bench/items" "$T/res"
export TGT_BENCH_DIR=$T/bench TGT_RES_DIR=$T/res TGT_FAKE_HOSTS=h1,h2,h3,h4
NF=0
ok() { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; NF=$((NF + 1)); }
chk() { local msg=$1; shift; if "$@"; then ok "$msg"; else bad "$msg"; fi; }

# ---- fake items
cat > "$T/bench/items/gen.sh" <<'IT'
#!/bin/bash
# fake item: gen.sh MODE [SECS]: logs start/end to $TGT_BENCH_DIR/../runlog, checks the environment
. "$TGT_ROOT/lib/common.sh"
mode=${1:-pass}; secs=${2:-1}
echo "$ID start $(date +%s.%N)" >> "$TGT_BENCH_DIR/../runlog"
n=$(echo "$HOSTS" | tr ',' '\n' | grep -c .)
[ "$n" = "$NODES" ] || { echo "hosts $n != nodes $NODES"; exit 5; }
[ -d "$OUT/raw" ] && [ "$PWD" = "$OUT" ] || exit 6
sleep "$secs"
tgt_kv m.value "$NODES x"
tgt_kv q.TQ01 "answer from $ID"
tgt_kv e.mode "$mode"
echo "$ID end $(date +%s.%N)" >> "$TGT_BENCH_DIR/../runlog"
case $mode in
    fail) exit 3;;
    partial) tgt_kv status PARTIAL;;
    skip) tgt_kv status SKIP;;
    hold) sleep 1000;;
esac
exit 0
IT
printf 'id\ttier\tprio\tnodes\test_min\tdeps\tquestions\tcmd\n' > "$T/bench/manifest.tsv"
row() { printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >> "$T/bench/manifest.tsv"; }
row cal01 1 10 1 1 - TQ01 "gen.sh pass 1"
row cal02 1 20 2 1 cal01 TQ01 "gen.sh pass 1"
row cal03 2 30 4 1 cal01 TQ01 "gen.sh pass 2"
row cal04 1 40 8 1 - TQ01 "gen.sh pass 1"
row cal05 1 15 1 1 - TQ01 "gen.sh fail 1"
row cal06 1 50 1 1 cal05 TQ01 "gen.sh pass 1"
row cal07 1 60 1 1 - TQ01 "gen.sh partial 1"
row cal08 1 70 0 1 - TQ01 "gen.sh pass 1"
row cal09 1 80 1 1 cal07 TQ01 "gen.sh pass 1"
for k in 10 11 12 13 14 15; do row cal$k 1 $((100 + k)) 1 1 - TQ01 "gen.sh pass 2"; done
# a tab-fragment in manifest.d is read too
mkdir -p "$T/bench/manifest.d"
{ printf 'id\ttier\tprio\tnodes\test_min\tdeps\tquestions\tcmd\n'; printf 'cal20\t1\t5\t2\t1\tcal09\tTQ01\tgen.sh pass 1\n'; } > "$T/bench/manifest.d/10_x.tsv"
cat > "$T/bench/QUESTIONS.md" <<'Q'
| TQ | question | decision | feeds | planned measurement | items | prio |
|---|---|---|---|---|---|---|
| TQ01 | fake question | none | none | none | cal01, cal02 | 10 |
| TQ02 | fake unplanned | none | none | none | cal99 | 300 |
Q

echo "validate"
chk "manifest valid" python3 "$here/manifest.py" validate >/dev/null
printf 'id\ttier\tprio\tnodes\test_min\tdeps\tquestions\tcmd\ncal01\t1\t1\t1\t1\t-\t-\tgen.sh\n' > "$T/bad.tsv"
chk "duplicate id detected" bash -c "cp $T/bad.tsv $T/bench/manifest.d/99_bad.tsv; python3 $here/manifest.py validate >/dev/null; r=\$?; rm $T/bench/manifest.d/99_bad.tsv; [ \$r = 1 ]"
printf 'id\ttier\tprio\tnodes\test_min\tdeps\tquestions\tcmd\ncal30\t1\t1\t1\t1\tnope\t-\tgen.sh\n' > "$T/bench/manifest.d/99_bad.tsv"
chk "unknown dep detected" bash -c "python3 $here/manifest.py validate >/dev/null; [ \$? = 1 ]"
rm "$T/bench/manifest.d/99_bad.tsv"

echo "dry-run"
d=$("$here/run.sh" --jobid 1 --dry-run 2>&1)
chk "dry-run lists cal20? (blocked behind cal09) no, cal01 first" bash -c "echo '$d' | grep -m1 'would run' | grep -q cal01"
chk "dry-run claimed nothing" test ! -d "$T/res/bench/cal01"
chk "tier 2 only: no tier-1 item listed" bash -c "'$here/run.sh' --jobid 1 --dry-run --tier 2 2>&1 | grep -c 'would run' | grep -qx 0"
chk "--only picks just that id" bash -c "[ \"\$('$here/run.sh' --jobid 1 --dry-run --only cal10 2>&1 | grep -c 'would run cal10')\" = 1 ]"

echo "three workers in parallel (4 fake hosts)"
for w in A B C; do "$here/run.sh" --jobid 1 --worker W$w > "$T/w$w.log" 2>&1 & done
wait
cat "$T"/w?.log | grep -E 'done:' | sed 's/^/    /'
log=$T/runlog
chk "no item ran twice" bash -c "[ -z \"\$(grep ' start ' $log | awk '{print \$1}' | sort | uniq -d)\" ]"
st() { grep '^status=' "$T/res/bench/$1/summary.txt" 2>/dev/null | tail -1 | cut -d= -f2; }
chk "cal01 PASS" test "$(st cal01)" = PASS
chk "cal05 FAIL (rc 3)" test "$(st cal05)" = FAIL
chk "cal05 rc recorded" grep -q '^rc=3' "$T/res/bench/cal05/summary.txt"
chk "cal06 not run (dep FAILed)" test ! -e "$T/res/bench/cal06"
chk "cal04 not run (8 nodes > 4 hosts)" test ! -e "$T/res/bench/cal04"
chk "cal07 PARTIAL (item status kept)" test "$(st cal07)" = PARTIAL
chk "cal09 ran after PARTIAL dep" test "$(st cal09)" = PASS
chk "cal20 (manifest.d, dep cal09) PASS" test "$(st cal20)" = PASS
chk "cal08 nodes=0 PASS" test "$(st cal08)" = PASS
chk "cal03 used 4 hosts" grep -q '^hosts=h1,h2,h3,h4' "$T/res/bench/cal03/summary.txt"
chk "one status line per summary" bash -c "for f in $T/res/bench/*/summary.txt; do [ \$(grep -c '^status=' \$f) = 1 ] || exit 1; done"
chk "DONE files hold rc" bash -c "[ \$(cat $T/res/bench/cal05/DONE) = 3 ] && [ \$(cat $T/res/bench/cal01/DONE) = 0 ]"
chk "dependencies respected" python3 - "$log" "$T/bench" <<'PY'
import sys, glob, os
log, bench = sys.argv[1], sys.argv[2]
t = {}
for l in open(log):
    i, k, v = l.split(); t[(i, k)] = float(v)
deps = {"cal02": "cal01", "cal03": "cal01", "cal09": "cal07", "cal20": "cal09"}
for i, d in deps.items():
    if (i, "start") in t and not t[(d, "end")] <= t[(i, "start")]:
        sys.exit(1)
PY
chk "parallel: two items overlapped" python3 - "$log" <<'PY'
import sys
t = {}
for l in open(sys.argv[1]):
    i, k, v = l.split(); t[(i, k)] = float(v)
ids = sorted({i for i, _ in t})
iv = sorted((t[(i, "start")], t[(i, "end")]) for i in ids if (i, "end") in t)
sys.exit(0 if any(b[0] < a[1] for a, b in zip(iv, iv[1:])) else 1)
PY
chk "pick says cal04/cal06 are what is left" bash -c "'$here/run.sh' --jobid 1 2>&1 | grep -q 'cal04 needs 8 nodes'"

echo "status / plan / reclaim"
chk "--status table" bash -c "'$here/run.sh' --status | grep -q 'cal05 .*done-FAIL'"
chk "--plan 3" bash -c "'$here/run.sh' --plan 3 | grep -q 'plan: 3 workers'"
mkdir -p "$T/res/bench/cal04/claim.d"; : > "$T/res/bench/cal04/claim.lock"; echo "x pid 1 d WX" > "$T/res/bench/cal04/claim.d/who"
chk "fresh claim is not stale" bash -c "'$here/run.sh' --reclaim cal04 >/dev/null; [ \$? = 1 ]"
touch -d '5 hours ago' "$T/res/bench/cal04/claim.lock"
chk "stale claim reclaimed" bash -c "'$here/run.sh' --reclaim cal04 >/dev/null && [ ! -e '$T/res/bench/cal04/claim.lock' ]"
chk "FAIL needs --force" bash -c "'$here/run.sh' --reclaim cal05 >/dev/null; [ \$? = 1 ]"
chk "--force redoes FAIL" bash -c "'$here/run.sh' --reclaim cal05 --force >/dev/null && [ ! -e '$T/res/bench/cal05' ]"

echo "remaining time of the allocation"
export TGT_FAKE_REMAIN_MIN=0.5
chk "no item fits 0.5 min" bash -c "'$here/run.sh' --jobid 1 --worker R 2>&1 | grep -q 'min left in the allocation'"
unset TGT_FAKE_REMAIN_MIN

echo "aggregate"
python3 "$here/aggregate.py" > "$T/agg1.out"
chk "METRICS.tsv rows" bash -c "grep -q -P '^cal01\tvalue\t1\tx' $T/res/bench/METRICS.tsv"
chk "ANSWERS.md has the answer" grep -q 'cal01: answer from cal01' "$T/res/bench/ANSWERS.md"
chk "coverage warning for TQ02" grep -q 'TQ02 has no item' "$T/res/bench/ANSWERS.md"
chk "progress line" grep -q 'bench progress:' "$T/agg1.out"
cp "$T/res/bench/METRICS.tsv" "$T/m1"; cp "$T/res/bench/ANSWERS.md" "$T/a1"
python3 "$here/aggregate.py" > /dev/null
chk "aggregate idempotent" bash -c "cmp -s $T/m1 $T/res/bench/METRICS.tsv && cmp -s $T/a1 $T/res/bench/ANSWERS.md"

echo "real manifest"
unset TGT_BENCH_DIR TGT_RES_DIR TGT_FAKE_HOSTS
if python3 "$here/manifest.py" validate; then ok "real manifest valid"; else bad "real manifest invalid"; fi

echo
if [ $NF = 0 ]; then echo "selftest PASS"; else echo "selftest FAIL ($NF)"; fi
[ "$1" = keep ] && echo "kept $T"
exit $((NF > 0))

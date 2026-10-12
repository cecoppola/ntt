#!/bin/bash
# aac7q queue item (H4, PLAN 28.5, results/H4.md): the multi-node and timing gates of branch h4 (decimal limbs only).
#   1. tools/gate.sh --build --nodes 2 (SHMEM build of ~/ntt-wt/h4, 1e9 over 2 nodes, digits identical)
#   2. tests/t_mn_grid 1 28 at 2, 3 and 6 node-processes (one per node; 3 and 6 are not powers of two: the general map)
#   3. 4e10 digits at 1 node, no digit writes, h4 vs main (0298fe2, built in ~/ntt-wt/h4main): 5 paired ABBA rounds on the
#      same node (MNRUN_NODES=1), the e16_headline aac7 launch line; mean difference h4 - main with its 95 % CI (t, 4 df)
# Output under ~/h4/q/: log/, gate2/, abba.tsv, summary.txt, done-marker H4_DONE.
# OWNER=h4
# NODES=6
# EST_MIN=50
# APPROVED=coordinator-2026-10-11
# EXCLUSIVE=0
set -u
J=${SLURM_JOB_ID:-}; NN=${AQ_NODES:-6}; WT=${WT:-$HOME/ntt-wt/h4}; MAIN=${MAIN:-$HOME/ntt-wt/h4main}; OUT=${H4_OUT:-$HOME/h4/q}
BUDGET_S=${BUDGET_S:-2700}; NPAIR=${NPAIR:-5}; DIG=${DIG:-40000000000}
mkdir -p $OUT/log $OUT/gate2; rm -f $OUT/H4_DONE; SUM=$OUT/summary.txt; : > $SUM; T0=$(date +%s); NPASS=0; NFAIL=0
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $SUM; }
pass() { NPASS=$((NPASS + 1)); say "PASS $1: $2"; }
fail() { NFAIL=$((NFAIL + 1)); say "FAIL $1: $2"; }
left() { echo $(( BUDGET_S - ($(date +%s) - T0) )); }
finish() { squeue -s -j $J -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 | sed 's/^/step still listed: /' | tee -a $SUM
  say "done: $NPASS PASS, $NFAIL FAIL"; echo "$([ $NFAIL = 0 ] && echo SUCCESS || echo FAILED) pass=$NPASS fail=$NFAIL $(date +%s)" > $OUT/H4_DONE; }
trap 'say "item interrupted"; finish; exit 1' INT TERM
[ -n "$J" ] && [ "$(squeue -j $J -h -o %T)" = RUNNING ] || { echo "hold $J not running"; exit 2; }
say "H4 aac7 item: hold $J, $NN nodes ($(squeue -j $J -h -o %N)); h4 $(git -C $WT log --oneline -1 | cut -c1-50); main $(git -C $MAIN log --oneline -1 | cut -c1-50)"
cd $WT/ecalc || exit 2; source aac7env.sh > /dev/null 2>&1
# ---- 1. gate, 2 nodes (builds h4 with SHMEM)
t1=$(date +%s); bash $WT/tools/gate.sh --build --nodes 2 --hold $J --line aac7 --out $OUT/gate2 > $OUT/log/gate2.log 2>&1; rc=$?
g=$(cat $OUT/gate2/gate.txt 2>/dev/null)
if [ $rc -eq 0 ]; then pass "gate 2 nodes" "$g"; else fail "gate 2 nodes" "rc $rc: ${g:-$(tail -2 $OUT/log/gate2.log | tr '\n' ' ')}"; fi
# ---- 2. t_mn_grid at 2, 3, 6 nodes
for n in 2 3 6; do
  [ $n -le $NN ] || continue; [ $(left) -gt 600 ] || { fail "t_mn_grid $n nodes" "no time left"; continue; }
  log=$OUT/log/t_mn_grid_$n.log; t1=$(date +%s)
  ( cd $WT/ecalc && SLURM_JOB_ID=$J MNRUN_NODES=$n MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) timeout 900 ./mnrun.sh $n env COMM_TRANSPORT=shmem ECALC_SEGV_TRACE=1 ./tests/t_mn_grid 1 28 ) > $log 2>&1 < /dev/null; rc=$?
  ok=$(grep -ac 'VERIFY OK' $log)
  if [ $rc -eq 0 ] && [ $ok -ge 1 ] && ! grep -aq 'VERIFY FAILED' $log; then pass "t_mn_grid $n nodes" "$ok VERIFY OK; $(( $(date +%s) - t1 )) s"
  else fail "t_mn_grid $n nodes" "rc $rc, $ok VERIFY OK; $(grep -a -m1 'VERIFY FAILED\|error\|abort\|Segmentation' $log | cut -c1-160)"; fi
done
# ---- 3. 4e10 ABBA at 1 node, h4 vs main
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ $WT/ecalc/e16_headline.sh)
[ -x $MAIN/ecalc/ecalc ] && ldd $MAIN/ecalc/ecalc | grep -q libsma || fail "abba" "main binary $MAIN/ecalc/ecalc missing or without SHMEM"
run1() { # dir label -> total seconds (or NA) in $TOT, VERIFY in $V
  local dir=$1 lab=$2 log=$OUT/log/$2.log
  ( cd $dir/ecalc && SLURM_JOB_ID=$J MNRUN_NODES=1 MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) timeout 600 ./mnrun.sh 1 env $LINE ECALC_SEGV_TRACE=1 ./ecalc $DIG ) > $log 2>&1 < /dev/null; RC=$?
  TOT=$(sed -E 's/^ *[0-9]+: //' $log | grep -a -m1 '^total' | awk '{print ($2 ~ /^[0-9.]+$/) ? $2 : "NA"}'); TOT=${TOT:-NA}
  V=none; grep -aq 'VERIFY OK' $log && V=ok; grep -aq 'VERIFY FAILED' $log && V=FAILED; }
printf 'pair\torder\th4_s\tmain_s\tdiff_s\tverify\n' > $OUT/abba.tsv
if [ -x $MAIN/ecalc/ecalc ]; then
  for p in $(seq 1 $NPAIR); do
    [ $(left) -gt 330 ] || { say "abba: budget, stop before pair $p"; break; }
    if [ $((p % 2)) = 1 ]; then order=AB; run1 $WT h4_$p; a=$TOT; va=$V; run1 $MAIN main_$p; b=$TOT; vb=$V
    else order=BA; run1 $MAIN main_$p; b=$TOT; vb=$V; run1 $WT h4_$p; a=$TOT; va=$V; fi
    d=$(awk -v a=$a -v b=$b 'BEGIN { if (a == "NA" || b == "NA") print "NA"; else printf "%.2f", a - b }')
    printf '%s\t%s\t%s\t%s\t%s\t%s/%s\n' $p $order $a $b $d $va $vb >> $OUT/abba.tsv
    say "abba pair $p ($order): h4 $a s (verify $va), main $b s (verify $vb), diff $d s"
  done
  awk -F'\t' 'NR > 1 && $5 != "NA" { n++; s += $5; ss += $5 * $5; A += $3; B += $4 }
    END { if (n < 2) { print "abba: fewer than 2 complete pairs"; exit }
      m = s / n; sd = sqrt((ss - n * m * m) / (n - 1)); se = sd / sqrt(n); tcrit = (n == 5) ? 2.776 : (n == 4) ? 3.182 : (n == 3) ? 4.303 : (n == 2) ? 12.706 : 2.571
      t = (se > 0) ? m / se : 0; at = (t < 0) ? -t : t; u = at / sqrt(4 + at * at); p = (n == 5) ? 2 * (1 - (0.5 + u * (3 - u * u) / 4)) : -1   # Student t, 4 df: F(t) = 1/2 + u (3 - u^2) / 4, u = t / sqrt(4 + t^2)
      printf "abba: n %d pairs: h4 mean %.2f s, main mean %.2f s, diff h4-main %+.2f s, 95%% CI [%+.2f, %+.2f] (t %.2f, p %s), sd %.2f\n", n, A / n, B / n, m, m - tcrit * se, m + tcrit * se, t, (p < 0 ? "n/a" : sprintf("%.3f", p < 0 ? 0 : p)), sd }' $OUT/abba.tsv | tee -a $SUM
  nv=$(awk -F'\t' 'NR > 1 && $6 != "ok/ok"' $OUT/abba.tsv | wc -l); nc=$(awk -F'\t' 'NR > 1 && $5 != "NA"' $OUT/abba.tsv | wc -l)
  if [ $nv -eq 0 ] && [ $nc -ge 2 ]; then pass "abba 4e10" "$nc complete pairs, every run VERIFY OK (the CI above says whether the rate changed)"; else fail "abba 4e10" "$nc complete pairs, $nv with a run not VERIFY OK"; fi
fi
finish

#!/bin/bash
# aac7q queue item (H4b, results/H4.md): re-run of ONLY the 4e10 ABBA, h4b (019e652, Makefile crt.o fix) vs main 0298fe2.
#   Worktree ~/ntt-wt/h4b (SHMEM build done on the login node), baseline ~/ntt-wt/h4main. 1 node, no digit writes,
#   e16_headline aac7 launch line; up to NPAIR=5 paired ABBA rounds, early stop after pair 3 or 4 when |t| exceeds the
#   two-sided 5 % critical value (4.303 / 3.182 for 2 / 3 df); mean difference h4b - main with its 95 % CI.
# Output under ~/h4/q2/: log/, abba.tsv, summary.txt, done-marker H4_DONE.
# OWNER=h4
# NODES=1
# EST_MIN=45
# APPROVED=yes (coordinator, 2026-10-11)
# EXCLUSIVE=0
set -u
J=${SLURM_JOB_ID:-}; NN=${AQ_NODES:-1}; WT=${WT:-$HOME/ntt-wt/h4b}; MAIN=${MAIN:-$HOME/ntt-wt/h4main}; OUT=${H4_OUT:-$HOME/h4/q2}
BUDGET_S=${BUDGET_S:-2700}; NPAIR=${NPAIR:-5}; DIG=${DIG:-40000000000}
mkdir -p $OUT/log; rm -f $OUT/H4_DONE; SUM=$OUT/summary.txt; : > $SUM; T0=$(date +%s); NPASS=0; NFAIL=0
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $SUM; }
pass() { NPASS=$((NPASS + 1)); say "PASS $1: $2"; }
fail() { NFAIL=$((NFAIL + 1)); say "FAIL $1: $2"; }
left() { echo $(( BUDGET_S - ($(date +%s) - T0) )); }
finish() { squeue -s -j $J -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 | sed 's/^/step still listed: /' | tee -a $SUM
  say "done: $NPASS PASS, $NFAIL FAIL"; echo "$([ $NFAIL = 0 ] && echo SUCCESS || echo FAILED) pass=$NPASS fail=$NFAIL $(date +%s)" > $OUT/H4_DONE; }
trap 'say "item interrupted"; finish; exit 1' INT TERM
[ -n "$J" ] && [ "$(squeue -j $J -h -o %T)" = RUNNING ] || { echo "hold $J not running"; exit 2; }
say "H4b aac7 ABBA item: hold $J, $NN nodes ($(squeue -j $J -h -o %N)); h4 $(git -C $WT log --oneline -1 | cut -c1-50); main $(git -C $MAIN log --oneline -1 | cut -c1-50)"
cd $WT/ecalc || exit 2; source aac7env.sh > /dev/null 2>&1
ldd $WT/ecalc/ecalc | grep -q libsma || fail "abba" "h4b binary without SHMEM"
# ---- 3. 4e10 ABBA at 1 node, h4 vs main
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ $WT/ecalc/e16_headline.sh)
[ -x $MAIN/ecalc/ecalc ] && ldd $MAIN/ecalc/ecalc | grep -q libsma || fail "abba" "main binary $MAIN/ecalc/ecalc missing or without SHMEM"
run1() { # dir label -> total seconds (or NA) in $TOT, VERIFY in $V
  local dir=$1 lab=$2 log=$OUT/log/$2.log
  ( cd $dir/ecalc && SLURM_JOB_ID=$J MNRUN_NODES=1 MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) timeout 600 ./mnrun.sh 1 env $LINE ECALC_SEGV_TRACE=1 ./ecalc $DIG ) > $log 2>&1 < /dev/null; RC=$?
  TOT=$(sed -E 's/^ *[0-9]+: //' $log | grep -a -m1 '^total' | awk '{print ($2 ~ /^[0-9.]+$/) ? $2 : "NA"}'); TOT=${TOT:-NA}
  V=none; grep -aq 'VERIFY OK' $log && V=ok; grep -aq 'VERIFY FAILED' $log && V=FAILED; }
printf 'pair\torder\th4b_s\tmain_s\tdiff_s\tverify\n' > $OUT/abba.tsv
if [ -x $MAIN/ecalc/ecalc ]; then
  for p in $(seq 1 $NPAIR); do
    [ $(left) -gt 330 ] || { say "abba: budget, stop before pair $p"; break; }
    if [ $((p % 2)) = 1 ]; then order=AB; run1 $WT h4_$p; a=$TOT; va=$V; run1 $MAIN main_$p; b=$TOT; vb=$V
    else order=BA; run1 $MAIN main_$p; b=$TOT; vb=$V; run1 $WT h4_$p; a=$TOT; va=$V; fi
    d=$(awk -v a=$a -v b=$b 'BEGIN { if (a == "NA" || b == "NA") print "NA"; else printf "%.2f", a - b }')
    printf '%s\t%s\t%s\t%s\t%s\t%s/%s\n' $p $order $a $b $d $va $vb >> $OUT/abba.tsv
    say "abba pair $p ($order): h4 $a s (verify $va), main $b s (verify $vb), diff $d s"
    if [ $p -ge 3 ] && [ $p -lt $NPAIR ]; then stop=$(awk -F'\t' 'NR > 1 && $5 != "NA" { n++; s += $5; ss += $5 * $5 } END { if (n < 3) { print 0; exit }
      m = s / n; sd = sqrt((ss - n * m * m) / (n - 1)); t = (sd > 0) ? m / (sd / sqrt(n)) : 0; if (t < 0) t = -t; c = (n == 3) ? 4.303 : (n == 4) ? 3.182 : 2.776; print (t > c) ? 1 : 0 }' $OUT/abba.tsv)
      [ "$stop" = 1 ] && { say "abba: |t| above the 5 % critical value after pair $p, stop"; break; }; fi
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

#!/bin/bash
# aac7q queue item K1c (results/K1b.md section 6): DIST_GATHER_RAW at 2 nodes, aac7 launch line, 6.441e10 digits per node.
#   1. tools/gate.sh --build --nodes 2 --env "DIST_GATHER_RAW=2" (SHMEM build of k1b in ~/ntt-wt/k1b, -DCOMM_SHMEM checked by gate.sh).
#   2. paired ABBA rounds (odd ABBA, even BAAB), A = DIST_GATHER_RAW=0, B = DIST_GATHER_RAW=2, 1.2882e11 digits on 2 nodes,
#      no digit output; stop rule: p<0.05 (|t| > t crit, 2 / 3 df) after >= 3 rounds, cap 4 rounds or the budget (40 min).
#   Per run: total, leaf mdev per node (the changed code path), bs, dm, VERIFY, rank-0 memory (device in use, pool peak
#   live, pool hipMalloc, the largest APU's driver-used GB), DIST_GATHER_RAW fallback notes.
# Output ~/k1c/: gate/, log/, runs.tsv, rounds.tsv, summary.txt, marker DONE.  Steps via mnrun.sh (--overlap --mem=0, no -w).
# OWNER=k1c
# NODES=2
# EST_MIN=45
# APPROVED=yes (coordinator, 2026-10-12)
# EXCLUSIVE=0
set -u
J=${SLURM_JOB_ID:-}; WT=${WT:-$HOME/ntt-wt/k1b}; E=$WT/ecalc; O=${K1C_OUT:-$HOME/k1c}
BUDGET_S=${BUDGET_S:-2400}; NROUND=${NROUND:-4}; DIG=${DIG:-128820000000}; NN=2
mkdir -p $O/log; rm -f $O/DONE; SUM=$O/summary.txt; : > $SUM; T0=$(date +%s); NFAIL=0
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $SUM; }
left() { echo $(( BUDGET_S - ($(date +%s) - T0) )); }
finish() { [ -n "$J" ] && squeue -s -j $J -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 | sed 's/^/step still listed: /' | tee -a $SUM
  say "done: $NFAIL FAIL"; echo "$([ $NFAIL = 0 ] && echo SUCCESS || echo FAILED) $1 $(date +%s)" > $O/DONE; }
trap 'say "item interrupted"; NFAIL=$((NFAIL + 1)); finish interrupted; exit 1' INT TERM
for f in $E/mnrun.sh $WT/tools/gate.sh $E/e16_headline.sh; do [ -e $f ] || { say "missing $f"; NFAIL=1; finish missing; exit 2; }; done
grep -q k_gather_raw $E/rns_dist.c || { say "not the k1b tree"; NFAIL=1; finish tree; exit 2; }
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ $E/e16_headline.sh); [ -n "$LINE" ] || { say "no LINE"; NFAIL=1; finish line; exit 2; }
STUB=${STUB:-0}
if [ $STUB = 0 ]; then
  [ -n "$J" ] && [ "$(squeue -j $J -h -o %T)" = RUNNING ] || { echo "hold $J not running"; exit 2; }
  say "K1c item: hold $J, $NN nodes; k1b $(git -C $WT log --oneline -1 | cut -c1-60)"
  cd $E; source aac7env.sh >/dev/null 2>&1; module load cray-dsmml cray-openshmemx >/dev/null 2>&1
  bash $WT/tools/gate.sh --build --nodes 2 --hold $J --env "DIST_GATHER_RAW=2" --out $O/gate --timeout 900 < /dev/null > $O/gate.out 2>&1; grc=$?
  say "gate rc=$grc: $(cut -c1-300 $O/gate/gate.txt 2>/dev/null)"
  grep -q -- -DCOMM_SHMEM $O/gate/build.log 2>/dev/null || { say "build log without -DCOMM_SHMEM"; grc=1; }
  [ $grc = 0 ] || { NFAIL=1; finish gate; exit 1; }
fi
run1() { # arm(0|2) label -> TOT MDEV(max over nodes) BS DM V MEMDEV PEAK HIPM DRV NOTE in globals
  local arm=$1 log=$O/log/$2.log
  if [ $STUB = 1 ]; then { echo "0: bs 48.9 s   N (seeds 2.6 school 0.0 batch 21.1 mdev $((7 - arm / 2)).2; pool)"; echo "1: bs 49.0 s (seeds 2.6 school 0.0 batch 21.1 mdev $((8 - arm / 2)).8; pool)"
      echo "0: mem[0] [bs] device 317.8 GB in use (per APU 79.5..79.5): planes 103.1, regions 0.0, block pool 214.2 (donated 0.0, borrowed 214.2, hipMalloc 0.0; live 50.0, peak live 1$((arm + 1))0.3, free 9.0), tables 0.01"
      echo "0: mem[0] [bs]   APU0: in use 79.5 GB: planes 25.77 (driver: 8$arm.9 of 137.4 GB used) | x"; echo "0: dm 61.5 s   X"; echo "0: VERIFY OK"
      echo "0: total   $((140 - arm)).$((RANDOM % 90 + 10)) s   (bs 48.9)"; } > $log
  else ( cd $E && SLURM_JOB_ID=$J MNRUN_NODES=$NN MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) timeout 480 ./mnrun.sh $NN env $LINE DIST_GATHER_RAW=$arm ECALC_SEGV_TRACE=1 ./ecalc $DIG ) > $log 2>&1 < /dev/null; fi
  TOT=$(grep -a -m1 -E '^ *0: total ' $log | awk '{print ($3 ~ /^[0-9.]+$/) ? $3 : "NA"}'); TOT=${TOT:-NA}
  MDEV=$(grep -a -E '^ *[0-9]+: bs ' $log | sed -nE 's/.* mdev ([0-9.]+).*/\1/p' | sort -g | tail -1); MDEV=${MDEV:-NA}
  BS=$(grep -a -E '^ *[0-9]+: bs ' $log | awk '{print $3}' | sort -g | tail -1); BS=${BS:-NA}
  DM=$(grep -a -m1 -E '^ *0: dm ' $log | awk '{print $3}'); DM=${DM:-NA}
  V=none; grep -aq 'VERIFY OK' $log && V=ok; grep -aq 'VERIFY FAILED' $log && V=FAILED
  MEMDEV=$(grep -a -E '^ *0: mem\[0\] \[[a-z0-9]+\] device' $log | sed -nE 's/.* device ([0-9.]+) GB in use.*/\1/p' | sort -g | tail -1); MEMDEV=${MEMDEV:-NA}
  PEAK=$(grep -a -E '^ *0: mem\[0\] \[[a-z0-9]+\] device' $log | sed -nE 's/.*peak live ([0-9.]+).*/\1/p' | sort -g | tail -1); PEAK=${PEAK:-NA}
  HIPM=$(grep -a -E '^ *0: mem\[0\] \[[a-z0-9]+\] device' $log | sed -nE 's/.*block pool [0-9.]+ \([^)]*hipMalloc ([0-9.]+);.*/\1/p' | sort -g | tail -1); HIPM=${HIPM:-NA}
  DRV=$(grep -a -E '^ *0: mem\[0\] ' $log | sed -nE 's/.*driver: ([0-9.]+) of.*/\1/p' | sort -g | tail -1); DRV=${DRV:-NA}
  NOTE=$(grep -a -c 'DIST_GATHER_RAW:' $log); }
printf 'round\tpos\tarm\ttotal_s\tmdev_max_s\tbs_max_s\tdm_s\tverify\tr0_dev_inuse_GB\tr0_pool_peaklive_GB\tr0_pool_hipMalloc_GB\tr0_maxAPU_driver_used_GB\tnotfit_notes\n' > $O/runs.tsv
printf 'round\torder\tA_mean_s\tB_mean_s\tB-A_s\tA_mdev\tB_mdev\tB-A_mdev_s\n' > $O/rounds.tsv
stat() { # column of rounds.tsv -> "n mean ci t"
  awk -F'\t' -v c=$1 'NR > 1 && $c != "NA" { n++; s += $c; ss += $c * $c } END { if (n < 2) { print n + 0, "NA NA NA NA"; exit } m = s / n; sd = sqrt((ss - n * m * m) / (n - 1)); se = sd / sqrt(n)
    tc = (n == 2) ? 12.706 : (n == 3) ? 4.303 : (n == 4) ? 3.182 : 2.776; t = (se > 0) ? m / se : 0; printf "%d %.3f %.3f %.2f %.2f\n", n, m, tc * se, t, tc }' $O/rounds.tsv; }
for r in $(seq 1 $NROUND); do
  [ $(left) -gt 760 ] || { say "budget: stop before round $r ($(left) s left)"; break; }
  if [ $((r % 2)) = 1 ]; then order=ABBA; seq="0 2 2 0"; else order=BAAB; seq="2 0 0 2"; fi
  i=0; ta=0; tb=0; ma=0; mb=0; ok=1
  for arm in $seq; do i=$((i + 1)); lab=r${r}_${i}$([ $arm = 0 ] && echo A || echo B)
    run1 $arm $lab
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $r $i $arm $TOT $MDEV $BS $DM $V $MEMDEV $PEAK $HIPM $DRV $NOTE >> $O/runs.tsv
    say "run $lab: total $TOT s, leaf mdev max $MDEV s, bs $BS, dm $DM, verify $V; rank 0: device $MEMDEV GB, pool peak live $PEAK, pool hipMalloc $HIPM, max APU driver-used $DRV GB, notes $NOTE"
    [ "$V" = ok ] || NFAIL=$((NFAIL + 1)); [ "$TOT" = NA ] || [ "$MDEV" = NA ] && ok=0
    [ $ok = 1 ] && if [ $arm = 0 ]; then ta=$(awk -v x=$ta -v y=$TOT 'BEGIN{print x+y}'); ma=$(awk -v x=$ma -v y=$MDEV 'BEGIN{print x+y}'); else tb=$(awk -v x=$tb -v y=$TOT 'BEGIN{print x+y}'); mb=$(awk -v x=$mb -v y=$MDEV 'BEGIN{print x+y}'); fi
  done
  if [ $ok = 1 ]; then awk -v r=$r -v o=$order -v ta=$ta -v tb=$tb -v ma=$ma -v mb=$mb 'BEGIN { printf "%d\t%s\t%.2f\t%.2f\t%.3f\t%.2f\t%.2f\t%.3f\n", r, o, ta/2, tb/2, (tb-ta)/2, ma/2, mb/2, (mb-ma)/2 }' >> $O/rounds.tsv
  else printf '%s\t%s\tNA\tNA\tNA\tNA\tNA\tNA\n' $r $order >> $O/rounds.tsv; fi
  say "round $r ($order): $(tail -1 $O/rounds.tsv | cut -f3-)"
  set -- $(stat 5)
  if [ $r -ge 3 ] && [ "$4" != NA ] && awk -v t=$4 -v c=$5 'BEGIN { exit !((t < 0 ? -t : t) > c) }'; then say "stop rule: p<0.05 after $r rounds"; break; fi
done
set -- $(stat 5); say "ABBA total (B=2 minus A=0): n $1 rounds, mean $2 s, 95% CI half-width $3 s, t $4 (crit $5)"
set -- $(stat 8); say "ABBA leaf mdev max over nodes (B-A): n $1 rounds, mean $2 s, 95% CI half-width $3 s, t $4 (crit $5)"
for a in 0 2; do awk -F'\t' -v a=$a 'NR > 1 && $3 == a { n++; if ($9+0 > d) d = $9; if ($10+0 > p) p = $10; if ($11+0 > h) h = $11; if ($12+0 > v) v = $12; k += $13 }
  END { printf "memory arm %d (rank 0, max over %d runs): device in use %.1f GB, pool peak live %.2f GB, pool hipMalloc %.2f GB, max APU driver-used %.1f GB, not-fit notes %d\n", a, n, d, p, h, v, k }' $O/runs.tsv | tee -a $SUM; done
finish "rounds=$(($(wc -l < $O/rounds.tsv) - 1))"

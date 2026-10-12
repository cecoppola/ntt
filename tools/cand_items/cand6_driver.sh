#!/bin/bash
# cand6_driver.sh (CAND, results/CAND.md on branch cand) - aac6 fire-and-forget, one PPAC_MI300A_SPX node:
#   clones ~/ntt-cand (cand) and ~/ntt-cand-main (main a448d73) from the full bundle ~/cand6/cand.bundle; builds main on the login node;
#   one <= 45-min hold (waits up to PEND_MAX for it); tools/gate.sh --build --nodes 1 --line none with all three switches on (1e9);
#   paired AB/BA rounds at 6.441e10 (A = main binary, B = cand binary with DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1);
#   stop: >= 4 pairs and p < 0.05 on the total, or 8 pairs, or no run starts after 40 min of the hold.  Rank-0 memory per arm.
# Start: cd ~ && setsid nohup bash ~/cand6/cand6_driver.sh > ~/cand6/driver.log 2>&1 < /dev/null &
# Output ~/cand6/: summary.txt pairs.tsv runs.tsv log/ gate/; marker ~/cand6/DONE (SUCCESS|FAILED: reason).  Cancels only its own hold.
set -u
O=$HOME/cand6; CB=$HOME/ntt-cand; CM=$HOME/ntt-cand-main; S=$O/summary.txt; N=${N:-64410000000}; PEND_MAX=${PEND_MAX:-21600}
SW="DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1"; MAINREV=${MAINREV:-a448d73}
VERDICT="FAILED: driver ended early"; J=
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $S; }
fin() { if [ -n "$J" ]; then scancel $J 2>/dev/null; sleep 3; say "hold $J cancelled (own job); squeue lines $(squeue -j $J -h 2>/dev/null | wc -l)"; fi
        echo "$VERDICT $(date +%s)" > $O/DONE; say "$VERDICT"; }
trap fin EXIT
mkdir -p $O/log; rm -f $O/DONE; : > $S
module load rocm >/dev/null 2>&1
rm -rf $CB $CM
git clone -q -b cand $O/cand.bundle $CB && git clone -q -b main $O/cand.bundle $CM && git -C $CM checkout -q $MAINREV || { VERDICT="FAILED: clone"; exit 1; }
say "cand $(git -C $CB log --oneline -1 | cut -c1-60) | main $(git -C $CM log --oneline -1 | cut -c1-60)"
bash -lc "module load rocm >/dev/null 2>&1; cd $CM/ecalc && make -s -j16" > $O/build_main.log 2>&1 && [ -x $CM/ecalc/ecalc ] || { VERDICT="FAILED: main build"; exit 1; }
say "main built"
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J cand6 --parsable -o /dev/null --wrap "sleep 2700" < /dev/null 2>>$O/sbatch.err); J=${J%%;*}
[ -n "$J" ] || { VERDICT="FAILED: sbatch $(tail -1 $O/sbatch.err)"; exit 1; }
say "hold $J submitted"; w=0
while [ "$(squeue -j $J -h -o %T)" != RUNNING ]; do
  [ -z "$(squeue -j $J -h)" ] && { VERDICT="FAILED: hold $J vanished"; J=; exit 1; }
  sleep 30; w=$((w + 30)); [ $w -ge $PEND_MAX ] && { VERDICT="FAILED: hold pending $PEND_MAX s"; exit 1; }
done
T0=$(date +%s); say "hold $J running after ${w} s pending"
el() { echo $(( $(date +%s) - T0 )); }
bash -lc "module load rocm >/dev/null 2>&1; cd $CB && tools/gate.sh --build --nodes 1 --hold $J --line none --env '$SW' --out $O/gate --timeout 900" > $O/gate.out 2>&1 < /dev/null
say "gate rc $? el $(el): $(tail -1 $O/gate/gate.txt 2>/dev/null | cut -c1-250)"
grep -q "gate PASS" $O/gate/gate.txt 2>/dev/null || { VERDICT="FAILED: gate"; exit 1; }
printf 'pair\tA_total\tB_total\n' > $O/pairs.tsv
printf 'run\tverify\ttotal_s\tr0_dev_inuse_GB\tr0_pool_peaklive_GB\tr0_maxAPU_driver_used_GB\n' > $O/runs.tsv
run() { # label arm -> RT
  local lab=$1 arm=$2 dir=$CM/ecalc sw=""; [ $arm = B ] && { dir=$CB/ecalc; sw=$SW; }
  local log=$O/log/$lab.log
  timeout 600 srun --jobid=$J -N1 -n1 --mem=0 --gpus=4 -c 192 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $dir; env ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ECALC_SEGV_TRACE=1 $sw ./ecalc $N" > $log 2>&1 < /dev/null
  local rc=$? v=none; grep -aq 'VERIFY OK' $log && v=ok; grep -aq 'VERIFY FAILED' $log && v=FAILED
  RT=$(grep -a -m1 '^total' $log | awk '{print $2}')
  local DEV=$(grep -a -E 'mem\[0\] \[[a-z0-9]+\] device' $log | sed -nE 's/.* device ([0-9.]+) GB in use.*/\1/p' | sort -g | tail -1)
  local PK=$(grep -a -E 'mem\[0\] \[[a-z0-9]+\] device' $log | sed -nE 's/.*peak live ([0-9.]+).*/\1/p' | sort -g | tail -1)
  local DR=$(grep -a -E 'mem\[0\] ' $log | sed -nE 's/.*driver: ([0-9.]+) of.*/\1/p' | sort -g | tail -1)
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' $lab $v ${RT:-NA} ${DEV:-NA} ${PK:-NA} ${DR:-NA} >> $O/runs.tsv
  say "$lab rc $rc verify $v total ${RT:-NA} el $(el) r0 dev ${DEV:-NA} peak ${PK:-NA} drv ${DR:-NA}"
  [ $rc = 0 ] && [ $v = ok ] && [ -n "$RT" ]; }
STATS="import sys; sys.path.insert(0, '$O'); from cand_stats import paired
ds = [float(l.split()[2]) - float(l.split()[1]) for l in open('$O/pairs.tsv').readlines()[1:]]
p = paired(ds)
print('n=%d mean B-A %+.3f s CI [%+.3f, %+.3f] p=%.4f STOP %d' % (p[4], p[0], p[1], p[2], p[3], int(p[4] >= 4 and p[3] < 0.05)) if p else 'n=%d STOP 0' % len(ds))"
NBAD=0; NP=0
for k in 1 2 3 4 5 6 7 8; do
  [ $(el) -lt 2400 ] || { say "pair $k not started (time)"; break; }
  if [ $((k % 2)) = 1 ]; then o="A B"; else o="B A"; fi
  ok=1; for a in $o; do run p${k}_$a $a || { ok=0; break; }; eval T$a=\$RT; done
  [ $ok = 1 ] || { NBAD=$((NBAD + 1)); say "pair $k dropped"; [ $NBAD -ge 2 ] && { VERDICT="FAILED: $NBAD failed pairs"; exit 1; }; continue; }
  printf '%d\t%s\t%s\n' $k $TA $TB >> $O/pairs.tsv; NP=$((NP + 1))
  ST=$(python3 -c "$STATS"); say "after pair $k: $ST"
  case "$ST" in *"STOP 1"*) say "stop rule met"; break;; esac
done
for a in A B; do awk -F'\t' -v a=$a 'NR > 1 && $1 ~ "_" a "$" { n++; if ($4+0 > d) d = $4; if ($5+0 > p) p = $5; if ($6+0 > v) v = $6 }
  END { printf "memory arm %s (rank 0, max over %d runs): device in use %.1f GB, pool peak live %.2f GB, max APU driver-used %.1f GB\n", a, n, d, p, v }' $O/runs.tsv | tee -a $S; done
[ $NP -ge 2 ] && VERDICT="SUCCESS: $NP pairs; $(python3 -c "$STATS")" || VERDICT="FAILED: only $NP pairs"

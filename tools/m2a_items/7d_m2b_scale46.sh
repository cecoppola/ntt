#!/bin/bash
# OWNER=m2a
# NODES=6
# EST_MIN=85
# APPROVED=yes (coordinator, 2026-10-12)
# EXCLUSIVE=0
# m2b (results/M2a.md, opportunity M2): scaling-study points at 4 and 6 nodes, 6.441e10 digits/node (2.5764e11 and 3.8646e11), main's SHMEM build
#  ~/ntt-wt/h4main (not modified; binary copied), e16_headline aac7 launch line + COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1, no digit writes.
#  Alternating order 4 6 6 4 4 6 6 4 ... (ABBA; at least 3 runs of each), one network program per node, no run starts after 80 min (max 12 runs).
#  Per run: total, stage times, init, XSTATS GB / t_wait, VERIFY, NIC MCU group; same parsing and columns as item 77 (77_m2a_scale12.sh).
# Own output dir and marker (nothing shared with items 77/78/7a): output ~/m2b/ (log/, nic/, summary.tsv, summary.txt); marker ~/m2b/DONE.  STUB=1: dry run.
set -u
WT=${WT:-$HOME/ntt-wt/h4main}; E=$WT/ecalc; O=$HOME/m2b; DONE=$HOME/m2b/DONE
DPN=${DPN:-64410000000}; BUDGET_S=${BUDGET_S:-4800}; MAXRUN=${MAXRUN:-12}; SEQ=${SEQ:-"4 6 6 4 4 6 6 4 4 6 6 4"}
mkdir -p $O/log $O/nic; rm -f $DONE; SUM=$O/summary.txt; : > $SUM
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ $E/e16_headline.sh); [ -n "$LINE" ] || { echo "no LINE"; exit 2; }
if [ "${STUB:-0}" = 1 ]; then echo "STUB ok: out $O done $DONE seq [$SEQ] dpn $DPN LINE=${LINE:0:60}..."; bash -n $0 && echo syntax ok
  [ -x $E/ecalc ] && ldd $E/ecalc 2>/dev/null | grep -q libsma && echo "binary ok (SHMEM)"; [ -f $E/mnrun.sh ] && echo mnrun ok; echo "STUB $(date +%s)" > $O/STUB_OK; exit 0; fi
J=${SLURM_JOB_ID:-}; [ -n "$J" ] && [ "$(squeue -j $J -h -o %T)" = RUNNING ] || { echo "hold $J not running"; exit 2; }
T0=$(date +%s); NFAIL=0; NPASS=0
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $SUM; }
finish() { squeue -s -j $J -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 | sed 's/^/step still listed: /' | tee -a $SUM
  say "done: $NPASS runs ok, $NFAIL not ok"; echo "$([ $NFAIL = 0 ] && echo SUCCESS || echo FAILED) ok=$NPASS bad=$NFAIL $(date +%s)" > $DONE; }
trap 'say "item interrupted"; finish; exit 1' INT TERM
cd $E || exit 2; source aac7env.sh > /dev/null 2>&1
EC=$O/ecalc_m2b; cp $E/ecalc $EC.tmp && mv $EC.tmp $EC; ldd $EC | grep -q libsma || { say "binary without SHMEM"; NFAIL=1; finish; exit 2; }
say "m2b: hold $J ($(squeue -j $J -h -o %N)), main $(git -C $WT log --oneline -1 | cut -c1-60), seq [$SEQ], $DPN digits/node"
printf 'k\tnodes\tdigits\trc\twall_s\ttotal_s\tbs_s\trecip_s\tdm_s\tinit_s\tverify\txGB_pe0\txtwait_pe0\tnicgrp\n' > $O/summary.tsv
X="COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1 ECALC_SEGV_TRACE=1"
snap() { # snap <file> <nodes>
  srun --jobid=$J -N$2 -n$2 --ntasks-per-node=1 --overlap --mem=0 -c 1 bash -c 'h=$(hostname -s); for c in 0 1 2 3; do t=/sys/class/cxi/cxi$c/device/telemetry; for k in 18 19 20 21 22 23 24 25 28 29 30 31 32 33; do echo "$h cxi$c mcu_$k $(cat $t/oxe_mcu_meas_$k 2>/dev/null | tr "\n" " ")"; done; done' < /dev/null > $1 2>&1; }
k=0
for n in $SEQ; do
  k=$((k + 1)); [ $k -le $MAXRUN ] || break
  [ $(( $(date +%s) - T0 )) -lt $BUDGET_S ] || { say "budget: stop before run $k"; break; }
  lab=r${k}_n$n; log=$O/log/$lab.log; D=$((n * DPN))
  srun --jobid=$J -N$n -n$n --ntasks-per-node=1 --overlap --mem=0 -c1 timeout 100 bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done' < /dev/null > /dev/null 2>&1
  snap $O/nic/${lab}_pre.txt $n; t1=$(date +%s)
  ( SLURM_JOB_ID=$J MNRUN_NODES=$n MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) timeout 600 ./mnrun.sh $n env $LINE $X $EC $D ) < /dev/null > $log 2>&1; rc=$?
  wall=$(( $(date +%s) - t1 )); snap $O/nic/${lab}_post.txt $n
  pl=$O/log/$lab.plain; sed -E 's/^ *[0-9]+: //' $log > $pl; grep -a 'wait' $pl | grep -a -i 'stat\|comm' | head -200 > $O/log/$lab.wait
  tot=$(grep -a -m1 '^total' $pl | awk '{print $2}'); st() { grep -a -m1 "^$1 " $pl | awk '{print $2}'; }
  ini=$(grep -a -m1 '^total' $pl | sed -nE 's/.*init ([0-9.]+).*/\1/p')
  v=none; grep -aq 'VERIFY OK' $pl && v=ok; grep -aq 'VERIFY FAILED' $pl && v=FAILED
  xg=$(awk '$1=="xstats" && $3=="0" && $4=="phase" { for (i=1;i<=NF;i++) { if ($i=="GB") g+=$(i+1); if ($i=="t_wait") w+=$(i+1) } } END { printf "%.1f\t%.1f", g, w }' $pl)
  grp=$(awk 'FNR==NR { if ($3 ~ /^mcu_/) { split($4, v, "@"); p[$1 " " $2 " " $3] = v[1] } ; next }
    $3 ~ /^mcu_/ { split($4, v, "@"); d = v[1] - p[$1 " " $2 " " $3]; kk = substr($3, 5) + 0; if (kk <= 25) a += d; else b += d }
    END { printf "%s(A=%.3g,B=%.3g)", (a > b ? "A" : (b > 0 ? "B" : "?")), a, b }' $O/nic/${lab}_pre.txt $O/nic/${lab}_post.txt 2>/dev/null)
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $k $n $D $rc $wall "${tot:-NA}" "$(st bs)" "$(st recip)" "$(st dm)" "${ini:-NA}" $v "$xg" "$grp" >> $O/summary.tsv
  say "run $k ($n node, $D digits): rc $rc, total ${tot:-NA} s, verify $v, wall $wall s, NIC group $grp"
  if [ $rc = 0 ] && [ $v = ok ] && [ -n "$tot" ]; then NPASS=$((NPASS + 1)); else NFAIL=$((NFAIL + 1)); fi
done
awk -F'\t' 'NR > 1 && $6 != "NA" && $11 == "ok" { n[$2]++; s[$2] += $6 } END { for (x in n) printf "mean total, %s node(s): %.2f s over %d runs (measured)\n", x, s[x] / n[x], n[x] }' $O/summary.tsv | sort | tee -a $SUM
finish

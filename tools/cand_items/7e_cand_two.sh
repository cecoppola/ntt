#!/bin/bash
# aac7q queue item CAND (results/CAND.md on branch cand): interaction test of the three off-by-default switches together.
#   1. tools/gate.sh --build --nodes 2 with DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1 (SHMEM build of cand in ~/ntt-wt/cand, -DCOMM_SHMEM checked).
#   2. paired AB/BA rounds at 2 nodes x 6.441e10 digits per node, aac7 LINE (e16_headline.sh), no digit writes:
#      A = main binary (~/ntt-wt/h4main/ecalc/ecalc, copied to ~/cand/ecalc_main), switches unset;
#      B = cand binary (~/cand/ecalc_cand) with all three on.  Print-only COMM_HS_TS=1 NEWTON_MN_RECIP_TS=2 in both arms.
#      Stop: >= 4 pairs and p < 0.05 on the total, or 6 pairs, or no run starts after 65 min.
#   NIC launch group per run (envprobe), rank-0 memory per arm.  Own output dir ~/cand/ only; marker ~/cand/DONE.  STUB=1: dry run.
# OWNER=cand
# NODES=2
# EST_MIN=80
# APPROVED=yes (coordinator, 2026-10-12)
# EXCLUSIVE=0
# ALLOW_SLOW=0
set -u
WT=$HOME/ntt-wt/cand; E=$WT/ecalc; O=$HOME/cand; NN=2; DPN=64410000000; EP=$WT/tools/envprobe.sh; ST=$O/cand_stats.py
EA=$O/ecalc_main; EB=$O/ecalc_cand; SW="DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1"
mkdir -p $O/log; rm -f $O/DONE; cd $E || exit 2
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ e16_headline.sh)
case "$LINE" in *MN_T_CHUNK_MB=2048*COMM_LAYER_INTER2=1*COMM_LAYER_VSLOT_POOL=1*) ;; *) echo "unexpected LINE: $LINE"; exit 2;; esac
grep -q BS_SEED_PULL dbig.c && grep -q DBIG_SMALL_BOUNCE dbig.c && grep -q DIST_GATHER_RAW rns_dist.c || { echo "not the cand tree"; exit 2; }
if [ "${STUB:-0}" = 1 ]; then
  echo "STUB ok: rev $(git rev-parse --short HEAD); LINE=${LINE:0:60}..."; bash -n $0 && echo syntax ok
  python3 $ST $O/log >/dev/null && echo stats ok
  ldd $HOME/ntt-wt/h4main/ecalc/ecalc | grep -q libsma && echo "main binary ok (SHMEM)"
  [ -f $EP ] && bash -n $EP && bash $EP --group /dev/null /dev/null && echo "envprobe ok"; exit 0; fi
: ${SLURM_JOB_ID:?need the runner allocation}
J=$SLURM_JOB_ID; T0=$(date +%s); S=$O/summary.txt; : > $S
el() { echo $(( $(date +%s) - T0 )); }
fin() { squeue -s -j $J -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 | sed 's/^/step still listed: /' >> $S; echo "DONE $1 $(date) el=$(el)" >> $S; echo "$1 $(date +%s)" > $O/DONE; }
trap 'echo "item interrupted" >> $S; fin INTERRUPTED; exit 1' INT TERM
source aac7env.sh >/dev/null 2>&1; module load cray-dsmml cray-openshmemx >/dev/null 2>&1
echo "start $(date) hold $J cand rev $(git rev-parse --short HEAD) hosts $(squeue -j $J -h -o %N)" >> $S
cp $HOME/ntt-wt/h4main/ecalc/ecalc $EA
bash $WT/tools/gate.sh --build --nodes 2 --hold $J --env "$SW" --out $O/gate --timeout 900 < /dev/null > $O/gate.out 2>&1; grc=$?
echo "gate rc=$grc el=$(el): $(cut -c1-300 $O/gate/gate.txt 2>/dev/null)" >> $S
grep -q -- -DCOMM_SHMEM $O/gate/build.log 2>/dev/null || { echo "build log without -DCOMM_SHMEM" >> $S; grc=1; }
[ $grc = 0 ] || { fin FAILED_gate; exit 1; }
cp $E/ecalc $EB
echo "binaries: A main $(md5sum < $EA | cut -c1-8) B cand $(md5sum < $EB | cut -c1-8)" >> $S
X="ECALC_SEGV_TRACE=1 FI_MR_CACHE_MONITOR=userfaultfd NEWTON_MN_RECIP_TS=2 COMM_HS_TS=1"
printf 'run\tgroup\tfgfc_M\n' > $O/groups.tsv; : > $O/mem.tsv
printf 'run\tverify\ttotal_s\tr0_dev_inuse_GB\tr0_pool_peaklive_GB\tr0_pool_hipMalloc_GB\tr0_maxAPU_driver_used_GB\n' > $O/runs.tsv
one() { # pair arm
  local k=$1 arm=$2 ec=$EA aw=""; [ $arm = B ] && { ec=$EB; aw=$SW; }
  [ $(el) -lt 3900 ] || { echo "skip p${k}_$arm (time)" >> $S; return 1; }
  srun --jobid=$J -N$NN -n$NN --ntasks-per-node=1 --overlap --mem=0 -c1 timeout 100 bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done; echo "$(hostname) $(grep -E "^(MemFree|Cached):" /proc/meminfo | tr -s " " | tr "\n" " ")"' < /dev/null 2>/dev/null | sed "s/^[^ ]*/node/; s/^/p${k}_$arm /" >> $O/mem.tsv
  local lab=cand_p${k}_$arm t1=$(date +%s) log=$O/log/cand_p${k}_$arm.log
  SLURM_JOB_ID=$J bash $EP --snap $O/$lab.pre > /dev/null 2>&1
  SLURM_JOB_ID=$J MNRUN_NODES=$NN MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) timeout 420 ./mnrun.sh $NN env $LINE $X $aw $ec $((NN * DPN)) < /dev/null > $log 2>&1; local rc=$?
  SLURM_JOB_ID=$J bash $EP --snap $O/$lab.post > /dev/null 2>&1
  local grp=$(bash $EP --group $O/$lab.pre $O/$lab.post 2>&1 | tail -1); rm -f $O/$lab.pre $O/$lab.post
  printf '%s\t%s\n' $lab "$grp" >> $O/groups.tsv
  local V=none; grep -aq "all $NN nodes: VERIFY OK" $log && V=ok; grep -aq 'VERIFY FAILED' $log && V=FAILED
  local TOT=$(grep -a -m1 -E '^ *0: total ' $log | awk '{print $3}')
  local DEV=$(grep -a -E '^ *0: mem\[0\] \[[a-z0-9]+\] device' $log | sed -nE 's/.* device ([0-9.]+) GB in use.*/\1/p' | sort -g | tail -1)
  local PK=$(grep -a -E '^ *0: mem\[0\] \[[a-z0-9]+\] device' $log | sed -nE 's/.*peak live ([0-9.]+).*/\1/p' | sort -g | tail -1)
  local HM=$(grep -a -E '^ *0: mem\[0\] \[[a-z0-9]+\] device' $log | sed -nE 's/.*block pool [0-9.]+ \([^)]*hipMalloc ([0-9.]+);.*/\1/p' | sort -g | tail -1)
  local DR=$(grep -a -E '^ *0: mem\[0\] ' $log | sed -nE 's/.*driver: ([0-9.]+) of.*/\1/p' | sort -g | tail -1)
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $lab $V ${TOT:-NA} ${DEV:-NA} ${PK:-NA} ${HM:-NA} ${DR:-NA} >> $O/runs.tsv
  echo "$lab rc=$rc wall=$(( $(date +%s) - t1 )) el=$(el) verify=$V total=${TOT:-NA} nicgroup/fgfcM=[$grp] r0 dev ${DEV:-NA} GB peak ${PK:-NA} hipMalloc ${HM:-NA} drv ${DR:-NA}" >> $S
  return 0; }
for k in 1 2 3 4 5 6; do
  if [ $((k % 2)) = 1 ]; then one $k A && one $k B || break; else one $k B && one $k A || break; fi
  if [ $k -ge 4 ] && python3 $ST $O/log --stop > /dev/null 2>&1; then echo "stop rule met after pair $k" >> $S; break; fi
done
python3 $ST $O/log > $O/stats.txt 2>&1; cat $O/stats.txt >> $S
for a in A B; do awk -F'\t' -v a=$a 'NR > 1 && $1 ~ "_" a "$" { n++; if ($4+0 > d) d = $4; if ($5+0 > p) p = $5; if ($6+0 > h) h = $6; if ($7+0 > v) v = $7 }
  END { printf "memory arm %s (rank 0, max over %d runs): device in use %.1f GB, pool peak live %.2f GB, pool hipMalloc %.2f GB, max APU driver-used %.1f GB\n", a, n, d, p, h, v }' $O/runs.tsv >> $S; done
{ echo "NIC launch group per run:"; cat $O/groups.tsv; } >> $S
nruns=$(ls $O/log/cand_p*_?.log 2>/dev/null | wc -l); nok=$(grep -l "all $NN nodes: VERIFY OK" $O/log/cand_p*_?.log 2>/dev/null | wc -l)
echo "runs=$nruns verify_ok=$nok" >> $S
fin SUCCESS; exit 0

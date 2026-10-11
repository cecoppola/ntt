#!/bin/bash
# aac7q queue item DRAFT (C10) - NOT in the queue.  Localises the reciprocal "chain" stall and the cold-start init on a fresh 4-node hold.
# OWNER=c10
# NODES=4
# EST_MIN=30
# APPROVED=coordinator-2026-10-11
# EXCLUSIVE=0
# ALLOW_SLOW=0
#
# What it does: 4 back-to-back 4-node runs (6.441e10 digits/node, production aac7 line from e16_headline.sh, no digit writes) on the fresh hold, so the
# hold-start spikes (first 4-9 runs of every hold seen so far) fall inside the item.  Added print-only switches NEWTON_MN_RECIP_TS=2 (per node: shift, gather,
# db_init, first pool get, upload, chain, in recip_mn_body) and ECALC_INIT_TL=1 (init timeline), plus a 1 Hz host-memory / reclaim sampler on every node per run.
# Driver: the a13 driver (copied here with snap.sh and t12_stats.py) with the two stand-alone probes removed
# by sed in a private copy.  Output: ~/sess1011c/c10/ (runs.tsv, log/r00N.log, mon/*, stats.txt, C10_DONE).  Exits by itself; the daemon cancels the hold.
set -u
J=$SLURM_JOB_ID
WT=${WT:-$HOME/ntt-wt/c10}; OUT=$HOME/sess1011c/c10; DRV=$WT/archive/drivers/c10; HD=$OUT/drv
mkdir -p $OUT $HD; cp $DRV/a13_drive.sh $DRV/t12_stats.py $DRV/snap.sh $HD/
cp $WT/archive/drivers/c10/c10_mon.sh $HD/
# private copy of the driver: no probes; start the per-node sampler before each run (300 s covers a run + prep slack, then it ends by itself)
sed -e 's|^  if \[ \$n = 1 \]; then snap p000_pre; probe p000; snap p000_post; fi$||' \
    -e 's|^  \[ -e \$OUT/STOP \] \|\| { probe p\$(printf %03d \$n); snap p\$(printf %03d \$n)_post; }$||' \
    -e 's|^  snap \${lab}_pre; run_one \$lab; snap \${lab}_post; parse \$lab$|  mon_start $lab; snap ${lab}_pre; run_one $lab; snap ${lab}_post; parse $lab|' \
    -e 's|^PREPS=|mon_start() { [ $STUB = 1 ] \&\& return 0; ( timeout 330 srun --jobid=$HOLD -N$NN -n$NN --ntasks-per-node=1 --overlap --mem=0 bash $HD/c10_mon.sh $1 ${MONSEC:-300} >/dev/null 2>\&1 < /dev/null \& ); }\nPREPS=|' \
    -e 's|--overlap bash|--overlap --mem=0 bash|g' \
    $HD/a13_drive.sh > $HD/c10_drive.sh
grep -q 'mon_start \$lab' $HD/c10_drive.sh || { echo "sed did not patch the driver"; exit 3; }
grep -q -- '--overlap --mem=0 bash' $HD/c10_drive.sh || { echo "mem=0 sed failed"; exit 3; }
grep -q 'probe p0' $HD/c10_drive.sh && { echo "probe still in driver"; exit 3; }
sed -i "s|sess1011c/a13|sess1011c/c10|g" $HD/snap.sh
if [ "${STUB:-0}" = 1 ]; then echo "STUB: driver patched OK ($HD/c10_drive.sh)"; fi
export HOLD=$J OUT WT HOLD_NAME=aq_c10 KEEP_HOLD=1 MAXRUNS=${MAXRUNS:-4} SPAN=${SPAN:-1500} MAXBAD=2 TMO=${TMO:-420}
export XTRA="NEWTON_MN_RECIP_TS=2 ECALC_LOG_CLOCKS=1"        # the driver already adds FI_MR_CACHE_MONITOR=userfaultfd COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1
cd $HD && bash $HD/c10_drive.sh > $OUT/driver.log 2>&1 < /dev/null
rc=$?
squeue -s -j $J -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3
exit $rc

#!/bin/bash
# aac7q queue item (O4 rerun, results/O4.md): the 1e10 byte comparison that item 80 could not make (its driver passed REF10 with a doubled path,
# because bash expands "O=$O/main ... REF10=$O/main/..." left to right in one prefix-assignment list).  Redoes only 1e10, 4 nodes, ASCII (fmt 0) and 60-bit packed 2 (fmt 2):
# digcmp against the exact 1e10 prefix of ~/ref/e_1e11.out (head -c 10000000002 + newline, the layout of ~/ref/e_1000000000.txt), unpack_digits -o + cat + cmp for fmt 2,
# digit files deleted afterwards (as is the 10 GB reference copy).  Build: ~/ntt-wt/o4 (item 80's binary).  Output ~/o4_aac7b/ (main/summary.txt, main/rates.tsv, summary.txt); marker ~/o4_aac7b/DONE.
# OWNER=o4
# NODES=4
# EST_MIN=30
# APPROVED=coordinator-2026-10-12
# EXCLUSIVE=0
set -u
WT=$HOME/ntt-wt/o4; O=$HOME/o4_aac7b; mkdir -p $O; rm -f $O/DONE
REF=$HOME/ref/e_1e11.out; R10=$O/main/data/ref10.txt
for f in $WT/ecalc/ecalc $WT/tools/unpack_digits $WT/tools/o4_rate.sh $REF; do [ -e $f ] || { echo "missing $f"; exit 2; }; done
ldd $WT/ecalc/ecalc | grep -q libsma || { echo "ecalc built without SHMEM"; exit 2; }
grep -q 'FMTS' $WT/tools/o4_rate.sh || { echo "tools/o4_rate.sh lacks FMTS (update ~/ntt-wt/o4 tools)"; exit 2; }
if [ "${STUB:-0}" = 1 ]; then echo "STUB ok (syntax: $(bash -n $0 && bash -n $WT/tools/o4_rate.sh && echo ok)); ref size $(stat -c %s $REF); prefix head: $(head -c 12 $REF)"; exit 0; fi
: ${SLURM_JOB_ID:?need the hold}; J=$SLURM_JOB_ID
[ "$(df --output=avail -BG $HOME | tail -1 | tr -dc 0-9)" -gt 80 ] || { echo "not enough free space"; exit 2; }
mkdir -p $O/main/data; (head -c 10000000002 $REF; echo) > $R10
[ "$(stat -c %s $R10)" = 10000000003 ] || { echo "reference prefix has the wrong size"; rm -f $R10; exit 2; }
cmp -n 1000000002 $R10 $HOME/ref/e_1000000000.txt || { echo "reference prefix differs from the 1e9 reference"; rm -f $R10; exit 2; }
ENVS="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1"
# absolute paths only (no $O in a later assignment of the same prefix list)
MODE=aac7 J=$J NN=4 E=$WT/ecalc U=$WT/tools/unpack_digits O=$HOME/o4_aac7b/main PLAN="10" ROUNDS=1 FMTS="0 2" RECHECK=0 CAP_S=1500 ENVS="$ENVS" REF9=$HOME/ref/e_1000000000.txt REF10=$R10 bash $WT/tools/o4_rate.sh > $O/main.out 2>&1
rm -f $R10; rm -f $O/main/data/s10_* 2>/dev/null
{ echo "== 1e10 byte comparison"; cat $O/main/summary.txt; echo; echo "reference: $REF first 10000000002 bytes + newline; $(grep -c 'identical' $O/main/summary.txt) identical, $(grep -c 'DIFFERS' $O/main/summary.txt) DIFFERS"; } > $O/summary.txt
rmdir $O/main/data 2>/dev/null; touch $O/DONE

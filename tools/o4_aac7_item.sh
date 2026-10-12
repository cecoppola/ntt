#!/bin/bash
# aac7q queue item (O4, results/O4.md): write time / rate and round trip of the digit output, ASCII vs packed 1 vs 60-bit packed 2 (ECALC_OUT_PACKED), 4 nodes,
# digits written (gate-type run): 1e9 and 1e10 against ~/ref (1e10: the exact-length prefix of ~/ref/e_1e11.out), unpack_digits -o text byte-identical, RECHECK;
# then 1e9 once more with ECALC_CHECKPOINT=1 (checkpoint + RECHECK from it).  Build: ~/ntt-wt/o4 at the o4 commit, make -B SHMEM_CRAY=1 GMP_HOME=~/gmp (needs -DCOMM_SHMEM).
# OWNER=o4
# NODES=4
# EST_MIN=45
# APPROVED=coordinator-2026-10-11
# EXCLUSIVE=0
set -u
WT=$HOME/ntt-wt/o4; O=$HOME/o4_aac7; mkdir -p $O; rm -f $O/O4_DONE
for f in $WT/ecalc/ecalc $WT/tools/unpack_digits $WT/tools/o4_rate.sh $HOME/ref/e_1000000000.txt $HOME/ref/e_1e11.out; do [ -e $f ] || { echo "missing $f"; exit 2; }; done
ldd $WT/ecalc/ecalc | grep -q libsma || { echo "ecalc built without SHMEM"; exit 2; }
if [ "${STUB:-0}" = 1 ]; then echo "STUB ok (syntax: $(bash -n $0 && echo ok))"; exit 0; fi
: ${SLURM_JOB_ID:?need the hold}; J=$SLURM_JOB_ID
[ "$(df --output=avail -BG $HOME | tail -1 | tr -dc 0-9)" -gt 120 ] || { echo "not enough free space"; exit 2; }
mkdir -p $O/main/data; (head -c 10000000002 $HOME/ref/e_1e11.out; echo) > $O/main/data/ref10.txt
ENVS="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1"
MODE=aac7 J=$J NN=4 E=$WT/ecalc U=$WT/tools/unpack_digits O=$O/main PLAN="9 10" ROUNDS=2 CAP_S=1700 ENVS="$ENVS" REF9=$HOME/ref/e_1000000000.txt REF10=$O/main/data/ref10.txt bash $WT/tools/o4_rate.sh > $O/main.out 2>&1
rm -f $O/main/data/ref10.txt
MODE=aac7 J=$J NN=4 E=$WT/ecalc U=$WT/tools/unpack_digits O=$O/ckpt PLAN="9" ROUNDS=1 CAP_S=500 ENVS="$ENVS ECALC_CHECKPOINT=1" REF9=$HOME/ref/e_1000000000.txt REF10= bash $WT/tools/o4_rate.sh > $O/ckpt.out 2>&1
{ echo "== main"; cat $O/main/summary.txt; echo "== ckpt"; cat $O/ckpt/summary.txt; } > $O/summary.txt
echo "SUCCESS $(date +%s)" > $O/O4_DONE

#!/bin/bash
# p24_job.sh <batches> - Phase 15 Batch 3 agent P24 (results/P2415.md): the node tests, unattended
# (login node: setsid nohup bash p24_job.sh 12 > ../results/P2415/job_<tag>.log 2>&1 &).  One 1-node job (-J P24, <= 45 min),
# cancelled at the end.  NODE=<name> pins the node (-w); the batches run in the order given:
#   1  t_p24 (the device part: k_crt24 against the host model), t_mn_grid with MN_P24=1 ECALC_NP=4 at 2, 3 (the general map) and
#      4 node-processes, at 2 with MN_T_CHUNK_MB=1 (the chunked rounds), and MN_P24=1 / 2 under ECALC_NP=auto with
#      ECALC_NP_AUTO_TERMS=1000000 (both prime counts in one run) at 2
#   2  mnaccept --only unit,mn with MN_P24=1 ECALC_NP=4 (10^8 at sizes 2, 3, 4 and 10^9 at 2, 4: identical digits)
#   3  mnaccept --only mn with MN_P24=2 ECALC_NP=auto ECALC_NP_AUTO_TERMS=100000000
#   4  the gates with the switch off: mnaccept --only unit,e9,mn (t_newton, t_mul, ..., 10^9 both bases, the mn step), 4e10 at size 1 against
#      results/e_4e10.out (the reference evicted first)
#   5  a timing pair at size 2 (10^10, ECALC_NP=4, RNS_VERBOSE=1): MN_P24 0 and 1, twice each, the dist_mn lines kept (tests/p24_timing.py)
#   6  after a kernel change: t_mn_grid at 2 and 3 under MN_P24=1 ECALC_NP=4, mnaccept --only mn under it
B=${1:-1}
cd "$(dirname "$0")" || exit 2
OUT=../results/P2415; mkdir -p "$OUT"
W=${NODE:+-w $NODE}
J=$(sbatch -p PPAC_MI300A_SPX -N1 $W --gpus=4 -t 0:45:00 -J P24 --parsable --wrap "sleep 2700") || exit 2
echo "p24_job batches $B: job $J submitted $(date)"
trap 'scancel $J' EXIT
while [ "$(squeue -j "$J" -h -o %T)" != "RUNNING" ]; do sleep 20; [ -z "$(squeue -j "$J" -h -o %T)" ] && { echo "job $J gone"; exit 2; }; done
NODEN=$(squeue -j "$J" -h -o %N); echo "job $J running on $NODEN $(date)"
R() { srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $PWD; $*"; }
M() { local to=$1 p=$2; shift 2; SLURM_JOB_ID=$J timeout "$to" ./mnrun.sh "$p" env "$@"; }
v() { local f=$1; echo "$(grep -ac 'VERIFY OK' "$f") VERIFY OK, $(grep -ac 'VERIFY FAILED' "$f") FAILED; $(grep -a 'VERIFY FAILED\|FAILED \|FATAL\|abort' "$f" | head -2 | cut -c1-200)"; }
b1() {
    local t0=$(date +%s)
    R "timeout 600 ./tests/t_p24" > "$OUT/t_p24_dev.log" 2>&1; echo "t_p24 rc $? $(tail -1 "$OUT/t_p24_dev.log") ($(( $(date +%s) - t0 )) s)"
    for p in 2 3 4; do t0=$(date +%s); M 900 $p MN_P24=1 ECALC_NP=4 ./tests/t_mn_grid 1 28 > "$OUT/grid_np4_$p.log" 2>&1; echo "t_mn_grid MN_P24=1 ECALC_NP=4 size $p rc $?: $(v "$OUT/grid_np4_$p.log") ($(( $(date +%s) - t0 )) s)"; done
    t0=$(date +%s); M 900 2 MN_P24=1 ECALC_NP=4 MN_T_CHUNK_MB=1 ./tests/t_mn_grid 1 28 > "$OUT/grid_np4_chunk.log" 2>&1; echo "t_mn_grid MN_P24=1 ECALC_NP=4 MN_T_CHUNK_MB=1 size 2 rc $?: $(v "$OUT/grid_np4_chunk.log") ($(( $(date +%s) - t0 )) s)"
    for m in 1 2; do t0=$(date +%s); M 900 2 MN_P24=$m ECALC_NP=auto ECALC_NP_AUTO_TERMS=1000000 ./tests/t_mn_grid 1 28 > "$OUT/grid_auto_$m.log" 2>&1; echo "t_mn_grid MN_P24=$m ECALC_NP=auto TERMS=1e6 size 2 rc $?: $(v "$OUT/grid_auto_$m.log") ($(( $(date +%s) - t0 )) s)"; done
}
b2() {
    local f=/tmp/p24_e8v; M 600 2 MN_P24=1 ECALC_NP=4 POOL_LOG=27 RNS_VERBOSE=1 ./ecalc 100000000 $f > "$OUT/e8_verbose.log" 2>&1
    echo "10^8 size 2 MN_P24=1 RNS_VERBOSE=1 rc $?: $(grep -ac 'P24' "$OUT/e8_verbose.log") P24 lines, $(grep -ac '^dist_mn node 0: 2^' "$OUT/e8_verbose.log") piece lines on node 0; $(grep -a 'mn: all 2 nodes' "$OUT/e8_verbose.log" | head -1 | cut -c1-60); digits $(R "./digcmp.sh $f $HOME/ntt/ecalc/ref/e_100000000.txt; rm -rf $f $f.*" 2>&1 | tail -1)"
    MN_P24=1 ECALC_NP=4 ./mnaccept.sh "$J" --only unit,mn > "$OUT/mnaccept_p24_np4.log" 2>&1; echo "mnaccept MN_P24=1 ECALC_NP=4 rc $?"; grep -aE "^(PASS|FAIL|==)" "$OUT/mnaccept_p24_np4.log"; }
b3() { MN_P24=2 ECALC_NP=auto ECALC_NP_AUTO_TERMS=100000000 ./mnaccept.sh "$J" --only mn > "$OUT/mnaccept_p24_auto2.log" 2>&1; echo "mnaccept MN_P24=2 auto rc $?"; grep -aE "^(PASS|FAIL|==)" "$OUT/mnaccept_p24_auto2.log"; }
b4() {
    ./mnaccept.sh "$J" --only unit,e9,mn > "$OUT/mnaccept_off.log" 2>&1; echo "mnaccept (switch off) rc $?"; grep -aE "^(PASS|FAIL|==)" "$OUT/mnaccept_off.log"
    F=/tmp/p24_e4e10.out
    R "python3 -c \"import os; fd=os.open('$HOME/ntt/ecalc/results/e_4e10.out', os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\"; rm -rf $F*; ./ecalc 40000000000 $F" > "$OUT/e4e10.log" 2>&1; echo "e4e10 rc $? $(grep -a '^total' "$OUT/e4e10.log" | tail -1)"
    R "./digcmp.sh $F $HOME/ntt/ecalc/results/e_4e10.out; rm -rf $F*" > "$OUT/e4e10_cmp.log" 2>&1; echo "e4e10 against results/e_4e10.out: $(cat "$OUT/e4e10_cmp.log")"
}
b5() {
    local REF10=/tmp/p24_ref_1e10.txt                        # the first 10^10 digits of results/e_4e10.out ("2." + digits + newline)
    R "head -c 10000000002 $HOME/ntt/ecalc/results/e_4e10.out > $REF10; echo >> $REF10; ls -la $REF10" 2>&1 | tail -1
    for rep in 1 2; do for p in 0 1; do
        local f=/tmp/p24_t10_$p; t0=$(date +%s)
        M 1200 2 MN_P24=$p ECALC_NP=4 POOL_LOG=29 RNS_VERBOSE=1 ./ecalc 10000000000 $f > "$OUT/t10_p${p}_$rep.log" 2>&1; rc=$?
        echo "10^10 size 2 MN_P24=$p rep $rep rc $rc: $(grep -a 'mn: all 2 nodes' "$OUT/t10_p${p}_$rep.log" | head -1 | cut -c1-80); $(grep -a '^total' "$OUT/t10_p${p}_$rep.log" | tail -1) ($(( $(date +%s) - t0 )) s)"
        echo "   digits: $(R "./digcmp.sh $f $REF10; rm -rf $f $f.*" 2>&1 | tail -1)"
    done; done
    R "rm -f $REF10"
}
b6() {   # after a kernel change: t_mn_grid at 2 and 3 under P24, and the mn step
    for p in 2 3; do t0=$(date +%s); M 900 $p MN_P24=1 ECALC_NP=4 ./tests/t_mn_grid 1 28 > "$OUT/grid6_np4_$p.log" 2>&1; echo "t_mn_grid MN_P24=1 ECALC_NP=4 size $p rc $?: $(v "$OUT/grid6_np4_$p.log") ($(( $(date +%s) - t0 )) s)"; done
    MN_P24=1 ECALC_NP=4 ./mnaccept.sh "$J" --only mn > "$OUT/mnaccept6_p24_np4.log" 2>&1; echo "mnaccept mn MN_P24=1 ECALC_NP=4 rc $?"; grep -aE "^(PASS|FAIL|==)" "$OUT/mnaccept6_p24_np4.log"
}
for b in $(echo "$B" | grep -o .); do echo "== batch $b $(date)"; b$b; done
echo "p24_job batches $B done $(date)"

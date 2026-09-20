#!/bin/bash
# mnaccept.sh <jobid> [--full] [--only <step,...>] - the standing regression of ecalc (PLAN.md §21 D3:
# the integrator's verify*.sh scripts made permanent).  Run on the login node from the ecalc/ of any
# clone against a one-node allocation (sbatch ... --wrap "sleep 2700"); everything runs on the node
# through srun / mnrun.sh, the digits go to the node's local /tmp, the logs to results/mnaccept/<jobid>/.
# One pass/fail line per step as it completes, a summary at the end, exit status = number of failures.
#
# Steps (--only picks a subset; --full adds the last):
#   unit   t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid at 2 node-processes
#   e9     10^9 at size 1, decimal (the default) and binary limbs (LIMB_BASE=2), cmp'd against the reference
#   mn     10^8 at sizes 2, 3, 4 and 10^9 at sizes 2, 4 on one node (the part files concatenated and cmp'd)
#   ckpt   10^8 at size 2: BS_CKPT_ABORT=6 on every node, then BS_RESTART=1 (restart identical to the reference)
#   full   4 x 10^10 at size 1 (--full only; ~ 2 min of the whole node): the reference evicted from the page
#          cache first, the wall printed, digits cmp'd against results/e_4e10.out
# References: ref/e_<digits>.txt of this clone or, when absent, ECALC_REF (default ~/ntt/ecalc/ref);
# the 4e10 file ECALC_REF_4E10 (default ~/ntt/ecalc/results/e_4e10.out).
J=$1; shift; [ -n "$J" ] || { echo "usage: $0 <jobid> [--full] [--only unit,e9,mn,ckpt,full]"; exit 2; }
FULL=0; ONLY=""
while [ $# -gt 0 ]; do case $1 in --full) FULL=1;; --only) ONLY=$2; shift;; *) echo "unknown option $1"; exit 2;; esac; shift; done
cd "$(dirname "$0")" || exit 2
ECALC_DIR=$PWD
REF=${ECALC_REF:-$HOME/ntt/ecalc/ref}; [ -s ref/e_1000000000.txt ] && REF=$ECALC_DIR/ref
REF4=${ECALC_REF_4E10:-$HOME/ntt/ecalc/results/e_4e10.out}
[ "$(squeue -j "$J" -h -o %T 2>/dev/null)" = "RUNNING" ] || { echo "job $J is not running"; exit 2; }
NODE=$(squeue -j "$J" -h -o %N)
OUT=results/mnaccept/$J; mkdir -p "$OUT"; SUM=$OUT/summary.txt
TMP=/tmp/mnaccept_$J
SHA=$(git rev-parse --short HEAD 2>/dev/null)
echo "== mnaccept: job $J on $NODE, $(date -Is), $(git log --oneline -1 2>/dev/null); ref $REF ==" | tee "$SUM"
T0=$(date +%s)
NPASS=0; NFAIL=0; FAILED=""
# pass <step> <text> / fail <step> <text>: one line each, into the summary
pass() { NPASS=$((NPASS + 1)); echo "PASS $1: $2" | tee -a "$SUM"; }
fail() { NFAIL=$((NFAIL + 1)); FAILED="$FAILED $1"; echo "FAIL $1: $2" | tee -a "$SUM"; }
want() { [ -z "$ONLY" ] || [[ ",$ONLY," == *",$1,"* ]]; }
# on the node: R <timeout> <cmd...> (single process, the four APUs); N <cmd...> (a shell on the node, no GPUs)
R() { local to=$1; shift; timeout "$to" srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $ECALC_DIR; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
# M <timeout> <procs> <cmd...>: node-processes through mnrun.sh
M() { local to=$1 p=$2; shift 2; SLURM_JOB_ID=$J timeout "$to" ./mnrun.sh "$p" env "$@"; }
# the digits of a run (a file, or the concatenated part files) against the reference: prints identical / DIFFERS
cmpref() { N "cat $1.part* > $1.all 2>/dev/null || cp $1 $1.all 2>/dev/null; cmp -s $1.all $2 && echo identical || echo DIFFERS; rm -f $1 $1.*"; }
total_of() { grep -a '^total' "$1" | tail -1 | sed 's/  */ /g' | cut -c1-40; }
N "mkdir -p $TMP; rm -f $TMP/*"

# ---- unit tests -------------------------------------------------------------------------------------------
if want unit; then
  for t in "t_ntt 24" "t_mul 20" "t_bs" "t_dbig 0" "t_newton 20" "t_verify" "t_out"; do
    n=${t// /_}; log=$OUT/unit_$n.log
    R 1200 "./tests/$t" > "$log" 2>&1; rc=$?
    v=$(grep -a 'VERIFY' "$log" | tail -1)
    if [ $rc -eq 0 ] && grep -aq 'VERIFY OK' "$log" && ! grep -aq 'VERIFY FAILED' "$log"; then pass "unit $t" "$v"; else fail "unit $t" "rc $rc; ${v:-no VERIFY line}"; fi
  done
  log=$OUT/unit_t_mn_grid_2.log
  M 1200 2 ./tests/t_mn_grid 1 28 > "$log" 2>&1; rc=$?
  ok=$(grep -ac 'VERIFY OK' "$log")
  if [ $rc -eq 0 ] && [ "$ok" -ge 1 ] && ! grep -aq 'VERIFY FAILED' "$log"; then pass "unit t_mn_grid (2 procs)" "$ok VERIFY OK: $(grep -a 'VERIFY OK' "$log" | head -1)"
  else fail "unit t_mn_grid (2 procs)" "rc $rc, $ok VERIFY OK; $(grep -a 'VERIFY FAILED\|error\|abort' "$log" | head -1)"; fi
fi

# ---- 10^9 at size 1, both bases -----------------------------------------------------------------------------
if want e9; then
  for b in 10 2; do
    log=$OUT/e9_b$b.log; f=$TMP/e9_b$b.txt
    R 900 "env LIMB_BASE=$b ./ecalc 1000000000 $f" > "$log" 2>&1; rc=$?
    c=$(cmpref "$f" "$REF/e_1000000000.txt")
    if [ $rc -eq 0 ] && grep -aq '^VERIFY OK' "$log" && [ "$c" = identical ]; then pass "e9 size 1 LIMB_BASE=$b" "$c; $(total_of "$log")"
    else fail "e9 size 1 LIMB_BASE=$b" "rc $rc; $c; $(grep -a 'VERIFY\|abort\|error' "$log" | head -1)"; fi
  done
fi

# ---- multi-process on one node --------------------------------------------------------------------------------
mnrun_check() { local tag=$1 p=$2 d=$3 pl=$4 to=$5; shift 5   # tag procs digits pool_log timeout [env...]
  local log=$OUT/$tag.log f=$TMP/$tag.txt
  M "$to" "$p" POOL_LOG=$pl "$@" ./ecalc "$d" "$f" > "$log" 2>&1; local rc=$?
  local c; c=$(cmpref "$f" "$REF/e_$d.txt")
  local ok; ok=$(grep -ac 'VERIFY OK' "$log")
  if [ $rc -eq 0 ] && grep -aq "mn: all $p nodes: VERIFY OK" "$log" && [ "$c" = identical ]; then pass "$tag" "$c; all $p nodes VERIFY OK; $(total_of "$log")"
  else fail "$tag" "rc $rc; $c; $ok VERIFY OK; $(grep -a 'VERIFY FAILED\|abort\|error\|Killed\|reset\|cannot' "$log" | head -1 | cut -c1-120)"; fi
}
if want mn; then
  for p in 2 3 4; do mnrun_check "mn e8 size $p" $p 100000000 27 600; done
  for p in 2 4;   do mnrun_check "mn e9 size $p" $p 1000000000 29 900; done
fi

# ---- checkpoint + restart at size 2 -----------------------------------------------------------------------------
if want ckpt; then
  CK=$TMP/ck8; N "rm -rf $CK"
  log=$OUT/ckpt_abort.log; f=$TMP/ck8.txt
  M 600 2 POOL_LOG=27 BS_CKPT_DIR=$CK BS_CKPT_EVERY=2 BS_CKPT_MIN_LEVEL=2 BS_CKPT_ABORT=6 ./ecalc 100000000 "$f" > "$log" 2>&1
  nab=$(grep -ac 'BS_CKPT_ABORT' "$log")
  log2=$OUT/ckpt_restart.log
  M 600 2 POOL_LOG=27 BS_CKPT_DIR=$CK BS_RESTART=1 ./ecalc 100000000 "$f" > "$log2" 2>&1; rc=$?
  c=$(cmpref "$f" "$REF/e_100000000.txt")
  rs=$(grep -a 'restart from' "$log2" | head -1 | sed 's/  */ /g' | cut -c1-80)
  if [ "$nab" -eq 2 ] && [ $rc -eq 0 ] && grep -aq 'mn: all 2 nodes: VERIFY OK' "$log2" && [ "$c" = identical ]; then pass "ckpt e8 size 2" "2 nodes exited at leaf level 6; restart: $c; $rs"
  else fail "ckpt e8 size 2" "$nab nodes exited; restart rc $rc; $c; $(grep -a 'VERIFY FAILED\|abort\|error\|another run' "$log2" | head -1 | cut -c1-120)"; fi
  N "rm -rf $CK"
fi

# ---- 4 x 10^10 at size 1 (--full) ------------------------------------------------------------------------------------
if [ $FULL = 1 ] && want full; then
  log=$OUT/full_4e10.log; f=$TMP/e4e10.txt
  N "python3 -c \"import os,sys; fd=os.open(sys.argv[1],os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\" $REF4 2>/dev/null; true"
  t1=$(date +%s)
  R 1800 "env ECALC_VERBOSE=2 ./ecalc 40000000000 $f" > "$log" 2>&1; rc=$?
  el=$(( $(date +%s) - t1 ))
  c=$(cmpref "$f" "$REF4")
  tot=$(grep -a '^total' "$log" | tail -1 | sed 's/  */ /g')
  echo "  4e10: $tot" | tee -a "$SUM"
  echo "  4e10: $(grep -a '^bs \|^dm \|^init' "$log" | sed 's/  */ /g' | cut -c1-80 | tr '\n' ';')" | tee -a "$SUM"
  if [ $rc -eq 0 ] && grep -aq '^VERIFY OK' "$log" && [ "$c" = identical ]; then pass "full 4e10 size 1" "$c; wall $(echo "$tot" | cut -c1-16) (${el} s elapsed with the write)"
  else fail "full 4e10 size 1" "rc $rc; $c; $(grep -a 'VERIFY\|abort\|error\|Killed' "$log" | head -1 | cut -c1-120)"; fi
fi

N "rm -rf $TMP"
echo "== $NPASS passed, $NFAIL failed${FAILED:+ ($FAILED )}; $(( $(date +%s) - T0 )) s; $SHA; logs in $OUT ==" | tee -a "$SUM"
exit $NFAIL

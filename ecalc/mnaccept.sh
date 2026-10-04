#!/bin/bash
# mnaccept.sh <jobid> [--full] [--only <step,...>] - the standing regression of ecalc (PLAN.md §21 D3:
# the integrator's verify*.sh scripts made permanent).  Run on the login node from the ecalc/ of any
# clone against a one-node allocation (sbatch ... --wrap "sleep 2700"); everything runs on the node
# through srun / mnrun.sh, the digits go to the node's local /tmp, the logs to results/mnaccept/<jobid>/.
# One pass/fail line per step as it completes, a summary at the end, exit status = number of failures.
#
# Steps (--only picks a subset; --full adds the last):
#   unit   t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_patch (Phase 15 KP), t_mn_grid at 2 node-processes
#   e9     10^9 at size 1, decimal (the default) and binary limbs (LIMB_BASE=2), cmp'd against the reference
#   mn     10^8 at sizes 2, 3, 4 and 10^9 at sizes 2, 4 on one node (the part files concatenated and cmp'd)
#   ckpt   10^8 at size 2: BS_CKPT_ABORT=6 on every node, then BS_RESTART=1 (restart identical to the reference)
#   recheck 10^9 at size 1 and 10^8 at size 2 with the top-level P, Q set (ECALC_CKPT_TOP=1, the default <outfile>.top), then
#          ECALC_RECHECK=1 on their files (RECHECK OK on every node, P and Q from the checkpoint) and on a copy with one
#          digit flipped (RECHECK FAILED)
#   corr   Phase 15 KP: forced division corrections on the defaults (packed output, ECALC_CORR_PATCH=2, MN_OUT_EARLY=1), at sizes
#          where e's digits end in a run of 0s / 9s, so the deferred correction's patch rewrites digits inside the file (not only
#          past d_out): 511461828 at size 1 with ECALC_TEST_CORR=14 (digits [511461818, 511461828]), then ECALC_RECHECK=1 on the
#          patched file; 108388422 at sizes 2 and 4 with ECALC_TEST_CORR=36 (digits [108388413, 108388422], node 0's part).
#          Each: VERIFY OK on every node, a patch line with bytes rewritten and none FAILED, the digits identical to the first
#          d + 2 bytes of the 10^9 reference + a newline
#   full   the standard size at size 1 (--full only; ECALC_STD_DIGITS, default 10^11 since Phase 14, ~5 min; 4 x 10^10 before): the reference evicted from the page
#          cache first, the wall printed, digits cmp'd against results/e_4e10.out; then ECALC_RECHECK=1 on its files
#          (the top-level set the run wrote by default into <outfile>.top) -- a second PASS/FAIL line with its time
#   stress 10 runs of 10^9 at size 4 with a plane pool forced to grow inside bs (--stress only, ~ 10 min; Phase 12 R):
#          RNS_POOL_GROW=1 POOL_LOG=27 RNS_POOL1_GB=0.4 RNS_BATCH_LOCAL_MIN=2 MEM_DPOOL_FILL=1 -- pool 1 at 381 MiB
#          against the batch-local tiles' 512 MiB of B planes (EC_NP x L x 8 at the pool-0-capped tile), so the tier
#          grows it inside bs (the growth is logged and required in every run) and the grown pool is filled with 0xA5;
#          every run must be identical with all 4 nodes VERIFY OK
# References: ref/e_<digits>.txt of this clone or, when absent, ECALC_REF (default ~/ntt/ecalc/ref);
# the 4e10 file ECALC_REF_4E10 (default ~/ntt/ecalc/results/e_4e10.out).
# Phase 16 A (results/A16.md): MNRUN_MODULES (the module line of R(); mnrun.sh's default), MNRUN_CPUS_PER_TASK (srun -c for R();
# `auto` = the node's CPUs: aac7 confines a task to 2 CPUs otherwise), MNACCEPT_TMP (the digits' directory on the node; default
# /tmp/mnaccept_<jobid> -- aac7's /tmp is RAM, so a directory under $HOME there).
J=$1; shift; [ -n "$J" ] || { echo "usage: $0 <jobid> [--full] [--stress] [--only unit,e9,mn,ckpt,recheck,corr,full,stress]"; exit 2; }
FULL=0; STRESS=0; ONLY=""
while [ $# -gt 0 ]; do case $1 in --full) FULL=1;; --stress) STRESS=1;; --only) ONLY=$2; shift;; *) echo "unknown option $1"; exit 2;; esac; shift; done
cd "$(dirname "$0")" || exit 2
ECALC_DIR=$PWD
REF=${ECALC_REF:-$HOME/ntt/ecalc/ref}; [ -s ref/e_1000000000.txt ] && REF=$ECALC_DIR/ref
for f in "$REF"/e_*; do [ -e "ref/$(basename "$f")" ] || ln -s "$f" ref/; done   # t_bs reads ref/e_<d>.sha256 relative to ecalc/ (the files are git-ignored)
STD=${ECALC_STD_DIGITS:-100000000000}                                   # the standard size (Phase 14: 10^11; ECALC_STD_DIGITS=40000000000 = the old 4e10 step)
case $STD in 40000000000) STDT=4e10;; 100000000000) STDT=1e11;; *) STDT=$STD;; esac
REF4=${ECALC_REF_STD:-${ECALC_REF_4E10:-$HOME/ntt/ecalc/results/e_$STDT.out}}
[ "$(squeue -j "$J" -h -o %T 2>/dev/null)" = "RUNNING" ] || { echo "job $J is not running"; exit 2; }
NODE=$(squeue -j "$J" -h -o %N)
OUT=results/mnaccept/$J; mkdir -p "$OUT"; SUM=$OUT/summary.txt
TMP=${MNACCEPT_TMP:-/tmp/mnaccept_$J}
if [ -z "${MNRUN_MODULES+x}" ]; then if [ -d /opt/cray/pe/sma ]; then MNRUN_MODULES="cray-dsmml cray-openshmemx rocm"; else MNRUN_MODULES=rocm; fi; fi
export MNRUN_MODULES
case "${MNRUN_CPUS_PER_TASK:-}" in "") RC=;; auto) RC="-c $(scontrol show node "$NODE" -o 2>/dev/null | sed -n 's/.*CPUTot=\([0-9]*\).*/\1/p')";; *) RC="-c $MNRUN_CPUS_PER_TASK";; esac
SHA=$(git rev-parse --short HEAD 2>/dev/null)
echo "== mnaccept: job $J on $NODE, $(date -Is), $(git log --oneline -1 2>/dev/null); ref $REF ==" | tee "$SUM"
T0=$(date +%s)
NPASS=0; NFAIL=0; FAILED=""
# pass <step> <text> / fail <step> <text>: one line each, into the summary
pass() { NPASS=$((NPASS + 1)); echo "PASS $1: $2" | tee -a "$SUM"; }
fail() { NFAIL=$((NFAIL + 1)); FAILED="$FAILED $1"; echo "FAIL $1: $2" | tee -a "$SUM"; }
want() { [ -z "$ONLY" ] || [[ ",$ONLY," == *",$1,"* ]]; }
# on the node: R <timeout> <cmd...> (single process, the four APUs); N <cmd...> (a shell on the node, no GPUs)
R() { local to=$1; shift; timeout "$to" srun --jobid="$J" -N1 --gpus=4 $RC --overlap bash -lc "module load $MNRUN_MODULES; cd $ECALC_DIR; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
# M <timeout> <procs> <cmd...>: node-processes through mnrun.sh
M() { local to=$1 p=$2; shift 2; SLURM_JOB_ID=$J timeout "$to" ./mnrun.sh "$p" env "$@"; }
# the digits of a run (a file, or the concatenated part files) against the reference: prints identical / DIFFERS
# the digits of a run (a file, or the concatenated part files) against the reference: prints identical / DIFFERS.
# Phase 15: digcmp.sh -- packed parts (the default output) through tools/unpack_digits --cmp, ASCII with O_DIRECT reads
cmpref() { N "$PWD/digcmp.sh $1 $2; rm -rf $1 $1.*"; }   # (also the .t1 sidecar and the .top directory)
total_of() { grep -a '^total' "$1" | tail -1 | sed 's/  */ /g' | cut -c1-40; }
N "mkdir -p $TMP; rm -f $TMP/*"

# ---- unit tests -------------------------------------------------------------------------------------------
if want unit; then
  for t in "t_ntt 24" "t_mul 20" "t_bs" "t_dbig 0" "t_newton 20" "t_verify" "t_out" "t_patch"; do   # (t_patch: Phase 15 KP -- both output forms)
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
  local log=$OUT/$tag.log f=$TMP/$tag.txt name="mn e$(( ${#d} - 1 )) size $p"
  M "$to" "$p" POOL_LOG=$pl "$@" ./ecalc "$d" "$f" > "$log" 2>&1; local rc=$?
  local c; c=$(cmpref "$f" "$REF/e_$d.txt")
  local ok; ok=$(grep -ac 'VERIFY OK' "$log")
  if [ $rc -eq 0 ] && grep -aq "mn: all $p nodes: VERIFY OK" "$log" && [ "$c" = identical ]; then pass "$name" "$c; all $p nodes VERIFY OK; $(total_of "$log")"
  else fail "$name" "rc $rc; $c; $ok VERIFY OK; $(grep -a 'VERIFY FAILED\|abort\|error\|Killed\|reset\|cannot' "$log" | head -1 | cut -c1-120)"; fi
}
if want mn; then
  for p in 2 3 4; do mnrun_check mn_e8_$p $p 100000000 27 600; done
  for p in 2 4;   do mnrun_check mn_e9_$p $p 1000000000 29 900; done
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

# ---- the recheck mode (Phase 12 W, results/W.md) -----------------------------------------------------------------------------
# recheck_check <tag> <procs> <digits> <pool_log> [env...]: the run with the top-level set, the recheck of its files, the recheck of a corrupted copy
recheck_check() { local tag=$1 p=$2 d=$3 pl=$4; shift 4
  local log=$OUT/$tag.log f=$TMP/$tag.txt name="recheck e$(( ${#d} - 1 )) size $p" rc c lg lb
  if [ "$p" = 1 ]; then R 900 "env POOL_LOG=$pl ECALC_CKPT_TOP=1 $* ./ecalc $d $f" > "$log" 2>&1; rc=$?
  else M 600 "$p" POOL_LOG=$pl ECALC_CKPT_TOP=1 "$@" ./ecalc "$d" "$f" > "$log" 2>&1; rc=$?; fi
  N "$PWD/digcmp.sh $f $REF/e_$d.txt" > "$OUT/$tag.cmp" 2>&1; c=$(cat "$OUT/$tag.cmp")
  if ! { [ $rc -eq 0 ] && grep -aq "VERIFY OK" "$log" && ! grep -aq "VERIFY FAILED" "$log" && [ "$c" = identical ]; }; then
    fail "$name" "the run: rc $rc; $c; $(grep -a 'VERIFY FAILED\|abort\|error\|Killed' "$log" | head -1 | cut -c1-120)"; N "rm -rf $f $f.* "; return; fi
  lg=$OUT/${tag}_recheck.log
  if [ "$p" = 1 ]; then R 600 "env ECALC_RECHECK=1 ./ecalc $d $f" > "$lg" 2>&1; rc=$?
  else M 600 "$p" ECALC_RECHECK=1 ./ecalc "$d" "$f" > "$lg" 2>&1; rc=$?; fi
  local nok; nok=$(grep -ac 'RECHECK OK' "$lg"); local nck; nck=$(grep -ac 'from the checkpoint' "$lg")
  local rl; rl=$(grep -a 'recheck: .*digits read' "$lg" | head -1 | sed 's/.*digits read from [^ ]* in \([0-9.]* s\).*/read in \1/')
  # one digit flipped in the (last) part file: the recheck must fail
  local pf; pf=$(N "ls $f.part* 2>/dev/null | tail -1"); [ -z "$pf" ] && pf=$f
  N "python3 -c \"import sys; p=sys.argv[1]; f=open(p,'r+b'); pk=f.read(8)==b'ECPACK18'; o=4096+8*100 if pk else 1000; f.seek(o); c=f.read(1); f.seek(o); f.write(bytes([c[0]^1]) if pk else (b'0' if c != b'0' else b'1')); f.close()\" $pf"   # (Phase 15: in a packed part, the low byte of a limb past the header)
  lb=$OUT/${tag}_recheck_bad.log
  if [ "$p" = 1 ]; then R 600 "env ECALC_RECHECK=1 ./ecalc $d $f" > "$lb" 2>&1
  else M 600 "$p" ECALC_RECHECK=1 ./ecalc "$d" "$f" > "$lb" 2>&1; fi
  local nbad; nbad=$(grep -ac 'RECHECK FAILED' "$lb"); local want_ok=$p; [ "$p" -gt 1 ] && want_ok=$((p + 1))
  if [ $rc -eq 0 ] && [ "$nok" -eq "$want_ok" ] && ! grep -aq 'RECHECK FAILED' "$lg" && [ "$nck" -ge "$p" ] && [ "$nbad" -ge 1 ] && ! grep -aq 'RECHECK OK' "$lb"; then
    pass "$name" "run $c; recheck: $nok RECHECK OK, P, Q from the checkpoint on $nck node(s), $rl; corrupted copy: $nbad RECHECK FAILED"
  else fail "$name" "run $c; recheck rc $rc: $nok RECHECK OK ($want_ok wanted), $nck from the checkpoint; corrupted: $nbad RECHECK FAILED; $(grep -a 'RECHECK FAILED\|DIFFER\|BAD\|cannot\|error' "$lg" | head -1 | cut -c1-120)"; fi
  N "rm -rf $f $f.*"
}
if want recheck; then
  recheck_check recheck_e9_1 1 1000000000 29
  recheck_check recheck_e8_2 2 100000000 27
fi

# ---- Phase 15 KP: forced corrections on the defaults, patching file bytes (results/KP15.md) ------------------------------------
# corr_check <tag> <procs> <digits> <k> [recheck]: the run with ECALC_TEST_CORR=<k>, the patch lines, the digits against the reference prefix
corr_check() { local tag=$1 p=$2 d=$3 k=$4 rk=$5 log=$OUT/$1.log f=$TMP/$1.txt rf=$TMP/ref_$3.txt name="corr d=$3 size $2 TEST_CORR=$4" rc c np nb nf
  N "head -c $(( d + 2 )) $REF/e_1000000000.txt > $rf; echo >> $rf"
  if [ "$p" = 1 ]; then R 600 "env POOL_LOG=29 ECALC_TEST_CORR=$k ./ecalc $d $f" > "$log" 2>&1; rc=$?
  else M 600 "$p" POOL_LOG=27 ECALC_TEST_CORR=$k ./ecalc "$d" "$f" > "$log" 2>&1; rc=$?; fi
  c=$(N "$PWD/digcmp.sh $f $rf")
  np=$(grep -ac 'patch: X [+-]' "$log"); nf=$(grep -a 'patch: X [+-]\|mn_out: patch' "$log" | grep -ac 'FAILED\|holds\|does not\|cannot\|not this run')
  nb=$(grep -a 'patch: digits' "$log" | grep -ao '[0-9]* bytes in [1-9]' | head -1)
  local okrun=0; if [ "$p" = 1 ]; then grep -aq '^VERIFY OK' "$log" && okrun=1; else grep -aq "mn: all $p nodes: VERIFY OK" "$log" && okrun=1; fi
  local rtxt=""
  if [ -n "$rk" ] && [ $okrun = 1 ]; then
    R 600 "env ECALC_RECHECK=1 ./ecalc $d $f" > "$OUT/${tag}_recheck.log" 2>&1
    if grep -aq '^RECHECK OK' "$OUT/${tag}_recheck.log"; then rtxt="; ECALC_RECHECK=1 on the patched file: RECHECK OK"; else rtxt="; ECALC_RECHECK=1: FAILED"; okrun=0; fi
  fi
  if [ $rc -eq 0 ] && [ $okrun = 1 ] && [ "$c" = identical ] && [ "$np" -ge 1 ] && [ "$nf" -eq 0 ] && [ -n "$nb" ]; then pass "$name" "$c; patched ($nb part file); $(total_of "$log")$rtxt"
  else fail "$name" "rc $rc; $c; $np patch lines, $nf failures, rewritten: ${nb:-none}$rtxt; $(grep -a 'VERIFY FAILED\|FAILED\|abort\|holds' "$log" | head -1 | cut -c1-120)"; fi
  N "rm -rf $f $f.* $rf"
}
if want corr; then
  corr_check corr_c511_1 1 511461828 14 recheck
  corr_check corr_c108_2 2 108388422 36
  corr_check corr_c108_4 4 108388422 36
fi

# ---- 4 x 10^10 at size 1 (--full) ------------------------------------------------------------------------------------
if [ $FULL = 1 ] && want full; then
  log=$OUT/full_$STDT.log; f=$TMP/e$STDT.txt
  N "python3 -c \"import os,sys; fd=os.open(sys.argv[1],os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\" $REF4 2>/dev/null; true"
  t1=$(date +%s)
  R 1800 "env ECALC_VERBOSE=2 ./ecalc $STD $f" > "$log" 2>&1; rc=$?
  el=$(( $(date +%s) - t1 ))
  lg=$OUT/full_${STDT}_recheck.log; t2=$(date +%s)                        # Phase 12 W: the recheck from the run's files (digits, .t1, .top)
  R 1200 "env ECALC_RECHECK=1 ./ecalc $STD $f" > "$lg" 2>&1; rc2=$?
  el2=$(( $(date +%s) - t2 ))
  c=$(cmpref "$f" "$REF4")
  tot=$(grep -a '^total' "$log" | tail -1 | sed 's/  */ /g')
  echo "  $STDT: $tot" | tee -a "$SUM"
  echo "  $STDT: $(grep -a '^bs \|^dm \|^init\|checkpoint: the top-level' "$log" | sed 's/  */ /g' | cut -c1-100 | tr '\n' ';')" | tee -a "$SUM"
  if [ $rc -eq 0 ] && grep -aq '^VERIFY OK' "$log" && [ "$c" = identical ]; then pass "full $STDT size 1" "$c; wall $(echo "$tot" | cut -c1-16) (${el} s elapsed with the write)"
  else fail "full $STDT size 1" "rc $rc; $c; $(grep -a 'VERIFY\|abort\|error\|Killed' "$log" | head -1 | cut -c1-120)"; fi
  if [ $rc2 -eq 0 ] && grep -aq '^RECHECK OK' "$lg" && grep -aqE 'from the (checkpoint|sidecar)' "$lg"; then pass "full $STDT recheck" "RECHECK OK, P, Q from the $(grep -aoE 'from the (checkpoint|sidecar)' "$lg" | head -1 | cut -d' ' -f3) (the top set is off by default since 3524146); ${el2} s; $(grep -a 'recheck: .*digits read' "$lg" | head -1 | sed 's/.*digits read from [^ ]* in \([0-9.]* s\).*/file read in \1/')"
  else fail "full $STDT recheck" "rc $rc2; $(grep -a 'RECHECK\|DIFFER\|BAD\|cannot\|error' "$lg" | head -1 | cut -c1-120)"; fi
fi

# ---- forced pool growth at 10^9 size 4 (--stress) ---------------------------------------------------------------------
if [ $STRESS = 1 ] && want stress; then
  nok=0; nfail=0; first=""
  for i in $(seq 1 10); do
    log=$OUT/stress_$i.log; f=$TMP/stress_$i.txt
    M 900 4 POOL_LOG=27 RNS_POOL_GROW=1 RNS_POOL1_GB=0.4 RNS_BATCH_LOCAL_MIN=2 MEM_DPOOL_FILL=1 RNS_VERBOSE=1 ./ecalc 1000000000 "$f" > "$log" 2>&1; rc=$?
    c=$(cmpref "$f" "$REF/e_1000000000.txt")
    ng=$(grep -ac 'grows inside a phase' "$log")
    if [ $rc -eq 0 ] && grep -aq "mn: all 4 nodes: VERIFY OK" "$log" && [ "$c" = identical ] && [ "$ng" -ge 1 ]; then nok=$((nok + 1))
    else nfail=$((nfail + 1)); [ -n "$first" ] || first="run $i: rc $rc; $c; $ng growth lines; $(grep -a 'VERIFY FAILED\|abort\|error\|Killed\|would grow' "$log" | head -1 | cut -c1-100)"; fi
    echo "  stress run $i: $c, $(grep -ac 'VERIFY OK' "$log") VERIFY OK, $ng pool growths, $(total_of "$log")" | tee -a "$SUM"
  done
  if [ $nfail -eq 0 ]; then pass "stress e9 size 4 forced growth" "$nok of 10 identical, all nodes VERIFY OK, the growth logged in every run"
  else fail "stress e9 size 4 forced growth" "$nok of 10 passed; $first"; fi
fi

N "rm -rf $TMP"
echo "== $NPASS passed, $NFAIL failed${FAILED:+ ($FAILED )}; $(( $(date +%s) - T0 )) s; $SHA; logs in $OUT ==" | tee -a "$SUM"
exit $NFAIL

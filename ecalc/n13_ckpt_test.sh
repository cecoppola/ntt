#!/bin/bash
# n13_ckpt_test.sh <jobid> [--only restart,bg,budget,time] - Phase 13 N's checkpoint tests (results/N13.md), run from ecalc/ on
# the login node against a one-node allocation, like mnaccept.sh; logs in results/n13/<jobid>/, one PASS/FAIL line per test.
#   restart  TASKS 1.7: tree sets at 10^8 size 4 (BS_CKPT_ABORT_TREE=1, the default schedule 2,4); a restart under MN_GROUPS=4
#            must fail on every node with the schedule message (no hang); a restart at size 2 from them must fail too;
#            the matching restart must be identical; the same pair at 10^9 size 2 (the top set: the background writer's abort path).
#            With ./ecalc.base (the pre-Phase-13 binary, when present) the MN_GROUPS=4 restart shows the trap it closes.
#   bg       TASKS 1.4: the size > 1 top set in the background (ECALC_CKPT_TOP=1) at 10^9 size 2 and 10^8 size 3 (a schedule
#            level that is not log2 size), each rechecked from its set; BS_CKPT_TREE_BG=0 (synchronous) for the comparison
#   budget   TASKS 4.1: at 10^9, sizes 1 and 2, a slow disk emulated (BS_CKPT_BG_MBS): ECALC_CKPT_TOP=2 must drop the set (no
#            files left), =1 must keep it (the wait shows); digits identical; ECALC_CKPT_TOP=2 on the real disk
#   time     TASKS 4.1: 4 x 10^10 at size 1 (N13_TIME="tag bin mode mbs;..." overrides): ECALC_CKPT_TOP=2 and =1 at an emulated
#            310 MB/s (aac6's slow path), =1 on the node's disk, =1 with ./ecalc.base (Phase 12: Q released at the end of the
#            division), no set -- walls and waits
J=$1; shift; [ -n "$J" ] || { echo "usage: $0 <jobid> [--only restart,bg,budget,time]"; exit 2; }
ONLY=""; while [ $# -gt 0 ]; do case $1 in --only) ONLY=$2; shift;; *) echo "unknown option $1"; exit 2;; esac; shift; done
cd "$(dirname "$0")" || exit 2
ECALC_DIR=$PWD
REF=${ECALC_REF:-$HOME/ntt/ecalc/ref}; [ -s ref/e_1000000000.txt ] && REF=$ECALC_DIR/ref
REF4=${ECALC_REF_4E10:-$HOME/ntt/ecalc/results/e_4e10.out}
NODE=$(squeue -j "$J" -h -o %N)
OUT=results/n13/$J; mkdir -p "$OUT"; SUM=$OUT/summary.txt
TMP=/tmp/n13_$J
echo "== n13_ckpt_test: job $J on $NODE, $(date -Is), $(git log --oneline -1 2>/dev/null) ==" | tee "$SUM"
NPASS=0; NFAIL=0
pass() { NPASS=$((NPASS + 1)); echo "PASS $1: $2" | tee -a "$SUM"; }
fail() { NFAIL=$((NFAIL + 1)); echo "FAIL $1: $2" | tee -a "$SUM"; }
note() { echo "  $*" | tee -a "$SUM"; }
want() { [ -z "$ONLY" ] || [[ ",$ONLY," == *",$1,"* ]]; }
R() { local to=$1; shift; timeout "$to" srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $ECALC_DIR; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
M() { local to=$1 p=$2; shift 2; SLURM_JOB_ID=$J timeout "$to" ./mnrun.sh "$p" env "$@"; }
cmpf() { N "cat $1.part* > $1.all 2>/dev/null || cp $1 $1.all 2>/dev/null; cmp -s $1.all $2 && echo identical || echo DIFFERS; rm -f $1.all"; }
tot() { grep -a '^total' "$1" | tail -1 | sed 's/  */ /g' | cut -c1-60; }
ckl() { grep -a 'checkpoint: the top-level\|checkpoint tree level\|ABANDONED' "$1" | sed 's/  */ /g' | head -${2:-4}; }
N "mkdir -p $TMP; rm -rf $TMP/*"

# ---- TASKS 1.7: the schedule / node-count trap ------------------------------------------------------------------------------
if want restart; then
  # (a) the trap with the pre-Phase-13 binary: tree sets written under 2,4, restarted under MN_GROUPS=4
  if [ -x ./ecalc.base ]; then
    CK=$TMP/ckbase; f=$TMP/base.txt; N "rm -rf $CK"
    M 600 4 POOL_LOG=27 BS_CKPT_DIR=$CK BS_CKPT_ABORT_TREE=1 ./ecalc.base 100000000 "$f" > "$OUT/base_abort.log" 2>&1
    M 600 4 POOL_LOG=27 BS_CKPT_DIR=$CK BS_RESTART=1 MN_GROUPS=4 ./ecalc.base 100000000 "$f" > "$OUT/base_mismatch.log" 2>&1; rc=$?
    c=$(cmpf "$f" "$REF/e_100000000.txt")
    note "pre-Phase-13 binary, sets under 2,4 restarted under MN_GROUPS=4: rc $rc, digits $c; $(grep -a 'restart from tree\|VERIFY\|all 4 nodes' "$OUT/base_mismatch.log" | sed 's/  */ /g' | sort | uniq -c | head -4 | tr '\n' ';')"
    N "rm -rf $CK $f $f.*"
  fi
  CK=$TMP/ck4; f=$TMP/ck4.txt; N "rm -rf $CK"
  M 600 4 POOL_LOG=27 BS_CKPT_DIR=$CK BS_CKPT_ABORT_TREE=1 ./ecalc 100000000 "$f" > "$OUT/r4_abort.log" 2>&1
  nab=$(grep -ac 'BS_CKPT_ABORT_TREE: exiting' "$OUT/r4_abort.log")
  t0=$(date +%s); M 300 4 POOL_LOG=27 BS_CKPT_DIR=$CK BS_RESTART=1 MN_GROUPS=4 ./ecalc 100000000 "$f" > "$OUT/r4_mismatch.log" 2>&1; rc=$?; el=$(( $(date +%s) - t0 ))
  nmsg=$(grep -ac 'was written under the schedule' "$OUT/r4_mismatch.log")
  if [ "$nab" -eq 4 ] && [ $rc -ne 0 ] && [ $rc -ne 124 ] && [ "$nmsg" -eq 4 ] && grep -aq 'BS_RESTART refused' "$OUT/r4_mismatch.log" && ! grep -aq 'VERIFY' "$OUT/r4_mismatch.log"; then
    pass "1.7 restart under another MN_GROUPS (e8 size 4)" "rc $rc in ${el} s, $nmsg nodes name the schedule: $(grep -a 'BS_RESTART refused' "$OUT/r4_mismatch.log" | cut -c1-110)"
  else fail "1.7 restart under another MN_GROUPS (e8 size 4)" "$nab aborted; rc $rc in ${el} s; $nmsg schedule messages"; fi
  t0=$(date +%s); M 300 2 POOL_LOG=27 BS_CKPT_DIR=$CK BS_RESTART=1 ./ecalc 100000000 "$f" > "$OUT/r4_size2.log" 2>&1; rc=$?; el=$(( $(date +%s) - t0 ))
  if [ $rc -ne 0 ] && [ $rc -ne 124 ] && grep -aq 'BS_RESTART refused' "$OUT/r4_size2.log" && ! grep -aq 'VERIFY' "$OUT/r4_size2.log"; then
    pass "1.7 restart at another node count (size 4 sets at size 2)" "rc $rc in ${el} s: $(grep -a 'another run' "$OUT/r4_size2.log" | head -1 | cut -c1-140)"
  else fail "1.7 restart at another node count (size 4 sets at size 2)" "rc $rc in ${el} s; $(grep -a 'refused\|another\|VERIFY' "$OUT/r4_size2.log" | head -2 | tr '\n' ';')"; fi
  M 600 4 POOL_LOG=27 BS_CKPT_DIR=$CK BS_RESTART=1 ./ecalc 100000000 "$f" > "$OUT/r4_match.log" 2>&1; rc=$?
  c=$(cmpf "$f" "$REF/e_100000000.txt")
  if [ $rc -eq 0 ] && [ "$c" = identical ] && grep -aq 'mn: all 4 nodes: VERIFY OK' "$OUT/r4_match.log" && grep -aq 'restart from tree level 1' "$OUT/r4_match.log"; then
    pass "1.7 matching restart (e8 size 4, from tree level 1)" "$c; $(tot "$OUT/r4_match.log")"
  else fail "1.7 matching restart (e8 size 4)" "rc $rc; $c; $(grep -a 'restart from\|VERIFY FAILED\|refused' "$OUT/r4_match.log" | head -1)"; fi
  N "rm -rf $CK $f $f.*"
  # the gate: 10^9 at size 2 -- the top tree set (level 1) is the restart point, written by the background writer
  CK=$TMP/ck2; f=$TMP/ck2.txt; N "rm -rf $CK"
  M 900 2 POOL_LOG=29 BS_CKPT_DIR=$CK BS_CKPT_ABORT_TREE=1 ./ecalc 1000000000 "$f" > "$OUT/r2_abort.log" 2>&1
  nab=$(grep -ac 'BS_CKPT_ABORT_TREE: exiting' "$OUT/r2_abort.log")
  M 300 4 POOL_LOG=29 BS_CKPT_DIR=$CK BS_RESTART=1 ./ecalc 1000000000 "$f" > "$OUT/r2_size4.log" 2>&1; rc4=$?
  M 900 2 POOL_LOG=29 BS_CKPT_DIR=$CK BS_RESTART=1 ./ecalc 1000000000 "$f" > "$OUT/r2_match.log" 2>&1; rc=$?
  c=$(cmpf "$f" "$REF/e_1000000000.txt")
  if [ "$nab" -eq 2 ] && [ $rc4 -ne 0 ] && [ $rc4 -ne 124 ] && grep -aq 'BS_RESTART refused' "$OUT/r2_size4.log" && [ $rc -eq 0 ] && [ "$c" = identical ] && grep -aq 'mn: all 2 nodes: VERIFY OK' "$OUT/r2_match.log"; then
    pass "1.7 e9 size 2: at size 4 refused (rc $rc4), matching restart" "$c; $(grep -a 'restart from tree' "$OUT/r2_match.log" | head -1 | sed 's/  */ /g' | cut -c1-100); $(tot "$OUT/r2_match.log")"
  else fail "1.7 e9 size 2" "$nab aborted; size-4 rc $rc4; restart rc $rc; $c"; fi
  note "the abort run's set: $(ckl "$OUT/r2_abort.log" 2 | tr '\n' ';')"
  N "rm -rf $CK $f $f.*"
fi

# ---- TASKS 1.4: the size > 1 top set in the background ---------------------------------------------------------------------
recheck() { local p=$1 d=$2 f=$3 tag=$4
  M 600 "$p" ECALC_RECHECK=1 ./ecalc "$d" "$f" > "$OUT/${tag}_recheck.log" 2>&1
  echo "$(grep -ac 'RECHECK OK' "$OUT/${tag}_recheck.log") $(grep -ac 'from the checkpoint\|from the tree level' "$OUT/${tag}_recheck.log")"; }
if want bg; then
  for cfg in "2 1000000000 29" "3 100000000 27"; do
    set -- $cfg; p=$1 d=$2 pl=$3
    for bg in 1 0; do
      tag=bg_${p}_$bg; f=$TMP/$tag.txt
      M 900 "$p" POOL_LOG=$pl ECALC_CKPT_TOP=1 BS_CKPT_TREE_BG=$bg ./ecalc "$d" "$f" > "$OUT/$tag.log" 2>&1; rc=$?
      c=$(cmpf "$f" "$REF/e_$d.txt")
      r=$(recheck "$p" "$d" "$f" "$tag"); nok=${r% *}; nck=${r#* }
      if [ $rc -eq 0 ] && [ "$c" = identical ] && grep -aq "mn: all $p nodes: VERIFY OK" "$OUT/$tag.log" && [ "$nok" -eq $((p + 1)) ] && [ "$nck" -ge "$p" ]; then
        pass "1.4 top set size $p e$(( ${#d} - 1 )) BS_CKPT_TREE_BG=$bg" "$c; recheck $nok RECHECK OK, P, Q from the set on $nck; $(tot "$OUT/$tag.log")"
      else fail "1.4 top set size $p BS_CKPT_TREE_BG=$bg" "rc $rc; $c; recheck $nok OK, $nck from the set"; fi
      note "$(ckl "$OUT/$tag.log" 2 | tr '\n' ';' | cut -c1-400)"
      N "rm -rf $f $f.*"
    done
  done
fi

# ---- TASKS 4.1: the budgeted mode (BS_CKPT_BG_MBS: a slow disk emulated) ------------------------------------------------------
if want budget; then
  # slow disk, budgeted: the set must be dropped (no files left), digits identical; slow disk, complete: the set kept (the wait shows)
  for cfg in "1 2 100" "2 2 30" "1 1 100" "2 1 30"; do
    set -- $cfg; p=$1 mode=$2 mbs=$3; tag=budget_${p}_${mode}; f=$TMP/$tag.txt
    if [ $p = 1 ]; then R 900 "env POOL_LOG=29 ECALC_CKPT_TOP=$mode BS_CKPT_BG_MBS=$mbs ./ecalc 1000000000 $f" > "$OUT/$tag.log" 2>&1; rc=$?
    else M 900 2 POOL_LOG=29 ECALC_CKPT_TOP=$mode BS_CKPT_BG_MBS=$mbs ./ecalc 1000000000 "$f" > "$OUT/$tag.log" 2>&1; rc=$?; fi
    c=$(cmpf "$f" "$REF/e_1000000000.txt")
    left=$(N "ls $f.top 2>/dev/null | grep -v tmp | wc -l")
    nab=$(grep -ac 'ABANDONED' "$OUT/$tag.log")
    if [ $mode = 2 ]; then want_ab=$p; want_left=0; else want_ab=0; want_left=$((5 * p)); fi
    if [ $rc -eq 0 ] && [ "$c" = identical ] && [ "$nab" -eq "$want_ab" ] && [ "$left" -eq "$want_left" ] && ! grep -aq 'VERIFY FAILED' "$OUT/$tag.log"; then
      pass "4.1 ECALC_CKPT_TOP=$mode size $p at an emulated $mbs MB/s (e9)" "$c; $nab ABANDONED, $left files in .top; $(tot "$OUT/$tag.log")"
    else fail "4.1 ECALC_CKPT_TOP=$mode size $p at $mbs MB/s" "rc $rc; $c; $nab abandoned ($want_ab wanted); $left files ($want_left wanted)"; fi
    note "$(ckl "$OUT/$tag.log" 2 | tr '\n' ';' | cut -c1-500)"
    N "rm -rf $f $f.*"
  done
  tag=budget_1_2fast; f=$TMP/$tag.txt
  R 900 "env POOL_LOG=29 ECALC_CKPT_TOP=2 ./ecalc 1000000000 $f" > "$OUT/$tag.log" 2>&1; rc=$?
  c=$(cmpf "$f" "$REF/e_1000000000.txt")
  if [ $rc -eq 0 ] && [ "$c" = identical ]; then pass "4.1 ECALC_CKPT_TOP=2 size 1, the real disk (e9)" "$c; $(ckl "$OUT/$tag.log" 1 | cut -c1-240)"
  else fail "4.1 ECALC_CKPT_TOP=2 size 1, the real disk" "rc $rc; $c"; fi
  N "rm -rf $f $f.*"
fi

# ---- TASKS 4.1: timing at 4 x 10^10 ------------------------------------------------------------------------------------------
if want time; then
  N "df -h /tmp | tail -1" | sed 's/^/  disk: /' | tee -a "$SUM"
  IFS=';' read -ra CFGS <<< "${N13_TIME:-new2slow ./ecalc 2 310;new1slow ./ecalc 1 310;new1 ./ecalc 1 0;base1 ./ecalc.base 1 0;none ./ecalc 0 0}"
  for cfg in "${CFGS[@]}"; do
    set -- $cfg; tag=t4e10_$1 bin=$2 mode=$3 mbs=$4; f=$TMP/$tag.txt
    [ -x "$bin" ] || continue
    N "python3 -c \"import os,sys; fd=os.open(sys.argv[1],os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\" $REF4 2>/dev/null; sync; true"
    t1=$(date +%s)
    R 1200 "env ECALC_CKPT_TOP=$mode BS_CKPT_BG_MBS=$mbs $bin 40000000000 $f" > "$OUT/$tag.log" 2>&1; rc=$?
    el=$(( $(date +%s) - t1 ))
    c=$(cmpf "$f" "$REF4")
    dm=$(grep -a '^dm ' "$OUT/$tag.log" | sed 's/  */ /g' | cut -c1-20)
    if [ $rc -eq 0 ] && [ "$c" = identical ] && grep -aq '^VERIFY OK' "$OUT/$tag.log"; then pass "4.1 time 4e10 $tag" "$c; $(tot "$OUT/$tag.log" | cut -c1-24); $dm; ${el} s to exit"
    else fail "4.1 time 4e10 $tag" "rc $rc; $c"; fi
    note "$(ckl "$OUT/$tag.log" 2 | tr '\n' ';' | cut -c1-500)"
    N "rm -rf $f $f.*; sync"
  done
fi
N "rm -rf $TMP"
echo "== $NPASS passed, $NFAIL failed; logs in $OUT ==" | tee -a "$SUM"
exit $NFAIL

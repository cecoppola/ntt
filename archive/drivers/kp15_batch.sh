#!/bin/bash
# results/kp15_batch.sh <stage> - Phase 15 KP's node batches (results/KP15.md).  Run on the aac6 login node from ~/ntt-KP15
# (setsid nohup); each stage takes its own 45-min one-node job (-J KP), waits for it, runs, cancels it.  Logs: ~/kp15tmp/<stage>/.
#   A  the reproduction with the base binary (44c1642, ~/kp15tmp/base/ecalc) and the fix's forced-correction runs
#   B  ./mnaccept.sh <job> --only unit,e9,mn,recheck,corr
#   C  4 x 10^10 and 10^11 at size 1 on the defaults, identical to results/e_4e10.out / e_1e11.out
ST=$1; E=$HOME/ntt-KP15/ecalc; L=$HOME/kp15tmp/$ST; mkdir -p "$L"; cd "$E" || exit 2
REF=$HOME/ntt/ecalc/ref; BASE=$HOME/kp15tmp/base/ecalc; UNP=$E/../tools/unpack_digits
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J KP --parsable --wrap "sleep 2700"); echo "$J" > "$L/job"
while [ "$(squeue -j "$J" -h -o %T)" != RUNNING ]; do sleep 20; [ -z "$(squeue -j "$J" -h -o %T)" ] && { echo "job $J gone" >> "$L/batch.log"; exit 2; }; done
NODE=$(squeue -j "$J" -h -o %N); TMP=/tmp/kp15_$J
S=$L/batch.log; echo "== stage $ST: job $J on $NODE, $(date '+%F %T %Z') (Central), $(git log --oneline -1 | cut -c1-60) ==" >> "$S"
R() { local to=$1; shift; timeout "$to" srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $E; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
M() { local to=$1 p=$2; shift 2; SLURM_JOB_ID=$J timeout "$to" ./mnrun.sh "$p" env "$@"; }
N "mkdir -p $TMP"
# one <tag> <procs> <digits> <bin> [env...]: a run, then its file against the reference prefix, the converter's residue check, and a line
one() { local tag=$1 p=$2 d=$3 bin=$4; shift 4; local log=$L/$tag.log f=$TMP/$tag.txt rf=$TMP/ref_$d.txt t0 rc c u v pl
  [ "$d" = 1000000000 ] && rf=$REF/e_1000000000.txt || N "[ -s $rf ] || { head -c $(( d + 2 )) $REF/e_1000000000.txt > $rf; echo >> $rf; }"
  t0=$(date +%s)
  if [ "$p" = 1 ]; then R 600 "env $* $bin $d $f" > "$log" 2>&1; rc=$?; else M 600 "$p" POOL_LOG=27 "$@" "$bin" "$d" "$f" > "$log" 2>&1; rc=$?; fi
  c=$(N "$E/digcmp.sh $f $rf")
  if [ "$(N "p=\$(ls $f.part* 2>/dev/null | head -1); [ -n \"\$p\" ] || p=$f; head -c 8 \$p")" = ECPACK18 ]; then
    u=$(N "$UNP -q -n \$(ls $f.part* 2>/dev/null || echo $f) > /dev/null 2>&1 && echo 'residues ok' || echo 'residues FAILED'"); else u="ASCII"; fi
  if [ "$p" = 1 ]; then v=$(grep -a '^VERIFY' "$log" | tail -1); else v=$(grep -a 'mn: all .* nodes: VERIFY' "$log" | tail -1 | sed 's/mn: //'); fi
  pl=$(grep -a 'patch: digits\|mn_out: patch\|patch: X' "$log" | grep -a 'bytes in [1-9]\|FAILED\|holds\|not this\|does not' | sed 's/^mn: //; s/  */ /g' | cut -c1-150 | head -2 | tr '\n' '|')
  echo "$tag: size $p d $d $(basename "$bin") [$*]: rc $rc; ${v:-no VERIFY}; digits $c; $u; $(( $(date +%s) - t0 )) s; ${pl}" >> "$S"
  if [ -n "$RECHECK" ]; then
    if [ "$p" = 1 ]; then R 600 "env ECALC_RECHECK=1 $bin $d $f" > "$L/${tag}_recheck.log" 2>&1; else M 600 "$p" ECALC_RECHECK=1 "$bin" "$d" "$f" > "$L/${tag}_recheck.log" 2>&1; fi
    echo "   $tag recheck: $(grep -ac 'RECHECK OK' "$L/${tag}_recheck.log") RECHECK OK, $(grep -ac 'RECHECK FAILED' "$L/${tag}_recheck.log") FAILED" >> "$S"
  fi
  N "rm -rf $f $f.*"
}
case $ST in
A)
  one base_c511_1   1 511461828 "$BASE" ECALC_TEST_CORR=14
  one base_c511_1_ascii 1 511461828 "$BASE" ECALC_TEST_CORR=14 ECALC_OUT_PACKED=0
  one base_c820_1   1 820719000 "$BASE" ECALC_CORR_PATCH=1 ECALC_TEST_CORR=-44
  one base_c108u_2  2 108388422 "$BASE" ECALC_TEST_CORR=36
  one base_c108u_4  4 108388422 "$BASE" ECALC_TEST_CORR=36
  one base_e9p7_1   1 1000000000 "$BASE" ECALC_TEST_CORR=7
  RECHECK=1 one fix_c511_1 1 511461828 ./ecalc ECALC_TEST_CORR=14
  one fix_c511_1_od0 1 511461828 ./ecalc ECALC_TEST_CORR=14 ECALC_ODIRECT=0
  one fix_c511_1_ascii 1 511461828 ./ecalc ECALC_TEST_CORR=14 ECALC_OUT_PACKED=0
  one fix_c820_1_cp1 1 820719000 ./ecalc ECALC_CORR_PATCH=1 ECALC_TEST_CORR=-44
  RECHECK=1 one fix_c820_1 1 820719000 ./ecalc ECALC_TEST_CORR=-44
  RECHECK=1 one fix_e9p7_1 1 1000000000 ./ecalc ECALC_TEST_CORR=7
  one fix_e9m7_1    1 1000000000 ./ecalc ECALC_TEST_CORR=-7
  RECHECK=1 one fix_c108u_2 2 108388422 ./ecalc ECALC_TEST_CORR=36
  one fix_c108u_3   3 108388422 ./ecalc ECALC_TEST_CORR=36
  one fix_c108u_4   4 108388422 ./ecalc ECALC_TEST_CORR=36
  one fix_c108u_2_e0 2 108388422 ./ecalc ECALC_TEST_CORR=36 MN_OUT_EARLY=0
  one fix_c108u_4_e0 4 108388422 ./ecalc ECALC_TEST_CORR=36 MN_OUT_EARLY=0
  one fix_c108d_2   2 108072396 ./ecalc ECALC_TEST_CORR=-39
  RECHECK=1 one fix_c108d_4 4 108072396 ./ecalc ECALC_TEST_CORR=-39
  one fix_c108d_2_e0 2 108072396 ./ecalc ECALC_TEST_CORR=-39 MN_OUT_EARLY=0
  one fix_c108u_2_od0 2 108388422 ./ecalc ECALC_TEST_CORR=36 ECALC_ODIRECT=0
  one fix_e9p7_4    4 1000000000 ./ecalc ECALC_TEST_CORR=7
  ;;
B)
  ./mnaccept.sh "$J" --only unit,e9,mn,recheck,corr > "$L/mnaccept.log" 2>&1
  grep -a '^PASS\|^FAIL\|^==' "$L/mnaccept.log" >> "$S"
  ;;
C)
  for std in 40000000000 100000000000; do
    [ $std = 40000000000 ] && rf=$HOME/ntt/ecalc/results/e_4e10.out || rf=$HOME/ntt/ecalc/results/e_1e11.out
    N "python3 -c \"import os,sys; fd=os.open(sys.argv[1],os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\" $rf"
    log=$L/std_$std.log; f=$TMP/std_$std.txt; t0=$(date +%s)
    R 1500 "env ECALC_VERBOSE=1 ./ecalc $std $f" > "$log" 2>&1; rc=$?
    c=$(N "$E/digcmp.sh $f $rf")
    echo "std $std: rc $rc; $(grep -a '^VERIFY' "$log" | tail -1); digits $c; $(grep -a '^total' "$log" | tail -1 | sed 's/  */ /g' | cut -c1-40); $(grep -a 'patch: X\|corrections' "$log" | head -2 | sed 's/  */ /g' | tr '\n' '|'); wall $(( $(date +%s) - t0 )) s" >> "$S"
    N "rm -rf $f $f.*"
  done
  ;;
esac
N "rm -rf $TMP"
echo "== stage $ST done $(date '+%F %T %Z') ==" >> "$S"
scancel "$J"

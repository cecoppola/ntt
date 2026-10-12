#!/bin/bash
# o4_rate.sh - O4 (results/O4.md): write time and write rate of the digit output, ASCII (ECALC_OUT_PACKED=0) / packed 1 / 60-bit packed 2,
# with the round trip checked: digcmp against the reference, the unpacked text (tools/unpack_digits -o, one file per part, concatenated)
# byte-identical (cmp) to the reference, and ECALC_RECHECK on the packed parts.  Runs INSIDE a hold; no switch default changes.
#   MODE=aac6|aac7  J=<hold>  NN=<nodes>  E=<ecalc dir>  U=<tools/unpack_digits>  O=<outdir>  PLAN="9 10"  ROUNDS=2  CAP_S=<seconds for new runs>
#   FMTS="0 2" (optional: only these formats; default all three)  RECHECK=0 (skip the ECALC_RECHECK run)
#   REF9=<exact 1e9 ASCII reference>  REF10=<exact 1e10 ASCII reference, file or empty: then the run's own ASCII (fmt 0) output is the reference>
# Outputs: $O/rates.tsv (one row per run), $O/summary.txt, $O/log/*.log; digit files are deleted after each compare.
set -u
: "${MODE:?}" "${J:?}" "${E:?}" "${U:?}" "${O:?}"; NN=${NN:-1}; PLAN=${PLAN:-"9 10"}; ROUNDS=${ROUNDS:-2}; CAP_S=${CAP_S:-1800}
ENVS=${ENVS:-}; REF9=${REF9:-}; REF10=${REF10:-}; DATA=$O/data; mkdir -p $O/log $DATA; T0=$(date +%s)
S() { echo "$(TZ=America/New_York date +%H:%M:%S) ET $*" >> $O/summary.txt; }
[ -s $O/rates.tsv ] || printf 'tag\tdigits\tfmt\tnodes\trc\tverify_ok\tbytes_GB\twrite_s\trate_GBps\ttotal_s\twall_s\n' > $O/rates.tsv
launch() { # launch <fmt> <digits> <outfile> [extra env words]   (log on stdout)
  local fmt=$1 d=$2 of=$3; shift 3
  if [ $MODE = aac6 ]; then srun --jobid=$J --overlap bash -lc "module load rocm; cd $E; export $ENVS; env ECALC_OUT_PACKED=$fmt $* timeout 1500 ./ecalc $d $of" < /dev/null
  elif [ $MODE = local ]; then ( cd $E && env ECALC_OUT_PACKED=$fmt "$@" ./ecalc $d $of ) < /dev/null   # stub tests only
  else ( cd $E && source aac7env.sh >/dev/null 2>&1; SLURM_JOB_ID=$J timeout 900 ./mnrun.sh $NN env $ENVS ECALC_OUT_PACKED=$fmt "$@" ./ecalc $d $of ) < /dev/null; fi; }
run() { # run <tag> <digits> <fmt>  -> data/<tag>.* ; row in rates.tsv
  local tag=$1 d=$2 fmt=$3 t1=$(date +%s) rc; shift 3
  launch $fmt $d $DATA/$tag "$@" > $O/log/$tag.log 2>&1; rc=$?
  local wall=$(( $(date +%s) - t1 )) ok=$(grep -a -c 'VERIFY OK' $O/log/$tag.log)
  # "wrote F (G GB; write T s in the writer thread" / "(G1 + G2 GB; write T1 + T2 s": rate = sum GB / sum s per node line, the mean over nodes
  local rt=$(sed -nE 's/.*\(([0-9.]+) (\+ ([0-9.]+) )?GB; write ([0-9.]+)( \+ ([0-9.]+))? s in the writer.*/\1|\3|\4|\6/p' $O/log/$tag.log | awk -F'|' '{g=$1+$2; t=$3+$4; if (t>0) {G+=g; T+=t; R+=g/t; n++}} END{if(n) printf "%.2f\t%.2f\t%.3f", G, T/n, R/n; else printf "NA\tNA\tNA"}')
  local tot=$(sed -E 's/^ *[0-9]+: //' $O/log/$tag.log | grep -a -m1 '^total' | awk '{print $2}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $tag $d $fmt $NN $rc $ok "$rt" "${tot:-NA}" $wall >> $O/rates.tsv
  S "run $tag d=$d fmt=$fmt nodes=$NN rc=$rc VERIFY_OK=$ok bytes/write_s/rate=[$rt] total=${tot:-NA} wall=$wall; $(grep -a -m1 'file system' $O/log/$tag.log | cut -c1-110)"; return $rc; }
files() { ls $DATA/$1.part* 2>/dev/null | sort; [ -e $DATA/$1 ] && echo $DATA/$1; }
check() { # check <tag> <ref ASCII> <fmt>: digcmp, then (fmt>0) the unpacked text == ref, then (fmt 2) RECHECK
  local tag=$1 ref=$2 fmt=$3 f=$DATA/$1 sz=$(du -cb $(files $1) 2>/dev/null | tail -1 | cut -f1)
  S "digcmp $tag vs $(basename $ref): $($E/digcmp.sh $f $ref)  (on disk $sz B)"
  if [ $fmt -gt 0 ]; then local t=$(date +%s) k=0 p pids=""
    for p in $(files $tag); do "$U" -q -o $DATA/$tag.txt.$(printf %04d $k) $p & pids="$pids $!"; k=$((k+1)); done; wait $pids
    local tu=$(( $(date +%s) - t ))
    cat $(ls $DATA/$tag.txt.* | sort) | cmp - $ref > $O/log/$tag.cmp 2>&1 && S "unpack -o ($k parts in parallel, $tu s) + cat == $(basename $ref) byte-identical (cmp)" || S "UNPACK TEXT DIFFERS from $(basename $ref): $(head -c 200 $O/log/$tag.cmp)"
    rm -f $DATA/$tag.txt.*; fi
}
recheck() { local tag=$1 d=$2 fmt=$3 rc; launch $fmt $d $DATA/$tag ECALC_RECHECK=1 > $O/log/rc_$tag.log 2>&1; rc=$?
  S "RECHECK $tag rc=$rc: $(grep -a -i 'recheck' $O/log/rc_$tag.log | grep -a -c -i ' OK\|ok$') OK lines, $(grep -a -i -c 'FAIL\|DIFFER' $O/log/rc_$tag.log) FAIL lines"; }
left() { echo $(( CAP_S - ($(date +%s) - T0) )); }
S "start MODE=$MODE nodes=$NN hold=$J E=$E $(git -C $E rev-parse --short HEAD 2>/dev/null) PLAN='$PLAN' ROUNDS=$ROUNDS"
for sz in $PLAN; do
  case $sz in 9) d=${D9:-1000000000}; ref=$REF9; est=240;; 10) d=10000000000; ref=$REF10; est=900;; esac
  [ $(left) -gt $est ] || { S "skip 1e$sz (budget)"; continue; }
  own=0; if [ -z "$ref" ]; then own=1; ref=$DATA/own_ascii_$sz; fi   # no external reference: the run's own ASCII output (fmt 0) is it
  order=(0 1 2 2 1 0 1 2 0 0 2 1); NF=3
  if [ -n "${FMTS:-}" ]; then fm=($FMTS); NF=${#fm[@]}; order=(); for _r in 1 2 3 4; do if [ $((_r % 2)) = 1 ]; then order+=("${fm[@]}"); else for ((_i=NF-1; _i>=0; _i--)); do order+=(${fm[_i]}); done; fi; done; fi   # FMTS="0 2": only these formats, reversed in even rounds
  for r in $(seq $ROUNDS); do
    for k in $(seq 0 $((NF-1))); do f=${order[$(( (r-1)*NF + k ))]}
      [ $(left) -gt $est ] || { S "stop 1e$sz at round $r (budget)"; break 2; }
      tag=s${sz}_r${r}_f$f
      if [ $own = 1 ] && [ $f != 0 ] && [ ! -e $DATA/own_ascii_$sz.done ]; then   # the reference first
        run s${sz}_ref_f0 $d 0 && { cat $(files s${sz}_ref_f0) > $ref; rm -f $(files s${sz}_ref_f0); touch $DATA/own_ascii_$sz.done; S "own ASCII reference 1e$sz made: prefix of 1e9 == REF9: $(cmp -n $((1000000002)) $ref $REF9 >/dev/null 2>&1 && echo yes || echo NO)"; }
      fi
      run $tag $d $f || { S "run $tag failed; continuing"; rm -f $(files $tag); continue; }
      if [ $f = 0 ] && [ $own = 1 ] && [ ! -e $DATA/own_ascii_$sz.done ]; then cat $(files $tag) > $ref; touch $DATA/own_ascii_$sz.done; fi
      check $tag $ref $f
      [ $f = 2 ] && [ $r = 1 ] && [ "${RECHECK:-1}" = 1 ] && { recheck $tag $d 2; }
      rm -f $(files $tag) $DATA/$tag.top* 2>/dev/null
    done
  done
  rm -f $DATA/own_ascii_$sz $DATA/own_ascii_$sz.done
done
# summary table: mean rate / write time per (size, fmt)
{ echo; echo "per (digits, fmt): n, mean write_s, mean rate GB/s, mean bytes GB, mean total_s (rows with rc=0 and VERIFY OK)"
  awk -F'\t' 'NR>1 && $5==0 && $6>0 && $8!="NA" {k=$2" fmt"$3; n[k]++; w[k]+=$8; r[k]+=$9; b[k]+=$7; t[k]+=$10} END{for(k in n) printf "%s: n=%d write_s=%.2f rate=%.3f GB/s bytes=%.2f GB total=%.1f s\n", k, n[k], w[k]/n[k], r[k]/n[k], b[k]/n[k], t[k]/n[k]}' $O/rates.tsv | sort; } >> $O/summary.txt
S "all steps done"; [ $MODE = aac7 ] && squeue -s -j $J -h 2>/dev/null | grep -v 'batch\|extern' | head -3 >> $O/summary.txt
rmdir $DATA 2>/dev/null; echo "SUCCESS $(date +%s)" > $O/O4_DONE

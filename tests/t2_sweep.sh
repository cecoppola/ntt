#!/bin/bash
# T2 (Phase 15): the BS_SEED_TERMS / BS_SEED_FILL sweep, unattended (the private protocol).  On aac6:
#   setsid nohup bash ~/ntt-T215/tests/t2_sweep.sh <commit> <outdir> <plan file> [sbatch options...] > <outdir>.out 2>&1 < /dev/null &
# The plan: one run per line, "<tag> <digits> <nw|w> [VAR=value ...]" ('#' comments).  nw = no digit file (the wall without the
# write); w = the digits written to the node's /tmp (ECALC_ODIRECT as the defaults set it) and hashed with O_DIRECT against the
# reference's sha1 (4e10: results/e_4e10.out.sha1; 1e11: ~/V214/e_1e11.sha1): the byte compare without reading the reference from
# /shared, then deleted.  Every run: ECALC_VERBOSE=2; the log in <outdir>/<tag>.log, one line in <outdir>/summary.txt.
# Jobs: one 1-node job at a time (-J T2, <= 45 min); the runs that do not fit (the estimate below) go to the next job; at most
# T2_MAXJOBS (4) jobs.  The job is cancelled at the end (and on any exit).  The clone must be at <commit> (else WRONG COMMIT).
set -u
COMMIT=$1; OUT=$2; PLAN=$3; shift 3; SBOPT=("$@")
CLONE=${T2_CLONE:-$HOME/ntt-T215}; E=$CLONE/ecalc
MAXJOBS=${T2_MAXJOBS:-4}; BUDGET=${T2_BUDGET_S:-2580}          # a job's usable seconds (45 min less the start and the cancel)
mkdir -p "$OUT"; SUM=$OUT/summary.txt
log() { echo "$(date '+%F %T %Z') $*"; }
HEAD=$(git -C "$CLONE" rev-parse --short=7 HEAD)
if [ "${HEAD:0:7}" != "${COMMIT:0:7}" ]; then log "WRONG COMMIT: $CLONE is at $HEAD, the plan wants $COMMIT"; exit 1; fi
bash -lc "module load rocm; cd $E && make -s -j16 ecalc" > "$OUT/build.log" 2>&1 || { log "build failed (see $OUT/build.log)"; exit 1; }
[ "$(stat -c %Y "$E/ecalc")" -ge "$(git -C "$CLONE" log -1 --format=%ct)" ] || { log "the binary is older than the commit"; exit 1; }
SHA4=$(cut -c1-40 ~/ntt/ecalc/results/e_4e10.out.sha1); SHA11=$(cut -c1-40 ~/V214/e_1e11.sha1)
sha_of() { case $1 in 40000000000) echo $SHA4;; 100000000000) echo $SHA11;; *) echo none;; esac; }
est() { case $1 in 40000000000) [ $2 = w ] && echo 180 || echo 100;; 100000000000) [ $2 = w ] && echo 420 || echo 250;; *) echo 600;; esac; }   # seconds per run, measured walls + margin
mapfile -t RUNS < <(grep -v '^\s*#' "$PLAN" | grep -v '^\s*$')
log "T2 sweep: $CLONE at $HEAD, ${#RUNS[@]} runs from $PLAN, sbatch ${SBOPT[*]:-}"
[ -f "$SUM" ] || echo "# tag digits mode S | total | init | bs (seeds batch mdev) | recip | dm | T1 | verify | digits | elapsed | node | env" > "$SUM"
J=""; trap '[ -n "$J" ] && scancel $J 2>/dev/null' EXIT
i=0; njobs=0
while [ $i -lt ${#RUNS[@]} ] && [ $njobs -lt $MAXJOBS ]; do
  J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J T2 --parsable "${SBOPT[@]}" --wrap "sleep 2700") || { log "sbatch failed"; exit 1; }
  njobs=$((njobs + 1)); log "job $J submitted"
  while :; do st=$(squeue -h -j $J -o %T 2>/dev/null); [ "$st" = RUNNING ] && break; [ -z "$st" ] && { log "job $J vanished"; exit 1; }; sleep 20; done
  NODE=$(squeue -h -j $J -o %N); t0=$(date +%s); log "job $J running on $NODE"
  R() { srun --jobid=$J -N1 -n1 --gpus=4 --overlap "$@"; }
  TMP=/tmp/t2_$J; R bash -c "rm -rf /tmp/t2_*; mkdir -p $TMP; df -h /tmp | tail -1" | sed "s/^/  node \/tmp: /"
  while [ $i -lt ${#RUNS[@]} ]; do
    read -r tag D mode envs <<< "${RUNS[$i]}"
    left=$(( BUDGET - ($(date +%s) - t0) )); need=$(est $D $mode)
    if [ $need -gt $left ]; then log "run $tag needs ~$need s, $left s left in job $J: next job"; break; fi
    f=$TMP/e.out; lg=$OUT/$tag.log; t1=$(date +%s)
    if [ "$mode" = w ]; then arg="$f"; else arg=""; fi
    { echo "== T2 run $tag: $D $mode, env: $envs; job $J on $NODE, clone at $HEAD, $(date '+%F %T %Z')"
      time R bash -lc "module load rocm; cd $E && env ECALC_VERBOSE=2 $envs ./ecalc $D $arg"; } > "$lg" 2>&1; rc=$?
    el=$(( $(date +%s) - t1 )); dig=-
    if [ "$mode" = w ]; then
      want=$(sha_of $D)
      got=$(R bash -c "P=\$(ls $f.part* 2>/dev/null | sort); [ -n \"\$P\" ] || P=$f; for p in \$P; do dd if=\$p iflag=direct bs=64M status=none; done | sha1sum | cut -c1-40; rm -rf $f $f.*")
      [ "$got" = "$want" ] && dig=identical || dig="DIFFERS($got)"
    fi
    g() { grep -a -m1 "$1" "$lg" | sed 's/  */ /g'; }
    S=$(grep -ao 'spans of [0-9]*' "$lg" | head -1 | cut -d' ' -f3)
    tot=$(g '^total' | cut -d' ' -f2); ini=$(grep -a 'RESULT ecalc init s' "$lg" | awk '{print $5}')
    bsl=$(g '^bs [0-9. ]*s ' | sed 's/.*(seeds \([0-9.]*\) school [0-9.]* batch \([0-9.]*\) mdev \([0-9.]*\).*/\1 \2 \3/'); bst=$(g '^bs [0-9. ]*s ' | cut -d' ' -f2)
    rec=$(g '^recip ' | cut -d' ' -f2); dm=$(g '^dm ' | cut -d' ' -f2); t1s=$(g '^T1 ' | cut -d' ' -f2)
    ver=$(grep -a -m1 '^VERIFY' "$lg" || echo "no VERIFY"); [ $rc -ne 0 ] && ver="$ver rc $rc"
    rem=$(grep -a 'VMM arena released' "$lg" | sed 's/.*(\([0-9]*\) remaps of [0-9]* chunks in \([0-9.]*\) s.*/\1 \2/' | awk '{n += $1; s += $2} END {printf "%d remaps %.2f s", n, s}')   # DB_POOL_VERBOSE=1
    line="$tag $D $mode S=$S | total $tot | init $ini | bs $bst ($bsl) | recip $rec | dm $dm | T1 $t1s | $ver | $dig | $rem | ${el}s | $NODE | $envs"
    echo "$line" >> "$SUM"; log "$line"
    i=$((i + 1))
  done
  R bash -c "rm -rf $TMP" ; scancel $J; log "job $J cancelled ($(( $(date +%s) - t0 )) s used)"; J=""
done
[ $i -lt ${#RUNS[@]} ] && log "stopped with $(( ${#RUNS[@]} - i )) runs left (T2_MAXJOBS $MAXJOBS)"
log "done"

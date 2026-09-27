#!/bin/bash
# AS (Phase 15 Batch 2): node runs for the arena-sizing switch, unattended (docs/AGENT_PROTOCOL.md).  On aac6:
#   setsid nohup bash ~/ntt-AS15/tests/as_sweep.sh <commit> <outdir> <plan file> [sbatch options...] > <outdir>.out 2>&1 < /dev/null &
# Adapted from tests/t2_sweep.sh.  Plan: one run per line, "<tag> <digits> <nw|w|wc> [VAR=value ...]" ('#' comments).
#   nw = no digit file (the wall without the write); w = the digits written to the node's /tmp (packed, the default), unpacked by
#   tools/unpack_digits to a sha1 compared with the reference's (4e10: results/e_4e10.out.sha1, 1e11: ~/V214/e_1e11.sha1);
#   wc = as w, then ecalc/digcmp.sh against the reference file itself (reads it from /shared: slow at 1e11).
# Every run: ECALC_VERBOSE=2 DB_POOL_VERBOSE=1; `dbtrace:` lines (DB_POOL_TRACE=1) go to <tag>.trace, the rest to <tag>.log.
# One 1-node job at a time (-J AS, <= 45 min), at most AS_MAXJOBS (3); the job is cancelled at the end and on any exit.
set -u
COMMIT=$1; OUT=$2; PLAN=$3; shift 3; SBOPT=("$@")
CLONE=${AS_CLONE:-$HOME/ntt-AS15}; E=$CLONE/ecalc
MAXJOBS=${AS_MAXJOBS:-3}; BUDGET=${AS_BUDGET_S:-2580}
mkdir -p "$OUT"; SUM=$OUT/summary.txt
log() { echo "$(date '+%F %T %Z') $*"; }
HEAD=$(git -C "$CLONE" rev-parse --short=7 HEAD)
if [ "${HEAD:0:7}" != "${COMMIT:0:7}" ]; then log "WRONG COMMIT: $CLONE is at $HEAD, the plan wants $COMMIT"; exit 1; fi
bash -lc "module load rocm; cd $E && make -s -j16" > "$OUT/build.log" 2>&1 || { log "build failed (see $OUT/build.log)"; exit 1; }
[ "$(stat -c %Y "$E/ecalc")" -ge "$(git -C "$CLONE" log -1 --format=%ct)" ] || { log "the binary is older than the commit"; exit 1; }
SHA4=$(cut -c1-40 ~/ntt/ecalc/results/e_4e10.out.sha1); SHA11=$(cut -c1-40 ~/V214/e_1e11.sha1)
sha_of() { case $1 in 40000000000) echo $SHA4;; 100000000000) echo $SHA11;; *) echo none;; esac; }
ref_of() { case $1 in 40000000000) echo ~/ntt/ecalc/results/e_4e10.out;; 100000000000) echo ~/ntt/ecalc/results/e_1e11.out;; *) echo none;; esac; }
est() { case $1 in 40000000000) [ $2 = nw ] && echo 100 || echo 200;; 100000000000) [ $2 = nw ] && echo 250 || { [ $2 = wc ] && echo 1500 || echo 450; };; 130000000000) [ $2 = nw ] && echo 330 || echo 600;; 1[0-2]?000000000) echo 320;; [2-9]0000000000) echo 200;; *) echo 900;; esac; }
mapfile -t RUNS < <(grep -v '^\s*#' "$PLAN" | grep -v '^\s*$')
log "AS sweep: $CLONE at $HEAD, ${#RUNS[@]} runs from $PLAN, sbatch ${SBOPT[*]:-}"
[ -f "$SUM" ] || echo "# tag digits mode | total | init | bs | recip | dm | T1 | remaps (count, s) | dm-remaps | growth | verify | digits | elapsed | node | env" > "$SUM"
J=""; trap '[ -n "$J" ] && scancel $J 2>/dev/null' EXIT
i=0; njobs=0
while [ $i -lt ${#RUNS[@]} ] && [ $njobs -lt $MAXJOBS ]; do
  J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J AS --parsable "${SBOPT[@]}" --wrap "sleep 2700") || { log "sbatch failed"; exit 1; }
  njobs=$((njobs + 1)); log "job $J submitted"
  while :; do st=$(squeue -h -j $J -o %T 2>/dev/null); [ "$st" = RUNNING ] && break; [ -z "$st" ] && { log "job $J vanished"; exit 1; }; sleep 20; done
  NODE=$(squeue -h -j $J -o %N); t0=$(date +%s); log "job $J running on $NODE"
  R() { srun --jobid=$J -N1 -n1 --gpus=4 --overlap "$@"; }
  TMP=/tmp/as_$J; R bash -c "rm -rf /tmp/as_*; mkdir -p $TMP; df -h /tmp | tail -1" | sed "s/^/  node \/tmp: /"
  while [ $i -lt ${#RUNS[@]} ]; do
    read -r tag D mode envs <<< "${RUNS[$i]}"
    left=$(( BUDGET - ($(date +%s) - t0) )); need=$(est $D $mode)
    if [ $need -gt $left ]; then log "run $tag needs ~$need s, $left s left in job $J: next job"; break; fi
    f=$TMP/e.out; lg=$OUT/$tag.log; raw=$OUT/$tag.raw; t1=$(date +%s)
    if [ "$mode" = nw ]; then arg=""; else arg="$f"; fi
    { echo "== AS run $tag: $D $mode, env: $envs; job $J on $NODE, clone at $HEAD, $(date '+%F %T %Z')"
      time R bash -lc "module load rocm; cd $E && env ECALC_VERBOSE=2 DB_POOL_VERBOSE=1 $envs ./ecalc $D $arg"; } > "$raw" 2>&1; rc=$?
    el=$(( $(date +%s) - t1 )); dig=-
    grep -a '^dbtrace:' "$raw" > "$OUT/$tag.trace"; [ -s "$OUT/$tag.trace" ] || rm -f "$OUT/$tag.trace"
    grep -av '^dbtrace:' "$raw" > "$lg"; rm -f "$raw"
    if [ "$mode" != nw ]; then
      want=$(sha_of $D)
      got=$(R bash -c "P=\$(ls $f.part* 2>/dev/null | sort); [ -n \"\$P\" ] || P=$f; $CLONE/tools/unpack_digits -q \$P | sha1sum | cut -c1-40")
      [ "$got" = "$want" ] && dig=identical || dig="DIFFERS($got)"
      if [ "$mode" = wc ]; then c=$(R bash -c "$E/digcmp.sh $f $(ref_of $D)"); dig="$dig; digcmp $c"; fi
      R bash -c "rm -rf $f $f.*"
    fi
    g() { grep -a -m1 "$1" "$lg" | sed 's/  */ /g'; }
    tot=$(g '^total' | cut -d' ' -f2); ini=$(grep -a 'RESULT ecalc init s' "$lg" | awk '{print $5}')
    bst=$(g '^bs [0-9. ]*s ' | cut -d' ' -f2); rec=$(g '^recip ' | cut -d' ' -f2); dm=$(g '^dm ' | cut -d' ' -f2); t1s=$(g '^T1 ' | cut -d' ' -f2)
    ver=$(grep -a -m1 '^VERIFY' "$lg" || echo "no VERIFY"); [ $rc -ne 0 ] && ver="$ver rc $rc"
    rem=$(grep -a 'VMM remap for' "$lg" | sed 's/.* in \([0-9.]*\) s;.*/\1/' | awk '{n++; s += $1} END {printf "%d %.2f s", n, s}')
    dmr=$(awk '/^recip /{f=1} f && /VMM remap for/' "$lg" | sed 's/.* in \([0-9.]*\) s;.*/\1/' | awk '{n++; s += $1} END {printf "%d %.2f s", n, s}')
    gro=$(grep -ac 'VMM growth\|hipMalloc .* inside the phase' "$lg")
    line="$tag $D $mode | total $tot | init $ini | bs $bst | recip $rec | dm $dm | T1 $t1s | remaps $rem | after recip $dmr | growth $gro | $ver | $dig | ${el}s | $NODE | $envs"
    echo "$line" >> "$SUM"; log "$line"
    i=$((i + 1))
  done
  R bash -c "rm -rf $TMP" ; scancel $J; log "job $J cancelled ($(( $(date +%s) - t0 )) s used)"; J=""
done
[ $i -lt ${#RUNS[@]} ] && log "stopped with $(( ${#RUNS[@]} - i )) runs left (AS_MAXJOBS $MAXJOBS)"
log "done"

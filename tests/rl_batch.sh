#!/bin/bash
# RL (Phase 15): one unattended node batch (docs/AGENT_PROTOCOL.md).  On aac6:
#   setsid nohup bash ~/ntt-RL15/tests/rl_batch.sh <commit> <outdir> <plan file> [sbatch options...] > <outdir>.out 2>&1 < /dev/null &
# RL_CLONE (default ~/ntt-RL15) is the checkout to build and run (a scratch worktree for the merged timing build).
# The plan, one step per line ('#' comments):
#   <tag> node <command ...>       run on the node in ecalc/ (module rocm loaded; the four APUs)
#   <tag> login <command ...>      run on the login node in ecalc/ with $J set (mnaccept.sh $J ...)
#   <tag> <digits> <nw|w> [VAR=value ...]   an ecalc run as tests/t2_sweep.sh (nw: no digit file; w: to the node's /tmp,
#                                   hashed with O_DIRECT against the reference's sha1, then deleted); ECALC_VERBOSE=2
# One 1-node job (-J RL, <= 45 min); the steps that do not fit are reported and skipped.  The job is cancelled at the end.
set -u
COMMIT=$1; OUT=$2; PLAN=$3; shift 3; SBOPT=("$@")
CLONE=${RL_CLONE:-$HOME/ntt-RL15}; E=$CLONE/ecalc
BUDGET=${RL_BUDGET_S:-2580}
mkdir -p "$OUT"; SUM=$OUT/summary.txt
log() { echo "$(date '+%F %T %Z') $*"; }
HEAD=$(git -C "$CLONE" rev-parse --short=7 HEAD)
if [ "${HEAD:0:7}" != "${COMMIT:0:7}" ]; then log "WRONG COMMIT: $CLONE is at $HEAD, the plan wants $COMMIT"; exit 1; fi
bash -lc "module load rocm; cd $E && make -s -j16" > "$OUT/build.log" 2>&1 || { log "build failed (see $OUT/build.log)"; exit 1; }
SHA4=$(cut -c1-40 ~/ntt/ecalc/results/e_4e10.out.sha1); SHA11=$(cut -c1-40 ~/V214/e_1e11.sha1)
sha_of() { case $1 in 40000000000) echo $SHA4;; 100000000000) echo $SHA11;; *) echo none;; esac; }
est() { case $1 in 40000000000) [ $2 = w ] && echo 180 || echo 100;; 100000000000) [ $2 = w ] && echo 420 || echo 250;; *) echo 600;; esac; }
mapfile -t RUNS < <(grep -v '^\s*#' "$PLAN" | grep -v '^\s*$')
log "RL batch: $CLONE at $HEAD, ${#RUNS[@]} steps from $PLAN, sbatch ${SBOPT[*]:-}"
[ -f "$SUM" ] || echo "# tag | total | init | bs | recip | dm | T1 | verify | digits | remaps (count, s summed) | elapsed | node | env" > "$SUM"
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J RL --parsable "${SBOPT[@]}" --wrap "sleep 2700") || { log "sbatch failed"; exit 1; }
trap 'scancel $J 2>/dev/null' EXIT
log "job $J submitted"
while :; do st=$(squeue -h -j $J -o %T 2>/dev/null); [ "$st" = RUNNING ] && break; [ -z "$st" ] && { log "job $J vanished"; exit 1; }; sleep 20; done
NODE=$(squeue -h -j $J -o %N); t0=$(date +%s); log "job $J running on $NODE"
R() { srun --jobid=$J -N1 -n1 --gpus=4 --overlap "$@"; }
TMP=/tmp/rl_$J; R bash -c "rm -rf /tmp/rl_*; mkdir -p $TMP; df -h /tmp | tail -1" | sed "s/^/  node \/tmp: /"
for line in "${RUNS[@]}"; do
  read -r tag D rest <<< "$line"
  left=$(( BUDGET - ($(date +%s) - t0) ))
  lg=$OUT/$tag.log; t1=$(date +%s)
  case $D in
    node|login)
      if [ $left -lt 120 ]; then log "step $tag skipped: $left s left"; echo "$tag SKIPPED" >> "$SUM"; continue; fi
      { echo "== RL step $tag ($D): $rest; job $J on $NODE, clone at $HEAD, $(date '+%F %T %Z')"
        if [ $D = node ]; then time timeout $left srun --jobid=$J -N1 -n1 --gpus=4 --overlap bash -lc "module load rocm; cd $E && $rest"
        else (cd $E && export J && time timeout $left bash -c "$rest"); fi; } > "$lg" 2>&1; rc=$?
      el=$(( $(date +%s) - t1 ))
      v=$(grep -a 'VERIFY\|^== [0-9]* passed' "$lg" | tail -1)
      line2="$tag $D rc $rc | ${v:-no VERIFY line} | ${el}s | $rest"; echo "$line2" >> "$SUM"; log "$line2";;
    *)
      read -r mode envs <<< "$rest"; need=$(est $D $mode)
      if [ $need -gt $left ]; then log "run $tag needs ~$need s, $left s left: skipped"; echo "$tag SKIPPED" >> "$SUM"; continue; fi
      f=$TMP/e.out; if [ "$mode" = w ]; then arg="$f"; else arg=""; fi
      { echo "== RL run $tag: $D $mode, env: $envs; job $J on $NODE, clone at $HEAD, $(date '+%F %T %Z')"
        time R bash -lc "module load rocm; cd $E && env ECALC_VERBOSE=2 $envs ./ecalc $D $arg"; } > "$lg" 2>&1; rc=$?
      el=$(( $(date +%s) - t1 )); dig=-
      if [ "$mode" = w ]; then
        want=$(sha_of $D)
        got=$(R bash -c "P=\$(ls $f.part* 2>/dev/null | sort); [ -n \"\$P\" ] || P=$f; for p in \$P; do dd if=\$p iflag=direct bs=64M status=none; done | sha1sum | cut -c1-40; rm -rf $f $f.*")
        [ "$got" = "$want" ] && dig=identical || dig="DIFFERS($got)"
      fi
      g() { grep -a -m1 "$1" "$lg" | sed 's/  */ /g'; }
      tot=$(g '^total' | cut -d' ' -f2); ini=$(grep -a 'RESULT ecalc init s' "$lg" | awk '{print $5}')
      bst=$(g '^bs [0-9. ]*s ' | cut -d' ' -f2); rec=$(g '^recip ' | cut -d' ' -f2); dm=$(g '^dm ' | cut -d' ' -f2); t1s=$(g '^T1 ' | cut -d' ' -f2)
      nrm=$(grep -ac 'VMM remap for' "$lg"); srm=$(grep -a 'VMM remap for' "$lg" | sed 's/.* in \([0-9.]*\) s.*/\1/' | awk '{s += $1} END {printf "%.2f", s}')
      ver=$(grep -a -m1 '^VERIFY' "$lg" || echo "no VERIFY"); [ $rc -ne 0 ] && ver="$ver rc $rc"
      line2="$tag $D $mode | total $tot | init $ini | bs $bst | recip $rec | dm $dm | T1 $t1s | $ver | $dig | remaps $nrm, $srm s | ${el}s | $NODE | $envs"
      echo "$line2" >> "$SUM"; log "$line2";;
  esac
done
R bash -c "rm -rf $TMP"; scancel $J; log "job $J cancelled ($(( $(date +%s) - t0 )) s used)"; trap - EXIT
log "done"

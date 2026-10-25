#!/bin/bash
# ladder.sh - step 4 of the target session: the ecalc ladder L0 (1 node) / L1 (8) / L2 (64) / L3 (576), each group on physically
# adjacent nodes (target/FORMATS.md section 10).  Replicas of one level run in parallel as separate jobs on disjoint groups.
#
#   ladder.sh --level L0|L1|L2|L3 [--replicas R] [--path 1|2|both] [--run-tag T] [--set KEY=VALUE ...] [--dry-run]
#             [--site target|aac7] [--partition P] [--nodelist-file F] [--jobid J] [--exclude FILE] [--sbatch-opts "..."] [--no-plan]
#             [--nodes-from FILE]   (take the node table from a saved `scontrol show node -o` file instead of the live call)
#   ladder.sh --collect L [--run-tag T]       one summary of the replicas (default: the newest tag) -> stdout + results/ladder/LADDER.md
#
# What a submit does: load config/target.conf and levels/<L>.conf (--set wins) -> plan (FIT) check per path (refuses a level that
# does not fit and prints the largest digit count that does) -> pick R disjoint adjacent groups (locality.py pick; inside the
# allocation with --jobid / --nodelist-file, else the idle nodes of the partition) -> one driver per replica: an sbatch job, or
# with --jobid a background process inside that allocation.  Each driver runs ecalc through ecalc/mnrun.sh (srun), parses the log
# (parse_run.py), and writes results/ladder/<L>/<tag>-r<k>/{summary.txt,raw/,DONE}.  The submit prints the job ids and the DONE
# markers and returns; nothing waits.  Collect with --collect once the markers exist.
#
# --set KEY=VALUE: a conf key (NODES DIGITS PATH RUNS REPLICAS WRITE TIME_MIN KEEP_OUT REF) or any other name = an ecalc environment
# word (ENV_ prefix optional; an empty value drops the word of the conf), e.g. --set MN_T_CHUNK_MB=1024 --set ECALC_NP=4 --set NODES=2 --set DIGITS=5e10.
# Binaries: ECALC1 / ECALC2 (environment) or the newest target/build/*-p1|p2/ecalc (bench items bld01 / bld02); --site aac7 falls
# back to ecalc/ecalc of this tree for path 1.  Other environment: KEEP_OUT=1 keeps written digit files, REF=<ASCII reference>
# makes a written run be compared with ecalc/digcmp.sh, TGT_SBATCH_OPTS (default "--exclusive --gpus-per-node=4").
# Launcher: the SOURCE TREE's ecalc/mnrun.sh (not the build tree's copy), with MNRUN_MODULES from target.conf (the bld summary's module line
# is not used here: set MNRUN_MODULES in target.conf to the line the binary was built with).  Partition: TARGET_PARTITION of target.conf unless
# --partition.  Excluded hosts (--exclude, results/ladder/exclude.txt, MNRUN_EXCLUDE_HOSTS) leave the pool before the groups are picked; mnrun
# runs with MNRUN_EXCLUDE_MODE=warn, and a run whose node count differs from NODES is marked bad.
set -u
OUT=${OUT-}   # common.sh tgt_log reads $OUT
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/../lib/common.sh"
tgt_init
LOC=$HERE/locality.py PARSE=$HERE/parse_run.py
LADDER_RES=$TGT_RES/ladder

die() { echo "ladder.sh: $*" >&2; exit "${RC:-2}"; }
usage() { sed -n '2,/^set -u/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'; }
sci() { python3 -c 'import sys,decimal; print(int(decimal.Decimal(sys.argv[1])))' "$1" 2>/dev/null; }   # 5e10 -> 50000000000

# ------------------------------------------------------------------------------------------------------------ replica driver
# ladder.sh --replica SPEC: runs inside the replica's allocation (sbatch --wrap, or a background process under --jobid)
replica_main() {
    . "$1"                                       # LEVEL TAG K NODES DIGITS PATHS RUNS WRITE_THIS HOSTS TIME_MIN SITE REPO_ BIN1 BIN2 ENVW JOBID_ MODEL_*
    DIR=$LADDER_RES/$LEVEL/$TAG-r$K; OUT=$DIR
    tgt_conf >/dev/null 2>&1 || true
    tgt_sum_init "$DIR" "ladder-$LEVEL-$TAG-r$K"
    J=${SLURM_JOB_ID:-$JOBID_}
    tgt_kv level "$LEVEL"; tgt_kv nodes "$NODES"; tgt_kv digits "$DIGITS"; tgt_kv replica "$K"; tgt_kv hosts "$HOSTS"
    tgt_kv jobid "$J"; tgt_kv paths "$PATHS"; tgt_kv runs "$RUNS"; tgt_kv write "$WRITE_THIS"; tgt_kv site "$SITE"
    for kv in ${ENVW}; do tgt_kv "e.${kv%%=*}" "${kv#*=}"; done
    for v in ECALC_NP MN_T_CHUNK_MB FI_MR_CACHE_MONITOR ECALC_DEVICE_GB ECALC_MEM_GUARD_GB; do [ -n "${!v+x}" ] && tgt_kv "e.$v" "${!v}"; done
    npaths=$(echo $PATHS | wc -w)
    rmin=$(( (TIME_MIN - 3) / (npaths * RUNS) )); [ "$rmin" -ge 1 ] || rmin=1       # per-run timeout (minutes)
    nbad=0 nrun=0 rcall=0
    for p in $PATHS; do
        for i in $(seq 1 "$RUNS"); do
            P=p${p}r${i}; log=$DIR/raw/$P.log; nrun=$((nrun + 1))
            bin=$BIN1; words=($ENVW); [ "$p" = 2 ] && { bin=$BIN2; words+=($PATH2_WORDS); }
            out=; if [ "$WRITE_THIS" = 1 ] || { [ "$WRITE_THIS" = first ] && [ "$i" = 1 ]; }; then
                od=${ECALC_OUT_DIR:-${TMPDIR:-/tmp}/ladder_out}/ladder_${LEVEL}_${TAG}_r${K}_${P}; mkdir -p "$od" && out=$od/e
            fi
            t0=$(date +%s); tgt_log "$P: $NODES node(s) $DIGITS digits path $p${out:+, writing $out}"
            echo "# ladder.sh: mnrun.sh $NODES env ${words[*]} $bin $DIGITS ${out:+$out} (job $J, hosts $HOSTS)" > "$log"
            ( cd "$REPO_/ecalc" || exit 99
              [ "$SITE" = aac7 ] && [ -f ./aac7env.sh ] && . ./aac7env.sh >/dev/null 2>&1
              export SLURM_JOB_ID=$J MNRUN_NODELIST=$HOSTS MNRUN_EXCLUDE_MODE=warn
              tgt_timeout "$rmin" bash "$REPO_/ecalc/mnrun.sh" "$NODES" env "${words[@]}" "$bin" "$DIGITS" ${out:+"$out"} ) >> "$log" 2>&1 < /dev/null
            rc=$?
            wall=$(( $(date +%s) - t0 ))
            seen=$(grep -a -m1 -oE 'mn: pe [0-9]+ of [0-9]+ \(node [0-9]+ of [0-9]+' "$log" | sed -nE 's/.*\(node [0-9]+ of ([0-9]+)/\1/p')
            if [ -n "$seen" ] && [ "$seen" != "$NODES" ]; then
                tgt_kv "$P.nodes_seen" "$seen"; tgt_kv "note_$P" "FAIL: mnrun used $seen node(s), $NODES requested"; [ "$rc" = 0 ] && rc=96
            fi
            while IFS='=' read -r k v; do [ -n "$k" ] && tgt_kv "$P.$k" "$v"; done < <(python3 "$PARSE" parse "$log" --nodes "$NODES" --hosts "$HOSTS" --pernode "$DIR/raw/$P.pernode.tsv" 2>/dev/null)
            tgt_kv "$P.path" "$p"; tgt_kv "$P.rc" "$rc"; tgt_kv "$P.wall_s" "$wall"
            tgt_kv "$P.env" "${words[*]}"
            ver=$(grep -m1 "^$P.verify=" "$DIR/summary.txt" | cut -d= -f2)
            tot=$(grep -m1 "^$P.total_s=" "$DIR/summary.txt" | cut -d= -f2)
            [ -n "$tot" ] && tgt_kv "m.${P}_total" "$tot s"
            if [ -n "$out" ]; then
                if [ -n "${REF:-}" ] && [ -f "${REF}" ]; then
                    tgt_kv "$P.digcmp" "$(bash "$REPO_/ecalc/digcmp.sh" "$out" "$REF" 2>&1 | tail -1)"
                fi
                tgt_kv "$P.out_bytes" "$(du -sb "$(dirname "$out")" 2>/dev/null | cut -f1)"
                if [ "${KEEP_OUT:-0}" != 1 ]; then case "$od" in */ladder_${LEVEL}_*) rm -rf -- "$od"; tgt_kv "$P.out_removed" 1;; esac; fi
            fi
            [ "$rc" = 0 ] && [ "$ver" = OK ] || { nbad=$((nbad + 1)); rcall=$rc; [ "$rc" = 0 ] && rcall=1; }
            tgt_log "$P: rc=$rc verify=${ver:-?} total=${tot:-?} s wall=$wall s"
        done
    done
    st=PASS; [ "$nbad" -gt 0 ] && st=PARTIAL; [ "$nbad" -ge "$nrun" ] && st=FAIL
    tgt_kv note "$nrun run(s), $nbad not clean; model (mod): no-write ${MODEL_NOWRITE_S:-?} s, write ${MODEL_WRITE_S:-?} s"
    tgt_sum_end "$st" "$rcall"
    exit 0
}
if [ "${1:-}" = --replica ]; then replica_main "$2"; fi

# ------------------------------------------------------------------------------------------------------------ collect
collect_main() {
    local L=$1 tag=${2:-}
    [ -n "$tag" ] || tag=$(ls -t "$LADDER_RES/$L" 2>/dev/null | grep -- '-r[0-9]*$' | head -1 | sed 's/-r[0-9]*$//')
    [ -n "$tag" ] || die "no runs of $L under $LADDER_RES/$L"
    local dirs=() d; for d in "$LADDER_RES/$L/$tag"-r*/; do [ -f "$d/DONE" ] && dirs+=("${d%/}") || echo "ladder.sh: $d not DONE yet (skipped)" >&2; done
    [ ${#dirs[@]} -gt 0 ] || die "no finished replica of $L / $tag"
    local mnw= mw= cf=$HERE/levels/$L.conf
    mnw=$(sed -n 's/^MODEL_NOWRITE_S=\([0-9.]*\).*/\1/p' "$cf"); mw=$(sed -n 's/^MODEL_WRITE_S=\([0-9.]*\).*/\1/p' "$cf")
    local cd; cd=$(sed -n 's/^DIGITS=\([0-9]*\).*/\1/p' "$cf" | head -1)         # the model is for the conf's digits only
    [ "$(sed -n 's/^digits=//p' "${dirs[0]}/summary.txt" | head -1)" = "$cd" ] || { mnw=; mw=; echo "ladder.sh: digits differ from $L.conf: no model comparison" >&2; }
    python3 "$PARSE" collect "${dirs[@]}" ${mnw:+--model-nowrite "$mnw"} ${mw:+--model-write "$mw"} --md "$LADDER_RES/LADDER.md" --label "$L $tag $(date '+%F %T %Z')"
    exit $?
}

# ------------------------------------------------------------------------------------------------------------ arguments
LEVEL=; REPL=; PATHARG=; TAG=; DRY=0; SITE=target; PART=; NLF=; JOBID=; EXCL=; SBO=${TGT_SBATCH_OPTS---exclusive --gpus-per-node=4}; NOPLAN=0
SETS=(); COLLECT=; NFROM=
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --level) LEVEL=$2; shift 2 ;;
        --replicas) REPL=$2; shift 2 ;;
        --path) PATHARG=$2; shift 2 ;;
        --run-tag) TAG=$2; shift 2 ;;
        --set) SETS+=("$2"); shift 2 ;;
        --dry-run) DRY=1; shift ;;
        --site) SITE=$2; shift 2 ;;
        --partition) PART=$2; shift 2 ;;
        --nodelist-file) NLF=$2; shift 2 ;;
        --jobid) JOBID=$2; shift 2 ;;
        --exclude) EXCL=$2; shift 2 ;;
        --sbatch-opts) SBO=$2; shift 2 ;;
        --no-plan) NOPLAN=1; shift ;;
        --collect) COLLECT=$2; shift 2 ;;
        --nodes-from) NFROM=$2; shift 2 ;;
        *) die "unknown option $1 (--help)" ;;
    esac
done
[ -n "$COLLECT" ] && collect_main "$COLLECT" "$TAG"
if [ -z "$LEVEL" ]; then usage; exit 2; fi
case "$LEVEL" in 0|1|2|3) LEVEL=L$LEVEL;; esac
case "$LEVEL" in L0|L1|L2|L3) ;; *) die "--level L0|L1|L2|L3" ;; esac
case "$SITE" in target|aac7) ;; *) die "--site target|aac7" ;; esac
[ -f "$HERE/levels/$LEVEL.conf" ] || die "no $HERE/levels/$LEVEL.conf"

tgt_conf
[ -n "$PART" ] || PART=${TARGET_PARTITION:-}
[ "$PART" = __FILL__ ] && PART=
[ "$SITE" = aac7 ] && for kv in ECALC_NODE_GB=420 ECALC_DEVICE_GB=444.0 MN_T_CHUNK_MB=2048 ECALC_MEM_GUARD_GB=6; do
    k=${kv%%=*}; [ -n "${!k+x}" ] || export "$k=${kv#*=}"; done

# level conf into an array (never into shell variables: the key PATH must not touch the shell's PATH)
declare -A C=()
while IFS= read -r line; do
    line=${line%%#*}; line=${line%"${line##*[![:space:]]}"}
    [[ $line =~ ^[A-Za-z_][A-Za-z0-9_]*= ]] && C[${line%%=*}]=${line#*=}
done < "$HERE/levels/$LEVEL.conf"
CONFKEYS=" LEVEL NODES DIGITS PATH RUNS REPLICAS WRITE TIME_MIN KEEP_OUT REF MODEL_NOWRITE_S MODEL_WRITE_S PATH2_WORDS "
for s in "${SETS[@]+"${SETS[@]}"}"; do
    k=${s%%=*}; v=${s#*=}; [ "$k" != "$s" ] || die "--set needs KEY=VALUE: $s"
    if [[ "$CONFKEYS" == *" $k "* || "$k" == ENV_* ]]; then C[$k]=$v; else C[ENV_$k]=$v; fi
done
[ -n "$REPL" ] && C[REPLICAS]=$REPL
[ -n "$PATHARG" ] && C[PATH]=$PATHARG
NODES=${C[NODES]}; RUNS=${C[RUNS]:-1}; REPL=${C[REPLICAS]:-1}; WRITE=${C[WRITE]:-0}; TIME_MIN=${C[TIME_MIN]:-30}
DIGITS=$(sci "${C[DIGITS]}"); [ -n "$DIGITS" ] || die "DIGITS=${C[DIGITS]} is not a number"
[ -n "${C[KEEP_OUT]:-}" ] && export KEEP_OUT=${C[KEEP_OUT]}; [ -n "${C[REF]:-}" ] && export REF=${C[REF]}
case "${C[PATH]:-both}" in 1|2) PATHS=${C[PATH]};; both) PATHS="1 2";; *) die "PATH must be 1, 2 or both";; esac
for v in NODES RUNS REPL TIME_MIN; do [[ ${!v} =~ ^[0-9]+$ ]] && [ "${!v}" -ge 1 ] || die "$v=${!v}: a positive integer"; done
case "$WRITE" in 0|1|first) ;; *) die "WRITE must be 0, 1 or first";; esac
PATH2_WORDS=${C[PATH2_WORDS]:-ECALC_PE_PER_GPU=1 XSHMEM_LANES=2}
ENVW=; for k in "${!C[@]}"; do case "$k" in ENV_*) [ -n "${C[$k]}" ] && ENVW="$ENVW ${k#ENV_}=${C[$k]}";; esac; done   # an empty value (--set ENV_X=) drops the word
ENVW=$(echo $ENVW | tr ' ' '\n' | sort | tr '\n' ' '); ENVW=${ENVW% }
[ -n "$TAG" ] || TAG=$(date +%Y%m%d-%H%M)-$TGT_SHA

find_bin() {     # path number -> binary
    local v=ECALC$1 d
    [ -n "${!v:-}" ] && { echo "${!v}"; return 0; }
    d=$(ls -td "$TGT_ROOT"/build/*-p"$1" 2>/dev/null | head -1)
    [ -n "$d" ] && [ -x "$d/ecalc" ] && { echo "$d/ecalc"; return 0; }
    [ "$SITE" = aac7 ] && [ "$1" = 1 ] && [ -x "$REPO/ecalc/ecalc" ] && { echo "$REPO/ecalc/ecalc"; return 0; }
    return 1
}
BIN1=; BIN2=; keep=
for p in $PATHS; do
    if b=$(find_bin "$p"); then eval "BIN$p=\$b"; keep="$keep $p"
    elif [ "$DRY" = 1 ]; then eval "BIN$p=\"<ECALC$p: no binary yet>\""; keep="$keep $p"; echo "ladder.sh: path $p: no binary (set ECALC$p or build bld0$p); dry run goes on" >&2
    else echo "ladder.sh: path $p: no binary (ECALC$p unset, no target/build/*-p$p/ecalc)" >&2; fi
done
PATHS=${keep# }; [ -n "$PATHS" ] || { RC=2 die "no binary for any requested path"; }

# ------------------------------------------------------------------------------------------------------------ plan (FIT)
pwords() { local w=$ENVW; [ "$1" = 2 ] && w="$w $PATH2_WORDS"; echo $w; }
plan_fits() {    # path digits -> 0 fits, 1 refused, 2 planner failed
    python3 "$REPO/tools/plan_values.py" --ecalc "$(eval echo \$BIN$1)" --env "$(pwords "$1")" --fit "$2" "$NODES" > "${3:-/dev/null}" 2>&1
}
largest_fit() {  # path -> prints the largest digit count (multiple of 1e8 x nodes... of 1e9) that fits, 0 if none found
    local lo=0 hi=$((DIGITS - 1)) mid
    while [ $((hi - lo)) -gt $((DIGITS / 100 + 1)) ]; do
        mid=$(( (lo + hi) / 2 ))
        plan_fits "$1" "$mid"; case $? in 0) lo=$mid;; *) hi=$mid;; esac
    done
    echo $(( lo / 1000000000 * 1000000000 ))
}
if [ "$NOPLAN" = 1 ]; then echo "ladder.sh: plan check skipped (--no-plan)" >&2
else
    for p in $PATHS; do
        b=$(eval echo \$BIN$p)
        if [ ! -x "$b" ]; then echo "ladder.sh: plan check path $p skipped (no binary)" >&2; continue; fi
        mkdir -p "$LADDER_RES/$LEVEL"
        plan_fits "$p" "$DIGITS" "$LADDER_RES/$LEVEL/$TAG.plan-p$p.txt"; rc=$?
        if [ $rc = 0 ]; then echo "ladder.sh: plan path $p: $DIGITS digits on $NODES node(s) FITS ($(grep -m1 'FITS:' "$LADDER_RES/$LEVEL/$TAG.plan-p$p.txt" | sed 's/.*FITS: //' | cut -c1-120))" >&2
        else
            echo "ladder.sh: REFUSED path $p: $DIGITS digits on $NODES node(s) rc=$rc (details $LADDER_RES/$LEVEL/$TAG.plan-p$p.txt):" >&2
            grep -a -E 'REFUSED|FATAL|verdict|margin' "$LADDER_RES/$LEVEL/$TAG.plan-p$p.txt" | head -4 | cut -c1-220 | sed 's/^/    /' >&2
            [ $rc = 1 ] && { lf=$(largest_fit "$p"); echo "ladder.sh: largest digit count that fits path $p on $NODES node(s): $lf  (re-run with --set DIGITS=$lf)" >&2; }
            RC=4; die "level $LEVEL refused before any node time was used"
        fi
    done
fi

# ------------------------------------------------------------------------------------------------------------ pick the groups
pickargs=(--size "$NODES" --count "$REPL")
if [ -n "$JOBID" ]; then
    HF=$(mktemp); if [ "$DRY" = 1 ] && ! command -v squeue >/dev/null 2>&1; then : > "$HF"; else tgt_hosts "$JOBID" > "$HF"; fi
    pickargs+=(--only-hosts "$HF" --state any)
elif [ -n "$NLF" ]; then pickargs+=(--only-hosts "$NLF" --state any)
else pickargs+=(--state idle); [ -n "$PART" ] && pickargs+=(--partition "$PART"); fi   # PART: --partition or TARGET_PARTITION
[ -z "$EXCL" ] && [ -f "$LADDER_RES/exclude.txt" ] && EXCL=$LADDER_RES/exclude.txt
if [ -n "${MNRUN_EXCLUDE_HOSTS:-}" ] || [ -n "$EXCL" ]; then    # one exclude file: the file's hosts plus MNRUN_EXCLUDE_HOSTS
    EXF=$(mktemp); { [ -n "$EXCL" ] && sed 's/#.*//' "$EXCL"; echo "${MNRUN_EXCLUDE_HOSTS:-}"; } | tr ', \t' '\n\n\n' | grep . > "$EXF" || true
    pickargs+=(--exclude "$EXF")
fi
[ -n "$NFROM" ] && pickargs+=(--from "$NFROM")
GRP=(); mapfile -t GRP < <(python3 "$LOC" pick "${pickargs[@]}" 2> >(sed 's/^/  /' >&2))
[ ${#GRP[@]} -ge 1 ] || { RC=3; die "no group of $NODES adjacent nodes available (locality.py pick found none)"; }
if [ ${#GRP[@]} -lt "$REPL" ]; then echo "ladder.sh: only ${#GRP[@]} of $REPL groups of $NODES available: running ${#GRP[@]} replica(s)" >&2; REPL=${#GRP[@]}; fi

# ------------------------------------------------------------------------------------------------------------ one driver per replica
SUBMIT=$LADDER_RES/$LEVEL/$TAG.submit.txt
[ "$DRY" = 1 ] || { mkdir -p "$LADDER_RES/$LEVEL"; : > "$SUBMIT"; }
rel() { echo "${1#$TGT_ROOT/}"; }          # repo-relative paths in the submit record (no home paths in committed files)
say() { echo "$*"; [ "$DRY" = 1 ] || echo "$*" >> "$SUBMIT"; }
say "ladder $LEVEL tag=$TAG nodes=$NODES digits=$DIGITS paths='$PATHS' runs=$RUNS replicas=$REPL write=$WRITE site=$SITE env='$ENVW'"
for k in $(seq 1 "$REPL"); do
    hosts=${GRP[$((k - 1))]}; DIR=$LADDER_RES/$LEVEL/$TAG-r$k
    pa=$PATHS; [ $((k % 2)) = 0 ] && [ "$(echo $PATHS | wc -w)" = 2 ] && pa="2 1"
    wt=0; if [ "$WRITE" = 1 ] || { [ "$WRITE" = first ] && [ "$k" = 1 ]; }; then wt=$WRITE; fi
    SPEC=$DIR/spec.env
    if [ "$DRY" = 0 ]; then
        mkdir -p "$DIR"
        { for kv in LEVEL=$LEVEL TAG=$TAG K=$k NODES=$NODES DIGITS=$DIGITS "PATHS=$pa" RUNS=$RUNS WRITE_THIS=$wt "HOSTS=$hosts" TIME_MIN=$TIME_MIN \
                SITE=$SITE REPO_=$REPO "BIN1=$BIN1" "BIN2=$BIN2" "ENVW=$ENVW" "PATH2_WORDS=$PATH2_WORDS" "JOBID_=$JOBID" \
                "MODEL_NOWRITE_S=${C[MODEL_NOWRITE_S]:-}" "MODEL_WRITE_S=${C[MODEL_WRITE_S]:-}"; do printf '%s=%q\n' "${kv%%=*}" "${kv#*=}"; done; } > "$SPEC"
    fi
    say "replica $k: hosts=$hosts paths='$pa' write=$wt dir=$(rel "$DIR") done-marker=$(rel "$DIR")/DONE"
    if [ -n "$JOBID" ]; then
        cmd="nohup env SLURM_JOB_ID=$JOBID bash $HERE/ladder.sh --replica $SPEC > $DIR/driver.out 2>&1 &"
        if [ "$DRY" = 1 ]; then echo "  [dry-run] $cmd"; else eval "$cmd"; say "  started in allocation $JOBID (pid $!)"; fi
    else
        sb=(sbatch --parsable -N "$NODES" -w "$hosts" -t "$TIME_MIN" -J "ladder-$LEVEL-$TAG-r$k" -o "$DIR/sbatch.out" ${PART:+-p "$PART"} $SBO --wrap "bash $HERE/ladder.sh --replica $SPEC")
        if [ "$DRY" = 1 ]; then printf '  [dry-run]'; printf ' %q' "${sb[@]}"; echo
        else jid=$("${sb[@]}" 2>&1) || { say "  sbatch failed: $jid"; continue; }; say "  jobid $jid"; fi
    fi
    if [ "$DRY" = 1 ]; then
        first=${pa%% *}; bin=$BIN1; w=$ENVW; [ "$first" = 2 ] && { bin=$BIN2; w="$w $PATH2_WORDS"; }
        echo "  [dry-run] first run: SLURM_JOB_ID=<job> MNRUN_NODELIST=$hosts bash $REPO/ecalc/mnrun.sh $NODES env $w $bin $DIGITS"
    fi
done
[ "$DRY" = 1 ] && echo "ladder.sh: dry run: nothing submitted. A real run prints the job ids; collect with: $HERE/ladder.sh --collect $LEVEL --run-tag $TAG"
exit 0

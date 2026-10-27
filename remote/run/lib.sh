# remote/run/lib.sh - shared functions of the remote stage scripts (source it; it runs nothing).
# Contract: remote/FACTS.md and the summary block below.  Settings: $REMOTE_OUT/site.env (written by s1/s2/alloc), REMOTE_SITE=aac7 = rehearsal mode.
# Summary block (stdout of every script, <= ~80 lines):
#   === REMOTE <stage> SUMMARY ===
#   stage=<stage> verdict=PASS|FAIL|PARTIAL|SKIP run=<dir> wall_s=<n>
#   KEY=VALUE lines and tables, on FAIL up to 15 error lines, next=<one command>
#   === END <stage> ===
# Exit status: 0 PASS/PARTIAL/SKIP, 1 FAIL, 2 usage.

RL_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
R=$(cd "$RL_DIR/../.." && pwd)
REMOTE_OUT=${REMOTE_OUT:-$HOME/remote_runs}
REMOTE_SITE=${REMOTE_SITE:-target}
SITE_ENV=$REMOTE_OUT/site.env
DRY=${DRY:-0}
export R REMOTE_OUT REMOTE_SITE
RL_T0=$SECONDS; RL_FINISHED=0; RUN=; STAGE=; RL_ERRFILES=()

# ---- help / usage ---------------------------------------------------------------------------------------------------
rl_help() { sed -n '2,/^[^#]/p' "$1" | grep '^#' | sed 's/^# \{0,1\}//'; }   # header comment of the calling script
rl_usage() { echo "$(basename "$0"): $*" >&2; RL_FINISHED=1; exit 2; }

# ---- run dir, logging -------------------------------------------------------------------------------------------------
rl_init() {     # STAGE
    STAGE=$1; RL_T0=$SECONDS
    local ts base; ts=$(date +%Y%m%d-%H%M%S); base=$REMOTE_OUT/${STAGE}_$ts; RUN=$base
    mkdir -p "$REMOTE_OUT" || { echo "cannot create $REMOTE_OUT" >&2; exit 1; }
    local i=0; while ! mkdir "$RUN" 2>/dev/null; do i=$((i + 1)); RUN=${base}_$i; [ $i -gt 50 ] && { echo "cannot create run dir" >&2; exit 1; }; done
    mkdir -p "$RUN/raw"
    if [ "$DRY" != 1 ]; then ln -sfn "$RUN" "$REMOTE_OUT/${STAGE}_latest"; ln -sfn "$RUN" "$REMOTE_OUT/latest"; fi
    : > "$RUN/summary.body"; : > "$RUN/log.txt"
    export RUN STAGE
    trap rl_trap EXIT
    rl_site_load
    rl_say "start ($(date '+%F %T %Z'), site=$REMOTE_SITE$([ "$DRY" = 1 ] && echo ", dry-run")) run dir $RUN"
}
rl_say() { local l; l="[$STAGE $(date +%H:%M:%S)] $*"; echo "$l"; [ -n "$RUN" ] && echo "$l" >> "$RUN/log.txt"; return 0; }
rl_log() { [ -n "$RUN" ] && echo "$(date +%T) $*" >> "$RUN/log.txt"; return 0; }
rl_die() { rl_say "ERROR: $*"; rl_finish FAIL "$*" "${NEXT_HINT:-$0 --help}"; }
rl_kv() { local k=$1; shift; echo "$k=$*" >> "$RUN/summary.body"; }
rl_line() { echo "$*" >> "$RUN/summary.body"; }
rl_would() { echo "  would run: $*" >> "$RUN/summary.body"; }
rl_errfiles() { RL_ERRFILES+=("$@"); }

rl_cat() { local x; for x in "$@"; do [ -f "$x" ] || continue; case $x in *.gz) zcat "$x";; *) cat "$x";; esac; done 2>/dev/null; }
rl_errs() {     # FILES... -> up to 15 distinct error-looking lines (rank labels / times stripped, digits folded for the dedupe)
    local f=("$@"); [ ${#f[@]} -gt 0 ] || f=("$RUN"/raw/*.log "$RUN"/*.log)
    rl_cat "${f[@]}" \
      | grep -a -i -E 'error|fail|abort|segv|refused|timeout|fatal|no hipcc|undefined reference|not found' \
      | grep -a -v -i -E 'fail(ed)?=0|errors?[:=] *0|without error|VERIFY OK|no error|0 fail' \
      | sed -E 's/^ *[0-9]+: //; s/^\[[a-z0-9_]+ [0-9:]+\] //; s/^[0-9]{4}-[0-9-]+ [0-9:]+ [A-Z]* //' | cut -c1-200 \
      | awk '{ k = $0; gsub(/[0-9]+/, "N", k); if (!(k in s)) { s[k] = 1; print } }' | head -n 15
}

rl_finish() {   # VERDICT [NOTE] NEXT: print the summary block, exit by verdict
    local v=$1 note=${2-} next=${3-}
    [ $# -eq 2 ] && { next=$note; note=; }
    RL_FINISHED=1
    local wall=$((SECONDS - RL_T0)) body="$RUN/summary.body" out="$RUN/summary.txt" nl
    {
        echo "=== REMOTE $STAGE SUMMARY ==="
        echo "stage=$STAGE verdict=$v run=$RUN wall_s=$wall"
        [ -n "$note" ] && echo "note=$note"
        cat "$body"
        if [ "$v" = FAIL ]; then
            local e; e=$(rl_errs "${RL_ERRFILES[@]}")
            [ -n "$e" ] && { echo "errors:"; echo "$e" | sed 's/^/  /'; }
        fi
        echo "next=$next"
        echo "=== END $STAGE ==="
    } > "$out"
    nl=$(wc -l < "$out")
    if [ "$nl" -gt 84 ]; then { head -n 70 "$out"; echo "... ($((nl - 74)) more lines: $out)"; tail -n 4 "$out"; }; else cat "$out"; fi
    echo "$v" > "$RUN/DONE"
    case $v in FAIL) exit 1;; *) exit 0;; esac
}
rl_trap() { local rc=$?; if [ "$RL_FINISHED" != 1 ] && [ -n "$RUN" ]; then rl_finish FAIL "script ended unexpectedly (rc $rc)" "show.sh $RUN tail 40"; fi; }

# ---- timeouts and helpers --------------------------------------------------------------------------------------------
rl_tmo() { local s=$1; shift; timeout -k 10 "$s" "$@"; }       # SECONDS CMD...
rl_num() { awk -v x="$1" 'BEGIN { printf "%.0f", x }'; }        # 64e9 / 38.2e12 / plain integer -> integer
rl_isnum() { awk -v x="$1" 'BEGIN { exit !(x ~ /^[0-9]+([.][0-9]+)?([eE][+]?[0-9]+)?$/ && x + 0 > 0) }'; }
rl_fmt() { awk -v x="$1" 'BEGIN { printf "%.4g", x }'; }
rl_user() { id -un 2>/dev/null || echo "${USER:-user}"; }
rl_have() { command -v "$1" >/dev/null 2>&1; }
rl_module_init() {
    type module >/dev/null 2>&1 && return 0
    local f; for f in /usr/share/lmod/lmod/init/bash /opt/cray/pe/lmod/lmod/init/bash /usr/share/Modules/init/bash /etc/profile.d/modules.sh; do [ -f "$f" ] && . "$f" && return 0; done
    return 1
}

# ---- site.env ---------------------------------------------------------------------------------------------------------
rl_site_load() {    # KEY=VALUE lines of site.env; the caller's environment wins
    local line k
    if [ -f "$SITE_ENV" ]; then
        while IFS= read -r line; do
            [[ $line =~ ^[A-Za-z_][A-Za-z0-9_]*= ]] || continue
            k=${line%%=*}; [ -n "${!k+x}" ] && continue
            eval "export $line" 2>/dev/null
        done < "$SITE_ENV"
    fi
    rl_site_defaults
}
rl_site_set() {     # KEY VALUE: write/replace in site.env (atomic), and export
    local k=$1 v=$2 t
    [ "$DRY" = 1 ] && return 0
    mkdir -p "$REMOTE_OUT"; t=$(mktemp "$REMOTE_OUT/.site.XXXXXX")
    { [ -f "$SITE_ENV" ] && grep -v "^$k=" "$SITE_ENV"; printf '%s=%q\n' "$k" "$v"; } > "$t"; mv "$t" "$SITE_ENV"
    export "$k=$v"
}
rl_site_unset() { local t; [ -f "$SITE_ENV" ] || return 0; [ "$DRY" = 1 ] && return 0; t=$(mktemp "$REMOTE_OUT/.site.XXXXXX"); grep -v "^$1=" "$SITE_ENV" > "$t"; mv "$t" "$SITE_ENV"; unset "$1"; }

# REMOTE_SITE=aac7: the rehearsal machine (modules, GMP, partition, node exclusions, digits scaled to 1-2 nodes)
rl_site_defaults() {
    [ "$REMOTE_SITE" = aac7 ] || return 0
    local rocm=${AAC7_ROCM:-rocm/7.2.4}
    export REMOTE_MODULES_P1=${REMOTE_MODULES_P1:-$rocm}
    export REMOTE_MODULES_P2=${REMOTE_MODULES_P2:-cray-dsmml cray-openshmemx $rocm}
    export REMOTE_UNLOAD=${REMOTE_UNLOAD-rocm/7.0.3}
    export GMP_HOME=${GMP_HOME:-$HOME/gmp}
    export REMOTE_PARTITION=${REMOTE_PARTITION:-${AAC7_PARTITION:-192C4G1H_MI300A_RHEL9_A1}}
    export TCONF=${TCONF:-/nonexistent/target.conf}        # not the target's settings
    export ECALC_DEVICE_GB=${ECALC_DEVICE_GB:-444.0} ECALC_NODE_GB=${ECALC_NODE_GB:-420} ECALC_MEM_GUARD_GB=${ECALC_MEM_GUARD_GB:-6}
    export MN_T_CHUNK_MB=${MN_T_CHUNK_MB:-2048} ECALC_NP=${ECALC_NP:-auto} MNRUN_CPUS_PER_TASK=${MNRUN_CPUS_PER_TASK:-auto}
    export REMOTE_MAXD_PER_NODE=${REMOTE_MAXD_PER_NODE:-10000000000}
}
rl_digits_site() {  # DIGITS NODES -> digits actually run (aac7: capped at REMOTE_MAXD_PER_NODE x nodes; the target: unchanged)
    local d; d=$(rl_num "$1")
    if [ "$REMOTE_SITE" = aac7 ]; then awk -v d="$d" -v n="$2" -v m="${REMOTE_MAXD_PER_NODE:-10000000000}" 'BEGIN { c = m * n; v = (d > c) ? c : d; printf "%.0f", v }'; else echo "$d"; fi
}

# ---- modules per path -------------------------------------------------------------------------------------------------
rl_mods() {         # PATH -> module line (site.env, else target.conf's MNRUN_MODULES), empty = let the build helper detect
    local p=$1 m
    if [ "$p" = 1 ]; then m=${REMOTE_MODULES_P1:-}; else m=${REMOTE_MODULES_P2:-}; fi
    if [ -z "$m" ] && [ "$REMOTE_SITE" != aac7 ] && [ -n "${MNRUN_MODULES:-}" ]; then
        if [ "$p" = 2 ]; then m=$MNRUN_MODULES; else m=$(echo "$MNRUN_MODULES" | tr ' ' '\n' | grep -v -E '^cray-(dsmml|openshmemx|shmem)' | paste -sd' '); fi
    fi
    echo "$m"
}
rl_unload() { echo "${REMOTE_UNLOAD-${MNRUN_UNLOAD:-}}"; }

# ---- target.conf / _ecalc.sh -------------------------------------------------------------------------------------------
rl_conf() {         # load target/config/target.conf (not in aac7 mode) into the environment
    . "$R/target/lib/common.sh" || return 1
    tgt_init
    [ "$REMOTE_SITE" = aac7 ] && return 0
    tgt_conf 2>>"$RUN/log.txt"
    return 0
}
rl_nou() { set +u; "$@"; local r=$?; set -u; return $r; }   # _ecalc.sh and the target helpers are not written for set -u

# ---- allocation ---------------------------------------------------------------------------------------------------------
rl_job_state() { rl_tmo 30 squeue -h -j "$1" -o %T 2>/dev/null | head -1; }
rl_job_mine() { [ "$(rl_tmo 30 squeue -h -j "$1" -o %u 2>/dev/null | head -1)" = "$(rl_user)" ]; }
rl_hosts_all() { local n; n=$(rl_tmo 30 squeue -h -j "$1" -o %N 2>/dev/null | head -1); [ -n "$n" ] && rl_tmo 30 scontrol show hostnames "$n" 2>/dev/null; }
rl_exclude_list() { # comma list: --exclude / REMOTE_EXCLUDE, target.conf's MNRUN_EXCLUDE_HOSTS, aac7's ~/aac7q/exclude* files
    local l=${EXCLUDE:-}${REMOTE_EXCLUDE:+,$REMOTE_EXCLUDE}${MNRUN_EXCLUDE_HOSTS:+,$MNRUN_EXCLUDE_HOSTS}
    if [ "$REMOTE_SITE" = aac7 ]; then local f; for f in "$HOME"/aac7q/exclude*; do [ -f "$f" ] && l="$l,$(grep -v '^ *#' "$f" | tr -s ' \n,' ',' )"; done; fi
    echo "$l" | tr ',' '\n' | grep -E '^[A-Za-z0-9._-]+$' | sort -u | paste -sd,
}
rl_pick_hosts() {   # JOB N [SKIP] -> comma list of N usable hosts of the allocation (excluded hosts left out); rc 1 if fewer
    local job=$1 n=$2 skip=${3:-0} ex all l
    ex=$(rl_exclude_list); all=$(rl_hosts_all "$job") || return 1
    [ -n "$all" ] || return 1
    if [ -n "$ex" ]; then all=$(echo "$all" | grep -Fvxf <(echo "$ex" | tr ',' '\n') || true); fi
    l=$(echo "$all" | tail -n +$((skip + 1)) | head -n "$n" | paste -sd,)
    [ "$(echo "$l" | tr ',' '\n' | grep -c .)" -ge "$n" ] || return 1
    echo "$l"
}
rl_resolve_job() {  # sets JOB from --jobid (JOB already set) or REMOTE_JOB; rc 1 if none
    JOB=${JOB:-${REMOTE_JOB:-}}; [ -n "$JOB" ]
}
rl_need_running() { # JOB: dies unless the job is RUNNING
    local s; s=$(rl_job_state "$1")
    [ "$s" = RUNNING ] || rl_die "job $1 is not running (state: ${s:-gone}); run alloc.sh first or pass --jobid"
}

# ---- ecalc runs through target/bench/items/_ecalc.sh -----------------------------------------------------------------
rl_bin() {          # PATH -> binary of the build (site.env ECALC1/ECALC2)
    if [ "$1" = 1 ]; then echo "${REMOTE_ECALC1:-}"; else echo "${REMOTE_ECALC2:-}"; fi
}
rl_ecsetup() {      # JOB HOSTS: export what _ecalc.sh needs and source it (once)
    local job=$1 hosts=$2
    export OUT=$RUN JOBID=$job HOSTS=$hosts ID=remote
    export TGT_RES_DIR=$RUN/tgtres TGT_SCRATCH=${REMOTE_SCRATCH:-$REMOTE_OUT/scratch} TGT_DIGITS_SCALE=1
    export TGT_SITE=target
    export ECALC1=$(rl_bin 1) ECALC2=$(rl_bin 2)
    [ -n "$ECALC1" ] && export MNRUN_DIR1=$(dirname "$ECALC1")
    [ -n "$ECALC2" ] && export MNRUN_DIR2=$(dirname "$ECALC2")
    export MNRUN_MODULES_P1=$(rl_mods 1) MNRUN_MODULES_P2=$(rl_mods 2) MNRUN_UNLOAD=$(rl_unload)
    mkdir -p "$TGT_SCRATCH" "$TGT_RES_DIR"
    if [ -z "${_ECALC_SH_LOADED:-}" ]; then
        set +u; . "$R/target/bench/items/_ecalc.sh" 2>>"$RUN/log.txt"; set -u
    fi
}
rl_shape() { [ "$1" = 2 ] && echo C || echo D; }
rl_cmdline() {      # PATH NODES DIGITS [words] -> the launch line as it will run (dry-run output and log)
    local p=$1 n=$2 d=$3; shift 3
    local shape=; [ "$p" = 2 ] && shape="ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 "
    local bin; bin=$(rl_bin "$p"); [ -n "$bin" ] || bin="<build p$p>/ecalc/ecalc"
    echo "SLURM_JOB_ID=${JOB:-<job>} MNRUN_NODELIST=<$n hosts> MNRUN_MODULES=\"$(rl_mods "$p")\" $(dirname "$bin")/mnrun.sh $n env ${shape}$* $bin $d"
}
rl_prep() {         # "PATHS" MAXNODES: config, job, binaries, hosts, _ecalc.sh; sets JOB, HOSTS_ALL, NHOSTS; dies when the job or all binaries are missing
    local paths=$1 maxn=$2 p n h any=0
    rl_conf
    rl_resolve_job || rl_die "no allocation: run alloc.sh first (or pass --jobid J)"
    rl_need_running "$JOB"
    for p in $paths; do if [ -x "$(rl_bin "$p")" ]; then any=1; else rl_say "path $p: no binary (run s2_build.sh)"; fi; done
    [ $any = 1 ] || rl_die "no ecalc binary for path(s) $paths (s2_build.sh first)"
    n=$maxn; HOSTS_ALL=
    while [ "$n" -ge 1 ]; do HOSTS_ALL=$(rl_pick_hosts "$JOB" "$n") && break; HOSTS_ALL=; n=$((n - 1)); done
    [ -n "$HOSTS_ALL" ] || rl_die "no usable node in job $JOB"
    NHOSTS=$n
    rl_ecsetup "$JOB" "$HOSTS_ALL"
    rl_kv job "$JOB"; rl_kv nodes_usable "$NHOSTS"
}
rl_ecrun() {        # TAG PATH NODES DIGITS [VAR=VALUE ...] -> E_* (see _ecalc.sh); rc = ecalc_run's
    local tag=$1 p=$2 n=$3 d=$4; shift 4
    rl_nou ecalc_run "$tag" "$(rl_shape "$p")" "$n" "$d" "$@"
}
rl_ecfield() { eval "echo \${$1:-}"; }
rl_digits_tail() {  # LOG -> last digits printed by the process that holds the end of the result (empty if none)
    local f=$1; [ -f "$f" ] || f=$f.gz; [ -f "$f" ] || return 0
    { case $f in *.gz) zcat "$f";; *) cat "$f";; esac; } | sed -E 's/^ *[0-9]+: //' | grep -a -E '^digits: [0-9]*\.\.\.[0-9]+' | sed -E 's/.*\.\.\.//' | sort -u | paste -sd,
}

# ---- planner ------------------------------------------------------------------------------------------------------------
rl_fit() {          # PATH NODES DIGITS [words] -> rc 0 fits / 1 refused / 2 failed; output in $RUN/raw/fit_*.log; FIT_LINE
    local p=$1 n=$2 d=$3; shift 3
    local bin lf env=("$@")
    bin=$(rl_bin "$p"); lf=$RUN/raw/fit_${p}_${n}_$d.log
    [ "$p" = 2 ] && env=(ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 "${env[@]}")
    local rc
    MNRUN_MODULES=$(rl_mods "$p") MNRUN_UNLOAD=$(rl_unload) rl_tmo 300 python3 -I "$R/tools/plan_values.py" --fit --ecalc "$bin" --env "${env[*]}" "$d" "$n" > "$lf" 2>&1 < /dev/null
    rc=$?
    FIT_LINE=$(grep -a -E 'plan fit +(FITS|REFUSED)|plan REFUSED' "$lf" | tail -1 | sed 's/^ *//' | cut -c1-200)
    [ $rc -le 2 ] && return $rc; return 2
}
rl_maxfit() {       # PATH NODES HIGH [words] -> the largest digit count (3 significant figures) that fits, by bisection over [1e8 x nodes, HIGH]
    local p=$1 n=$2 hi=$3; shift 3
    local lo mid i best=0
    lo=$(awk -v n="$n" 'BEGIN { printf "%.0f", 1e8 * n }')
    rl_fit "$p" "$n" "$lo" "$@" || { echo 0; return; }
    best=$lo
    for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
        mid=$(awk -v a="$best" -v b="$hi" 'BEGIN { m = (a + b) / 2; printf "%.3g", m; }'); mid=$(rl_num "$mid")
        [ "$mid" -le "$best" ] && break
        if rl_fit "$p" "$n" "$mid" "$@"; then best=$mid; else hi=$mid; fi
    done
    echo "$best"
}
rl_model() {        # NODES DIGITS -> the modelled no-write wall in seconds, or NA
    local n=$1 d=$2 fab=() out
    [ "$REMOTE_SITE" = aac7 ] && fab=(--fabric aac7)
    out=$(rl_tmo 120 python3 -I "$R/ecalc/mn_model.py" estimate --g "$n" --D "$(awk -v d="$d" -v n="$n" 'BEGIN { printf "%.6g", d / n }')" "${fab[@]}" 2>/dev/null \
        | awk -v n="$n" '$1 == n && $4 == "|" { print $5; exit }')
    echo "${out:-NA}"
}

# ---- statistics -----------------------------------------------------------------------------------------------------------
rl_mean() { awk 'BEGIN { for (i = 1; i < ARGC; i++) { s += ARGV[i]; n++ } if (n) printf "%.3f", s / n; else printf "NA" }' "$@"; }
rl_ci() {           # d1 d2 ... (paired differences) -> "mean lo hi" 95 % t interval, or "mean NA NA" for n < 2
    awk 'BEGIN { t[1]=12.706; t[2]=4.303; t[3]=3.182; t[4]=2.776; t[5]=2.571; t[6]=2.447; t[7]=2.365; t[8]=2.306; t[9]=2.262; t[10]=2.228
        n = ARGC - 1; for (i = 1; i < ARGC; i++) s += ARGV[i]; m = s / n
        if (n < 2) { printf "%.3f NA NA", m; exit }
        for (i = 1; i < ARGC; i++) q += (ARGV[i] - m) ^ 2; sd = sqrt(q / (n - 1)); tt = (n - 1 <= 10) ? t[n - 1] : 1.96; h = tt * sd / sqrt(n)
        printf "%.3f %.3f %.3f", m, m - h, m + h }' "$@"
}

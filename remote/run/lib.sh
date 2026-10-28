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
. "$R/ecalc/gres.sh"     # MNRUN_GRES=auto|on|off: GPU options only where the nodes list a gpu GRES (FACTS.md Known problems)
rl_gres_opts() { mn_gres_on "${2:-}" "${1:-}" && echo --gpus-per-node=4; return 0; }   # [PARTITION] [NODE] -> the sbatch GPU option, or nothing
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
rl_tilde() {        # stdin -> stdout with the value of $HOME written as ~ (pasted paths stay usable as commands, the home path is not shown)
    local h=${HOME:-}; if [ -z "$h" ] || [ "$h" = / ]; then cat; return; fi
    h=$(printf '%s' "$h" | sed 's/[][\.*^$#+?(){}|]/\\&/g'); sed -E "s#$h(/|\$|[^A-Za-z0-9_.-])#~\\1#g"
}

rl_cat() { local x; for x in "$@"; do [ -f "$x" ] || continue; case $x in *.gz) zcat "$x";; *) cat "$x";; esac; done 2>/dev/null; }
rl_errs() {     # FILES... -> up to 15 distinct error-looking lines (rank labels / times stripped, digits folded for the dedupe)
    local f=("$@"); [ ${#f[@]} -gt 0 ] || f=("$RUN"/raw/*.log "$RUN"/*.log)
    rl_cat "${f[@]}" \
      | grep -a -i -E 'error|fail|abort|segv|refused|timeout|fatal|no hipcc|undefined reference|not found|\*\*\*' \
      | grep -a -v -i -E 'fail(ed)?=0|errors?[:=] *0|without error|VERIFY OK|no error|0 fail|^ *([0-9]+: )?config: |SEGV_TRACE' \
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
            local e; e=$(rl_errs ${RL_ERRFILES[@]+"${RL_ERRFILES[@]}"})
            [ -n "$e" ] && { echo "errors:"; echo "$e" | sed 's/^/  /'; }
        fi
        echo "next=$next"
        echo "=== END $STAGE ==="
    } | rl_tilde > "$out"
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
rl_posint() { case ${1:-} in ''|*[!0-9]*) return 1;; esac; [ "$1" -gt 0 ] 2>/dev/null; }    # positive integer (digits only, no 1e3 / 1.5)
rl_tlim_secs() {    # SLURM TIME -> seconds: MM | MM:SS | HH:MM:SS | D-HH | D-HH:MM | D-HH:MM:SS; rc 1 if not one of these
    case ${1:-} in ''|*[!0-9:-]*) return 1;; esac
    echo "$1" | awk '{ d = 0; r = $0; if (index(r, "-")) { split(r, a, "-"); if (a[1] == "" || a[2] == "" || index(a[2], "-")) exit 1; d = a[1]; r = a[2]; dd = 1 } else if (r ~ /-/) exit 1
        n = split(r, f, ":"); if (n > 3) exit 1; for (i = 1; i <= n; i++) if (f[i] == "") exit 1
        if (dd) { s = d * 86400 + f[1] * 3600 + (n > 1 ? f[2] * 60 : 0) + (n > 2 ? f[3] : 0) }
        else if (n == 1) s = f[1] * 60; else if (n == 2) s = f[1] * 60 + f[2]; else s = f[1] * 3600 + f[2] * 60 + f[3]
        print s }'
}
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
    export MN_T_CHUNK_MB=${MN_T_CHUNK_MB:-2048} ECALC_NP=${ECALC_NP:-auto} MNRUN_CPUS_PER_TASK=${MNRUN_CPUS_PER_TASK-auto}
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
rl_job_state() {    # JOB -> state (PENDING, RUNNING, ...) or empty = gone; an empty squeue is retried 3 x (5 s apart) and checked with scontrol before "gone"
    local i s
    for i in 1 2 3; do
        s=$(rl_tmo 30 squeue -h -j "$1" -o %T 2>/dev/null | head -1); [ -n "$s" ] && { echo "$s"; return 0; }
        s=$(rl_tmo 30 scontrol show job -o "$1" 2>/dev/null | grep -a -o 'JobState=[A-Z_]*' | head -1 | cut -d= -f2); [ -n "$s" ] && { echo "$s"; return 0; }
        [ $i -lt 3 ] && sleep 5
    done
    return 0
}
rl_job_alive() { case $(rl_job_state "$1") in PENDING|RUNNING|CONFIGURING|COMPLETING|SUSPENDED|RESIZING) return 0;; *) return 1;; esac; }
rl_job_mine() { [ "$(rl_tmo 30 squeue -h -j "$1" -o %u 2>/dev/null | head -1)" = "$(rl_user)" ]; }
rl_hosts_all() { local n; n=$(rl_tmo 30 squeue -h -j "$1" -o %N 2>/dev/null | head -1); [ -n "$n" ] && rl_tmo 30 scontrol show hostnames "$n" 2>/dev/null; }
rl_exclude_list() { # comma list: --exclude / REMOTE_EXCLUDE, target.conf's MNRUN_EXCLUDE_HOSTS, aac7's $HOME/aac7q/exclude* files (REMOTE_EXCLUDE_FILES=0: not those, for items on nodes the lists name)
    local l=${EXCLUDE:-}${REMOTE_EXCLUDE:+,$REMOTE_EXCLUDE}${MNRUN_EXCLUDE_HOSTS:+,$MNRUN_EXCLUDE_HOSTS}
    if [ "$REMOTE_SITE" = aac7 ] && [ "${REMOTE_EXCLUDE_FILES:-1}" != 0 ]; then local f; for f in "$HOME"/aac7q/exclude "$HOME"/aac7q/exclude_slow; do [ -f "$f" ] && l="$l,$(sed 's/#.*//' "$f" | awk '{ print $1 }' | paste -sd,)"; done; fi
    # split at commas/blanks outside [...] so Slurm ranges like nid[001-003,007] stay whole
    echo "$l" | tr ' ' ',' | awk '{ d = 0; o = ""; for (i = 1; i <= length($0); i++) { c = substr($0, i, 1); if (c == "[") d++; else if (c == "]") d--
        if (c == "," && d <= 0) { print o; o = "" } else o = o c } print o }' | grep -E '^[][A-Za-z0-9._,-]+$' | sort -u | paste -sd,
}
rl_usable_hosts() { # JOB -> the hosts of the allocation, one per line, excluded hosts left out
    local ex all; ex=$(rl_exclude_list); all=$(rl_hosts_all "$1") || return 0
    [ -n "$all" ] || return 0
    if [ -n "$ex" ]; then all=$(echo "$all" | grep -Fvxf <(rl_tmo 30 scontrol show hostnames "$ex" 2>/dev/null || echo "$ex" | tr ',' '\n') || true); fi
    echo "$all"
}
rl_pick_hosts() {   # JOB N [SKIP] -> comma list of N usable hosts of the allocation (excluded hosts left out); rc 1 if fewer
    local job=$1 n=$2 skip=${3:-0} l
    l=$(rl_usable_hosts "$job" | tail -n +$((skip + 1)) | head -n "$n" | paste -sd,)
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
    HOSTS_ALL=$(rl_usable_hosts "$JOB")    # one Slurm query, then truncate
    [ -n "$HOSTS_ALL" ] || rl_die "no usable node in job $JOB"
    NHOSTS=$(echo "$HOSTS_ALL" | grep -c .); [ "$NHOSTS" -gt "$maxn" ] && NHOSTS=$maxn
    HOSTS_ALL=$(echo "$HOSTS_ALL" | head -n "$NHOSTS" | paste -sd,)
    rl_ecsetup "$JOB" "$HOSTS_ALL"
    rl_kv job "$JOB"; rl_kv nodes_usable "$NHOSTS"
}
rl_ecrun() {        # TAG PATH NODES DIGITS [VAR=VALUE ...] -> E_* (see _ecalc.sh); rc = ecalc_run's
    local tag=$1 p=$2 n=$3 d=$4; shift 4
    rl_nou ecalc_run "$tag" "$(rl_shape "$p")" "$n" "$d" "$@"
}
rl_ecfield() { eval "echo \${$1:-}"; }
rl_digits_tail() {  # LOG -> last digits printed by the process that holds the END of the result (one string; empty if none)
    # lines: "digits: 2718...5103..." (one process) | "mn: pe|node N: digits: HEAD...TAIL (this node's range)"; ranks without a TAIL are skipped,
    # of several with a tail the highest pe/node index wins (the last range ends the result), ties: the last line
    local f=$1; [ -f "$f" ] || f=$f.gz; [ -f "$f" ] || return 0
    { case $f in *.gz) zcat "$f";; *) cat "$f";; esac; } | sed -E 's/^ *[0-9]+: //' | grep -a -E '^(mn: (pe|node) [0-9]+: )?digits: [0-9]*\.\.\.[0-9]+' \
      | awk 'BEGIN { br = -2 } { r = -1; if (match($0, /^mn: (pe|node) [0-9]+: /)) { q = substr($0, 1, RLENGTH); gsub(/[^0-9]/, "", q); r = q + 0 }
               t = $0; sub(/^[^.]*\.\.\./, "", t); sub(/[^0-9].*/, "", t)
               if (r >= br) { br = r; bt = t } } END { if (bt != "") print bt }'
}

rl_runfacts() {     # LOG [PATH] -> RF_* from what ecalc printed (empty = not printed); one pass over the log; the summary lines are made by rl_factlines
    local f=$1 pth=${2:-}; RF_PES= RF_PERNODE= RF_NODES= RF_BG= RF_TEAMBS= RF_TEAMRES= RF_I=; RF_RN= RF_RS= RF_DN= RF_DS= RF_P24LINES= RF_P24PROD= RF_P24FL= RF_PLANAPU=
    [ -f "$f" ] || f=$f.gz; [ -f "$f" ] || return 0
    local tmp; tmp=$(mktemp "${TMPDIR:-/tmp}/rlf.XXXXXX")
    { case $f in *.gz) zcat "$f";; *) cat "$f";; esac; } | sed -E 's/^ *[0-9]+: //' | grep -a -E '^(vpe: |omp team: |tl +[a-z]+ [0-9]+: init = |recip\(mn\) |divmod\(mn|P24_CHECK: [0-9]|memory fit: [0-9]|mn: (pe|node) [0-9]+ of [0-9]+)' > "$tmp"
    local l
    l=$(grep -a -m1 '^vpe: ' "$tmp" | sed -n 's/^vpe: \([0-9]*\) PEs = \([0-9]*\) per node x \([0-9]*\) node.*background team \([0-9]*\) threads per PE.*/\1 \2 \3 \4/p')
    if [ -n "$l" ]; then read -r RF_PES RF_PERNODE RF_NODES RF_BG <<< "$l"
    else l=$(grep -a -m1 -E '^mn: (pe|node) [0-9]+ of [0-9]+' "$tmp" | sed -E 's/^mn: (pe|node) [0-9]+ of ([0-9]+)( \(node [0-9]+ of ([0-9]+))?.*/\2 \4/')
        if [ -n "$l" ]; then set -- $l; RF_PES=$1; RF_NODES=${2:-$1}; RF_PERNODE=$((RF_PES / RF_NODES)); fi; fi
    l=$(grep -a -m1 '^omp team: ' "$tmp" | sed -n 's/.*bs team \([0-9]*\), residue team \([0-9]*\).*/\1 \2/p'); [ -n "$l" ] && read -r RF_TEAMBS RF_TEAMRES <<< "$l"
    RF_I=$(grep -a -m1 -E '^tl +[a-z]+ [0-9]+: init = ' "$tmp" | sed -n 's/.*init = \([0-9.]*\) s: plan\/fit\/pool rule before the transport \([0-9.]*\) + mn_init \([0-9.]*\) + rns_init \([0-9.]*\) + mn_selftest \([0-9.]*\) + region pools \([0-9.]*\) + other \([0-9.]*\).*/\1 \2 \3 \4 \5 \6 \7/p')
    l=$(grep -a -m1 '^recip(mn) ' "$tmp" | sed -n 's/.*shifts \([0-9]*\)\/\([0-9.]*\).*/\1 \2/p'); [ -n "$l" ] && read -r RF_RN RF_RS <<< "$l"
    l=$(grep -a -m1 '^divmod(mn' "$tmp" | sed -n 's/.*shifts \([0-9]*\)\/\([0-9.]*\) s.*/\1 \2/p'); [ -n "$l" ] && read -r RF_DN RF_DS <<< "$l"
    l=$(grep -a '^P24_CHECK: ' "$tmp" | awk '{ n++; if ($2 + 0 > m) m = $2 + 0; for (i = 1; i < NF; i++) if ($(i + 1) == "flags") fl += $i } END { if (n) print n, m, fl + 0 }'); [ -n "$l" ] && read -r RF_P24LINES RF_P24PROD RF_P24FL <<< "$l"
    RF_PLANAPU=$(grep -a -m1 '^memory fit: ' "$tmp" | sed -n 's/.*per node = \([0-9.]*\) GB per APU.*/\1/p')
    rm -f "$tmp"
    if [ -z "$RF_PES" ] && [ "$pth" = 1 ]; then RF_NOTE=" (layout D: one process per node, from the launch line)"; else RF_NOTE=; fi
}
rl_factlines() {    # NODES -> the KEY=VALUE summary lines of the last rl_runfacts (NA = not printed)
    local n=$1 p=${RF_PES:-}; [ -z "$p" ] && [ -n "${RF_NOTE:-}" ] && { p=$n; RF_PERNODE=1; RF_NODES=$n; }
    rl_kv layout "pes=${p:-NA} per_node=${RF_PERNODE:-NA} nodes=${RF_NODES:-NA} bg_team=${RF_BG:-NA} bs_team=${RF_TEAMBS:-NA} residue_team=${RF_TEAMRES:-NA}${RF_NOTE:-}"
    if [ -n "$RF_I" ]; then set -- $RF_I; rl_kv init_split "init=$1 plan_fit=$2 mn_init=$3 rns_init=$4 selftest=$5 region_pools=$6 other=$7 (s, rank 0, ECALC_INIT_TL=1)"; else rl_kv init_split "NA (set ECALC_INIT_TL=1)"; fi
    rl_kv shifts "recip=${RF_RN:-NA}/${RF_RS:-NA} divmod=${RF_DN:-NA}/${RF_DS:-NA} (count/seconds, rank 0)"
    if [ -n "$RF_P24LINES" ]; then rl_kv p24_check "$RF_P24LINES line(s), up to $RF_P24PROD P24 products per PE, $RF_P24FL flags in total"; else rl_kv p24_check "NA (no P24_CHECK line printed: P24_CHECK=1 is off, or no P24 product formed, as on one node)"; fi
}

# ---- planner ------------------------------------------------------------------------------------------------------------
rl_fit() {          # PATH NODES DIGITS [words] -> rc 0 fits / 1 refused / 2 failed; output in $RUN/raw/fit_*.log; FIT_LINE
    local p=$1 n=$2 d=$3; shift 3
    local bin lf env=("$@")
    bin=$(rl_bin "$p"); lf=$RUN/raw/fit_${p}_${n}_$d.log
    [ "$p" = 2 ] && env=(ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 ${env[@]+"${env[@]}"})
    local rc via=login
    MNRUN_MODULES=$(rl_mods "$p") MNRUN_UNLOAD=$(rl_unload) rl_tmo 300 python3 -I "$R/tools/plan_values.py" --fit --ecalc "$bin" --env "${env[*]-}" "$d" "$n" > "$lf" 2>&1 < /dev/null
    rc=$?
    FIT_LINE=$(grep -a -E 'plan fit +(FITS|REFUSED)|plan REFUSED' "$lf" | tail -1 | sed 's/^ *//' | cut -c1-200)
    [ $rc = 1 ] && [ -z "$FIT_LINE" ] && grep -a -q -E 'Traceback|Error' "$lf" && rc=2    # a crash is not a refusal
    if [ $rc -ge 2 ] && [ -n "${JOB:-}" ] && [ "$(rl_job_state "$JOB")" = RUNNING ]; then   # planner could not run here: once more on a node of the allocation
        rl_log "fit p$p n$n d$d: planner failed on the login node (rc $rc); retry through srun on job $JOB"
        mv "$lf" "$lf.login"; via=srun
        MNRUN_MODULES=$(rl_mods "$p") MNRUN_UNLOAD=$(rl_unload) rl_tmo 330 srun --overlap --jobid "$JOB" -N1 -n1 --export=ALL python3 -I "$R/tools/plan_values.py" --fit --ecalc "$bin" --env "${env[*]-}" "$d" "$n" > "$lf" 2>&1 < /dev/null
        rc=$?
        FIT_LINE=$(grep -a -E 'plan fit +(FITS|REFUSED)|plan REFUSED' "$lf" | tail -1 | sed 's/^ *//' | cut -c1-200)
        [ $rc = 1 ] && [ -z "$FIT_LINE" ] && rc=2    # srun's own failure looks like rc 1
    fi
    rl_log "fit p$p n$n d$d: planner via $via, rc $rc"
    FIT_VIA=$via
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
    out=$(rl_tmo 120 python3 -I "$R/ecalc/mn_model.py" estimate --g "$n" --D "$(awk -v d="$d" -v n="$n" 'BEGIN { printf "%.6g", d / n }')" ${fab[@]+"${fab[@]}"} 2>/dev/null \
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

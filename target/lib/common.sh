# target/lib/common.sh - shared shell functions of the target folder (contract: target/FORMATS.md §3).
# Source it; it defines functions only and sets nothing until tgt_init is called.

tgt_init() {
    TGT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
    REPO=$(cd "$TGT_ROOT/.." && pwd)
    TGT_RES=$TGT_ROOT/results
    TGT_SHA=$(git -C "$REPO" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)
    export TGT_ROOT REPO TGT_RES TGT_SHA
}

# load config/target.conf (KEY=VALUE lines, # comments) into the environment; TGT_STRICT=1 refuses __FILL__ keys
tgt_conf() {
    local f=${TCONF:-$TGT_ROOT/config/target.conf} k v bad=
    [ -f "$f" ] || { tgt_log "no $f (run probe/gen_conf.py first); using the environment as is"; return 0; }
    export TCONF=$f
    while IFS= read -r line; do
        line=${line%%#*}; line=${line%"${line##*[![:space:]]}"}
        [[ $line =~ ^[A-Za-z_][A-Za-z0-9_]*= ]] || continue
        k=${line%%=*}; v=${line#*=}
        [[ $v =~ ^\"(.*)\"$ || $v =~ ^\'(.*)\'$ ]] && v=${BASH_REMATCH[1]}   # KEY="a b" as a shell would read it (gen_conf.py quotes lists)
        if [ "$v" = __FILL__ ]; then bad="$bad $k"; continue; fi
        [ -n "${!k+x}" ] || export "$k=$v"          # the caller's environment wins
    done < "$f"
    if [ -n "$bad" ]; then
        tgt_log "target.conf keys still __FILL__:$bad"
        [ "${TGT_STRICT:-0}" = 1 ] && return 1
    fi
    return 0
}

tgt_log() {
    local l; l="$(date '+%F %T %Z') $*"
    echo "$l" >&2
    [ -n "${OUT-}" ] && [ -d "${OUT-}" ] && echo "$l" >> "$OUT/log.txt"
    return 0
}

tgt_sum_init() {    # DIR ID
    TGT_SUM_DIR=$1; mkdir -p "$1/raw"
    TGT_SUM_T0=$(date +%s)
    : > "$1/summary.txt"
    tgt_kv id "$2"
    tgt_kv start "$(date -Iseconds)"
    tgt_kv host "$(hostname -s)"
    tgt_kv commit "${TGT_SHA:-unknown}"
}

tgt_kv() {          # KEY VALUE...
    local k=$1; shift
    local v="$*"; v=${v//$'\n'/ / }
    echo "$k=$v" >> "$TGT_SUM_DIR/summary.txt"
}

tgt_sum_end() {     # STATUS RC
    tgt_kv end "$(date -Iseconds)"
    tgt_kv wall_s $(( $(date +%s) - TGT_SUM_T0 ))
    tgt_kv status "$1"
    tgt_kv rc "$2"
    echo "$2" > "$TGT_SUM_DIR/DONE"
}

tgt_claim() {       # DIR: 0 = claimed by us, 1 = held by someone else
    # the atomic step is an O_EXCL create (bash noclobber) of DIR/claim.lock: uutils' mkdir (some Linux distributions) is not atomic
    # (two processes both got rc 0 for the same new directory, measured 2026-10-25); claim.d/who is written after it
    mkdir -p "$1"
    ( set -o noclobber; : > "$1/claim.lock" ) 2>/dev/null || return 1
    mkdir -p "$1/claim.d"
    echo "$(hostname -s) pid $$ $(date -Iseconds) ${TGT_WORKER:-}" > "$1/claim.d/who"
}

tgt_timeout() {     # MIN CMD...: run CMD in its own process group, kill the group after MIN minutes (rc 124)
    local min=$1; shift
    # setsid -w: a bare setsid forks when its caller leads a process group (timeout does) and returns at once with rc 0
    setsid -w timeout --kill-after=30 "$(awk -v m="$min" 'BEGIN { printf "%d", m * 60 }')" "$@"
}

tgt_hosts() {       # JOBID: hosts of the allocation, one per line
    scontrol show hostnames "$(squeue -h -j "$1" -o %N)"
}

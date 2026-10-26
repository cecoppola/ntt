#!/bin/bash
# items/_ecalc.sh - shared ecalc run helper of the bench items.  SOURCE it (it defines functions and a few variables, runs nothing):
#     . "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
# It sources lib/common.sh, calls tgt_init and tgt_conf, and needs the item environment of FORMATS.md §5 (OUT, JOBID, HOSTS, ECALC1,
# ECALC2, MNRUN_DIR1/2, MNRUN_MODULES_P1/P2).  Interface (stable; B3 and the ladder code against it):
#
#   ecalc_digits X                 prints the integer digit count X x TGT_DIGITS_SCALE (default 1; TGT_SITE=aac7 defaults the scale to 0.1;
#                                  never below 1e8 when the scale is < 1).  Items write ecalc_digits 1e10, never a bare number, so one
#                                  env var shrinks every item on a smaller machine.
#   ecalc_run TAG SHAPE NODES DIGITS [VAR=VALUE ...]
#                                  run ecalc once.  SHAPE: D = ECALC1, layout D (one process per node, 4 APU threads); P = ECALC1 +
#                                  ECALC_PE_PER_GPU=1 (4 processes per node); C = ECALC2 + ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 (path 2).
#                                  Hosts: the first NODES of $HOSTS_SUB (comma list) when set, else of $HOSTS (the item's own).  The launch is
#                                  the build tree's mnrun.sh (SLURM_JOB_ID=$JOBID, MNRUN_NODELIST=hosts, the build's module line), so the FIT
#                                  check, pool sizing and FI_* defaults are mnrun's.  VAR=VALUE words are settings: MNRUN_* words go to the
#                                  launcher's environment (e.g. MNRUN_LABEL=1), all others become env words of the command line (after the
#                                  shape words, so they win).  target.conf settings are already in the environment (tgt_conf); ECALC_VERBOSE=2
#                                  MEM_REPORT_DEVS=1 ECALC_SEGV_TRACE=1 ECALC_MEM_GUARD_GB=6 are added unless set.  No digit file unless
#                                  WRITE=1 (then the parts go to a scratch dir on the shared file system, are hashed with tools/unpack_digits
#                                  | sha1sum (16 hex, the aac7 convention) and removed; KEEP=1 keeps them).  Per-call env: TMO=<minutes>
#                                  (default TGT_ITEM_MIN, the item's est_min), WRITE, KEEP.  Log: $OUT/raw/TAG.log (gzipped when > 2 MB).
#                                  Returns ecalc's rc (the launcher's rc), 99 when the binary of that path is absent (E_SKIP=1, nothing run),
#                                  98 when the item has fewer than NODES hosts.  Sets (and writes $OUT/raw/TAG.res for ecalc_load):
#       E_RC E_SKIP(0/1) E_VERIFY(OK|FAIL|NONE: OK needs rc 0, a VERIFY OK on every PE and none FAILED) E_REFUSED(0/1: mnrun's FIT refused)
#       E_TOTAL E_INIT E_BS E_DM E_RECIP E_10DP E_DC E_T1 E_T2 E_OTHER E_PHASES    seconds; the maximum over all PEs that printed it (the
#                                  critical path); empty when not printed
#       E_WALL_S                   the launch's own wall time (srun start to end, includes the module load and the planner call)
#       E_DEVPEAK_GB               node device peak, GB (max per-process 'device' column of the memory table x processes per node)
#       E_DEVPEAK_APU_GB           the largest per-APU in-use figure of the table
#       E_HOSTPEAK_GB              node host peak estimate, GB (max per-process VmHWM x processes per node);  E_HOSTHWM_PROC_GB per process
#       E_HASH                     digit hash (WRITE=1 only, else empty);  E_HASH_REF the known hash of that digit count (1e9, 1e10, 1e11)
#                                  or empty;  E_HASH_MATCH = yes|no|-;  E_UNPACK_RC the residue check's rc
#       E_CONFIG_HASH E_NPES E_NODES E_SHAPE E_DIGITS E_HOSTS E_FIT (the 'FITS' line of the planner) E_LOG (path of the log)
#   ecalc_load TAG                 re-read $OUT/raw/TAG.res (the results of an ecalc_run, also one run in the background)
#   ecalc_kv TAG                   write the metrics of the (loaded) run into summary.txt: m.TAG.total_s/init_s/bs_s/dm_s/recip_s/dc_s/wall_s
#                                  (value + unit), m.TAG.devpeak_gb/devpeak_apu_gb/hostpeak_gb/hosthwm_proc_gb, m.TAG.rc, and e.TAG.shape/
#                                  digits/nodes/hosts/verify/hash/hash_match/config_hash (TAG lower-cased)
#   ecalc_plan NODES DIGITS SHAPE [VAR=VALUE ...]
#                                  the planner's memory FIT on this host (MN_PLAN_ONLY, no device touched; the build's modules loaded).  Return 0 =
#                                  fits, 1 = does not fit or plan REFUSED, 2 = planner printed nothing.  Sets P_FIT (yes|no|NA) P_DEV_GB P_DEV_LIMIT_GB
#                                  P_DEV_MARGIN_GB P_NODE_GB P_NODE_LIMIT_GB P_NODE_MARGIN_GB P_APU_GB P_WRAP P_POOL_MB P_CHECK (OK|REFUSED|NONE)
#                                  P_LINE (the verdict line); the log is $OUT/raw/plan_<digits>_<nodes>_<shape>.log.  Without ECALC1/ECALC2 it returns 2.
#   ecalc_bg TAG SHAPE NODES DIGITS [VAR=VALUE ...]   ecalc_run in the background (a subshell); give each arm its own hosts with a prefix
#   ecalc_wait                     assignment: `HOSTS_SUB=$(ecalc_hosts 0 2) ecalc_bg a D 2 ...; HOSTS_SUB=$(ecalc_hosts 2 2) ecalc_bg b ...; ecalc_wait`.
#                                  ecalc_wait waits for all of them and returns how many returned nonzero; then ecalc_load TAG for each.
#   ecalc_hosts START COUNT        COUNT hosts of $HOSTS from offset START, comma list (disjoint groups for concurrent arms)
#   ecalc_srun TAG PATHNO NODES CMD...   run CMD on NODES hosts (first of HOSTS_SUB/HOSTS), one task per node unless PPN=<k> (k tasks per node),
#                                  4 GPUs per node (GPUS=<n>), modules of path PATHNO (1|2; 0 = none) loaded in each task, --overlap, log
#                                  $OUT/raw/TAG.log, returns the rc, sets E_SRUN_WALL_S.  CMD is a path (use ecalc_tool for build-tree programs).
#   ecalc_tool PATHNO REL          absolute path of a build-tree file of path PATHNO, e.g. `ecalc_tool 1 tests/t_edge` (tree/ecalc/tests/t_edge), `ecalc_tool 2 tools/unpack_digits` (tree/tools/..)
#   ecalc_have PATHNO              0 when the binary of that path exists (prints nothing)
#   ecalc_stat "A1 A2 ..." "B1 B2 ..."   prints `meanA meanB diff pct` (B minus A, % of A) for two lists of numbers
#   ecalc_mean / ecalc_median / ecalc_max  of the numbers on the command line
#   ecalc_grab FILE REGEX         the first line of FILE (rank labels stripped, gz ok) matching REGEX
#   ecalc_refresh [SECONDS]        path-2 items list only bld01 as a dependency (a failed dep blocks for good); call this first: when bld02 is claimed
#                                  but not DONE it waits up to SECONDS (default 900), then re-reads ECALC2 / MNRUN_DIR2 / MNRUN_MODULES_P2 from
#                                  results/bench/bld02/summary.txt.  Returns 0 when path 2 is usable, 1 when not (the item then skips its arms).
#   ecalc_ok / ecalc_brief TAG     the loaded run is good (rc 0, VERIFY OK, no hash mismatch) / one phrase about it for q.* lines
#   ecalc_xt TAG PATHNO NODES "SECTIONS" [VAR=VALUE...]   t_xshmem on NODES hosts x 4 PEs (heap and lanes set; see its comment); sets X_RC X_RESULT X_SKIP X_WALL_S
#   ecalc_xt_rows TAG KIND         the ROW lines of one t_xshmem table (lat|stream|a2a)
#   ecalc_q TQnn TEXT...           q.TQnn=TEXT (one line)       ecalc_end STATUS    write status=STATUS and exit 0
# Replay (parser tests without a cluster): ECALC_REPLAY=<saved ecalc log> makes ecalc_run copy that log instead of launching (rc 0).
# TGT_NO_NIC=1: the allocation has no usable NICs (aac7 rehearsal holds): ecalc_run of shape P/C or on > 1 node and ecalc_xt are skipped (rc 99, E_SKIP=1).
# Site: TGT_SITE=target (default) | aac7 (the sister system: smaller digit counts, items skip what needs the target's hardware).

[ -n "${_ECALC_SH_LOADED:-}" ] && return 0
_ECALC_SH_LOADED=1
_ec_here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$_ec_here/../../lib/common.sh"
tgt_init
tgt_conf 2>/dev/null
[ -n "${OUT:-}" ] || { echo "_ecalc.sh: OUT is not set (the item runner exports it; no \$PWD default)" >&2; return 1 2>/dev/null || exit 1; }
mkdir -p "$OUT/raw"
: "${TGT_SUM_DIR:=$OUT}"; export TGT_SUM_DIR
TGT_SITE=${TGT_SITE:-target}
if [ -z "${TGT_DIGITS_SCALE:-}" ]; then [ "$TGT_SITE" = aac7 ] && TGT_DIGITS_SCALE=0.1 || TGT_DIGITS_SCALE=1; fi
export TGT_SITE TGT_DIGITS_SCALE
EC_BG_PIDS=()
# TGT_SHARE: digits per node of the first target campaign (user decision Q120, 2026-10-25: reduce the digits until every code path runs
# at 576 nodes; raise them later).  Planner check (target limits): 8e9 per node fits D, P and C at 1, 8, 64 and 576 nodes; the old share
# 6.632e10 is refused for P and C from 8 nodes on.  Items write `dig $TGT_SHARE [NODES]` / `ecalc_digits $TGT_SHARE`, so TGT_DIGITS_SCALE
# (TGT_SITE=aac7 scale 0.1) still applies on top of it.  The later goal stays 3.82e13 in total (cor03 checks it too).
TGT_SHARE=${TGT_SHARE:-8000000000}; export TGT_SHARE
# TGT_SHARE_CAP: the plane cap of runs at exactly the campaign share per node (user decision Q122, 2026-10-25: ECALC_PLANE_CAP=2^30
# on the 8e9 campaign's launch line; aac7 1 n 8e9: total -2.64 s [-2.98, -2.30], compute unchanged, digits identical, m).  A caller's
# ECALC_PLANE_CAP word or environment wins; TGT_SHARE_CAP= (empty) turns it off.  Not applied at other sizes (the cap refuses ~9e9+ per node).
TGT_SHARE_CAP=${TGT_SHARE_CAP-2^30}; export TGT_SHARE_CAP

# known digit hashes (unpack_digits -q | sha1sum | cut -c1-16, main, aac7, identical on both paths and every node count)
ecalc_ref_hash() { case "$1" in 1000000000) echo bf9f4b08758b40d9;; 10000000000) echo 3fc025473586ed22;; 100000000000) echo 578f5efb0ff2b9af;; esac; }

ecalc_digits() { awk -v x="$1" -v s="${TGT_DIGITS_SCALE:-1}" 'BEGIN { v = x * s; if (s < 1 && v < 1e8) v = 1e8; printf "%.0f\n", v }'; }
ecalc_mean() { awk 'BEGIN { for (i = 1; i < ARGC; i++) { s += ARGV[i]; n++ } if (n) printf "%.4g\n", s / n }' "$@"; }
ecalc_max() { awk 'BEGIN { for (i = 1; i < ARGC; i++) if (i == 1 || ARGV[i] + 0 > m) m = ARGV[i] + 0; if (ARGC > 1) printf "%.6g\n", m }' "$@"; }
ecalc_median() { printf '%s\n' "$@" | sort -g | awk '{ a[NR] = $1 } END { if (NR) printf "%.4g\n", NR % 2 ? a[(NR + 1) / 2] : (a[NR / 2] + a[NR / 2 + 1]) / 2 }'; }
ecalc_stat() { awk -v A="$1" -v B="$2" 'BEGIN { na = split(A, a, " "); nb = split(B, b, " "); for (i = 1; i <= na; i++) sa += a[i]; for (i = 1; i <= nb; i++) sb += b[i];
    if (na && nb) { ma = sa / na; mb = sb / nb; printf "%.3f %.3f %+.3f %+.2f\n", ma, mb, mb - ma, ma ? 100 * (mb - ma) / ma : 0 } }'; }

# the first line of a (possibly gzipped) log matching REGEX, rank label "<n>: " stripped
ecalc_grab() { local f=$1; [ -f "$f" ] || f=$f.gz; [ -f "$f" ] || return 0
    if [ "${f%.gz}" != "$f" ]; then zcat "$f"; else cat "$f"; fi | sed -E 's/^ *[0-9]+: //' | grep -a -m1 -E "$2"; }

ecalc_hosts() { local s=${1:-0} n=${2:-1}; echo "$HOSTS" | tr ',' '\n' | grep . | tail -n +$((s + 1)) | head -n "$n" | paste -sd, -; }

# path -> EC_BIN EC_DIR EC_MODS EC_UNLOAD
_ec_path() {
    local bld; EC_BIN= EC_DIR= EC_MODS= EC_UNLOAD=
    case $1 in
        1) EC_BIN=${ECALC1:-}; EC_DIR=${MNRUN_DIR1:-}; EC_MODS=${MNRUN_MODULES_P1:-}; bld=bld01;;
        2) EC_BIN=${ECALC2:-}; EC_DIR=${MNRUN_DIR2:-}; EC_MODS=${MNRUN_MODULES_P2:-}; bld=bld02;;
        0) EC_MODS=; EC_UNLOAD=; return 0;;
        *) return 1;;
    esac
    [ -n "$EC_DIR" ] || [ -z "$EC_BIN" ] || EC_DIR=$(dirname "$EC_BIN")
    [ -n "$EC_MODS" ] || EC_MODS=${MNRUN_MODULES:-}
    local sf=${TGT_RES_DIR:-$TGT_RES}/bench/$bld/summary.txt
    if [ -f "$sf" ] && grep -q '^e.MNRUN_UNLOAD=' "$sf"; then EC_UNLOAD=$(grep '^e.MNRUN_UNLOAD=' "$sf" | tail -1 | cut -d= -f2-); else EC_UNLOAD=${MNRUN_UNLOAD-}; fi
    [ -n "$EC_BIN" ] && [ -x "$EC_BIN" ]
}
ecalc_have() { _ec_path "$1"; }
ecalc_tool() {      # REL below the ecalc dir (tests/t_edge, mnrun.sh) or below the tree root (tools/unpack_digits, tools/initchk.py)
    _ec_path "$1" >/dev/null 2>&1; [ -n "$EC_DIR" ] || return 0
    if [ -e "$EC_DIR/$2" ]; then echo "$EC_DIR/$2"; else echo "$(cd "$EC_DIR/.." && pwd)/$2"; fi; }

# the per-task module preamble of the srun wrappers (as the kit's module_preamble and mnrun.sh)
_ec_pre() { printf '[ -n "%s" ] && module unload %s >/dev/null 2>&1; [ -n "%s" ] && module load %s >/dev/null 2>&1; ' "$EC_UNLOAD" "$EC_UNLOAD" "$EC_MODS" "$EC_MODS"; }

_ec_hosts_of() {      # NODES -> comma list of the first NODES hosts of HOSTS_SUB / HOSTS (empty when fewer)
    local src=${HOSTS_SUB:-$HOSTS} n=$1 l c
    l=$(echo "$src" | tr ',' '\n' | grep . | head -n "$n" | paste -sd, -); c=$(echo "$l" | tr ',' '\n' | grep -c .)
    [ "$c" -ge "$n" ] && echo "$l"
}

_ec_clean_log() { if [ -f "$1" ] && [ "$(stat -c %s "$1" 2>/dev/null || echo 0)" -gt 2097152 ]; then gzip -f "$1" && E_LOG=$1.gz; fi; }

ecalc_srun() {        # TAG PATHNO NODES CMD...
    local tag=$1 pn=$2 n=$3 h t0 rc lg ppn=${PPN:-1} gpus=${GPUS:-4} copt=(); shift 3
    lg=$OUT/raw/$tag.log
    _ec_path "$pn" >/dev/null 2>&1
    h=$(_ec_hosts_of "$n") || true; [ -n "$h" ] || { echo "ecalc_srun $tag: fewer than $n hosts" > "$lg"; return 98; }
    # cpus per task: CPT, else the node's CPUs / tasks per node (mnrun.sh's `auto`; srun may otherwise confine a task to 2 CPUs)
    local cpt=${CPT:-} nc
    if [ -z "$cpt" ]; then nc=${SLURM_CPUS_ON_NODE:-$(scontrol show node "${h%%,*}" -o 2>/dev/null | sed -n 's/.*CPUTot=\([0-9]*\).*/\1/p')}
        [ -n "$nc" ] && [ "$nc" -ge "$ppn" ] && cpt=$((nc / ppn)); fi
    [ -n "$cpt" ] && copt=(-c "$cpt")
    t0=$(date +%s)
    timeout -k 20 $(( ${TMO:-${TGT_ITEM_MIN:-10}} * 60 )) srun --jobid="$JOBID" --export=ALL -N "$n" -w "$h" --ntasks=$((n * ppn)) --ntasks-per-node="$ppn" --gpus-per-node="$gpus" "${copt[@]}" --overlap \
        bash -lc "$(_ec_pre)"'exec "$@"' _ "$@" > "$lg" 2>&1 < /dev/null
    rc=$?; E_SRUN_WALL_S=$(( $(date +%s) - t0 )); return $rc
}

# ---------------------------------------------------------------------------------------------------------------- parsing
_ec_parse() {         # LOGFILE SHAPE -> E_* (the run's own output)
    local f=$1 shape=$2 ppn=1 tmp; E_LOG=$f
    [ -f "$f" ] || f=$f.gz
    [ -f "$f" ] || return 1
    case $shape in P|C) ppn=4;; esac
    tmp=$(mktemp "${TMPDIR:-/tmp}/ecp.XXXXXX")
    if [ "${f%.gz}" != "$f" ]; then zcat "$f"; else cat "$f"; fi | sed -E 's/^ *[0-9]+: //' > "$tmp"
    local nm k v
    for nm in total init bs dm recip 10dP dc T1 T2 other phases; do
        v=$(awk -v nm="$nm" '$1=="RESULT" && $2=="ecalc" && $3==nm && $4=="s" { if (!n++ || $5 + 0 > m) m = $5 + 0 } END { if (n) printf "%.6g", m }' "$tmp")
        if [ -z "$v" ] && [ "$nm" != "10dP" ]; then   # the phase lines of older binaries / recip has no RESULT line
            v=$(awk -v nm="$nm" '$1==nm && $2 ~ /^[0-9.]+$/ && $3=="s" { if (!n++ || $2 + 0 > m) m = $2 + 0 } END { if (n) printf "%.6g", m }' "$tmp")
        fi
        case $nm in total) E_TOTAL=$v;; init) E_INIT=$v;; bs) E_BS=$v;; dm) E_DM=$v;; recip) E_RECIP=$v;; 10dP) E_10DP=$v;; dc) E_DC=$v;; T1) E_T1=$v;; T2) E_T2=$v;; other) E_OTHER=$v;; phases) E_PHASES=$v;; esac
    done
    # the total line also carries the per-process VmHWM
    E_HOSTHWM_PROC_GB=$(awk '$1=="RESULT" && $2=="ecalc" && $3=="vmhwm" { if (!n++ || $5 + 0 > m) m = $5 + 0 } END { if (n) printf "%.4g", m }' "$tmp")
    # memory table rows: mem[i] <phase> <device> <maxAPU> | planes ... | host staging regist X digits other HWM
    read -r E_DEVPEAK_PROC E_DEVPEAK_APU_GB hw < <(awk '/^mem\[[0-9]+\] [A-Za-z0-9_]+ +[0-9.]+ +[0-9.]+ +\|/ { d = $3 + 0; a = $4 + 0; h = $NF + 0; if (d > md) md = d; if (a > ma) ma = a; if (h > mh) mh = h }
        END { printf "%.4g %.4g %.4g\n", md, ma, mh }' "$tmp")
    [ -z "$E_HOSTHWM_PROC_GB" ] && [ "${hw:-0}" != 0 ] && E_HOSTHWM_PROC_GB=$hw
    E_DEVPEAK_GB=; [ -n "$E_DEVPEAK_PROC" ] && [ "$E_DEVPEAK_PROC" != 0 ] && E_DEVPEAK_GB=$(awk -v d="$E_DEVPEAK_PROC" -v p="$ppn" 'BEGIN { printf "%.4g", d * p }')
    [ "$E_DEVPEAK_APU_GB" = 0 ] && E_DEVPEAK_APU_GB=
    E_HOSTPEAK_GB=; [ -n "$E_HOSTHWM_PROC_GB" ] && E_HOSTPEAK_GB=$(awk -v d="$E_HOSTHWM_PROC_GB" -v p="$ppn" 'BEGIN { printf "%.4g", d * p }')
    # VERIFY
    local nok nfail nall
    nok=$(grep -a -c 'VERIFY OK' "$tmp"); nfail=$(grep -a -c 'VERIFY FAIL' "$tmp"); nall=$(grep -a -c 'mn: all [0-9]* \(PEs\|nodes\): VERIFY OK' "$tmp")
    if [ "$nfail" -gt 0 ]; then E_VERIFY=FAIL
    elif [ "$nall" -gt 0 ]; then E_VERIFY=OK                                   # multi-process run: rank 0's "all N PEs: VERIFY OK"
    elif [ "$nok" -gt 0 ] && [ "$E_NODES" = 1 ] && [ "$ppn" = 1 ]; then E_VERIFY=OK   # layout D on one node: the plain line
    else E_VERIFY=NONE; fi
    E_NPES=$(grep -a -c '^mn: pe [0-9]* of [0-9]*.*one flat communicator\|^pe [0-9]* .*affinity' "$tmp"); [ "$E_NPES" = 0 ] && E_NPES=
    E_CONFIG_HASH=$(grep -a -m1 -o '^config: hash=[0-9a-f]*' "$tmp" | cut -d= -f2)
    E_FIT=$(grep -a -m1 -E 'plan fit +(FITS|REFUSED)|^memory fit: (FITS|REFUSED)' "$tmp" | cut -c1-240)
    grep -aq 'REFUSED before launch' "$tmp" && E_REFUSED=1
    rm -f "$tmp"
}

_ec_reset() {
    E_RC= E_SKIP=0 E_VERIFY=NONE E_REFUSED=0 E_TOTAL= E_INIT= E_BS= E_DM= E_RECIP= E_10DP= E_DC= E_T1= E_T2= E_OTHER= E_PHASES= E_WALL_S=
    E_DEVPEAK_GB= E_DEVPEAK_APU_GB= E_HOSTPEAK_GB= E_HOSTHWM_PROC_GB= E_HASH= E_HASH_REF= E_HASH_MATCH=- E_UNPACK_RC= E_CONFIG_HASH= E_NPES=
    E_NODES= E_SHAPE= E_DIGITS= E_HOSTS= E_FIT= E_LOG=
}
_EC_VARS="E_RC E_SKIP E_VERIFY E_REFUSED E_TOTAL E_INIT E_BS E_DM E_RECIP E_10DP E_DC E_T1 E_T2 E_OTHER E_PHASES E_WALL_S E_DEVPEAK_GB E_DEVPEAK_APU_GB E_HOSTPEAK_GB E_HOSTHWM_PROC_GB E_HASH E_HASH_REF E_HASH_MATCH E_UNPACK_RC E_CONFIG_HASH E_NPES E_NODES E_SHAPE E_DIGITS E_HOSTS E_FIT E_LOG"
_ec_save() { local v; : > "$OUT/raw/$1.res"; for v in $_EC_VARS; do printf '%s=%q\n' "$v" "${!v}" >> "$OUT/raw/$1.res"; done; }
ecalc_load() { local t=${1,,}; t=${t//[^a-z0-9_]/_}; [ -f "$OUT/raw/$t.res" ] || return 1; . "$OUT/raw/$t.res"; }

# ---------------------------------------------------------------------------------------------------------------- run
ecalc_run() {         # TAG SHAPE NODES DIGITS [VAR=VALUE ...]
    local tag=${1,,} shape=$2 n=$3 d=$4 pn bld; shift 4
    tag=${tag//[^a-z0-9_]/_}
    local words=() lw=() shapew=() w h lg of= t0 rc cmd
    _ec_reset; E_SHAPE=$shape; E_NODES=$n; E_DIGITS=$(awk -v x="$d" 'BEGIN { printf "%.0f", x }'); d=$E_DIGITS
    lg=$OUT/raw/$tag.log; E_LOG=$lg
    case $shape in D) pn=1;; P) pn=1; shapew=(ECALC_PE_PER_GPU=1);; C) pn=2; shapew=(ECALC_PE_PER_GPU=1 XSHMEM_LANES=2);; *) echo "ecalc_run $tag: shape D|P|C, not $shape" > "$lg"; E_RC=97; _ec_save "$tag"; return 97;; esac
    if ! _ec_path "$pn" >/dev/null 2>&1 && [ -z "${ECALC_REPLAY:-}" ]; then
        echo "ecalc_run $tag: no binary for path $pn (bld0$pn not done): skipped" > "$lg"; E_SKIP=1; E_RC=99; _ec_save "$tag"; return 99
    fi
    if [ "${TGT_NO_NIC:-0}" = 1 ] && { [ "$shape" != D ] || [ "$n" -gt 1 ]; } && [ -z "${ECALC_REPLAY:-}" ]; then
        echo "ecalc_run $tag: skipped: TGT_NO_NIC=1 (the allocation has no usable NICs; shape $shape on $n node(s) needs the communication layer)" > "$lg"; E_SKIP=1; E_RC=99; _ec_save "$tag"; return 99
    fi
    for w in "$@"; do case $w in MNRUN_*=*) lw+=("$w");; *=*) words+=("$w");; esac; done
    # reporting settings unless the caller or target.conf set them
    for w in ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ECALC_SEGV_TRACE=1 ECALC_MEM_GUARD_GB=6; do
        local k=${w%%=*}; [ -n "${!k+x}" ] || words=("$w" "${words[@]}"); done
    if [ -n "$TGT_SHARE_CAP" ] && [ -z "${ECALC_PLANE_CAP+x}" ] && [[ " ${words[*]} " != *" ECALC_PLANE_CAP="* ]] \
       && [ "$d" = "$(awk -v s="$(ecalc_digits "$TGT_SHARE")" -v n="$n" 'BEGIN { printf "%.0f", s * n }')" ]; then
        words+=("ECALC_PLANE_CAP=$TGT_SHARE_CAP")      # Q122: the campaign share runs as the target launch line does
    fi
    h=$(_ec_hosts_of "$n") || true
    if [ -z "$h" ] && [ -z "${ECALC_REPLAY:-}" ]; then echo "ecalc_run $tag: fewer than $n hosts in HOSTS_SUB/HOSTS" > "$lg"; E_RC=98; _ec_save "$tag"; return 98; fi
    E_HOSTS=$h
    if [ "${WRITE:-0}" = 1 ]; then
        local sd=${TGT_SCRATCH:-${ECALC_OUT_DIR:-$TGT_ROOT/build/scratch}}/${ID:-item}-$$-$tag
        mkdir -p "$sd" && of=$sd/$tag.out
    fi
    local ltm=${TMO:-${TGT_ITEM_MIN:-10}}
    local lenv=(SLURM_JOB_ID="$JOBID" MNRUN_NODELIST="$h" MNRUN_UNLOAD="$EC_UNLOAD" MNRUN_EXCLUDE_MODE=warn)   # warn: mnrun never drops a host (a drop shrinks the node count); the runner already left excluded hosts out of HOSTS
    [ -n "$EC_MODS" ] && lenv+=(MNRUN_MODULES="$EC_MODS")
    t0=$(date +%s)
    if [ -n "${ECALC_REPLAY:-}" ]; then
        cp "$ECALC_REPLAY" "$lg" 2>/dev/null || zcat "$ECALC_REPLAY" > "$lg"; rc=0
    else
        timeout -k 20 $(( ltm * 60 )) env "${lenv[@]}" "${lw[@]}" "$EC_DIR/mnrun.sh" "$n" env "${shapew[@]}" "${words[@]}" "$EC_BIN" "$d" ${of:+"$of"} > "$lg" 2>&1 < /dev/null
        rc=$?
    fi
    E_WALL_S=$(( $(date +%s) - t0 )); E_RC=$rc
    [ "$rc" = 124 ] || [ "$rc" = 137 ] && echo "ecalc_run $tag: killed by the call timeout (${ltm} min)" >> "$lg"
    _ec_parse "$lg" "$shape"
    [ "$rc" = 4 ] && E_REFUSED=1
    # the node count mnrun used must be the requested one (never a silent shrink): "mn: pe i of M (node k of K" lines carry K
    local seen; seen=$(ecalc_grab "$lg" 'mn: pe [0-9]+ of [0-9]+ \(node [0-9]+ of [0-9]+' | sed -nE 's/.*\(node [0-9]+ of ([0-9]+).*/\1/p')
    if [ -n "$seen" ] && [ "$seen" != "$n" ] && [ -z "${ECALC_REPLAY:-}" ]; then
        echo "ecalc_run $tag: FAIL: mnrun used $seen node(s), $n requested (node count shrunk)" >> "$lg"; E_VERIFY=FAIL; [ "$rc" = 0 ] && rc=96; E_RC=$rc
    fi
    if [ "$rc" != 0 ] && [ "$E_VERIFY" = OK ]; then E_VERIFY=FAIL; fi
    # digit hash (on the first host, where the parts are readable)
    if [ -n "$of" ] && [ "$rc" = 0 ]; then
        local unp; unp=$(ecalc_tool "$pn" tools/unpack_digits)
        if [ -x "$unp" ]; then
            local hs
            hs='P=$(ls "$1".part* 2>/dev/null | sort); [ -n "$P" ] || P=$1; '"$unp"' -q $P | sha1sum | cut -c1-16; exit ${PIPESTATUS[0]}'
            if [ -n "${ECALC_REPLAY:-}" ] || ! command -v srun >/dev/null 2>&1 || [ "$JOBID" = none ]; then E_HASH=$(bash -c "$hs" _ "$of" 2>>"$lg"); E_UNPACK_RC=$?
            else E_HASH=$(srun --jobid="$JOBID" --export=ALL -N1 -n1 -w "${h%%,*}" -c 48 --overlap bash -c "$hs" _ "$of" 2>>"$lg"); E_UNPACK_RC=$?; fi
            E_HASH=$(echo "$E_HASH" | tail -1)
        else echo "ecalc_run $tag: no $unp: digit hash not taken" >> "$lg"; fi
        E_HASH_REF=$(ecalc_ref_hash "$d")
        if [ -n "$E_HASH_REF" ] && [ -n "$E_HASH" ]; then [ "$E_HASH" = "$E_HASH_REF" ] && E_HASH_MATCH=yes || E_HASH_MATCH=no; fi
        [ "${KEEP:-0}" = 1 ] || rm -rf "${of%/*}"
    fi
    _ec_clean_log "$lg"
    _ec_save "$tag"
    return $rc
}

ecalc_kv() {          # TAG -> summary lines
    local t=${1,,}; t=${t//[^a-z0-9_]/_}
    local p=m.$t
    [ -n "$E_TOTAL" ] && tgt_kv $p.total_s "$E_TOTAL s"
    [ -n "$E_INIT" ] && tgt_kv $p.init_s "$E_INIT s"
    [ -n "$E_BS" ] && tgt_kv $p.bs_s "$E_BS s"
    [ -n "$E_DM" ] && tgt_kv $p.dm_s "$E_DM s"
    [ -n "$E_RECIP" ] && tgt_kv $p.recip_s "$E_RECIP s"
    [ -n "$E_DC" ] && tgt_kv $p.dc_s "$E_DC s"
    [ -n "$E_WALL_S" ] && tgt_kv $p.wall_s "$E_WALL_S s"
    [ -n "$E_DEVPEAK_GB" ] && tgt_kv $p.devpeak_gb "$E_DEVPEAK_GB GB"
    [ -n "$E_DEVPEAK_APU_GB" ] && tgt_kv $p.devpeak_apu_gb "$E_DEVPEAK_APU_GB GB"
    [ -n "$E_HOSTPEAK_GB" ] && tgt_kv $p.hostpeak_gb "$E_HOSTPEAK_GB GB"
    [ -n "$E_HOSTHWM_PROC_GB" ] && tgt_kv $p.hosthwm_proc_gb "$E_HOSTHWM_PROC_GB GB"
    tgt_kv $p.rc "${E_RC:-NA}"
    tgt_kv e.$t.shape "$E_SHAPE"; tgt_kv e.$t.digits "$E_DIGITS"; tgt_kv e.$t.nodes "$E_NODES"; tgt_kv e.$t.hosts "${E_HOSTS:--}"
    tgt_kv e.$t.verify "$E_VERIFY"
    [ -n "$E_HASH" ] && { tgt_kv e.$t.hash "$E_HASH"; tgt_kv e.$t.hash_match "$E_HASH_MATCH"; }
    [ -n "$E_CONFIG_HASH" ] && tgt_kv e.$t.config_hash "$E_CONFIG_HASH"
    return 0
}

# ---------------------------------------------------------------------------------------------------------------- planner
ecalc_plan() {        # NODES DIGITS SHAPE [VAR=VALUE ...]
    local n=$1 d shape=$3 pn plf rc w ppn=() words=(); d=$(awk -v x="$2" 'BEGIN { printf "%.0f", x }'); shift 3
    P_FIT=NA P_DEV_GB= P_DEV_LIMIT_GB= P_DEV_MARGIN_GB= P_NODE_GB= P_NODE_LIMIT_GB= P_NODE_MARGIN_GB= P_APU_GB= P_WRAP= P_POOL_MB= P_CHECK=NONE P_LINE=
    case $shape in D) pn=1;; P) pn=1; ppn=(ECALC_PE_PER_GPU=1 SLURM_NTASKS_PER_NODE=4);; C) pn=2; ppn=(ECALC_PE_PER_GPU=1 SLURM_NTASKS_PER_NODE=4 XSHMEM_LANES=2);; *) return 2;; esac
    _ec_path "$pn" >/dev/null 2>&1 || return 2
    plf=$OUT/raw/plan_${d}_${n}_$shape.log
    for w in "$@"; do case $w in *=*) words+=("$w");; esac; done
    local tr=(); { [ "$n" -gt 1 ] || [ "$shape" != D ]; } && tr=(COMM_TRANSPORT=xshmem)
    timeout -k 10 300 bash -lc '[ -n "'"$EC_UNLOAD"'" ] && module unload '"$EC_UNLOAD"' >/dev/null 2>&1; [ -n "'"$EC_MODS"'" ] && module load '"$EC_MODS"' >/dev/null 2>&1; exec "$@"' _ \
        env MN_PLAN_ONLY="$d:$n" "${ppn[@]}" "${tr[@]}" "${words[@]}" "$EC_BIN" > "$plf" 2>&1 < /dev/null
    rc=$?
    P_POOL_MB=$(sed -n 's/^plan pool.*COMM_SHMEM_POOL_MB=\([0-9][0-9]*\).*/\1/p' "$plf" | head -1)
    local last; last=$(grep -a -E '^plan (check|REFUSED)' "$plf" | tail -1)
    case "$last" in "plan check "*": OK"*) P_CHECK=OK;; "plan REFUSED"*) P_CHECK=REFUSED;; esac
    P_LINE=$(grep -a -E '^plan fit +(FITS|REFUSED)' "$plf" | tail -1 | cut -c1-260)
    P_APU_GB=$(sed -n 's/^plan fit .*device [0-9.]* GB per node = \([0-9.]*\) GB per APU.*/\1/p' "$plf" | head -1)
    P_WRAP=$(sed -n 's/^plan fit .*DM_MN_WRAP=\([01]\).*/\1/p' "$plf" | tail -1)
    if [ -n "$P_LINE" ]; then
        # "...: device X / Y GB (+m), node A / B GB (+m2), DM_MN_WRAP=.."
        read -r P_DEV_GB P_DEV_LIMIT_GB P_DEV_MARGIN_GB P_NODE_GB P_NODE_LIMIT_GB P_NODE_MARGIN_GB < <(echo "$P_LINE" | sed -nE 's/.*device ([0-9.]+) \/ ([0-9.]+) GB \(([-+0-9.]+)\), node ([0-9.]+) \/ ([0-9.]+) GB \(([-+0-9.]+)\).*/\1 \2 \3 \4 \5 \6/p')
        case "$P_LINE" in *"FITS"*) P_FIT=yes;; *) P_FIT=no;; esac
    elif [ "$P_CHECK" = REFUSED ]; then P_FIT=no; P_LINE=$last
    fi
    [ "$P_CHECK" = REFUSED ] && P_FIT=no
    case $P_FIT in yes) [ "$P_CHECK" = REFUSED ] && return 1; return 0;; no) return 1;; *) return 2;; esac
}

# ---------------------------------------------------------------------------------------------------------------- concurrency
ecalc_bg() { ( ecalc_run "$@" ) > /dev/null 2>&1 & EC_BG_PIDS+=($!); }
ecalc_wait() { local p f=0; for p in "${EC_BG_PIDS[@]}"; do wait "$p" || f=$((f + 1)); done; EC_BG_PIDS=(); return $f; }

# ---------------------------------------------------------------------------------------------------------------- misc
ecalc_refresh() {
    local w=${1:-900} t0 sf=$TGT_RES/bench/bld02 p
    t0=$(date +%s)
    while [ ! -f "$sf/DONE" ] && [ -d "$sf" ] && [ $(( $(date +%s) - t0 )) -lt "$w" ]; do sleep 20; done
    [ -f "$sf/DONE" ] || return 1
    grep -qE '^status=(PASS|PARTIAL)' "$sf/summary.txt" || return 1
    p=$(grep -E '^e\.binary=' "$sf/summary.txt" | tail -1 | cut -d= -f2-)
    [ -n "$p" ] || return 1
    case "$p" in /*) ;; *) p=$TGT_ROOT/$p;; esac
    [ -x "$p" ] || return 1
    export ECALC2=$p MNRUN_DIR2=$(dirname "$p")
    MNRUN_MODULES_P2=$(grep -E '^e\.MNRUN_MODULES=' "$sf/summary.txt" | tail -1 | cut -d= -f2-); export MNRUN_MODULES_P2
    return 0
}
ecalc_aac7() { [ "${TGT_SITE:-target}" = aac7 ]; }   # the sister system: no path 2 (no Cray SHMEM 12), so skipped path-2 arms do not lower the status
ecalc_q() { local k=$1; shift; tgt_kv "q.$k" "$*"; }
# ecalc_end STATUS: write the status and exit 0.  Fail-safe: a requested PASS stands only when the item has a key measurement: at least one
# counted ecalc run (raw/*.res of a run that is not a warm-up: rc 0 and VERIFY OK), or, when the item ran no ecalc at all, at least one numeric
# m.* metric in summary.txt.  Otherwise PARTIAL (some numeric m.* metrics) or FAIL (none), with a note naming the reason.
ecalc_end() {
    local st=$1 f ok=0 att=0 nm why=
    if [ "$st" = PASS ]; then
        for f in "$OUT"/raw/*.res; do [ -f "$f" ] || continue
            case ${f##*/} in *_warm.res) continue;; esac
            ( . "$f"; [ "$E_SKIP" = 1 ] && exit 2; [ "$E_RC" = 0 ] && [ "$E_VERIFY" = OK ] && exit 0; exit 1 ); case $? in 0) ok=$((ok + 1));; 1) att=$((att + 1));; esac
        done
        nm=$(grep -E '^m\.' "$OUT/summary.txt" 2>/dev/null | grep -vE '^m\.[^=]*\.(rc|runs_ok|runs_failed)=|^m\.runs_' | grep -cE '=[ ]*[-+]?[0-9.]+([eE][-+]?[0-9]+)?([ ]|$)')
        if [ "$ok" -gt 0 ]; then :
        elif [ "$att" -gt 0 ]; then why="no ecalc run was good (rc 0 and VERIFY OK) of $att attempted"
        elif [ "$nm" -gt 0 ]; then :
        else why="no measurement: no counted ecalc run and no numeric m.* metric"; fi
        if [ -n "$why" ]; then
            [ "$nm" -gt 0 ] && st=PARTIAL || st=FAIL
            [ "${TGT_NO_NIC:-0}" = 1 ] && why="$why (TGT_NO_NIC=1: runs needing NICs are skipped)"
            tgt_kv note "status lowered from PASS to $st: $why"
        fi
    fi
    tgt_kv status "$st"; exit 0
}
# ecalc_ok: the loaded run is good (rc 0, VERIFY OK, no digit-hash mismatch).  ecalc_brief TAG: one phrase for q.* lines.
ecalc_ok() { [ "$E_RC" = 0 ] && [ "$E_VERIFY" = OK ] && [ "$E_HASH_MATCH" != no ]; }
ecalc_brief() {
    if [ "$E_SKIP" = 1 ]; then echo "$1 skipped ($([ "${TGT_NO_NIC:-0}" = 1 ] && echo "no NICs or no binary" || echo "no binary"))"; return; fi
    printf '%s: %s' "$1" "$(ecalc_ok && echo OK || echo "BAD rc=$E_RC verify=$E_VERIFY${E_REFUSED:+ refused=$E_REFUSED}")"
    [ -n "$E_HASH" ] && printf ' sha=%s(%s)' "$E_HASH" "$E_HASH_MATCH"
    [ -n "$E_TOTAL" ] && printf ' total=%s s' "$E_TOTAL"; echo
}

# ecalc_xt TAG PATHNO NODES "SECTIONS" [VAR=VALUE ...]: tests/t_xshmem on NODES hosts, 4 PEs per node (ECALC_PE_PER_GPU=1 COMM_TRANSPORT=xshmem;
# path 2 adds XSHMEM_LANES=2 unless given), through the build's mnrun.sh with the planner skipped (MNRUN_PLAN_POOL=0).  Heap: XSHMEM_HEAP_MB
# (and SHMEM_SYMMETRIC_SIZE / XT_SYMMETRIC_HEAP_SIZE on path 2) = 2 x PEs x XT_MAXMB + 256 MiB unless given; XT_MAXMB default 64.
# MNRUN_* words go to the launcher.  Log raw/TAG.log (rank labels on).  Sets X_RC, X_RESULT (PASS|FAIL|NONE), X_SKIP, X_WALL_S; rc 99 = no binary.
ecalc_xt() {
    local tag=$1 pn=$2 n=$3 sec=$4 w h t0 rc lg=$OUT/raw/$1.log words=() lw=() mx=${XT_MAXMB:-64} hm np; shift 4
    X_RC= X_RESULT=NONE X_SKIP=0 X_WALL_S=
    _ec_path "$pn" >/dev/null 2>&1; local T; T=$(ecalc_tool "$pn" tests/t_xshmem)
    if [ "${TGT_NO_NIC:-0}" = 1 ]; then echo "ecalc_xt $tag: skipped: TGT_NO_NIC=1 (no usable NICs in the allocation)" > "$lg"; X_SKIP=1; X_RC=99; return 99; fi
    if [ ! -x "$T" ]; then echo "ecalc_xt $tag: no t_xshmem for path $pn" > "$lg"; X_SKIP=1; X_RC=99; return 99; fi
    h=$(_ec_hosts_of "$n") || true; [ -n "$h" ] || { echo "ecalc_xt $tag: fewer than $n hosts" > "$lg"; X_RC=98; return 98; }
    for w in "$@"; do case $w in MNRUN_*=*) lw+=("$w");; *=*) words+=("$w"); case $w in XT_MAXMB=*) mx=${w#*=};; esac;; esac; done
    np=$((n * 4)); hm=$(( 2 * np * mx + 256 ))
    local have=" ${words[*]} "
    case $have in *" XSHMEM_HEAP_MB="*) ;; *) words+=(XSHMEM_HEAP_MB=$hm);; esac
    if [ "$pn" = 2 ]; then
        case $have in *" XSHMEM_LANES="*) ;; *) words+=(XSHMEM_LANES=2);; esac
        case $have in *" SHMEM_SYMMETRIC_SIZE="*) ;; *) words+=(SHMEM_SYMMETRIC_SIZE=${hm}M XT_SYMMETRIC_HEAP_SIZE=${hm}M);; esac
    fi
    local lenv=(SLURM_JOB_ID="$JOBID" MNRUN_NODELIST="$h" MNRUN_UNLOAD="$EC_UNLOAD" MNRUN_PLAN_POOL=0 MNRUN_LABEL=1 MNRUN_EXCLUDE_MODE=warn)
    [ -n "$EC_MODS" ] && lenv+=(MNRUN_MODULES="$EC_MODS")
    t0=$(date +%s)
    timeout -k 20 $(( ${TMO:-${TGT_ITEM_MIN:-10}} * 60 )) env "${lenv[@]}" "${lw[@]}" "$EC_DIR/mnrun.sh" "$n" env ECALC_PE_PER_GPU=1 COMM_TRANSPORT=xshmem "${words[@]}" "$T" $sec > "$lg" 2>&1 < /dev/null
    rc=$?; X_RC=$rc; X_WALL_S=$(( $(date +%s) - t0 ))
    if grep -aq 'XT_RESULT PASS' "$lg"; then X_RESULT=PASS; elif grep -aq 'XT_RESULT FAIL\|<<< FAIL' "$lg"; then X_RESULT=FAIL; fi
    _ec_clean_log "$lg"; return $rc
}
# ecalc_xt_rows TAG KIND: the data rows of one table of a t_xshmem log (kind lat|stream|a2a|a2a_pairs), rank labels stripped, "|" kept
ecalc_xt_rows() { local f=$OUT/raw/$1.log; [ -f "$f" ] || f=$f.gz; { zcat -f "$f"; } 2>/dev/null | sed -E 's/^ *[0-9]+: //' | grep -a "^ROW $2 "; }
# ecalc_xt_col TAG KIND COL REGEX MODE: field COL (1 = "ROW kind", 2 = the label column "split|nbi ... bytes", 3.. the numbers: lat 3 = us mean, 4 = min;
# stream 6 = GB/s per PE mean, 8 = node GB/s; a2a 5 = total ms, 7 = GB/s per PE mean, 9 = node GB/s) of the rows whose label matches REGEX, MODE max|min|first|last
ecalc_xt_col() { ecalc_xt_rows "$1" "$2" | awk -F' [|] ' -v c="$3" -v re="${4:-.}" -v mode="${5:-max}" '$2 ~ re { v = $c + 0; if (!n++ || mode == "last" || (mode == "max" && v > r) || (mode == "min" && v < r)) r = v; if (mode == "first" && n == 1) { r = v; exit } } END { if (n) printf "%.4g\n", r }'; }
# ecalc_phases: "init X bs Y recip Z dm W total T" of the loaded run (seconds; NA when not printed)
ecalc_phases() { printf 'init %s bs %s recip %s dm %s total %s' "${E_INIT:-NA}" "${E_BS:-NA}" "${E_RECIP:-NA}" "${E_DM:-NA}" "${E_TOTAL:-NA}"; }

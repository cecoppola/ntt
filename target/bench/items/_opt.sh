#!/bin/bash
# items/_opt.sh - helpers of the optimization (prio 100-199) and calibration (prio >= 200) items.  SOURCE it after setting nothing:
#     . "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
# It sources _ecalc.sh (ecalc_run, ecalc_plan, ecalc_bg, ...; the interface is documented there) and adds:
#   skip MSG                    status SKIP + note, exit 0 (nothing measured)
#   need_path N                 skip unless the binary of path N (1|2) exists
#   dig X [NODES]               ecalc_digits of X digits per node x NODES (TGT_SITE=aac7 shrinks it)
#   tool PATHNO REL             absolute path of a build-tree file (tests/* inside ecalc/, tools/* beside it); 0 when absent
#   tleft_ok [FRAC]             0 while less than FRAC (default 0.85) of the item's est_min is used
#   fitd NODES SHAPE DIGITS [VAR=VALUE ...]   prints the digits (shrunk in 4 % steps, at most 12 times) the planner accepts; notes the shrink
#   Arms:  ARMV[name]="VAR=VALUE ..." (settings of an arm; may be empty)
#   one ARM ROUND SHAPE NODES DIGITS [VAR=VALUE ...]      one ecalc run, tag ARM_rROUND, row in raw/runs.tsv, m.<tag>.* keys
#   runseq SHAPE NODES DIGITS "A B B A" [VARS]                a sequence of one() runs on the item's first hosts, stops when the time budget is used
#   conc SHAPE NODES DIGITS ROUND "A B" [VARS]             arms at the same time on disjoint host groups of the allocation (needs len(arms) x NODES hosts)
#   arms_report BASE [METRIC [PREFIX]]                     per-arm mean, paired differences against BASE (_opt_stats.py); sets AB_LINE (text)
#   Parsers (on a log file, gz ok, rank labels stripped): tl_kv LOG PREFIX (init timeline), clocks_kv LOG PREFIX, wait_kv LOG PREFIX PER (wait stats)
#   xt_run TAG PATHNO NODES SECTIONS [VAR=VALUE ...]       tests/t_xshmem through mnrun.sh;  xt_rows LOG TABLE;  xt_kv TAG TABLE
#   build_test PATHNO NAME ...  (login-node make of extra test programs in the build tree of that path, flock'd)
#   finish [NOTE]               status from runs.tsv (PASS: every run counted, PARTIAL: some, FAIL: none) + notes, exit code
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
_OPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPT_T0=$(date +%s)
OPT_NOTES=()
RUNS=$OUT/raw/runs.tsv
[ -f "$RUNS" ] || printf 'tag\tarm\tround\tnodes\tdigits\trc\tverify\ttotal\tinit\tbs\tdm\trecip\tdevpeak\thostpeak\n' > "$RUNS"
declare -A ARMV=()

note() { OPT_NOTES+=("$*"); }
skip() { tgt_kv status SKIP; tgt_kv note "$*"; exit 0; }
need_path() { ecalc_have "$1" || skip "no binary for path $1 (build item bld0$1 not done)"; }
dig() { ecalc_digits "$(awk -v x="$1" -v n="${2:-1}" 'BEGIN { printf "%.6g", x * n }')"; }
elapsed_s() { echo $(( $(date +%s) - OPT_T0 )); }
tleft_ok() { [ $(( $(elapsed_s) * 100 )) -lt $(awk -v e="${TGT_ITEM_MIN:-30}" -v f="${1:-0.85}" 'BEGIN { printf "%d", e * 60 * f * 100 }') ]; }
tool() { _ec_path "$1" >/dev/null 2>&1; [ -n "$EC_DIR" ] || { echo 0; return 1; }
    local p; case $2 in tests/*) p=$EC_DIR/$2;; *) p=$(cd "$EC_DIR/.." && pwd)/$2;; esac
    [ -x "$p" ] && echo "$p" || { echo 0; return 1; }; }
hosts_n() { echo "$HOSTS" | tr ',' '\n' | grep -c .; }
site_is_aac7() { [ "${TGT_SITE:-target}" = aac7 ]; }

fitd() {      # NODES SHAPE DIGITS [VARS] -> digits that fit
    local n=$1 sh=$2 d=$3 i; shift 3
    for i in 0 1 2 3 4 5 6 7 8 9 10 11 12; do
        ecalc_plan "$n" "$d" "$sh" "$@"; local rc=$?
        [ $rc != 1 ] && break          # 0 fits, 2 no plan available: use as is
        d=$(awk -v d="$d" 'BEGIN { printf "%.0f", d * 0.96 }')
    done
    [ "$i" -gt 0 ] && note "digits cut ${i}x by 4 % to fit the planner at $n nodes shape $sh (final $d)"
    echo "$d"
}

# ------------------------------------------------------------------------------------------------------------ arms engine
rec() {       # TAG ARM ROUND : load the run's .res, append the runs.tsv row
    ecalc_load "$1" || return 1
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$E_NODES" "$E_DIGITS" "$E_RC" "$E_VERIFY" "$E_TOTAL" "$E_INIT" "$E_BS" "$E_DM" "$E_RECIP" "$E_DEVPEAK_GB" "$E_HOSTPEAK_GB" >> "$RUNS"
    ecalc_kv "$1"
    [ "$E_SKIP" = 1 ] && note "arm $2 round $3 skipped: no binary for the path"
    return 0
}
one() {       # ARM ROUND SHAPE NODES DIGITS [VARS]
    local arm=$1 rd=$2 sh=$3 n=$4 d=$5; shift 5
    ecalc_run "${arm}_r${rd}" "$sh" "$n" "$d" ${ARMV[$arm]-} "$@" || true
    rec "${arm,,}_r${rd}" "$arm" "$rd"
}
runseq() {    # SHAPE NODES DIGITS "A B B A" [VARS]  (runs on the first NODES hosts)
    local sh=$1 n=$2 d=$3 order=$4 a i=0 last=0; shift 4
    for a in $order; do
        i=$((i + 1))
        if [ "$last" -gt 0 ] && [ $(( $(elapsed_s) + last * 12 / 10 )) -gt $(( ${TGT_ITEM_MIN:-30} * 60 * 95 / 100 )) ]; then note "stopped before run $i ($a): time budget"; return 1; fi
        one "$a" "$i" "$sh" "$n" "$d" "$@"
        [ "$E_SKIP" = 1 ] && return 1
        last=${E_WALL_S:-0}
    done
}
conc() {      # SHAPE NODES DIGITS ROUND "A B ..." [VARS]
    local sh=$1 n=$2 d=$3 rd=$4 arms=$5 a i=0; shift 5
    [ "$(hosts_n)" -ge $(( n * $(echo $arms | wc -w) )) ] || { note "conc: need $((n * $(echo $arms | wc -w))) hosts"; return 1; }
    for a in $arms; do
        HOSTS_SUB=$(ecalc_hosts $((i * n)) "$n") ecalc_bg "${a}_r${rd}" "$sh" "$n" "$d" ${ARMV[$a]-} "$@"; i=$((i + 1))
    done
    ecalc_wait || true
    for a in $arms; do rec "${a,,}_r${rd}" "$a" "$rd"; done
}
arms_report() {   # BASE [METRIC [PREFIX]]
    local out; out=$(python3 -I "$_OPT_DIR/_opt_stats.py" "$RUNS" "$1" "${2:-total}" "${3:-m.arm}" 2>&1)
    AB_LINE=$(echo "$out" | sed -n 's/^LINE //p' | paste -sd';' - | sed 's/;/; /g')
    echo "$out" | grep -v '^LINE ' | while IFS='=' read -r k v; do [ -n "$k" ] && tgt_kv "$k" "$v"; done
}
runs_ok() { awk -F'\t' 'NR > 1 && $6 == 0 && $7 == "OK" { n++ } END { print n + 0 }' "$RUNS"; }
runs_bad() { awk -F'\t' 'NR > 1 && !($6 == 0 && $7 == "OK") { n++ } END { print n + 0 }' "$RUNS"; }
finish() {
    local ok bad; ok=$(runs_ok); bad=$(runs_bad); local extra=${OPT_EXTRA_FAIL:-0}
    [ "${OPT_NOTES[*]:-}" ] && tgt_kv note "$(printf '%s; ' "${OPT_NOTES[@]}")${1:-}" || { [ -n "${1:-}" ] && tgt_kv note "$1"; }
    tgt_kv m.runs_ok "$ok"; tgt_kv m.runs_failed "$bad"
    if [ "$ok" = 0 ] && [ "${OPT_NO_RUNS_OK:-0}" != 1 ]; then tgt_kv status FAIL; exit 1; fi
    if [ "$bad" != 0 ] || [ "$extra" != 0 ]; then tgt_kv status PARTIAL; exit 0; fi
    tgt_kv status PASS; exit 0
}

# ------------------------------------------------------------------------------------------------------------ log parsers
_cat() { local f=$1; [ -f "$f" ] || f=$f.gz; [ -f "$f" ] || return 1
    if [ "${f%.gz}" != "$f" ]; then zcat "$f"; else cat "$f"; fi | sed -E 's/^ *[0-9]+: //'; }

tl_kv() {     # LOG PREFIX : init timeline (ECALC_INIT_TL=1) -> m.PREFIX.* ; the lines go to raw/PREFIX.tl
    local lg=$1 p=$2
    _cat "$lg" | grep -a -E '^tl +[0-9.]+ ' > "$OUT/raw/$p.tl" || true
    [ -s "$OUT/raw/$p.tl" ] || { note "$p: no tl lines (ECALC_INIT_TL not effective)"; return 1; }
    awk -v p="$p" -v seedlate=18 -v seedaft=2 '
        function kv(k, v) { printf "m.%s.%s=%s\n", p, k, v }
        { t = $2 + 0; line = $0 }
        /seed thread ends/ { if (match(line, /ends \([0-9.]+/)) sd = substr(line, RSTART + 6, RLENGTH - 6) + 0; if (!ns++ || t > se) se = t }
        /background mapping done:/ { if (!nm++ || t > ml) ml = t
            if (match(line, /done: [0-9]+ chunks in [0-9.]+ s/)) { split(substr(line, RSTART + 6, RLENGTH - 6), a, " "); bgc += a[1]; bgt += a[4] }
            if (match(line, /mapper lock [0-9.]+/)) q += substr(line, RSTART + 12, RLENGTH - 12) }
        /arena: [0-9]+ chunks mapped in/ { if (match(line, /arena: [0-9]+ chunks mapped in [0-9.]+ s/)) { split(substr(line, RSTART + 7, RLENGTH - 7), a, " "); fgc += a[1]; fgt += a[5]; fgn++; if (a[5] / a[1] > fgmax) fgmax = a[5] / a[1] }
            if (t > ml2) ml2 = t }
        /plane pools done \(/ { if (match(line, /\([0-9.]+/)) pp = substr(line, RSTART + 1, RLENGTH - 1) + 0; ppt = t }
        /peer access done \(/ { if (match(line, /\([0-9.]+/)) peer = substr(line, RSTART + 1, RLENGTH - 1) + 0 }
        /staging and contexts done \(/ { if (match(line, /\([0-9.]+/)) stg = substr(line, RSTART + 1, RLENGTH - 1) + 0 }
        /seed thread: buffers allocated \(/ { if (match(line, /\([0-9.]+/)) sbuf = substr(line, RSTART + 1, RLENGTH - 1) + 0 }
        /the region pools are there \(waited/ { if (match(line, /waited [0-9.]+/)) sw = substr(line, RSTART + 7, RLENGTH - 7) + 0 }
        END {
            if (ns) { kv("seed_end_t_s", sprintf("%.2f s", se)); kv("seed_dur_s", sprintf("%.2f s", sd)) }
            if (nm) { kv("last_bgmap_done_t_s", sprintf("%.2f s", ml)); kv("bgmap_chunks", bgc); kv("bgmap_sum_s", sprintf("%.1f s", bgt)); kv("bgmap_lockqueue_sum_s", sprintf("%.1f s", q)) }
            if (fgn) { kv("arena_fg_chunks", fgc); kv("arena_fg_sum_s", sprintf("%.1f s", fgt)); kv("arena_fg_s_per_gb_apu", sprintf("%.4f s/GB", fgt / (fgc * 2.147))); kv("arena_fg_worst_s_per_chunk", sprintf("%.3f s", fgmax)) }
            if (pp != "") { kv("plane_pools_s", sprintf("%.2f s", pp)); kv("plane_pools_done_t_s", sprintf("%.2f s", ppt)) }
            if (peer != "") kv("peer_access_s", sprintf("%.2f s", peer))
            if (stg != "") kv("staging_s", sprintf("%.2f s", stg))
            if (sbuf != "") kv("seed_buffers_s", sprintf("%.2f s", sbuf))
            if (sw != "") kv("seed_wait_pools_s", sprintf("%.2f s", sw))
            if (ns && nm) { d = se - ml; kv("seed_after_last_map_s", sprintf("%+.2f s", d))
                v = (se > seedlate && d > seedaft) ? "BUILD-CANDIDATE" : (se > seedlate ? "MARGINAL" : "NO-GO"); kv("a37r1_verdict", v) }
        }' "$OUT/raw/$p.tl" | while IFS='=' read -r k v; do tgt_kv "$k" "$v"; done
}

clocks_kv() { # LOG PREFIX : aac7env clock lines (ECALC_LOG_CLOCKS) -> m.PREFIX.sclk_*_mhz ...
    local lg=$1 p=$2 f=$OUT/raw/$p.clocks
    _cat "$lg" | grep -a 'aac7env: node .* clocks t=' > "$f" || true
    [ -s "$f" ] || { note "$p: no clock lines"; return 1; }
    awk -v p="$p" '
        function num(s, k,   r) { if (match(s, k "=[0-9.]+")) return substr(s, RSTART + length(k) + 1, RLENGTH - length(k) - 1) + 0; return -1 }
        { for (i = 0; i < 3; i++) { k = (i == 0 ? "sclk" : i == 1 ? "mclk" : "fclk"); v = num($0, k); if (v < 0) continue
              n[k]++; s[k] += v; if (!(k in mn) || v < mn[k]) mn[k] = v; if (v > mx[k]) mx[k] = v; all[k, n[k]] = v } }
        END { for (k in n) { printf "m.%s.%s_mean_mhz=%.0f MHz\nm.%s.%s_min_mhz=%.0f MHz\nm.%s.%s_max_mhz=%.0f MHz\n", p, k, s[k] / n[k], p, k, mn[k], p, k, mx[k]
                  lo = 0; for (j = 1; j <= n[k]; j++) if (all[k, j] < 0.95 * mx[k]) lo++
                  printf "m.%s.%s_below95pct_frac=%.3f\n", p, k, lo / n[k] }
              printf "m.%s.samples=%d\n", p, n["sclk"] }' "$f" | while IFS='=' read -r k v; do tgt_kv "$k" "$v"; done
}

wait_kv() {   # LOG PREFIX PER : MN_WAIT_STATS lines; PER = APU threads per line (4 for layout D, 1 per process under P) -> m.PREFIX.*
    local lg=$1 p=$2 per=${3:-4}
    _cat "$lg" | grep -a 'wait-stats node [0-9]*:' > "$OUT/raw/$p.wait" || true
    [ -s "$OUT/raw/$p.wait" ] || { note "$p: no wait-stats lines"; return 1; }
    awk -v p="$p" -v per="$per" '
        { d = -1; r = -1
          if (match($0, /[|] dm barrier [0-9.]+ s [(][0-9]+[)] wait [0-9.]+/)) { t = substr($0, RSTART, RLENGTH); sub(/.* wait /, "", t); d = t + 0 }
          if (match($0, /[|] recip barrier [0-9.]+ s [(][0-9]+[)] wait [0-9.]+/)) { t = substr($0, RSTART, RLENGTH); sub(/.* wait /, "", t); r = t + 0 }
          if (d < 0) next
          k++; if (k == 1 || d > dmax) { dmax = d; rmax = r } if (k == 1 || d < dmin) dmin = d
          dv = d - r; if (k == 1 || dv > vmax) vmax = dv }
        END { if (!k) exit
              printf "m.%s.dm_wait_max_s=%.1f s\nm.%s.dm_wait_min_s=%.1f s\nm.%s.recip_wait_s=%.1f s\nm.%s.div_wait_max_s=%.1f s\nm.%s.lines=%d\n", p, dmax / per, p, dmin / per, p, rmax / per, p, vmax / per, p, k
              printf "m.%s.s2_verdict=%s\n", p, (dmax / per > 30) ? "BUILD-DIVISION-OVERLAP (exposed wait > 30 s per thread)" : "NO-GO (<= 30 s per thread)" }' "$OUT/raw/$p.wait" | while IFS='=' read -r k v; do tgt_kv "$k" "$v"; done
}

# ------------------------------------------------------------------------------------------------------------ t_xshmem
xt_run() {    # TAG PATHNO NODES SECTIONS [VAR=VALUE ...]  -> rc ; log raw/TAG.log ; XT_WALL_S
    local tag=$1 pn=$2 n=$3 sec=$4 w lw=() words=() h t0 rc lg tl; shift 4
    lg=$OUT/raw/$tag.log; XT_WALL_S=
    _ec_path "$pn" >/dev/null 2>&1; local tb; tb=$(tool "$pn" tests/t_xshmem)
    [ "$tb" != 0 ] || { echo "xt_run $tag: no tests/t_xshmem for path $pn" > "$lg"; return 99; }
    h=$(_ec_hosts_of "$n") || true; [ -n "$h" ] || { echo "xt_run $tag: fewer than $n hosts" > "$lg"; return 98; }
    for w in "$@"; do case $w in MNRUN_*=*) lw+=("$w");; *=*) words+=("$w");; esac; done
    local mb=64; for w in "${words[@]}"; do case $w in XT_MAXMB=*) mb=${w#*=};; esac; done
    words=(XT_MAXMB=$mb "${words[@]}")
    local heap=$(( 2 * 4 * n * mb + 96 ))
    local p2=(); [ "$pn" = 2 ] && p2=(XSHMEM_LANES=2 MNRUN_SYMHEAP_XSHMEM=1 XSHMEM_HEAP_SLACK_MB=192)
    local lenv=(SLURM_JOB_ID="$JOBID" MNRUN_NODELIST="$h" MNRUN_UNLOAD="$EC_UNLOAD"); [ -n "$EC_MODS" ] && lenv+=(MNRUN_MODULES="$EC_MODS")
    tl=${XT_TMO:-${TGT_ITEM_MIN:-10}}
    t0=$(date +%s)
    timeout -k 20 $(( tl * 60 )) env "${lenv[@]}" "${lw[@]}" "$EC_DIR/mnrun.sh" "$n" env COMM_TRANSPORT=xshmem ECALC_PE_PER_GPU=1 XSHMEM_HEAP_MB=$heap XT_TIMEOUT=100 "${p2[@]}" "${words[@]}" "$tb" $sec > "$lg" 2>&1 < /dev/null
    rc=$?; XT_WALL_S=$(( $(date +%s) - t0 ))
    _ec_clean_log "$lg"; return $rc
}
xt_rows() {   # LOG TABLE -> "f1<TAB>f2..." : the ROW lines of one table, labels stripped, fields trimmed, '|' -> tab
    _cat "$1" | grep -a "^ROW $2 |" | sed -E "s/^ROW $2 \| *//; s/ *\| */\t/g"
}
xt_kv() {     # TAG TABLE : stream / a2a / lat rows -> m.TAG.<table>.<form>.<bytes>.* + peak ; the table also goes to raw/TAG.<table>.tsv
    local tag=${1,,} tb=$2 lg=$OUT/raw/$1.log
    xt_rows "$lg" "$tb" > "$OUT/raw/$tag.$tb.tsv"
    [ -s "$OUT/raw/$tag.$tb.tsv" ] || { note "$tag: no $tb rows"; return 1; }
    case $tb in
    stream) awk -F'\t' -v p="m.$tag.stream" '{ split($1, a, " "); k = a[1] "_" a[2] "_" a[3]; printf "%s.%s.gbs_per_pe=%s GB/s\n%s.%s.gbs_node=%s GB/s\n", p, k, $5, p, k, $7
                if ($7 + 0 > mx) { mx = $7 + 0; mk = k } } END { printf "%s.peak_node_gbs=%.1f GB/s\n%s.peak_key=%s\n", p, mx, p, mk }' "$OUT/raw/$tag.$tb.tsv";;
    lat) awk -F'\t' -v p="m.$tag.lat" '{ gsub(/[ (),+\/]/, "_", $1); printf "%s.%s.us_mean=%s us\n%s.%s.us_min=%s us\n", p, $1, $2, p, $1, $3 }' "$OUT/raw/$tag.$tb.tsv";;
    *) awk -F'\t' -v p="m.$tag.$tb" '{ split($1, a, " "); k = a[1] "_n" a[2] "_" a[3]; printf "%s.%s.total_ms=%s ms\n%s.%s.gbs_per_pe=%s GB/s\n%s.%s.gbs_node=%s GB/s\n", p, k, $4, p, k, $6, p, k, $8
                if ($8 + 0 > mx) { mx = $8 + 0; mk = k } } END { printf "%s.peak_node_gbs=%.1f GB/s\n%s.peak_key=%s\n", p, mx, p, mk }' "$OUT/raw/$tag.$tb.tsv";;
    esac | while IFS='=' read -r k v; do tgt_kv "$k" "$v"; done
    # NIC counters of the run (cxi telemetry deltas summed over the node's NICs)
    _cat "$lg" | grep -a '^CTRSUM' | head -4 > "$OUT/raw/$tag.ctrsum" || true
}
xt_peak() {   # TAG TABLE -> the peak node GB/s of the table (number only)
    awk -F'\t' -v c="$([ "$2" = stream ] && echo 7 || echo 8)" '{ if ($c + 0 > mx) mx = $c + 0 } END { if (NR) printf "%.1f", mx }' "$OUT/raw/${1,,}.$2.tsv" 2>/dev/null
}

# ------------------------------------------------------------------------------------------------------------ builds
build_test() { # PATHNO NAME [NAME ...] : make the test programs in the build tree of that path (login node, flock, own module line)
    local pn=$1 nm rc=0; shift
    _ec_path "$pn" >/dev/null 2>&1; [ -n "$EC_DIR" ] || return 1
    local xs=ofi; [ "$pn" = 2 ] && xs=cray
    for nm in "$@"; do
        [ -x "$EC_DIR/tests/$nm" ] && continue
        ( flock -w 600 9
          [ -x "$EC_DIR/tests/$nm" ] && exit 0
          bash -lc '[ -n "'"$EC_UNLOAD"'" ] && module unload '"$EC_UNLOAD"' >/dev/null 2>&1; [ -n "'"$EC_MODS"'" ] && module load '"$EC_MODS"' >/dev/null 2>&1; cd "'"$EC_DIR"'" && make -j8 XSHMEM='$xs' tests/'$nm'' > "$OUT/raw/make_$nm.log" 2>&1
        ) 9> "$TGT_ROOT/build/.mk.lock" || rc=1
        [ -x "$EC_DIR/tests/$nm" ] || { rc=1; note "build of tests/$nm failed (raw/make_$nm.log)"; }
    done
    return $rc
}

# ------------------------------------------------------------------------------------------------------------ small statistics
spread_kv() { # PREFIX [METRIC] : spread of the counted runs of runs.tsv (A/A control): n mean sd cv min max slow fraction, bimodality -> m.PREFIX.*; sets SPREAD_LINE
    local p=$1 col=${2:-8}
    SPREAD_LINE=$(awk -F'\t' -v p="$p" -v c="$col" 'NR > 1 && $6 == 0 && $7 == "OK" && $c != "" { v[++n] = $c + 0 }
        END { if (n < 1) exit
              for (i = 1; i <= n; i++) { s += v[i]; if (i == 1 || v[i] < mn) mn = v[i]; if (v[i] > mx) mx = v[i] }
              m = s / n; for (i = 1; i <= n; i++) ss += (v[i] - m) ^ 2; sd = n > 1 ? sqrt(ss / (n - 1)) : 0
              for (i = 1; i <= n; i++) if (v[i] > mn * 1.05) slow++
              for (i = 1; i <= n; i++) a[i] = v[i]; for (i = 2; i <= n; i++) { t = a[i]; for (j = i - 1; j >= 1 && a[j] > t; j--) a[j + 1] = a[j]; a[j + 1] = t }
              gap = 0; gi = 0; for (i = 1; i < n; i++) if (a[i + 1] - a[i] > gap) { gap = a[i + 1] - a[i]; gi = i }
              bim = (n >= 4 && gap > 0.03 * mn && gi >= 2 && n - gi >= 2) ? "yes" : "no"
              printf "m.%s.n=%d\nm.%s.mean_s=%.3f s\nm.%s.sd_s=%.3f s\nm.%s.cv_pct=%.2f %%\nm.%s.min_s=%.3f s\nm.%s.max_s=%.3f s\nm.%s.max_over_min_pct=%.2f %%\nm.%s.slow_fraction=%.2f\nm.%s.bimodal=%s\nm.%s.largest_gap_pct=%.2f %%\n", \
                  p, n, p, m, p, sd, p, 100 * sd / m, p, mn, p, mx, p, 100 * (mx - mn) / mn, p, slow / n, p, bim, p, 100 * gap / mn
              printf "LINE n=%d mean %.2f s sd %.2f s (cv %.2f %%), min %.2f max %.2f (+%.1f %%), slow(>5%%) %d of %d, bimodal %s\n", n, m, sd, 100 * sd / m, mn, mx, 100 * (mx - mn) / mn, slow, n, bim }' "$RUNS")
    echo "$SPREAD_LINE" | grep -v '^LINE' | while IFS='=' read -r k v; do [ -n "$k" ] && tgt_kv "$k" "$v"; done
    SPREAD_LINE=$(echo "$SPREAD_LINE" | sed -n 's/^LINE //p')
}

ntt_rates_kv() { # LOG PREFIX : t_ntt section 5 -> m.PREFIX.fwd_ms_mean/min/max, fwd/inv TB/s mean, batched GB/s
    awk -v p="$2" '/^ +APU[0-9]+ 2\^[0-9]+: fwd/ { n++; f = $4 + 0; tf = $6; gsub(/\(/, "", tf); i = $9 + 0; ti = $11; gsub(/\(/, "", ti); lg = $2
            sf += f; stf += tf; sti += ti; if (n == 1 || f < mn) mn = f; if (f > mx) mx = f; np = $(NF - 1) }
        /batched log L = / { gsub(/,/, ""); k = $5; b[k] += $(NF - 1); nb[k]++ }
        END { if (!n) exit; printf "m.%s.size=%s\nm.%s.fwd_ms_mean=%.2f ms\nm.%s.fwd_ms_min=%.2f ms\nm.%s.fwd_ms_max=%.2f ms\nm.%s.fwd_tbs_mean=%.3f TB/s\nm.%s.inv_tbs_mean=%.3f TB/s\nm.%s.passes=%s\n", p, lg, p, sf / n, p, mn, p, mx, p, stf / n, p, sti / n, p, np
              for (k in b) printf "m.%s.batched_L%s_gbs=%.0f GB/s\n", p, k, b[k] / nb[k] }' <(_cat "$1") | while IFS='=' read -r k v; do tgt_kv "$k" "$v"; done
}

ab_main() {   # SHAPE NODES DIGITS "ORDER" BASE TQ : the common A/B item (arms in ARMV): sequence, report, q line, status
    local sh=$1 n=$2 d=$3 order=$4 base=$5 tq=$6
    if [ "$sh" = C ]; then need_path 2; else need_path 1; fi
    [ "$(hosts_n)" -ge "$n" ] || skip "needs $n hosts, have $(hosts_n)"
    runseq "$sh" "$n" "$d" "$order" || true
    arms_report "$base"
    tgt_kv q.$tq "${AB_LINE:-no counted run} (shape $sh, $n nodes, $d digits)"
    finish
}

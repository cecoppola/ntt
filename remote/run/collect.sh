#!/bin/bash
# collect.sh - gather everything the session logged into ONE frozen directory on the target and PRINT it to the screen in numbered parts (nothing leaves the
# target except what is pasted; the user copies the printed parts into the chat).
#   collect.sh --plan                          inventory: files, lines, bytes per category, and how many parts (pastes) a full print takes
#   collect.sh --print [--part K] [--lines N] [--only CAT[,CAT]] [--compact]
#                                              print parts; plain text: `=== ECALC LOG PART K/N <category> lines=L sha256=<of the body> ===` body `=== END PART K/N ===`
#                                              (part K is the same text every time: the data are frozen, the order is fixed; K/N count over ALL categories also with --only);
#                                              --compact: the same content gzip+base64 (76-column lines), sha256 of the DECODED bytes of the part, decode recipe in the frame
#   collect.sh --print --only raw --ranks LIST [--grep REGEX]   a FILTERED raw category (own part numbers): the lines of the listed ranks, plus every line that names no rank, and
#                                              (--grep) only lines matching REGEX.  LIST: comma list of rank numbers, ranges a-b and the words first, last, busiest (largest device peak
#                                              of the "device X GB in use" lines), slowest (the rank whose last log line comes last).  --plan shows the part count of the default set
#                                              (ranks first,last,busiest,slowest).  A rank is read from an "N: " line prefix or "pe N" / "node N" in the line.
#   collect.sh --paste                         digest of at most 60 lines (one line per s5 run) for the browser agent
#   collect.sh [--refresh] [--out DIR] [--dry-run]    --refresh makes a new freeze (default: the latest one is reused; a note says when runs are newer than it)
# Categories, in print order (most valuable first): summaries (run summaries, launch lines, digit file paths and hashes), stats (the diagnostic and phase / memory table lines
# of the rank logs: wait-stats, recip ts, init split, mem tables, T1/T2, comm_xshmem summaries), nodes (per-node NIC roll-up and the cross-node timeline of every run),
# env (capture/, site.env, kit identity, binary hashes, modules, versions, Slurm accounting, planner logs, other stages' summaries), nodes_full (per-NIC CSV, every
# monitor CSV, the node snapshots), raw (the rest of the rank logs, build logs, everything else).  stats + raw together are the complete rank logs.
# The digit files stay where they are (only path, size and printed hash are in summaries).  Works after the allocation is released; every missing tool is skipped.
# Reassemble plain parts: concatenate the bodies of one category in part order.  Compact: concatenate the body lines of one category in part order, then base64 -d | gunzip.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
OUTD= PASTE=0 PLAN=0 PRINT=0 PART= NL=1500 ONLY= COMPACT=0 REFRESH=0 RANKS= GREP=; DRY=0
while [ $# -gt 0 ]; do case $1 in
  --out) OUTD=${2:?}; shift;; --paste) PASTE=1;; --plan) PLAN=1;; --print) PRINT=1;; --part) PART=${2:?}; shift;; --lines) NL=${2:?}; shift;; --only) ONLY=${2:?}; shift;;
  --compact) COMPACT=1;; --ranks) RANKS=${2:?}; shift;; --grep) GREP=${2:?}; shift;; --refresh) REFRESH=1;; --dry-run) DRY=1;; -h|--help) rl_help "$0"; exit 0;; *) echo "collect.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
rl_posint "$NL" || { echo "collect.sh: --lines a positive integer" >&2; exit 2; }
[ -z "$PART" ] || rl_posint "$PART" || { echo "collect.sh: --part a positive integer" >&2; exit 2; }
[ "$PLAN$PRINT$PASTE$DRY$REFRESH" != 00000 ] || { PLAN=1; PASTE=1; }
[ -d "$REMOTE_OUT" ] || { echo "collect.sh: no $REMOTE_OUT (REMOTE_OUT)" >&2; exit 2; }
[ -n "$OUTD" ] || OUTD=$REMOTE_OUT
rl_site_load
CATS="summaries stats nodes env nodes_full raw"
kvf() { grep -a -m1 "^$2=" "$1" 2>/dev/null | cut -d= -f2-; }
RUNS=$(cd "$REMOTE_OUT" && ls -d */ 2>/dev/null | tr -d / | grep -E '^[a-z0-9]+_[0-9]{8}-[0-9]{6}' | grep -v '^collect_' | sort)
STATS_RE='^(== ecalc|memory fit|config:|mnrun\.sh:|vpe:|omp team:|mem (phase|[a-z]+ +[0-9])|mem summary|recip\(mn\) |divmod\(mn|P24_CHECK|VERIFY|digits:|total|phases?_)|wait-stats|recip ts|tl +[a-z]+ [0-9]+: (init =|process age)|xshmem: (init|summary)|^comm_xshmem: (node-process|pe [0-9]+: DATA heap)|XSHMEM_ROUND_STATS|memsample|mn: pe [0-9]+: (tree levels|T1|T2)|mn: (pe|node) [0-9]+ of [0-9]+|T[12] +[0-9.]+ s'
sec() { grep -a -v -i -E '^[^=]*(KEY|TOKEN|PASS|SECRET|CRED|AUTH|COOKIE)[^=]*='; }
t() { rl_tmo "${TMOS:-60}" "$@" 2>&1; }
newest_freeze() { ls -d "$OUTD"/collect_[0-9]*/ 2>/dev/null | sort | tail -1 | sed 's,/$,,'; }
JOBS=$(for r in $RUNS; do grep -a -h -o -E '^(job|hold_job|jobid)=[0-9]+' "$REMOTE_OUT/$r/summary.txt" 2>/dev/null; grep -a -h -o -E 'SLURM_JOB_ID=[0-9]+' "$REMOTE_OUT/$r/log.txt" "$REMOTE_OUT/$r/capture/cmdline.txt" 2>/dev/null; done | grep -o '[0-9]*$' | sort -un)
[ -z "${REMOTE_JOB:-}" ] || JOBS=$(printf '%s\n%s\n' "$JOBS" "$REMOTE_JOB" | grep . | sort -un)
HOSTL=$(for r in $RUNS; do cat "$REMOTE_OUT/$r/capture/hosts.txt" 2>/dev/null; done | grep . | sort -u | paste -sd,)
if [ "$DRY" = 1 ]; then
    echo "collect.sh dry-run: would freeze $(echo $RUNS | wc -w) run dir(s) of $REMOTE_OUT into $OUTD/collect_<time>/ (categories: $CATS)"
    for r in $RUNS; do echo "    $r  $(du -sk "$REMOTE_OUT/$r" 2>/dev/null | cut -f1) kB"; done
    echo "  slurm jobs found: $(echo $JOBS | tr '\n' ' '); hosts with capture files: $(echo "$HOSTL" | tr ',' '\n' | grep -c .)"
    echo "  not frozen: digit files (*.part*, over 4 GiB); latest freeze: $(newest_freeze)"; exit 0
fi

build_freeze() {
    local TS FZ M W r f rel c
    TS=$(date +%Y%m%d-%H%M%S); FZ=$OUTD/collect_$TS; mkdir -p "$FZ/meta/slurm" "$FZ/work" || { echo "collect.sh: cannot create $FZ" >&2; exit 2; }
    M=$FZ/meta; W=$FZ/work
    [ -f "$SITE_ENV" ] && sec < "$SITE_ENV" > "$M/site.env"
    { echo "kit dir: $R"; echo "date: $(date '+%F %T %Z')"; echo "host: $(hostname)"; echo "user: $(rl_user)"
      if [ -f "$R/remote/MANIFEST.sha256" ]; then
          ( cd "$R" && sha256sum -c remote/MANIFEST.sha256 2>&1 ) > "$M/manifest_check.txt"; echo "manifest check: $(grep -c ': OK$' "$M/manifest_check.txt") OK, $(grep -c -v ': OK$' "$M/manifest_check.txt") not OK (see manifest_check.txt)"
      else echo "manifest check: no remote/MANIFEST.sha256"; fi
      if [ -e "$R/.git" ] && rl_have git; then echo "git commit: $(git -C "$R" rev-parse HEAD 2>&1)"; git -C "$R" status --short 2>&1 | head -20; else echo "git: no .git in the kit copy"; fi
      [ -f "$R/remote/VERSION" ] && cat "$R/remote/VERSION"; } > "$M/kit.txt" 2>&1
    # ---- binaries, modules, versions
    { for p in 1 2; do b=$(rl_bin $p); [ -n "$b" ] && [ -f "$b" ] && { sha256sum "$b"; ls -l "$b"; echo "-- ldd p$p"; t ldd "$b"; }; done
      for f in "$R"/ecalc/mnrun.sh "$R"/ecalc/mn_model.py "$R"/tools/plan_values.py; do [ -f "$f" ] && sha256sum "$f"; done; } > "$M/binaries.txt" 2>&1
    { rl_module_init; set +u; echo "-- module list"; module list; echo "-- site modules: p1 '$(rl_mods 1)' p2 '$(rl_mods 2)' unload '$(rl_unload)'"; } > "$M/modules.txt" 2>&1
    { echo "-- uname"; uname -a; echo "-- os-release"; head -4 /etc/os-release 2>/dev/null
      for c in "gcc --version" "cc --version" "hipcc --version" "amd-smi version" "fi_info --version" "srun --version" "python3 --version" "make --version"; do set -- $c; rl_have "$1" && { echo "-- $c"; t $c | head -8; }; done
      echo "-- rocm"; ls -d /opt/rocm* 2>/dev/null; cat /opt/rocm/.info/version 2>/dev/null
      echo "-- shmem / libfabric / cray packages"; ls /opt/cray/pe/sma /opt/cray/libfabric 2>/dev/null | head -20
      rl_have rpm && rpm -qa 2>/dev/null | grep -i -E 'libfabric|cray-shmem|openshmemx|dsmml|rocm-core|rocm-hip|cxi|slingshot' | sort | head -40
      echo "-- env of the kit"; env | sort | grep -a -E '^(REMOTE_|MNRUN_|ECALC_|XSHMEM_|FI_|SHMEM_|COMM_|SLURM_JOB|TGT_)' | sec; } > "$M/versions.txt" 2>&1
    # ---- Slurm
    mkdir -p "$M/slurm"
    for j in $JOBS; do
        if rl_have sacct; then TMOS=120 t sacct -j "$j" --format=ALL -P > "$M/slurm/sacct_$j.txt"; fi
        if rl_have scontrol; then t scontrol show job -dd "$j" > "$M/slurm/scontrol_job_$j.txt"; grep -q -i 'invalid job id' "$M/slurm/scontrol_job_$j.txt" 2>/dev/null && rm -f "$M/slurm/scontrol_job_$j.txt"; fi
    done
    if [ -n "$HOSTL" ]; then
        rl_have scontrol && { TMOS=120 t scontrol show node "$HOSTL" -o > "$M/slurm/scontrol_nodes.txt"; }
        rl_have sinfo && { t sinfo -N -n "$HOSTL" -o '%N %P %T %c %m %G %f %E' > "$M/slurm/sinfo_nodes.txt"; }
    fi
    rl_have sinfo && t sinfo -o '%P %a %l %D %T %N' | head -50 > "$M/slurm/sinfo.txt"
    rl_have scontrol && { TMOS=120 t scontrol show topology | head -c 4000000 > "$M/slurm/topology.txt"; }
    [ -n "$(ls -A "$M/slurm" 2>/dev/null)" ] || echo "no Slurm tool answered (or no job id found)" > "$M/slurm/NONE.txt"
    # ---- digit files (not bundled): path, size, printed hash
    { echo "# digit files are NOT frozen; path, bytes now, hash the run printed"
      for r in $RUNS; do f=$REMOTE_OUT/$r/summary.txt; dfd=$(kvf "$f" digit_files); [ -n "$dfd" ] || continue
          echo "$r digits=$(kvf "$f" digits) dir=$dfd size_bytes=$(du -cb "$dfd" 2>/dev/null | tail -1 | cut -f1) digit_hash=$(kvf "$f" digit_hash) last_digits=$(kvf "$f" last_digits)"; done; } > "$M/digit_files.txt"
    # ---- classify every file of every run dir into a category list (path relative to REMOTE_OUT)
    ( cd "$REMOTE_OUT" && for r in $RUNS; do find "$r" \( -type f -o -type l \) \( -name '*.part[0-9]*' -o -size +4G \) -print; done ) > "$M/skipped_files.txt"
    ( cd "$REMOTE_OUT" && for r in $RUNS; do find "$r" -type f | sort; done | grep -F -v -x -f "$M/skipped_files.txt" ) > "$W/files.lst"
    local k; for k in $CATS ranklogs; do : > "$W/l_$k"; done
    while IFS= read -r f; do
        r=${f%%/*}; rel=${f#*/}; case $rel in
          summary.txt|log.txt|capture/cmdline.txt) case $r in s5_*) k=summaries;; *) k=env;; esac;;
          capture/*|raw/fit_*) k=env;;
          raw/nodescreen.log|raw/build*.log|*/raw/build.log) k=raw;;
          raw/*.log|raw/*.log.gz) k=ranklogs;;
          snap/*|monitor/*) k=nodes_full;;
          *) k=raw;;
        esac
        echo "$f" >> "$W/l_$k"
    done < "$W/files.lst"
    # ---- streams
    emit() { local x; for x in "$@"; do printf '##### FILE %s (%s bytes) #####\n' "$x" "$(stat -c %s "$REMOTE_OUT/$x")"; case $x in *.gz) zcat "$REMOTE_OUT/$x";; *) cat "$REMOTE_OUT/$x";; esac; printf '\n'; done 2>/dev/null; }
    { echo "##### META digit_files.txt #####"; cat "$M/digit_files.txt"; echo "##### META kit.txt #####"; cat "$M/kit.txt"; emit $(cat "$W/l_summaries"); } > "$W/c_summaries"
    { while IFS= read -r x; do printf '##### FILE %s (diagnostic lines only, an identical repeat of a line (one per PE) is left to category raw) #####\n' "$x"; { case $x in *.gz) zcat "$REMOTE_OUT/$x";; *) cat "$REMOTE_OUT/$x";; esac; } 2>/dev/null | grep -a -E "$STATS_RE" | awk '!s[$0]++'; done < "$W/l_ranklogs"; } > "$W/c_stats"
    { for r in $RUNS; do [ -f "$REMOTE_OUT/$r/snap/nic_summary.csv" ] || continue
          echo "##### NODES roll-up $r: per node, sum over its NICs (bytes estimated from the packet size buckets) #####"; echo "host,nics,tx_gb,rx_gb,paused_counts,error_like_counts"
          awk -F, 'NR > 1 { h = $1; n[h]++; tx[h] += $5; rx[h] += $6; z[h] += $7 + $8; e[h] += $10 } END { for (h in n) printf "%s,%d,%.3f,%.3f,%.0f,%.0f\n", h, n[h], tx[h] / 1e9, rx[h] / 1e9, z[h], e[h] }' "$REMOTE_OUT/$r/snap/nic_summary.csv" | sort
          ls "$REMOTE_OUT/$r"/monitor/*.csv > /dev/null 2>&1 || continue
          sec_=$(kvf "$REMOTE_OUT/$r/summary.txt" fulllog | sed -n 's/.*every \([0-9]*\) s.*/\1/p'); sec_=${sec_:-30}
          echo "##### TIMELINE $r: across all nodes, bucket ${sec_} s (t = seconds from the first sample) #####"; echo "t_s,nodes,min_memavail_gb,max_gpu_hot_c,max_gpu_vram_gb,max_gpu_gtt_gb,nic_tx_total_gb_since_start"
          awk -F, -v S="$sec_" 'FNR == 1 { delete ix; for (i = 1; i <= NF; i++) ix[$i] = i; nc = NF; for (i = 1; i <= NF; i++) { if ($i ~ /_hot_c$/) hot[i] = 1; else if ($i ~ /_vram_mb$/) vr[i] = 1; else if ($i ~ /_gtt_mb$/) gt[i] = 1; else if ($i ~ /_tx_bytes_est$/) tx[i] = 1 }; next }
              { b = int($1 / S); if (!t0 || b < t0) t0 = b; h = $2; s = 0; for (i in tx) { if (!((h, i) in f0)) f0[h, i] = $i; s += $i - f0[h, i] }
                nn[b]++; if (!(b in mn) || $ix["memavail_kb"] < mn[b]) mn[b] = $ix["memavail_kb"]
                for (i in hot) if ($i + 0 > mh[b]) mh[b] = $i + 0; for (i in vr) if ($i + 0 > mv[b]) mv[b] = $i + 0; for (i in gt) if ($i + 0 > mg[b]) mg[b] = $i + 0; ts[b] += s }
              END { for (b in nn) printf "%d,%d,%.1f,%d,%.1f,%.1f,%.2f\n", (b - t0) * S, nn[b], mn[b] / 1e6, mh[b], mv[b] / 1e3, mg[b] / 1e3, ts[b] / 1e9 }' "$REMOTE_OUT/$r"/monitor/*.csv | sort -t, -k1,1n
      done; } > "$W/c_nodes"
    { echo "##### META site.env (secrets dropped) #####"; cat "$M/site.env" 2>/dev/null; for x in kit.txt binaries.txt modules.txt versions.txt; do echo "##### META $x #####"; cat "$M/$x" 2>/dev/null; done
      for x in "$M"/slurm/*; do echo "##### META slurm/${x##*/} #####"; cat "$x"; done; emit $(cat "$W/l_env"); } > "$W/c_env"
    { echo "##### META manifest_check.txt #####"; cat "$M/manifest_check.txt" 2>/dev/null; emit $(cat "$W/l_nodes_full" | grep -F -v -e '/snap/' ; cat "$W/l_nodes_full" | grep -F -e '/snap/'); } > "$W/c_nodes_full"
    { while IFS= read -r x; do printf '##### FILE %s (all lines except the first copy of each diagnostic line, which is in category stats; the repeats follow at the end) #####\n' "$x"; { case $x in *.gz) zcat "$REMOTE_OUT/$x";; *) cat "$REMOTE_OUT/$x";; esac; } 2>/dev/null > "$W/raw.tmp"; grep -a -v -E "$STATS_RE" "$W/raw.tmp"; echo "##### (the repeated copies of the diagnostic lines of $x) #####"; grep -a -E "$STATS_RE" "$W/raw.tmp" | awk 's[$0]++'; done < "$W/l_ranklogs"
      echo "##### META skipped_files.txt (digit parts and files over 4 GiB, not frozen) #####"; cat "$M/skipped_files.txt"; emit $(cat "$W/l_raw"); } > "$W/c_raw"
    # ---- plain files per category, compact (gzip -n: deterministic) streams, manifest of the sources
    for c in $CATS; do mv "$W/c_$c" "$FZ/$c.txt"; gzip -9 -n < "$FZ/$c.txt" | base64 -w 76 > "$FZ/$c.gz.b64"; done
    ( cd "$REMOTE_OUT" && xargs -d '\n' -r sha256sum < "$W/files.lst" 2>/dev/null ) > "$FZ/MANIFEST.txt"
    rm -rf "$W"
    cat > "$FZ/README.txt" <<EOT
ecalc remote session, frozen log set $(basename "$FZ") ($(date '+%F %T %Z'), collect.sh). Stays on the target; printed to the screen with: collect.sh --plan / --print.
Categories: summaries stats nodes env nodes_full raw (see the header of collect.sh); <category>.txt = plain stream, <category>.gz.b64 = gzip + base64 of it; MANIFEST.txt = sha256 of every source file;
meta/ = kit identity, binaries, modules, versions, Slurm accounting (sacct -j <job> --format=ALL -P), digit_files.txt (where the digit files are, size, printed hash). Slurm jobs: $(echo $JOBS | tr '\n' ' ')
EOT
    touch "$FZ/.done"; echo "$FZ"
}

FZ=$(newest_freeze)
if [ "$REFRESH" = 1 ] || [ -z "$FZ" ] || [ ! -e "$FZ/.done" ]; then FZ=$(build_freeze); NEW=1; else NEW=0; fi
NEWER=$(cd "$REMOTE_OUT" && for r in $RUNS; do find "$r" -type f -newer "$FZ/.done" 2>/dev/null; done | head -1)

RAWF_DEFAULT=first,last,busiest,slowest
make_rawf() {    # RANKS GREP -> RAWC (category name of the filtered raw stream, built once per spec)
    local h; h=$(printf '%s|%s' "$1" "$2" | sha256sum | cut -c1-8); RAWC=rawf_$h
    [ -s "$FZ/$RAWC.txt" ] || {
        RK="$1" GR="$2" awk 'function rk(l,  m) { if (match(l, /^ *[0-9]+: /)) { m = substr(l, RSTART, RLENGTH); gsub(/[^0-9]/, "", m); return m + 0 }
                if (match(l, /(^|[ (])pe [0-9]+[ :(]/)) { m = substr(l, RSTART, RLENGTH); gsub(/[^0-9]/, "", m); return m + 0 }
                if (match(l, /(^|[ (])node [0-9]+[ :(]/)) { m = substr(l, RSTART, RLENGTH); gsub(/[^0-9]/, "", m); return m + 0 } return -1 }
            FNR == 1 { fi++ }
            fi < 3 { r = rk($0); if (r >= 0) { if (!(r in seen)) { seen[r] = 1; if (mn == "" || r < mn) mn = r; if (r > mx) mx = r }
                if (fi == 2) lastl[r] = FNR; if (match($0, /device [0-9.]+ GB in use/)) { v = substr($0, RSTART + 7, RLENGTH - 17) + 0; if (v > pk[r]) pk[r] = v } } next }
            fi == 3 && FNR == 1 { n = split(ENVIRON["RK"], T, ","); for (i = 1; i <= n; i++) { t = T[i]
                    if (t == "first") sel[mn] = 1; else if (t == "last") sel[mx] = 1
                    else if (t == "busiest") { b = -1; for (r in pk) if (b < 0 || pk[r] > pk[b]) b = r; if (b >= 0) sel[b] = 1 }
                    else if (t == "slowest") { b = -1; for (r in lastl) if (b < 0 || lastl[r] > lastl[b]) b = r; if (b >= 0) sel[b] = 1 }
                    else if (t ~ /^[0-9]+-[0-9]+$/) { split(t, A, "-"); for (r = A[1]; r <= A[2]; r++) sel[r] = 1 } else if (t ~ /^[0-9]+$/) sel[t + 0] = 1 }
                    printf "##### FILTER ranks=%s grep=%s -> ranks kept:", ENVIRON["RK"], ENVIRON["GR"]; for (r in sel) printf " %s", r; printf " (ranks seen %s-%s; lines that name no rank are kept) #####\n", mn, mx }
            fi == 3 { if ($0 ~ /^##### /) { print; next } r = rk($0)
                ok = (ENVIRON["RK"] == "" || r < 0 || (r in sel)); if (ok && ENVIRON["GR"] != "" && $0 !~ ENVIRON["GR"]) ok = 0; if (ok) print }' "$FZ/stats.txt" "$FZ/raw.txt" "$FZ/raw.txt" > "$FZ/$RAWC.txt"
        gzip -9 -n < "$FZ/$RAWC.txt" | base64 -w 76 > "$FZ/$RAWC.gz.b64"; }
}
RAWC=raw; CATU=$CATS
if [ -n "$RANKS$GREP" ]; then make_rawf "$RANKS" "$GREP"; CATU=${CATS/raw/$RAWC}; fi
# ---- part table: global numbering over the non-empty categories, in order
declare -A CL CP; TOTP=0; PT=()      # lines (plain: text lines; compact: base64 lines), parts per category
for c in $CATU; do
    if [ "$COMPACT" = 1 ]; then n=$(wc -l < "$FZ/$c.gz.b64"); else n=$(wc -l < "$FZ/$c.txt"); fi
    [ "$n" -gt 0 ] && [ "$(wc -c < "$FZ/$c.txt")" -gt 1 ] || continue
    CL[$c]=$n; CP[$c]=$(( (n + NL - 1) / NL ))
    for ((j = 1; j <= CP[$c]; j++)); do PT+=("$c:$j"); done
done
TOTP=${#PT[@]}
plan() {
    echo "=== ECALC LOG PLAN ($(basename "$FZ")$([ $NEW = 1 ] && echo ', new freeze'), $NL lines per part) ==="
    echo "category    files     lines       bytes  parts(plain) | gz+base64 bytes  parts(compact)"
    local c tp=0 tc=0 tb=0 tg=0 fl nf pl pc by gb
    for c in $CATS; do
        by=$(wc -c < "$FZ/$c.txt"); gb=$(wc -c < "$FZ/$c.gz.b64"); fl=$(wc -l < "$FZ/$c.txt"); nf=$(grep -a -c '^##### ' "$FZ/$c.txt")
        pl=$(( (fl + NL - 1) / NL )); pc=$(( ($(wc -l < "$FZ/$c.gz.b64") + NL - 1) / NL )); [ "$by" -gt 1 ] || { pl=0; pc=0; }
        printf '%-10s %6s %9s %11s  %6s       | %11s  %6s\n' "$c" "$nf" "$fl" "$by" "$pl" "$gb" "$pc"; tp=$((tp + pl)); tc=$((tc + pc)); tb=$((tb + by)); tg=$((tg + gb))
    done
    printf '%-10s %6s %9s %11s  %6s       | %11s  %6s   (ratio plain/compact %s)\n' total "" "" "$tb" "$tp" "$tg" "$tc" "$(awk -v a="$tb" -v b="$tg" 'BEGIN { printf "%.2f", (b > 0) ? a / b : 0 }')"
    local keep=$RAWC; make_rawf "$RAWF_DEFAULT" ""; fl=$(wc -l < "$FZ/$RAWC.txt"); printf '%-10s %6s %9s %11s  %6s       | %11s  %6s   (raw filtered to ranks %s: --only raw --ranks %s)\n' raw-filt "" "$fl" "$(wc -c < "$FZ/$RAWC.txt")" "$(( (fl + NL - 1) / NL ))" "$(wc -c < "$FZ/$RAWC.gz.b64")" "$(( ($(wc -l < "$FZ/$RAWC.gz.b64") + NL - 1) / NL ))" "$RAWF_DEFAULT" "$RAWF_DEFAULT"; RAWC=$keep
    echo "print: collect.sh --print [--part K] [--only CAT] [--lines N] [--compact] [--ranks LIST] [--grep RE]; one part = one paste; --lines must stay the same between calls (part K is then identical every time)"
    [ -z "$NEWER" ] || echo "note: files newer than this freeze exist (a later run?): collect.sh --refresh makes a new freeze (part numbers then change)"
}
body_of() {      # CAT J -> the body lines of part J (plain or compact)
    local f=$FZ/$1.txt; [ "$COMPACT" = 1 ] && f=$FZ/$1.gz.b64
    sed -n "$(( ($2 - 1) * NL + 1 )),$(( $2 * NL ))p" "$f"
}
print_part() {   # K
    local k=$1 e c j body sha n tag=; e=${PT[$((k - 1))]}; c=${e%%:*}; j=${e##*:}
    body=$(mktemp "${TMPDIR:-/tmp}/cpart.XXXXXX"); body_of "$c" "$j" > "$body"; n=$(wc -l < "$body")
    if [ "$COMPACT" = 1 ]; then sha=$(base64 -d < "$body" | sha256sum | cut -c1-64); tag=" compact"; else sha=$(sha256sum < "$body" | cut -c1-64); fi
    case $c in rawf_*) c="raw[ranks=${RANKS:-all}${GREP:+ grep=$GREP}]";; esac
    echo "=== ECALC LOG PART $k/$TOTP $c$tag lines=$n sha256=$sha ==="
    if [ "$k" = 1 ]; then
        if [ "$COMPACT" = 1 ]; then echo "# decode: per category, join the body lines of its parts in order, then:  base64 -d | gunzip > <category>.txt   (part sha256 = of the decoded bytes of that part; $NL lines per part)"
        else echo "# reassemble: per category, join the bodies of its parts in order ($NL lines per part); part sha256 = of the body text as printed"; fi
    fi
    cat "$body"; echo "=== END PART $k/$TOTP ==="; rm -f "$body"
}
[ "$PLAN" = 1 ] && plan
if [ "$PASTE" = 1 ]; then
    echo "=== REMOTE collect DIGEST (one line per run) ==="
    echo "runs: n | nodes digits path verdict | total_s phases(init/bs/dm/dc) | apu_peak/plan GB | nic tx GB/node | verify last20 hash | settings"
    nl=0
    for r in $RUNS; do f=$REMOTE_OUT/$r/summary.txt; [ -f "$f" ] || continue; case $r in s5_*) ;; *) continue;; esac
        grep -aq 'verdict=SKIP' "$f" && continue; [ -n "$(kvf "$f" nodes)" ] || continue; nl=$((nl + 1)); [ $nl -le 48 ] || continue
        v=$(grep -a -m1 -o 'verdict=[A-Z]*' "$f" | cut -d= -f2)
        ph=$(grep -a -m1 '^phases_s:' "$f" | sed -E 's/.*init=([^ ]*) bs=([^ ]*) recip=[^ ]* dm=([^ ]*) dc=([^ ]*).*/\1\/\2\/\3\/\4/')
        pk=$(kvf "$f" device_peak_apu_gb | awk '{ print $1 }'); pl=$(kvf "$f" device_plan_apu_gb | awk '{ print $1 }')
        nic=$(kvf "$f" nic_gb_per_node | awk '{ print ($1 == "tx") ? $2 : "-" }')
        echo "$(printf '%s | %s %s p%s %s | %s %s | %s/%s | %s | %s %s %s | %s' "$nl" "$(kvf "$f" nodes)n" "$(kvf "$f" digits)" "$(kvf "$f" path)" "$v" "$(kvf "$f" total_s)" "${ph:-NA}" "${pk:-NA}" "${pl:-NA}" "${nic:--}" "$(kvf "$f" verify)" "$(kvf "$f" last_digits | tr -d ' ' | cut -c1-20)" "$(kvf "$f" digit_hash | awk '$1 ~ /^[0-9a-f]+$/ { print $1; next } { print "-" }')" "$(kvf "$f" settings | cut -c1-110)")" | rl_tilde
    done
    [ $nl -gt 0 ] || echo "(no s5_run summary under REMOTE_OUT)"
    [ $nl -le 48 ] || echo "... $((nl - 48)) more run(s) in the bundle"
    echo "frozen: $(basename "$FZ") on the target: ${TOTP} plain part(s) of $NL lines (collect.sh --plan, --print); nothing is copied off the target"
    echo "=== END collect DIGEST ==="
fi
if [ "$PRINT" = 1 ]; then
    [ "$TOTP" -gt 0 ] || { echo "collect.sh: nothing to print (no run dir under $REMOTE_OUT)" >&2; exit 1; }
    sel=(); for ((k = 1; k <= TOTP; k++)); do e=${PT[$((k - 1))]}; c=${e%%:*}; case $c in rawf_*) c=raw;; esac
        [ -z "$ONLY" ] || case ",$ONLY," in *",$c,"*) ;; *) continue;; esac
        [ -z "$PART" ] || [ "$PART" = "$k" ] || continue; sel+=("$k"); done
    [ ${#sel[@]} -gt 0 ] || { echo "collect.sh: no such part (parts 1..$TOTP; categories: $CATS)" >&2; exit 2; }
    for k in "${sel[@]}"; do print_part "$k"; done
fi
exit 0

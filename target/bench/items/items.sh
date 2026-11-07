#!/bin/bash
# items.sh ID - the bench items (contract: target/FORMATS.md §5); run.sh starts `bash items/items.sh ID`.  One function per item, run in its own process.
d=$(dirname "${BASH_SOURCE[0]}")

item_cor01() {
    # cor01 (TQ06) - 1-node correctness gate: ecalc 1e9 (D path 1, P path 1, C path 2) and 1e10 (D, C), VERIFY OK and the digit sha against the
    # known references (1e9 bf9f4b08758b40d9, 1e10 3fc025473586ed22).  Counts are NOT scaled on aac7 (cheap).  FAIL only when a path-1 arm is
    # wrong; path 2 trouble (or absent) gives PARTIAL.
    ecalc_have 1 || { tgt_kv note "no path-1 binary"; ecalc_end SKIP; }
    ecalc_refresh 900 && P2=1 || P2=0
    tgt_kv e.path2_available "$P2"
    BAD1=0; BAD2=0; SK2=0; SUM=; HD=; HC=
    arm() {   # TAG SHAPE DIGITS
        WRITE=1 ecalc_run "$1" "$2" 1 "$3" > /dev/null; ecalc_load "$1"; ecalc_kv "$1"
        SUM="$SUM; $(ecalc_brief "$1")"
        if [ "$E_SKIP" = 1 ]; then SK2=1; elif ! ecalc_ok; then [ "$2" = C ] && BAD2=1 || BAD1=1; fi
        case $2$3 in D1000000000) HD=$E_HASH;; C1000000000) HC=$E_HASH;; esac
    }
    arm d_1e9 D 1000000000
    arm c_1e9 C 1000000000
    arm p_1e9 P 1000000000
    arm d_1e10 D 10000000000
    arm c_1e10 C 10000000000
    [ -n "$HD" ] && [ -n "$HC" ] && { [ "$HD" = "$HC" ] && tgt_kv e.paths_equal_1e9 yes || tgt_kv e.paths_equal_1e9 no; }
    ecalc_q TQ06 "1 node:${SUM#;} (refs 1e9 bf9f4b08758b40d9, 1e10 3fc025473586ed22)"
    if [ $BAD1 = 1 ]; then tgt_kv note "a path-1 arm is wrong: do not run the ladder before this is understood"; ecalc_end FAIL
    elif [ $BAD2 = 1 ]; then ecalc_end PARTIAL
    elif [ $SK2 = 1 ] && ! ecalc_aac7; then ecalc_end PARTIAL
    elif [ $SK2 = 1 ]; then tgt_kv note "path 2 arms skipped (aac7 has no path-2 build)"; ecalc_end PASS
    else ecalc_end PASS; fi
}

item_mem01() {
    # mem01 (TQ04, TQ05) - the node's memory edges on one node, each form ALONE: t_edge dev / vmm (device pools), host / malloc (host side),
    # then one ecalc 1e9 (layout D) with DB_POOL_VERBOSE=1 for the VMM map rate; snapshots of meminfo, ulimit, THP.
    # Metrics: m.edge_<form>_gb (node total, touched), m.map_s_per_gb; suggestion e.suggest_ECALC_DEVICE_GB / _NODE_GB.
    PN=1; ecalc_have 1 || PN=2
    T=$(ecalc_tool $PN tests/t_edge)
    [ -x "$T" ] || { tgt_kv note "no t_edge in the build tree (bld01/bld02 absent)"; ecalc_end SKIP; }
    STEP_D=${MEM01_STEP_DEV:-4}; CAP_D=${MEM01_CAP_DEV:-520}; STEP_H=${MEM01_STEP_HOST:-16}; CAP_H=${MEM01_CAP_HOST:-520}
    { echo "== meminfo"; grep -E '^(MemTotal|MemFree|MemAvailable|Cached|HugePages_Total|Hugepagesize|Shmem|SwapTotal)' /proc/meminfo
      echo "== ulimit"; ulimit -a; echo "== THP"; cat /sys/kernel/mm/transparent_hugepage/enabled 2>&1; echo "== overcommit"; cat /proc/sys/vm/overcommit_memory /proc/sys/vm/overcommit_ratio 2>&1
      echo "== numa"; command -v numactl >/dev/null && numactl -H 2>&1 | head -20; } > raw/snapshot_login.txt 2>&1
    srun --jobid="$JOBID" -N1 -n1 $([ "${TGT_SRUN_NO_W:-0}" = 1 ] || echo "-w $(ecalc_hosts 0 1)") ${TGT_SRUN_EXTRA:-} --overlap bash -c 'grep -E "^(MemTotal|MemFree|MemAvailable|HugePages_Total)" /proc/meminfo; ulimit -l; cat /sys/kernel/mm/transparent_hugepage/enabled' > raw/snapshot_node.txt 2>&1
    edge() {   # FORM STEP CAP
        TMO=${MEM01_TMO:-6} ecalc_srun edge_$1 $PN 1 "$T" "$1" "$2" "$3"
        local lg=$OUT/raw/edge_$1.log v fl av
        v=$(sed -n "s/^t_edge: the edge in form $1: \([0-9.]*\) GB.*/\1/p" "$lg" | tail -1)
        fl=$(grep -a -m1 'failed' "$lg" | cut -c1-200); av=$(sed -n 's/.*MemAvailable \([0-9.]*\) GB at the start.*/\1/p' "$lg" | head -1)
        if [ -z "$v" ]; then   # the kernel's OOM killer (host / malloc forms) ends the program before it reports: the last committed step is a lower bound of the edge
            v=$(sed -n 's/^t_edge: *\([0-9.]*\) GB committed in [0-9]* steps.*/\1/p' "$lg" | tail -1)
            [ -n "$v" ] && fl="killed by the kernel (OOM) after the step that committed $v GB; the edge lies within one step above"
        fi
        if [ -n "$v" ]; then tgt_kv m.edge_$1_gb "$v GB"; [ -n "$fl" ] && tgt_kv e.edge_$1_stop "$fl" || tgt_kv e.edge_$1_stop "cap $3 GB reached"
        else tgt_kv e.edge_$1_stop "no result: $(tail -1 "$lg" | cut -c1-160)"; fi
        [ -n "$av" ] && tgt_kv m.memavail_before_$1_gb "$av GB"
        eval "EDGE_$1=\${v:-}"
    }
    edge dev "$STEP_D" "$CAP_D"; edge vmm "$STEP_D" "$CAP_D"; edge host "$STEP_H" "$CAP_H"; edge malloc "$STEP_H" "$CAP_H"
    # the VMM map rate as ecalc sees it (a small run, layout D, 1 node)
    ecalc_have 1 && { TMO=3 ecalc_run map D 1 "$(ecalc_digits 1e9)" DB_POOL_VERBOSE=1; ecalc_kv map
      r=$(ecalc_grab "$E_LOG" 'dbig pool: APU[0-9]+ VMM arena' | sed -nE 's/.*\(([0-9.]+) s\/GB\).*/\1/p')
      all=$( { zcat -f "$E_LOG" 2>/dev/null || cat "$E_LOG"; } | sed -E 's/^ *[0-9]+: //' | sed -nE '/dbig pool: APU[0-9]+ VMM arena/ s/.*\(([0-9.]+) s\/GB\).*/\1/p' | tr '\n' ' ')
      [ -n "$all" ] && tgt_kv m.map_s_per_gb "$(ecalc_mean $all) s/GB"; }
    ecalc_q TQ04 "device edge per node (4 APUs): hipMalloc ${EDGE_dev:-NA} GB, VMM ${EDGE_vmm:-NA} GB (steps ${STEP_D} GB, cap ${CAP_D}); per APU ~$(awk -v v="${EDGE_vmm:-0}" 'BEGIN { printf "%.1f", v / 4 }') GB; ECALC_DEVICE_GB suggestion = the VMM edge minus the guard"
    ecalc_q TQ05 "host edge per node: hipHostMalloc ${EDGE_host:-NA} GB, malloc ${EDGE_malloc:-NA} GB (steps ${STEP_H} GB, cap ${CAP_H}); MemAvailable at start $(sed -n 's/^MemAvailable:[ ]*//p' raw/snapshot_login.txt | head -1); ECALC_NODE_GB suggestion = the smaller edge minus the guard"
    [ -n "${EDGE_vmm:-}" ] && tgt_kv e.suggest_ECALC_DEVICE_GB "$EDGE_vmm"
    { [ -n "${EDGE_host:-}" ] || [ -n "${EDGE_malloc:-}" ]; } && tgt_kv e.suggest_ECALC_NODE_GB "$(ecalc_stat "${EDGE_host:-${EDGE_malloc}}" "${EDGE_malloc:-${EDGE_host}}" | awk '{print ($1<$2)?$1:$2}')"
    if [ -n "${EDGE_vmm:-}" ] && [ -n "${EDGE_host:-}" ]; then ecalc_end PASS; elif [ -n "${EDGE_vmm:-}${EDGE_dev:-}${EDGE_host:-}${EDGE_malloc:-}" ]; then ecalc_end PARTIAL; else ecalc_end FAIL; fi
}

item_net02() {
    # net02 (TQ13 TQ14 TQ15) - t_xshmem lat, stream, a2a on 2 nodes x 4 PEs (off-node pairs), 64 MiB max message, per path and lane count:
    # p1 lanes 1 / 2 (libfabric), p2 lanes 1 / 2 (cray-shmem), run one after the other on the same 2 nodes.  Per-NIC counters (CTRSUM) are kept.
    # NET02_NODES (default 2): the aac7 dry run uses 1 (4 PEs on one node: the off-node rows are then empty and read NA).
    # Metrics per arm: lat 8 B and 4 KiB (us per hop, mean), stream peak GB/s per PE (and per node), a2a 64 MiB GB/s per PE (and per node).
    ecalc_refresh 900 && P2=1 || P2=0
    RES=; QA=; QL=; QS=
    arm() {   # TAG PATH LANES
        local tag=$1 pn=$2 ln=$3
        ecalc_have $pn || { tgt_kv e.$tag "skipped (no binary of path $pn)"; return; }
        TMO=5 XT_MAXMB=64 XT_REPS=3 ecalc_xt "$tag" "$pn" "${NET02_NODES:-2}" "lat stream a2a" XSHMEM_LANES=$ln
        [ "$X_SKIP" = 1 ] && { tgt_kv e.$tag "skipped"; return; }
        tgt_kv e.${tag}_result "$X_RESULT"; tgt_kv m.${tag}_wall_s "$X_WALL_S s"
        local l8 l4k sp spn a2 a2n
        l8=$(ecalc_xt_col $tag lat 3 'pingpong.* 8 B$' first); l4k=$(ecalc_xt_col $tag lat 3 'pingpong.* 4096 B$' first)
        sp=$(ecalc_xt_col $tag stream 6 'off-node' max); spn=$(ecalc_xt_col $tag stream 8 'off-node' max)
        a2=$(ecalc_xt_col $tag a2a 7 ' 67108864$' max); a2n=$(ecalc_xt_col $tag a2a 9 ' 67108864$' max)
        [ -n "$l8" ] && tgt_kv m.${tag}_lat_8b_us "$l8 us"; [ -n "$l4k" ] && tgt_kv m.${tag}_lat_4k_us "$l4k us"
        [ -n "$sp" ] && tgt_kv m.${tag}_stream_pe_gbs "$sp GB/s"; [ -n "$spn" ] && tgt_kv m.${tag}_stream_node_gbs "$spn GB/s"
        [ -n "$a2" ] && tgt_kv m.${tag}_a2a64m_pe_gbs "$a2 GB/s"; [ -n "$a2n" ] && tgt_kv m.${tag}_a2a64m_node_gbs "$a2n GB/s"
        zcat -f raw/$tag.log* 2>/dev/null | sed -E 's/^ *[0-9]+: //' | grep -a -E '^(CTRSUM|MAP pe)' > raw/$tag.ctr
        QL="$QL; $tag ${l8:-NA}/${l4k:-NA} us"; QS="$QS; $tag ${sp:-NA} GB/s per PE (${spn:-NA} node)"; QA="$QA; $tag ${a2:-NA} GB/s per PE (${a2n:-NA} node)"
        RES="$RES $X_RESULT"
    }
    arm xp1_l1 1 1; arm xp1_l2 1 2; arm xp2_l1 2 1; arm xp2_l2 2 2
    ecalc_q TQ13 "stream peak, off-node, 64 MiB max:${QS#;} (counters in raw/*.ctr)"
    ecalc_q TQ14 "lat 8 B / 4 KiB per hop:${QL#;}"
    ecalc_q TQ15 "a2a 64 MiB per PE:${QA#;}"
    case "$RES" in *PASS*) ;; *) ecalc_end FAIL;; esac
    case "$RES" in *FAIL*|*NONE*) ecalc_end PARTIAL;; esac
    ecalc_end PASS
}

item_net04() {
    # net04 (TQ16) - all-to-all rate per PE vs node count: t_xshmem a2a (64 MiB max) on 2, 4, 8 nodes of the 8-node allocation, path 1 then path 2 (NET04_NODES="1" for the aac7 dry run).
    ecalc_refresh 900 && P2=1 || P2=0
    QS=; RES=
    for pn in 1 2; do
        [ $pn = 2 ] && [ $P2 = 0 ] && continue
        ecalc_have $pn || continue
        base=
        for n in ${NET04_NODES:-2 4 8}; do tag=a2a_p${pn}_n$n
            TMO=4 XT_MAXMB=64 XT_REPS=3 ecalc_xt $tag $pn $n "a2a"
            [ "$X_SKIP" = 1 ] && continue
            a=$(ecalc_xt_col $tag a2a 7 ' 67108864$' max); an=$(ecalc_xt_col $tag a2a 9 ' 67108864$' max)
            tgt_kv e.${tag}_result "$X_RESULT"; tgt_kv m.${tag}_wall_s "$X_WALL_S s"
            [ -n "$a" ] && { tgt_kv m.${tag}_pe_gbs "$a GB/s"; tgt_kv m.${tag}_node_gbs "$an GB/s"; [ -z "$base" ] && base=$a
                tgt_kv m.${tag}_vs_2n_pct "$(awk -v a="$a" -v b="$base" 'BEGIN { printf "%.1f", b ? 100 * a / b : 0 }') %"; }
            QS="$QS; p$pn ${n}n ${a:-NA} GB/s per PE"; RES="$RES $X_RESULT"
            zcat -f raw/$tag.log* 2>/dev/null | sed -E 's/^ *[0-9]+: //' | grep -a '^CTRSUM' > raw/$tag.ctr
        done
    done
    ecalc_q TQ16 "a2a 64 MiB per PE (GB/s) by nodes:${QS#;}; fall-off = ratio to the 2-node value in m.*_vs_2n_pct"
    case "$RES" in *PASS*) ;; *) ecalc_end FAIL;; esac
    case "$RES" in *FAIL*|*NONE*) ecalc_end PARTIAL;; esac
    ecalc_end PASS
}

item_net05() {
    # net05 (TQ17) - 16, 32, 64 nodes: does the launch start cleanly (FI_UNIVERSE_SIZE / PMI bootstrap) and how do small-message latency and the
    # all-to-all (8 MiB messages: the heap per PE stays small) scale.  Path 1 at 16/32/64, then path 2 at 64/32/16.  Launch time = wall of the run.
    NH=$(echo "$HOSTS" | tr ',' '\n' | grep -c .)
    ecalc_refresh 600 && P2=1 || P2=0
    QS=; QL=; RES=
    one() {   # PATH NODES
        local pn=$1 n=$2 tag=big_p$1_n$2
        [ "$n" -le "$NH" ] || { tgt_kv e.$tag "skipped (only $NH hosts)"; return; }
        ecalc_have $pn || return
        TMO=5 XT_MAXMB=8 XT_REPS=3 XT_TIMEOUT=200 ecalc_xt $tag $pn $n "lat a2a"
        [ "$X_SKIP" = 1 ] && return
        local a an l; a=$(ecalc_xt_col $tag a2a 7 ' 8388608$' max); an=$(ecalc_xt_col $tag a2a 9 ' 8388608$' max); l=$(ecalc_xt_col $tag lat 3 'pingpong.* 8 B$' first)
        tgt_kv e.${tag}_result "$X_RESULT"; tgt_kv m.${tag}_launch_wall_s "$X_WALL_S s"; tgt_kv m.${tag}_rc "$X_RC"
        [ -n "$a" ] && tgt_kv m.${tag}_a2a8m_pe_gbs "$a GB/s"; [ -n "$an" ] && tgt_kv m.${tag}_a2a8m_node_gbs "$an GB/s"; [ -n "$l" ] && tgt_kv m.${tag}_lat_8b_us "$l us"
        grep -a -c 'REFUSED\|Abort\|abort\|PMI.*fail' raw/$tag.log* > /dev/null 2>&1 && tgt_kv e.${tag}_errors "$(zcat -f raw/$tag.log* | grep -a -m3 'REFUSED\|Abort\|abort\|PMI.*fail' | cut -c1-120 | tr '\n' '/')"
        QS="$QS; p$pn ${n}n ${a:-NA}"; QL="$QL; p$pn ${n}n ${l:-NA} us (launch ${X_WALL_S}s, $X_RESULT)"; RES="$RES $X_RESULT"
    }
    N5=${NET05_NODES:-16 32 64}; N5R=$(echo $N5 | tr " " "\n" | tac | tr "\n" " ")   # NET05_NODES="1" for the aac7 dry run
    for n in $N5; do one 1 $n; done
    [ $P2 = 1 ] && for n in $N5R; do one 2 $n; done
    ecalc_q TQ17 "a2a 8 MiB per PE (GB/s):${QS#;}; 8 B ping-pong and launch:${QL#;}"
    case "$RES" in *PASS*) ;; *) ecalc_end FAIL;; esac
    case "$RES" in *FAIL*|*NONE*) ecalc_end PARTIAL;; esac
    ecalc_end PASS
}

item_io01() {
    # io01 - writer rate at 1 node (TQ54): tools/wbench, direct mode, 8 vs 32 threads, plus buffered 8 threads; 8 GB per run (4 GB on aac7).
    WB=$(wb_bin); [ "$WB" != 0 ] || skip "no tools/wbench"
    DIR=$(wb_dir); SZ=8; site_is_aac7 && SZ=4
    for a in "direct 8" "direct 32" "buffered 8"; do
        set -- $a; tleft_ok 0.85 || break; t=wb_$1_$2
        ecalc_srun "$t" 1 1 "$WB" -m "$1" -t "$2" -c 256 -s "$SZ" -r "$DIR/io01-%h-%r.dat" || note "$t rc $?"
        S=$(wb_kv "$OUT/raw/$t.log" "$t"); R="$R $1/$2 thr ${S} GB/s;"
    done
    tgt_kv q.TQ54 "write per node:$R (8 vs 32 threads direct, then buffered 8); 1 node, $SZ GB"
    OPT_NO_RUNS_OK=1; finish
}

item_io02() {
    # io02 - aggregate write rate at 64 nodes (TQ55): per-node files vs one shared file name (all ranks write the same file: a lock/contention bound, not a striped partitioned write), direct, 8 threads.  IO02_SZ (GB per rank, 4), IO02_NODES (64).
    WB=$(wb_bin); [ "$WB" != 0 ] || skip "no tools/wbench"
    N=${IO02_NODES:-64}; [ "$(hosts_n)" -ge $N ] || N=$(hosts_n); DIR=$(wb_dir); SZ=${IO02_SZ:-4}; site_is_aac7 && SZ=2
    ecalc_srun wb_pernode 1 $N "$WB" -m direct -t 8 -c 256 -s "$SZ" "$DIR/io02-%h-%r.dat" || note "per-node rc $?"
    A=$(wb_kv "$OUT/raw/wb_pernode.log" wb_pernode)
    tleft_ok 0.7 && { ecalc_srun wb_shared 1 $N "$WB" -m direct -t 8 -c 256 -s "$SZ" -k "$DIR/io02-shared.dat" || note "shared rc $?"; B=$(wb_kv "$OUT/raw/wb_shared.log" wb_shared); }
    rm -f "$DIR/io02-shared.dat"
    tgt_kv q.TQ55 "aggregate write at $N nodes: per-node files ${A:-NA} GB/s, one shared file ${B:-NA} GB/s ($SZ GB per rank, direct)"
    OPT_NO_RUNS_OK=1; finish
}

item_io03() {
    # io03 - striping (TQ56): wbench at 8 nodes into directories with stripe count 1, 4, 16, all OSTs (lfs setstripe when present), per-node files, direct, 8 threads; the file system type and lfs getstripe of the base dir go to the summary.  IO03_SZ (GB per rank, 4), IO03_NODES (8), IO03_ARMS ("1 4 16 -1"; default | COUNT | COUNT:SIZE).
    WB=$(wb_bin); [ "$WB" != 0 ] || skip "no tools/wbench"
    N=${IO03_NODES:-8}; [ "$(hosts_n)" -ge $N ] || N=$(hosts_n); DIR=$(wb_dir); SZ=${IO03_SZ:-4}; site_is_aac7 && SZ=2
    tgt_kv e.fs "$(stat -f -c %T "$DIR")"
    command -v lfs >/dev/null 2>&1 && { tgt_kv e.lfs_getstripe "$(lfs getstripe -d "$DIR" 2>&1 | tr '\n' ' ' | cut -c1-200)"; HAVE_LFS=1; } || note "no lfs: stripe arms collapse to the default layout"
    for c in ${IO03_ARMS:-1 4 16 -1}; do      # arm = default (no setstripe: the PFL layout) | COUNT | COUNT:SIZE (e.g. 4:32M); the tag swaps ":" for "_" and "-" for "all"
        tleft_ok 0.85 || break
        ct=${c//:/_}; ct=${ct//-1/all}; cc=${c%%:*}; cs=; [ "$cc" != "$c" ] && cs=${c#*:}
        D2=$DIR/s$ct; mkdir -p "$D2"; [ -n "${HAVE_LFS:-}" ] && [ "$c" != default ] && lfs setstripe -c "$cc" ${cs:+-S "$cs"} "$D2" 2>/dev/null   # arm "default": the directory keeps the file system's own (PFL) layout
        ecalc_srun wb_s$ct 1 $N "$WB" -m direct -t 8 -c 256 -s "$SZ" "$D2/io03-%h-%r.dat" || note "stripe $c rc $?"
        S=$(wb_kv "$OUT/raw/wb_s$ct.log" "wb_s$ct"); R="$R c$ct=${S}GB/s"; [ -n "${HAVE_LFS:-}" ] && tgt_kv e.getstripe_$ct "$(lfs getstripe -d "$D2" 2>&1 | tr '\n' ' ' | cut -c1-160)"
    done
    tgt_kv q.TQ56 "aggregate write at $N nodes by stripe count:$R"
    OPT_NO_RUNS_OK=1; finish
}

item_io04() {
    # io04 - read-back rates (TQ57): wbench write+read (direct and buffered) of a 4 GB file on 1 node, and tools/unpack_digits over the digit parts of an ecalc 1e9 run (WRITE=1, KEEP=1).
    WB=$(wb_bin); [ "$WB" != 0 ] || skip "no tools/wbench"
    DIR=$(wb_dir); SZ=4
    for m in direct buffered; do
        ecalc_srun wb_$m 1 1 "$WB" -m $m -t 8 -c 256 -s "$SZ" -r "$DIR/io04-%h.dat" || note "$m rc $?"
        wb_kv "$OUT/raw/wb_$m.log" "wb_$m" > /dev/null
    done
    UN=$(tool 1 tools/unpack_digits)
    if [ "$UN" != 0 ] && need_path 1; then
        WRITE=1 KEEP=1 ecalc_run w1e9 D 1 "$(ecalc_digits 1e9)" || true; rec w1e9 W 1
        SD=${TGT_SCRATCH:-${ECALC_OUT_DIR:-$TGT_ROOT/build/scratch}}/${ID:-item}-$$-w1e9; tgt_kv e.kept_parts_dir "$SD"
        P=$(ls "$SD"/w1e9.out.part* "$SD"/w1e9.out 2>/dev/null | sort)
        if [ -n "$P" ]; then
            BY=$(du -cb $P | tail -1 | cut -f1); T1=$(date +%s.%N)
            ecalc_srun unpack 1 1 bash -c "$UN -q $P | wc -c"; T2=$(date +%s.%N)
            tgt_kv m.unpack_s "$(awk -v a=$T1 -v b=$T2 'BEGIN { printf "%.2f", b - a }') s"; tgt_kv m.unpack_bytes "$BY B"
            tgt_kv m.unpack_gbs "$(awk -v a=$T1 -v b=$T2 -v y=$BY 'BEGIN { printf "%.3f", y / (b - a) / 1e9 }') GB/s"
        else note "no digit parts found for unpack_digits"; fi
    fi
    g() { grep -h "^m.$1=" "$TGT_SUM_DIR/summary.txt" | tail -1 | cut -d= -f2; }
    tgt_kv q.TQ57 "wbench read-back direct $(g wb_direct.read_mean_gbs), buffered $(g wb_buffered.read_mean_gbs); unpack_digits $(g unpack_gbs) over the written parts (1 node)"
    OPT_NO_RUNS_OK=1; finish
}

case $1 in
    bld01) exec bash -l "$d/_build.sh" p1;;    # path 1: default build (XSHMEM=ofi, layout D) into build/<sha7>-p1/; login node
    bld02) exec bash -l "$d/_build.sh" p2;;    # path 2: XSHMEM=cray (run with ECALC_PE_PER_GPU=1 XSHMEM_LANES=2) into build/<sha7>-p2/; login node
    cor01) . "$d/_ecalc.sh"; item_cor01;;
    mem01) . "$d/_ecalc.sh"; item_mem01;;
    net02) . "$d/_ecalc.sh"; item_net02;;
    net04) . "$d/_ecalc.sh"; item_net04;;
    net05) . "$d/_ecalc.sh"; item_net05;;
    io01) . "$d/_opt.sh"; item_io01;;
    io02) . "$d/_opt.sh"; item_io02;;
    io03) . "$d/_opt.sh"; item_io03;;
    io04) . "$d/_opt.sh"; item_io04;;
    *) echo "items.sh: unknown item $1" >&2; exit 2;;
esac

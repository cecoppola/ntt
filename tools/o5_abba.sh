#!/bin/bash
# tools/o5_abba.sh - O5 (K7-a, results/O5.md): single-node paired ABBA of two environment settings of ecalc with a stop rule.
# Source it, then: o5_abba <outdir> <hold-jobid> <digits> <max-rounds> <time-budget-s> "<env A>" "<env B>" [extra env for both]
#   Each round = the pair A B then B A (4 runs, 2 paired differences B - A).  After every round from the third on it stops when
#   the paired t-test gives p < 0.05, or the 95 % CI half-width of the mean difference is <= O5_CI_HALF (default 0.15 s),
#   or the rounds reach <max-rounds>, or the time budget cannot hold another round (the last round's wall x 1.1).
#   Metrics (from the ecalc log): `total` wall (s) and the B-form products' seconds of the rns_dist strategy report
#   (`B N products X s` / `B5 N products X s`).  Writes <outdir>/pairs.tsv (round, arm, rc, verify, total, bform, stamp) and
#   <outdir>/abba.txt (n, mean difference, sd, t, p-band, 95 % CI, verdict) per metric.  Runs through mnrun.sh on one node
#   (no digit output, no -w).  STUB=1: fake runs (random totals), no srun.  Never cancels anything.
o5_run_one() {   # <log> <digits> <env words...>: one ecalc run on the hold $O5_J; prints "rc total bform verify"
    local log=$1 dig=$2; shift 2
    if [ "${STUB:-0}" = 1 ]; then
        local t=$(awk -v s=$RANDOM 'BEGIN{srand(s); printf "%.2f", 20 + rand()}')
        printf 'stub run\nrns_dist: RNS_STRATEGY=auto form B: C 1 products 1.00 s, B 2 products %.2f s, B4 0 products 0.00 s, B5 0 products 0.00 s; extra plane memory 0.00 GiB/APU (allocations 0.00 s)\nVERIFY OK\ntotal   %s s   (bs 1)\n' "$t" "$t" > "$log"
        sleep 1
    else
        ( cd "$O5_E" && SLURM_JOB_ID=$O5_J MNRUN_NODES=1 MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) \
          timeout "${O5_RUN_TMO:-900}" ./mnrun.sh 1 env "$@" ECALC_SEGV_TRACE=1 ./ecalc "$dig" ) > "$log" 2>&1 < /dev/null
    fi
    local rc=$? pl v tot bf
    pl=$(sed -E 's/^ *[0-9]+: //' "$log")
    v=none; echo "$pl" | grep -aqE 'VERIFY OK' && v=OK
    tot=$(echo "$pl" | grep -a -m1 '^total' | awk '{print ($2 ~ /^[0-9.]+$/) ? $2 : "NA"}'); tot=${tot:-NA}
    bf=$(echo "$pl" | grep -a -m1 '^rns_dist: RNS_STRATEGY=' | sed -E 's/.* B ([0-9]+) products ([0-9.]+) s, B4 [0-9]+ products ([0-9.]+) s, B5 [0-9]+ products ([0-9.]+) s.*/\2 \3 \4/' | awk '{print $1 + $2 + $3}'); bf=${bf:-NA}
    echo "$rc $tot $bf $v"
}
o5_stats() {   # <pairs.tsv> <column: 5 total | 6 bform> <name>: paired B - A statistics, appends to abba.txt; prints "n mean half stop"
    awk -v col=$2 -v name=$3 -F'\t' '
    $3 == 0 && $4 == "OK" && $col != "NA" { v[$1 "_" $2 "_" $7] = $col; seen[$1 "_" $7] = 1 }
    END {
        n = 0; for (k in seen) { split(k, a, "_"); if ((a[1] "_A_" a[2]) in v && (a[1] "_B_" a[2]) in v) { d[++n] = v[a[1] "_B_" a[2]] - v[a[1] "_A_" a[2]] } }
        if (n < 2) { printf "%s: n=%d (not enough pairs)\n", name, n; printf "%d NA NA 0\n", n > "/dev/stderr"; exit }
        s = 0; for (i = 1; i <= n; i++) s += d[i]; m = s / n; ss = 0; for (i = 1; i <= n; i++) ss += (d[i] - m)^2; sd = sqrt(ss / (n - 1)); se = sd / sqrt(n)
        split("12.706 4.303 3.182 2.776 2.571 2.447 2.365 2.306 2.262 2.228 2.201 2.179 2.160 2.145 2.131 2.120", tc, " "); df = n - 1; if (df > 16) df = 16
        t = se > 0 ? m / se : 0; half = tc[df] * se; p = (t < 0 ? -t : t) > tc[df] ? "p<0.05" : "p>=0.05"
        printf "%s: n=%d pairs, mean B-A %+.3f s, sd %.3f, se %.3f, t %.2f (%s), 95%% CI [%+.3f, %+.3f] s\n", name, n, m, sd, se, t, p, m - half, m + half
        printf "%d %.3f %.3f %d\n", n, m, half, (p == "p<0.05" || half <= ENVIRON["O5_CI_HALF"] + 0) ? 1 : 0 > "/dev/stderr"
    }' "$1"
}
o5_abba() {
    local out=$1 J=$2 dig=$3 maxr=$4 budget=$5 envA=$6 envB=$7; shift 7; local extra="$*"
    export O5_J=$J O5_CI_HALF=${O5_CI_HALF:-0.15}
    mkdir -p "$out/log"; local P=$out/pairs.tsv; : > "$P"; : > "$out/abba.txt"
    local t0=$(date +%s) r=1 lastw=0 stop=0
    echo "o5_abba: digits $dig, A='$envA' B='$envB' extra='$extra', max $maxr rounds, budget ${budget}s, hold $J" | tee -a "$out/abba.txt"
    while [ $r -le $maxr ]; do
        local rt=$(date +%s) order="A B B A" k=0 arm
        for arm in $order; do
            k=$((k + 1)); local pos=$(( (k - 1) / 2 + 1 ))      # pair 1 = A B, pair 2 = B A
            local e; [ $arm = A ] && e=$envA || e=$envB
            local res; res=$(o5_run_one "$out/log/r${r}_${pos}_${arm}.log" "$dig" $extra $e)
            echo -e "$r\t$arm\t$(echo $res | awk '{print $1 "\t" $4 "\t" $2 "\t" $3}')\t$pos\t$(date +%m-%d_%H:%M:%S)" >> "$P"
            echo "round $r pair $pos arm $arm: rc/total/bform/verify = $res"
            [ -e "$out/STOP" ] && { echo "STOP file seen" | tee -a "$out/abba.txt"; stop=1; break; }
        done
        lastw=$(( $(date +%s) - rt ))
        local st1 st2
        st1=$(o5_stats "$P" 5 "total($dig)" 2>&1 >> "$out/abba.txt" | tail -1); st2=$(o5_stats "$P" 6 "bform($dig)" 2>&1 >> "$out/abba.txt" | tail -1)
        echo "after round $r (${lastw}s): total [n mean half stop] $st1; bform $st2" | tee -a "$out/abba.txt"
        [ $stop = 1 ] && break
        if [ $r -ge 3 ] && [ "$(echo $st1 | awk '{print $4}')" = 1 ]; then echo "stop rule met (total) after round $r" | tee -a "$out/abba.txt"; break; fi
        if [ $(( $(date +%s) - t0 + lastw * 11 / 10 )) -gt $budget ]; then echo "time budget: no room for round $((r + 1))" | tee -a "$out/abba.txt"; break; fi
        r=$((r + 1))
    done
    echo "---- final ----" >> "$out/abba.txt"; o5_stats "$P" 5 "total($dig)" >> "$out/abba.txt" 2>/dev/null; o5_stats "$P" 6 "bform($dig)" >> "$out/abba.txt" 2>/dev/null
    tail -3 "$out/abba.txt"
}

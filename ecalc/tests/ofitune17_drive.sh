#!/bin/bash
# ofitune17_drive.sh - Phase 17 TUNE17 (results/TUNE17.md): comm_ofi tuning on aac7 inside job 12287, fire-and-forget (setsid nohup).
#   cd ecalc && setsid nohup bash tests/ofitune17_drive.sh [jobid] > ~/ofitune17/driver.out 2>&1 < /dev/null &
# Gate: starts only after $TUNE17_MARKER (~/ofimem17/R/STD17_DONE: the timed 10-node production run) exists, then waits for the job's
# other steps to end; deadline TUNE17_DEADLINE (epoch s, default 16:00 EDT today) -> SKIPPED.  ALWAYS writes $O/DONE (first line the verdict).
# Steps (TUNE17_STEPS, default all, in order):
#   sw4    t_comm --bw all-to-all at 4 nodes, COMM_OFI_CHUNK_MB {1,4,8} x COMM_OFI_WINDOW {16,64,128} (default 4 x 64 is in the grid)
#   sw10   the best two of sw4 (mean per-node aggregate GB/s at the largest slab) and the default, at 10 nodes
#   nic2   2 nodes: COMM_OFI_NICS default / "0,1;1,2;2,3;3,0" / "0,0;1,1;2,2;3,3": t_comm correctness, then --bw
#   ec2    ecalc 2e10 on 2 nodes, default NICs vs the better form of nic2: VERIFY OK, parts' sha1 equal, top part = the 1e11 prefix
# Output $O (~/ofitune17): drive.log (Eastern), bw.tsv (tag nodes chunk window nics GB/s...), walls.txt, the logs, DONE.
# Never cancels 12287 / 12294; never touches other users' jobs; no scancel at all.
set -u
J=${1:-12287}; STEPS=${TUNE17_STEPS:-sw4,sw10,nic2,ec2}
cd "$(dirname "$0")/.." || exit 1; HERE=$(pwd)
source ./aac7env.sh > /dev/null 2>&1; export MNRUN_CPUS_PER_TASK=${MNRUN_CPUS_PER_TASK:-auto}
O=${TUNE17_OUT:-$HOME/ofitune17}; mkdir -p "$O"; LOG=$O/drive.log; W=$O/walls.txt; T=$O/bw.tsv; rm -f "$O/DONE"
MARK=${TUNE17_MARKER:-$HOME/ofimem17/R/STD17_DONE}
DL=${TUNE17_DEADLINE:-$(TZ=America/New_York date -d "$(TZ=America/New_York date +%F) 16:00" +%s)}
REF=; for r in "$HOME/ref/e_1e11.out" "$HOME/ntt/ecalc/results/e_1e11.out"; do [ -s "$r" ] && { REF=$r; break; }; done
UNPACK=$(cd ../tools && pwd)/unpack_digits
say() { echo "$(TZ=America/New_York date '+%Y-%m-%d %H:%M:%S %Z') $*" | tee -a "$LOG"; }
want() { [[ ",$STEPS," == *",$1,"* ]]; }
VERDICT="FAIL: driver ended unexpectedly"
finish() { VERDICT=$1; exit 0; }
trap 'rm -rf "$O/ab" 2>/dev/null; say "finish: $VERDICT"; { echo "$VERDICT"; cat "$T" "$W" 2>/dev/null; tail -n 40 "$LOG"; } > "$O/DONE"' EXIT
NODES=$(scontrol show hostnames "$(squeue -j "$J" -h -o %N)")
onn() { local g=$1; shift; srun --jobid="$J" -N "$g" -w "$(echo "$NODES" | head -n "$g" | paste -sd,)" --ntasks="$g" --ntasks-per-node=1 -c 4 --overlap --export=ALL bash -c "$1"; }
ctr() { onn "$1" 'h=$(hostname -s); for i in 0 1 2 3; do t=/sys/class/cxi/cxi$i/device/telemetry; echo "$h cxi$i $(cut -d@ -f1 $t/hni_sts_tx_ok_octets 2>/dev/null) $(cut -d@ -f1 $t/hni_sts_rx_ok_octets 2>/dev/null)"; done' 2>/dev/null | sort; }
ctrdiff() { join <(awk '{print $1"_"$2, $3, $4}' "$1") <(awk '{print $1"_"$2, $3, $4}' "$2") | awk '{ printf "%s tx %.2f rx %.2f GB; ", $1, ($4 - $2) / 1e9, ($5 - $3) / 1e9 } END { print "" }'; }

say "tune17: job $J, out $O, marker $MARK, deadline $(TZ=America/New_York date -d @"$DL" '+%F %H:%M %Z'), ref ${REF:-none}; $HERE $(git log --oneline -1 | cut -c1-70)"
say "tune17: nodes $(echo "$NODES" | paste -sd,)"
while [ ! -e "$MARK" ]; do [ "$(date +%s)" -gt "$DL" ] && finish "SKIPPED: $MARK not there by $(TZ=America/New_York date -d @"$DL" '+%H:%M %Z')"; sleep 60; done
say "marker present: $(head -1 "$MARK" | cut -c1-200); waiting for job $J's other steps to end"
n=1; for i in $(seq 1 120); do n=$(squeue -s -j "$J" -h -o "%i %j" | grep -vE "\.(batch|extern|interactive) " | grep -vc "^$"); [ "$n" = 0 ] && break; sleep 30; done
squeue -s -j "$J" -h -o "%i %j %M" | tee -a "$LOG"
[ "$n" = 0 ] || finish "BLOCKED: job $J still has other steps after 1 h"
[ -s ./tests/t_comm ] && [ -s ./ecalc ] && [ -x "$UNPACK" ] || finish "FAIL: no build in $HERE (ecalc, tests/t_comm, ../tools/unpack_digits)"
printf "tag\tnodes\tchunk_MB\twindow\tnics\trc\tagg_maxslab_mean\tagg_maxslab_min\tthr_maxslab_mean\tagg_4MiB_mean\n" > "$T"

# bw <tag> <nodes> <chunk|-> <window|-> <nics|->: t_comm --bw, 4 threads per node, slabs 64 KiB .. mx MiB per destination; per-PE lines
# give per-thread and per-node aggregate GB/s; bw.tsv gets the mean / min over PEs at the largest slab and the mean at 4 MiB
bw() {
    local tag=$1 g=$2 ch=$3 wi=$4 ni=$5 lg=$O/$1.log mx=64 rc; [ "$g" -gt 2 ] && mx=16
    local ex=(COMM_TRANSPORT=shmem COMM_OFI=1 COMM_OFI_VERBOSE=1); [ "$ch" != - ] && ex+=(COMM_OFI_CHUNK_MB=$ch); [ "$wi" != - ] && ex+=(COMM_OFI_WINDOW=$wi); [ "$ni" != - ] && ex+=("COMM_OFI_NICS=$ni")
    ctr "$g" > "$O/$tag.c0"
    COMM_SHMEM_POOL_MB=$((8 * g * mx + 512)) COMM_OFI_POOL_MB=$((2 * g * mx + 256)) SLURM_JOB_ID=$J MNRUN_NODES=$g timeout 900 ./mnrun.sh "$g" env "${ex[@]}" ./tests/t_comm --bw 4 5 $mx > "$lg" 2>&1; rc=$?
    ctr "$g" > "$O/$tag.c1"
    local big=$((mx * 1048576))
    local s=$(grep -E ': pe [0-9]+ .* per slab' "$lg" | awk -v big=$big '{ for (i = 1; i <= NF; i++) { if ($(i+1) == "B" && $(i+2) == "per") b = $i; if ($(i+1) == "GB/s" && $(i+3) == "thread,") t = $i; if ($i == "aggregate") a = $(i+1) }
        if (b == big) { n++; sa += a; st += t; if (mn == "" || a < mn) mn = a } if (b == 4194304) { n4++; s4 += a } }
        END { if (n) printf "%.2f\t%.2f\t%.2f\t%.2f", sa / n, mn, st / n, n4 ? s4 / n4 : 0; else printf "-\t-\t-\t-" }')
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$tag" "$g" "$ch" "$wi" "$ni" "$rc" "$s" >> "$T"
    say "$tag: nodes $g chunk $ch window $wi nics $ni rc $rc | agg@${mx}MiB mean/min, thr mean, agg@4MiB: $(echo "$s" | tr '\t' ' ')"
    say "$tag pe0: $(grep -E ': pe 0 ' "$lg" | grep -oE '[0-9]+ B per slab: .*aggregate [0-9.]+' | sed 's/median.*), //' | paste -sd'|' | cut -c1-700)"
    say "$tag counters: $(ctrdiff "$O/$tag.c0" "$O/$tag.c1" | cut -c1-1200)"
    grep -m4 -E 'comm_ofi: .*chunk .*window|FATAL|rror' "$lg" | cut -c1-250 | sed 's/^/    /' >> "$LOG"
}
best() { awk -F'\t' -v p="$1" 'NR > 1 && $1 ~ "^"p && $6 == 0 && $7 != "-" { print $7 "\t" $3 "\t" $4 "\t" $1 }' "$T" | sort -t$'\t' -k1,1gr; }

if want sw4; then for ch in 1 4 8; do for wi in 16 64 128; do bw "sw4_c${ch}_w${wi}" 4 "$ch" "$wi" -; done; done
    say "sw4 ranking (agg mean GB/s, chunk, window):"; best sw4_ | sed 's/^/    /' | tee -a "$LOG"; fi
if want sw10; then
    sel=$( { best sw4_ | head -2 | cut -f2,3; printf "4\t64\n"; } | awk '!seen[$0]++')
    [ -n "$sel" ] || sel=$(printf "4\t64")
    while IFS=$'\t' read -r ch wi; do [ -n "$ch" ] && bw "sw10_c${ch}_w${wi}" 10 "$ch" "$wi" -; done <<< "$sel"
    say "sw10 ranking:"; best sw10_ | sed 's/^/    /' | tee -a "$LOG"; fi

FORMA="0,1;1,2;2,3;3,0"; FORMB="0,0;1,1;2,2;3,3"; BETTER=
if want nic2; then
    bw nic2_default 2 - - -
    for f in A B; do eval "nf=\$FORM$f"; lg=$O/nic2_tc_$f.log
        COMM_SHMEM_POOL_MB=1024 SLURM_JOB_ID=$J MNRUN_NODES=2 timeout 300 ./mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_OFI=1 COMM_OFI_VERBOSE=1 "COMM_OFI_NICS=$nf" ./tests/t_comm > "$lg" 2>&1; rc=$?
        ok=$(grep -c 'VERIFY OK' "$lg")
        say "nic2 t_comm form $f ($nf): rc $rc, $ok VERIFY OK of 2; $(grep -oE 'cxi[0-9] [0-9.]+ GB in [0-9]+ writes' "$lg" | sort | uniq -c | paste -sd';' | cut -c1-400)"
        if [ $rc = 0 ] && [ "$ok" = 2 ]; then bw "nic2_form$f" 2 - - "$nf"; else say "nic2 form $f: correctness failed, no --bw: $(grep -m2 -E 'FATAL|rror' "$lg" | cut -c1-200 | paste -sd' ')"; fi
    done
    b=$(best nic2_form | head -1 | cut -f4); [ -n "$b" ] && { eval "BETTER=\$FORM${b#nic2_form}"; say "nic2: the better form is ${b#nic2_form} ($BETTER)"; }
fi

BASE="COMM_TRANSPORT=shmem COMM_OFI=1 COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MN_COMM_MARK=1 COMM_OFI_VERBOSE=1"
run() {   # run <tag> <nodes> <digits> <extra env words...> (template: ofi17_drive.sh run)
    local tag=$1 g=$2 d=$3; shift 3; local of=$O/ab/$tag/e.out lg=$O/$tag.log t0=$SECONDS rc tot ver dig base=$O/ab/base_$g.sha1
    mkdir -p "$O/ab/$tag"; say "run $tag: mnrun.sh $g env $BASE $* ./ecalc $d"
    SLURM_JOB_ID=$J MNRUN_NODES=$g timeout 1500 ./mnrun.sh "$g" env $BASE "$@" ./ecalc "$d" "$of" > "$lg" 2>&1; rc=$?
    tot=$(grep -m1 -E '^total +[0-9.]+ s' "$lg" | sed -E 's/^total +([0-9.]+) s.*/\1/'); ver=$(grep -m1 -oE "mn: all $g nodes: VERIFY [A-Z]+" "$lg")
    onn "$g" "cd $O/ab/$tag 2>/dev/null || exit 0; for p in e.out.part*; do [ -f \$p ] || continue; echo \"\$p \$( (head -c 1024 \$p; tail -c +4097 \$p) | sha1sum | cut -c1-40)\"; done" 2>/dev/null | sort -u > "$O/$tag.sha1"
    if [ ! -s "$base" ] && [ -s "$O/$tag.sha1" ]; then
        cp "$O/$tag.sha1" "$base"; local top=$O/ab/top_$$.txt n
        "$UNPACK" -q "$O/ab/$tag/e.out.part0000" > "$top" 2>/dev/null; n=$(stat -c %s "$top" 2>/dev/null || echo 0)
        if [ -n "$REF" ] && [ "$n" -gt 0 ] && head -c "$n" "$REF" | cmp -s - "$top"; then dig="baseline; top part identical to the 1e11 prefix ($n bytes)"; else dig="baseline; top part DIFFERS from the 1e11 prefix ($n bytes)"; fi; rm -f "$top"
    else cmp -s "$O/$tag.sha1" "$base" && dig="identical to the baseline ($(wc -l < "$O/$tag.sha1") parts)" || dig="DIFFERS from the baseline"; fi
    rm -rf "$O/ab/$tag"
    echo "$tag nodes $g digits $d rc $rc total ${tot:-none} s elapsed $((SECONDS - t0)) s | ${ver:-no VERIFY} | $dig | $*" | tee -a "$W" >> "$LOG"
    grep -E "comm_shmem: pe 0: all-to-all|comm_ofi: device [0-3]|^comm-mark" "$lg" | head -30 | cut -c1-260 | sed 's/^/    /' >> "$LOG"
}
if want ec2; then
    if [ -z "$BETTER" ]; then say "ec2: no two-NIC form passed nic2; running the default only"; run ec2_default 2 20000000000
    else run ec2_default 2 20000000000; run ec2_form 2 20000000000 "COMM_OFI_NICS=$BETTER"; fi
    grep -q "^ec2_default .*rc 0 .*VERIFY OK | baseline; top part identical" "$W" || finish "FAIL: ec2_default: $(grep '^ec2_default' "$W" | cut -c1-250)"
    [ -z "$BETTER" ] || grep -q "^ec2_form .*rc 0 .*VERIFY OK | identical to the baseline" "$W" || finish "FAIL: ec2_form ($BETTER): $(grep '^ec2_form' "$W" | cut -c1-250)"
fi
b4=$(best sw4_ | head -1 | cut -f1-3 | tr '\t' ' '); b10=$(best sw10_ | head -1 | cut -f1-3 | tr '\t' ' ')
finish "SUCCESS ($STEPS): best 4-node ${b4:-none} (GB/s chunk window); best 10-node ${b10:-none}; better NIC form ${BETTER:-none}"

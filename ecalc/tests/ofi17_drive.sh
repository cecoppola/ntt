#!/bin/bash
# ofi17_drive.sh - Phase 17 (results/OFI17.md): the unattended multi-node test of COMM_OFI on aac7 inside an existing allocation.
#   ./tests/ofi17_drive.sh <jobid> [steps]     from ecalc/ (login node), after a build; steps (default all, in order):
#     wait   until every marker file of OFI17_MARKERS exists (and the job has no other step running), OFI17_DEADLINE (epoch s) at most
#     tc2    t_comm correctness over 2 nodes, COMM_OFI=1 and =0
#     acc    mnaccept --only unit,e9,mn with COMM_TRANSPORT=shmem COMM_OFI=1 MNRUN_NODES=2 (own MNACCEPT_TMP)
#     bw2    t_comm --bw over 2 nodes, 4 threads, both transports (per-thread GB/s)
#     ab     interleaved A/B of 1e10 per node at 2 and 4 nodes, COMM_OFI=0 (A) vs 1 (B), OFI17_REPS (2) each, MN_COMM_MARK=1;
#            digits: VERIFY OK on every node, every part's sha1 equal within a node count, the first run's top part against
#            the 1e11 reference's prefix
#     bwn    t_comm --bw at 4, 8, 10 nodes (all-to-all), both transports
# Output: OFI17_OUT (~/ofi17/R/drive): drive.log (Eastern times), walls.txt, the logs; DONE when finished (its first line the verdict).
set -u
J=${1:?jobid}; STEPS=${2:-wait,tc2,acc,bw2,ab,bwn}
cd "$(dirname "$0")/.."; HERE=$(pwd)
source ./aac7env.sh > /dev/null 2>&1; export MNRUN_CPUS_PER_TASK=${MNRUN_CPUS_PER_TASK:-auto}
O=${OFI17_OUT:-$HOME/ofi17/R/drive}; mkdir -p "$O"; LOG=$O/drive.log; W=$O/walls.txt; rm -f "$O/DONE"
REPS=${OFI17_REPS:-2}; REF=${OFI17_REF:-$HOME/ref/e_1e11.out}; UNPACK=${OFI17_UNPACK:-$(cd "$(dirname "$0")/../../tools" && pwd)/unpack_digits}   # 2026-10-06: ref moved to ~/ref (~/ntt deleted on aac7); unpack_digits from this clone's own tools/, not a cross-clone ~/ntt reference
say() { echo "$(TZ=America/New_York date '+%Y-%m-%d %H:%M:%S %Z') $*" | tee -a "$LOG"; }
want() { [[ ",$STEPS," == *",$1,"* ]]; }
finish() { say "finish: $1"; { echo "$1"; tail -n 60 "$LOG"; } > "$O/DONE"; exit 0; }
NODES=$(scontrol show hostnames "$(squeue -j "$J" -h -o %N)")
BASE="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 MN_COMM_MARK=1 COMM_OFI_VERBOSE=1"
onn() { local g=$1; shift; srun --jobid="$J" -N "$g" -w "$(echo "$NODES" | head -n "$g" | paste -sd,)" --ntasks="$g" --ntasks-per-node=1 -c 4 --overlap --export=ALL bash -c "$1"; }
ctr() { onn "$1" 'h=$(hostname -s); for i in 0 1 2 3; do t=/sys/class/cxi/cxi$i/device/telemetry; echo "$h cxi$i $(cat $t/hni_sts_tx_ok_octets 2>/dev/null | cut -d@ -f1) $(cat $t/hni_sts_rx_ok_octets 2>/dev/null | cut -d@ -f1)"; done' 2>/dev/null | sort; }
ctrdiff() { join <(awk '{print $1"_"$2, $3, $4}' "$1") <(awk '{print $1"_"$2, $3, $4}' "$2") | awk '{ printf "%s tx %.2f GB rx %.2f GB; ", $1, ($4 - $2) / 1e9, ($5 - $3) / 1e9 } END { print "" }'; }

if want wait; then
    MK=${OFI17_MARKERS:-$HOME/p16/R16/RUN16_DONE $HOME/nic16/2N_DONE}; DL=${OFI17_DEADLINE:-$(( $(date +%s) + 8 * 3600 ))}
    say "wait: for $MK (deadline $(TZ=America/New_York date -d @$DL '+%H:%M %Z'))"
    while :; do ok=1; for m in $MK; do [ -e "$m" ] || ok=0; done; [ $ok = 1 ] && break; [ "$(date +%s)" -gt "$DL" ] && finish "BLOCKED: markers not there by the deadline"; sleep 60; done
    say "wait: markers present; waiting for the job's other steps to end"
    for i in $(seq 1 40); do n=$(squeue -s -j "$J" -h -o "%i %j" | grep -vE "\.(batch|extern|interactive) " | grep -vc "^$"); [ "$n" = 0 ] && break; sleep 30; done
    squeue -s -j "$J" -h -o "%i %j %M" | tee -a "$LOG"
    ps -eo pid,etime,args | grep -E "[n]ic16/|[r]un16" | cut -c1-150 | tee -a "$LOG"
fi
[ -s ./tests/t_comm ] && [ -s ./ecalc ] || finish "FAIL: no build in $HERE"
if want tc2; then
    for ofi in 1 0; do
        lg=$O/tc2_ofi$ofi.log
        COMM_SHMEM_POOL_MB=1024 SLURM_JOB_ID=$J MNRUN_NODES=2 timeout 300 ./mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_OFI=$ofi COMM_OFI_VERBOSE=1 ./tests/t_comm > "$lg" 2>&1; rc=$?
        say "tc2 COMM_OFI=$ofi: rc $rc, $(grep -c 'VERIFY OK' "$lg") VERIFY OK of 2; $(grep -oE 'cxi[0-9] [0-9.]+ GB in [0-9]+ writes' "$lg" | sort | uniq -c | paste -sd';')"
        [ $ofi = 1 ] && [ "$(grep -c 'VERIFY OK' "$lg")" != 2 ] && finish "FAIL: t_comm over OFI at 2 nodes (rc $rc): $(grep -m2 -E 'FATAL|error' "$lg" | cut -c1-200 | paste -sd' ')"
    done
fi
if want acc; then
    say "acc: mnaccept --only unit,e9,mn, COMM_OFI=1, MNRUN_NODES=2"
    mkdir -p "$HOME/ofi17/tmp"
    COMM_TRANSPORT=shmem COMM_OFI=1 MNRUN_NODES=2 MNACCEPT_TMP=$HOME/ofi17/tmp timeout 3600 ./mnaccept.sh "$J" --only unit,e9,mn > "$O/acc.log" 2>&1
    say "acc: rc $?"; grep -E "^(PASS|FAIL)" "results/mnaccept/$J/summary.txt" | sed 's/^/    /' | tee -a "$LOG"
fi
bw() {   # bw <nodes> <ofi> <tag>: 4 threads (a communicator each = an APU thread's mesh), slabs 64 KiB .. mx MiB, symmetric buffers
    local g=$1 ofi=$2 tag=$3 lg=$O/$3.log c0=$O/$3.c0 c1=$O/$3.c1 mx=64; [ "$g" -gt 2 ] && mx=16
    ctr "$g" > "$c0"
    COMM_SHMEM_POOL_MB=$((8 * g * mx + 512)) COMM_OFI_POOL_MB=$((2 * g * mx + 256)) SLURM_JOB_ID=$J MNRUN_NODES=$g timeout 600 ./mnrun.sh "$g" env COMM_TRANSPORT=shmem COMM_OFI=$ofi COMM_OFI_VERBOSE=1 ./tests/t_comm --bw 4 5 $mx > "$lg" 2>&1; local rc=$?
    ctr "$g" > "$c1"
    say "$tag: rc $rc; pe 0: $(grep 'pe 0 ' "$lg" | grep -oE '[0-9]+ B per slab: .*GB/s per thread, aggregate [0-9.]+' | sed 's/median.*), //' | paste -sd'|')"
    say "$tag counters: $(ctrdiff "$c0" "$c1" | cut -c1-900)"
}
if want bw2; then bw 2 0 bw2_shmem; bw 2 1 bw2_ofi; fi
run() {   # run <tag> <nodes> <digits> <words...>
    local tag=$1 g=$2 d=$3; shift 3; local of=$O/ab/$tag/e.out lg=$O/$tag.log t0=$SECONDS rc tot ver dig base=$O/ab/base_$g.sha1
    mkdir -p "$O/ab/$tag"; say "run $tag: mnrun.sh $g env $BASE $* ./ecalc $d"
    SLURM_JOB_ID=$J MNRUN_NODES=$g timeout 1500 ./mnrun.sh "$g" env $BASE "$@" ./ecalc "$d" "$of" > "$lg" 2>&1; rc=$?
    tot=$(grep -m1 -E '^total +[0-9.]+ s' "$lg" | sed -E 's/^total +([0-9.]+) s.*/\1/'); ver=$(grep -m1 -oE "mn: all $g nodes: VERIFY [A-Z]+" "$lg")
    onn "$g" "cd $O/ab/$tag 2>/dev/null || exit 0; for p in e.out.part*; do [ -f \$p ] || continue; echo \"\$p \$( (head -c 1024 \$p; tail -c +4097 \$p) | sha1sum | cut -c1-40)\"; done" 2>/dev/null | sort -u > "$O/$tag.sha1"
    if [ ! -s "$base" ] && [ -s "$O/$tag.sha1" ]; then
        cp "$O/$tag.sha1" "$base"; local top=$O/ab/top_$$.txt n
        "$UNPACK" -q "$O/ab/$tag/e.out.part0000" > "$top" 2>/dev/null; n=$(stat -c %s "$top" 2>/dev/null || echo 0)
        [ "$n" -gt 0 ] && head -c "$n" "$REF" | cmp -s - "$top" && dig="baseline; top part identical to the 1e11 prefix ($n bytes)" || dig="baseline; top part DIFFERS from the 1e11 prefix ($n bytes)"; rm -f "$top"
    else cmp -s "$O/$tag.sha1" "$base" && dig="identical to the baseline ($(wc -l < "$O/$tag.sha1") parts)" || dig="DIFFERS from the baseline"; fi
    rm -rf "$O/ab/$tag"
    echo "$tag nodes $g digits $d rc $rc total ${tot:-none} s elapsed $((SECONDS - t0)) s | ${ver:-no VERIFY} | $dig | $*" | tee -a "$W" >> "$LOG"
    grep -E "comm_shmem: pe 0: all-to-all|comm_ofi: device [0-3]: pool peak|^comm-mark" "$lg" | head -30 | cut -c1-260 | sed 's/^/    /' >> "$LOG"
}
if want ab; then
    for g in 2 4; do d=$((g * 10000000000))
        for i in $(seq 1 "$REPS"); do run "ab${g}_shmem$i" "$g" "$d" COMM_OFI=0; run "ab${g}_ofi$i" "$g" "$d" COMM_OFI=1
            tail -1 "$W" | grep -q "^ab${g}_ofi$i .*rc 0 .*VERIFY OK" || finish "FAIL: ab${g}_ofi$i: $(tail -1 "$W" | cut -c1-200); $(grep -m2 -E 'FATAL|rror' "$O/ab${g}_ofi$i.log" | cut -c1-200 | paste -sd' ')"; done
    done
fi
if want bwn; then for g in 4 8 10; do bw "$g" 0 "bw${g}_shmem"; bw "$g" 1 "bw${g}_ofi"; done; fi
finish "COMPLETE ($STEPS)"

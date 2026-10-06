#!/bin/bash
# std17_drive.sh - Phase 17 STD17: the standard run with all NICs (comm_ofi on by default on cxi) -- RUN16 try 2's configuration
# (8.1e11 digits on 10 nodes of job 12287, the e16 launch line, MN_COMM_MARK=1, MEM_REPORT_DEVS=1), now with COMM_OFI unset
# (= on: cxi present) and the OFIMEM accounting (the SHMEM pool shrunk, the comm_ofi pools counted).  Template: run16b.sh.
# Fire-and-forget (setsid nohup): waits for $STD17_MARKER (another agent's 2-node test), then for job 12287's other steps to
# end; sizes the run from the nodes' MemAvailable (>= 12 GB below the minimum, the OFI layout table below); a 2-node sanity
# run; the record; RECHECK (packed); the 1e11 prefix; deletes the parts.  ALWAYS writes $OUT/STD17_DONE (first line = verdict).
# Never cancels 12287 / 12294; never touches other users' jobs.
set -u
cd "$(dirname "$0")/.." || exit 1
source ./aac7env.sh > /dev/null 2>&1
export MNRUN_CPUS_PER_TASK=${MNRUN_CPUS_PER_TASK:-auto}
J=${STD17_JOB:-12287}; export SLURM_JOB_ID=$J
OUT=${STD17_OUT:-$HOME/ofimem17/R}
MARK=${STD17_MARKER:-$HOME/ofi17/HANDOFF_DONE}
DL=${STD17_DEADLINE:-$(( $(date +%s) + 20 * 3600 ))}
TO=${STD17_TIMEOUT:-9000}
mkdir -p "$OUT"/n10 "$OUT"/log
LOG=$OUT/log/std17.log
say() { echo "$(TZ=America/New_York date '+%Y-%m-%d %H:%M:%S %Z') $*" | tee -a "$LOG"; }
VERDICT="FAIL: driver ended unexpectedly"
finish() {
    VERDICT=$1
    rm -f "$OUT"/n10/e.out.part???? "$OUT"/n10/e.out.t1 "$OUT"/n10/e.txt.part???? 2> /dev/null   # never leave digits on disk
    { echo "$VERDICT"; cat "$OUT/log/walls.txt" 2> /dev/null; } > "$OUT/STD17_DONE"
    say "std17 verdict: $VERDICT"
    exit 0
}
trap 'finish "$VERDICT"' EXIT

LINE10="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ECALC_LOG_CLOCKS=1 MN_COMM_MARK=1 COMM_OFI_VERBOSE=1"
# the OFI layout node per D per node (BS_LAYOUT_ONLY=D:10 with LINE10 and COMM_OFI_PLAN_CXI=1, login node, modelled GB; flat bands)
TABLE="81000000000:420.66 78000000000:412.07 74000000000:403.48 70000000000:394.89"
NODES=$(scontrol show hostnames "$(squeue -j "$J" -h -o %N)")
onn() { local g=$1; shift; srun --jobid="$J" -N "$g" -w "$(echo "$NODES" | head -n "$g" | paste -sd,)" --ntasks="$g" --ntasks-per-node=1 -c 4 --overlap --export=ALL bash -c "$1"; }
ctr() { onn 10 'h=$(hostname -s); for i in 0 1 2 3; do t=/sys/class/cxi/cxi$i/device/telemetry; echo "$h cxi$i $(cut -d@ -f1 $t/hni_sts_tx_ok_octets 2>/dev/null) $(cut -d@ -f1 $t/hni_sts_rx_ok_octets 2>/dev/null)"; done' 2> /dev/null | sort; }

say "std17: job $J, out $OUT, marker $MARK, timeout $TO; $(git log --oneline -1 | cut -c1-80)"
while [ ! -e "$MARK" ]; do [ "$(date +%s)" -gt "$DL" ] && { VERDICT="BLOCKED: $MARK not there by the deadline"; exit 0; }; sleep 60; done
say "marker present; waiting for job $J's other steps to end"
for i in $(seq 1 240); do n=$(squeue -s -j "$J" -h -o "%i %j" | grep -vE "\.(batch|extern|interactive) " | grep -vc "^$"); [ "$n" = 0 ] && break; sleep 30; done
squeue -s -j "$J" -h -o "%i %j %M" | tee -a "$LOG"
[ "${n:-1}" = 0 ] || { VERDICT="BLOCKED: job $J still has other steps after 2 h"; exit 0; }
[ -s ./ecalc ] && [ -x ../tools/unpack_digits ] || { VERDICT="FAIL: no build"; exit 0; }

onn 10 'echo "$(hostname -s) memavail $(awk "/MemAvailable/ { printf \"%.1f\", \$2/1e6 }" /proc/meminfo) GB"' 2>&1 | sort | tee "$OUT/log/nodes_start.txt" | tee -a "$LOG"
MIN=$(awk '/memavail/ { if (m == "" || $3 < m) m = $3 } END { print m }' "$OUT/log/nodes_start.txt")
[ -n "$MIN" ] && [ "$(grep -c memavail "$OUT/log/nodes_start.txt")" = 10 ] || { VERDICT="FAIL: MemAvailable not read on 10 nodes"; exit 0; }
DPER=; NODE=
for e in $TABLE; do d=${e%%:*}; nb=${e##*:}; if awk -v a="$MIN" -v b="$nb" 'BEGIN { exit !(b <= a - 12) }'; then DPER=$d; NODE=$nb; break; fi; done
[ -n "$DPER" ] || { VERDICT="BLOCKED: min MemAvailable $MIN GB leaves no size of the table 12 GB under it"; exit 0; }
D10=$((DPER * 10))
say "min MemAvailable $MIN GB: D per node $DPER (layout node $NODE GB modelled, $(awk -v a="$MIN" -v b="$NODE" 'BEGIN { printf "%.1f", a - b }') GB under), total $D10"

env $LINE10 MN_PLAN_ONLY="$D10:10" COMM_OFI_PLAN_CXI=1 ./ecalc > "$OUT/log/plan_n10.txt" 2>&1
env $LINE10 BS_LAYOUT_ONLY="$DPER:10" COMM_OFI_PLAN_CXI=1 ./ecalc "$D10" /dev/null > "$OUT/log/layout_n10.txt" 2>&1
grep -q 'plan check .*: OK' "$OUT/log/plan_n10.txt" || { VERDICT="FAIL: plan check not OK"; exit 0; }
grep -E '^plan (check|pool)|^room:' "$OUT/log/plan_n10.txt" "$OUT/log/layout_n10.txt" | cut -c1-400 | tee -a "$LOG"

launch() {
    local tag=$1 nodes=$2 to=$3 d=$4 of=$5; shift 5; local rc t0 el tot lg=$OUT/log/$tag.log
    say "launch $tag: mnrun.sh $nodes env $* ./ecalc $d $of (timeout $to)"
    t0=$SECONDS; timeout "$to" ./mnrun.sh "$nodes" env "$@" ./ecalc "$d" "$of" > "$lg" 2>&1; rc=$?; el=$((SECONDS - t0))
    tot=$(grep -m1 -E '^total +[0-9.]+ s' "$lg" | sed -E 's/^total +([0-9.]+) s.*/\1/')
    echo "$tag rc $rc total ${tot:-none} s elapsed $el s nodes $nodes digits $d" | tee -a "$OUT/log/walls.txt" | tee -a "$LOG"
    return $rc
}
# a 2-node sanity run (1e10 per node): the OFIMEM pools hold, VERIFY OK, before the long run
mkdir -p "$OUT/n2"
launch n2_sanity 2 900 20000000000 "$OUT/n2/e.out" $LINE10 || { rm -f "$OUT"/n2/e.out*; VERDICT="FAIL: 2-node sanity rc != 0 ($(grep -m2 -E 'FATAL|fatal|cannot hold' "$OUT/log/n2_sanity.log" | cut -c1-200 | paste -sd' '))"; exit 0; }
grep -q "all 2 nodes: VERIFY OK" "$OUT/log/n2_sanity.log" || { rm -f "$OUT"/n2/e.out*; VERDICT="FAIL: 2-node sanity without VERIFY OK"; exit 0; }
grep -E 'comm_ofi: device|comm_shmem pool|pool peak' "$OUT/log/n2_sanity.log" | sort | uniq -c | cut -c1-260 | head -20 | tee -a "$LOG"
rm -f "$OUT"/n2/e.out*

ctr > "$OUT/log/ctr_before.txt"
launch n10_record 10 "$TO" "$D10" "$OUT/n10/e.out" $LINE10 || { VERDICT="FAIL: record rc != 0 ($(grep -m2 -E 'FATAL|fatal|cannot hold|rc ' "$OUT/log/n10_record.log" | cut -c1-200 | paste -sd' '))"; exit 0; }
ctr > "$OUT/log/ctr_after.txt"
join <(awk '{print $1"_"$2, $3, $4}' "$OUT/log/ctr_before.txt") <(awk '{print $1"_"$2, $3, $4}' "$OUT/log/ctr_after.txt") | awk '{ printf "%s tx %.1f GB rx %.1f GB\n", $1, ($4 - $2) / 1e9, ($5 - $3) / 1e9 }' > "$OUT/log/ctr_diff.txt"
grep -q "all 10 nodes: VERIFY OK" "$OUT/log/n10_record.log" || { VERDICT="FAIL: record without 'all 10 nodes: VERIFY OK'"; exit 0; }
grep -E 'transform cache:|^total|VmHWM|comm-mark|comm_ofi: device .*pool peak|all 10 nodes: VERIFY OK' "$OUT/log/n10_record.log" | head -120 | cut -c1-260 >> "$LOG"

launch n10_recheck 10 "$TO" "$D10" "$OUT/n10/e.out" $LINE10 ECALC_RECHECK=1 || { VERDICT="FAIL: RECHECK rc != 0"; exit 0; }
grep -q "mn: all 10 nodes: RECHECK OK" "$OUT/log/n10_recheck.log" || { VERDICT="FAIL: no 'all 10 nodes: RECHECK OK'"; exit 0; }

REF11=; for r in "$HOME/ref/e_1e11.out" "$HOME/ntt/ecalc/results/e_1e11.out"; do [ -s "$r" ] && { REF11=$r; break; }; done
[ -n "$REF11" ] || { VERDICT="FAIL: no e_1e11.out reference (~/ref, ~/ntt/ecalc/results)"; exit 0; }
t0=$SECONDS; rc=0
for p in 0000 0001 0002; do ../tools/unpack_digits -q -o "$OUT/n10/e.txt.part$p" "$OUT/n10/e.out.part$p" >> "$OUT/log/n10_unpack.log" 2>&1 || rc=1; done
[ $rc = 0 ] || { VERDICT="FAIL: unpack"; exit 0; }
if cat "$OUT"/n10/e.txt.part0000 "$OUT"/n10/e.txt.part0001 "$OUT"/n10/e.txt.part0002 | head -c 100000000002 | cmp - <(head -c 100000000002 "$REF11"); then
    say "n10: the 1e11 prefix is identical to $REF11 ($((SECONDS - t0)) s)"
else VERDICT="FAIL: the 1e11 prefix DIFFERS from $REF11"; exit 0; fi
VERDICT="SUCCESS: D $D10 on 10 nodes, $(grep '^n10_record' "$OUT/log/walls.txt" | tail -1), VERIFY OK, RECHECK OK, 1e11 prefix identical"
exit 0

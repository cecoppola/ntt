#!/bin/bash
# e16_headline.sh - Phase 16 E (PLAN.md 39 row E, results/D16.md): the 12-node scaled headline on aac7 at the target's per-node
# share, the target's launch line (docs/TARGET.md 4) adapted to aac7, then the verification chain off the clock and the run one
# grid step below.  Everything under `timeout`, a `_DONE` marker per step, a retry on the open init segfault (results/A16.md 4:
# Cray's shmem_init_thread dies in ~13 % of the launches; PE 0 then hangs in PMI -- the timeout kills it, the launch is counted
# and repeated once).  Nothing here changes a default of ecalc: every switch is the launch line's.
#
#   SLURM_JOB_ID=<12-node allocation> ./e16_headline.sh [plan|record|dev|chain|below|all]      (default: all, in that order)
#
# The sizes (measured on aac7's login node with MN_PLAN_ONLY, ~/p16/D/plan/sweep12.txt, 2026-10-04):
#   E16_DIGITS = 1099200000000 = 12 x 9.16e10 (the target's top node holds 9.169e10 of 5.276e13 / 576): plan check OK, 102 products,
#     pieces (node 0 / critical path) 181 / 189, levels 12/16 12/16 40/40, the default MN_GROUPS schedule at g = 12 is 2,4,12
#     (groups of 2, then 4, then the 3-way top: "the powers of two dividing the size, then the odd part's prime factors"),
#     pool 8704 MiB (heap 9216M), C layout node 445.36 GB (TARGET_HW_REVIEW 4 row E: 445 GB modelled) -- inside 480.
#     The flat stretch is 1.091e12 .. 1.110e12 (189 on the critical path); the step below it at 1.090 -> 1.091e12 (185 -> 189,
#     the top level 36 -> 40 pieces).
#   E16_BELOW  = 1090000000000: the last size below that step (181 -> 177 pieces on node 0; the step's cost = the pair of walls).
# Modelled walls (estimate.py --g 12 --D 9.16e10 --write-bw 0.1, the NFS rate A measured, 0.116 GB/s single-stream, ASSUMED to
# hold with 12 writers): 323.3 s without the write / 691.9 s with it at --bw 25 (the 200 Gb/s NIC per APU, ASSUMED);
# 1140.4 / 1385.4 s at --bw 3.7 (the 2-node post-to-completion rate A measured, which holds the staging copies too -- an upper
# bound of the fabric's cost).  Both are modelled; the measured pair of walls goes in results/D16.md.
#
# Output: $E16_OUT (default ~/p16/E): rec/ (the record run: no checkpoint, no top set), dev/ (ECALC_CHECKPOINT=1, the
# development form), below/; the packed parts e.out.part0000..0023 (MN_OUT_DKM_HI=1: two per node) + e.out.t1; the ASCII parts
# e.txt.part0000..0023 from the chain.  Disk (NFS, 2.7 TB free on 2026-10-04): 488 GB packed per run, 1.1 TB of ASCII -- the
# dev run's parts are deleted after their byte compare with the record's, the ASCII parts after the ASCII RECHECK and the 1e11
# prefix, the below run's after its RECHECK (E16_KEEP=1 keeps everything).  E16_CANCEL=1 cancels the allocation at the end.
#
# Every launch: `timeout`, rc and both walls logged -- `total` (the digits computed and verified, the run's own clock) and the
# wrapper's elapsed seconds (the process's exit: with the write of the parts to NFS).
set -u
STEP=${1:-all}
cd "$(dirname "$0")"; HERE=$(pwd)
[ -n "${SLURM_JOB_ID:-}" ] || { echo "set SLURM_JOB_ID to the 12-node allocation"; exit 1; }
[ -f ./ecalc ] || { echo "no ./ecalc here (build: source aac7env.sh; make -s -j32 SHMEM_CRAY=1 GMP_HOME=\$HOME/gmp)"; exit 1; }
source ./aac7env.sh
export MNRUN_CPUS_PER_TASK=${MNRUN_CPUS_PER_TASK:-auto}

# 2026-10-04 (the integrator): 12 nodes are never free on aac7 (3 are held for days) -- the headline runs on 10; the 12-node figures
# stay as the modelled column.  At g = 10 (login node, ~/p16/D/plan/plan_916e9_10.txt, sweep10.txt): 9.16e11 digits -> plan check OK,
# 98 products, pieces 135 / 139 (node 0 / critical path), levels 12/16 34/34, the default schedule 2,10 (groups of 2, then the 5-way
# top), pool 8704 MiB, C layout node 453.95 GB (fits 480); the step below at 9.08 -> 9.10e11 (the top level 32 -> 34 pieces):
# E16_BELOW = 908000000000 (137 on the critical path).
G=${E16_NODES:-10}
DIGITS=${E16_DIGITS:-$((G * 91600000000))}
case $G in 12) BELOW=${E16_BELOW:-1090000000000};; 10) BELOW=${E16_BELOW:-908000000000};; *) BELOW=${E16_BELOW:-$((DIGITS * 99 / 100))};; esac
OUT=${E16_OUT:-$HOME/p16/E}
REF11=${E16_REF11:-$HOME/ref/e_1e11.out}      # sha1 578f5efb0ff2b9af6b681a375c9ff39197f55cb7 (2026-10-06: moved to ~/ref, ~/ntt deleted on aac7)
TO_RUN=${E16_TIMEOUT:-3600}        # one launch (the modelled wall with the NFS write is 692 s; the segfault's hang is killed here)
TO_CHAIN=${E16_TIMEOUT_CHAIN:-7200}
KEEP=${E16_KEEP:-0}
mkdir -p "$OUT"/rec "$OUT"/dev "$OUT"/below "$OUT"/log
LOG=$OUT/log/e16.log
say() { echo "$(date '+%Y-%m-%dT%H:%M:%S%z') $*" | tee -a "$LOG"; }
# the nodes of the allocation and their state at the start (B16: walls drift inside one allocation; a slow node must be nameable):
# per node the load, leftover GPU processes, free memory, rocm-smi's clocks -- $OUT/log/nodes_<jobid>.txt
NODES=$(scontrol show hostnames "$(squeue -j "$SLURM_JOB_ID" -h -o %N)" | paste -sd,)
say "job $SLURM_JOB_ID nodes $NODES"
srun --jobid="$SLURM_JOB_ID" -N "$G" --ntasks="$G" --ntasks-per-node=1 -c 4 --overlap --export=ALL bash -c \
    'h=$(hostname -s); echo "$h load $(cut -d" " -f1-3 /proc/loadavg) memavail $(awk "/MemAvailable/ { printf \"%.0f GB\", \$2 / 1e6 }" /proc/meminfo) tmp $(df -h /tmp | awk "NR == 2 { print \$3 }") gpu-procs $(rocm-smi --showpids 2>/dev/null | grep -cE "^[0-9]+ ") $(rocm-smi --showclocks 2>/dev/null | grep -oE "GPU\[[0-9]\].*sclk[^(]*\([0-9]+Mhz\)" | sed -E "s/.*GPU\[([0-9])\].*\(([0-9]+)Mhz\)/sclk\1=\2/" | paste -sd" ")"' \
    2>&1 | sort | tee "$OUT/log/nodes_$SLURM_JOB_ID.txt" | tee -a "$LOG"

# the target's launch line (docs/TARGET.md 4) on aac7: the transport's words, the design switches; the pool comes from mnrun.sh's own
# MN_PLAN_ONLY call (`plan pool` -> COMM_SHMEM_POOL_MB, the heap + 512 MiB), the schedule is the code's default at g = 12 (2,4,12).
# MN_GROUPS is not set: PLAN 39 / TARGET 4 -- set it only at 576 or a multiple of 9 * 2^k (at 12 the default is the only sane list).
LINE="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 DM_MN_LEAN=1 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ECALC_LOG_CLOCKS=1 ${E16_EXTRA:-}"   # DM_MN_LEAN=1 added 2026-10-05 (the user's decision; docs/TARGET.md 4, RESULTS §104): not a code default

# launch <tag> <timeout> <digits> <outfile> [more VAR=value words]: one mnrun.sh launch under timeout, retried once on the init
# segfault (rc 124 / 139 / "Segmentation fault" / "_pmi_network_allgather failed" before any `total`); prints rc; the log is
# $OUT/log/<tag>[.retry].log; both walls appended to $OUT/log/walls.txt
# E16_MEMWAIT_GB=<GB> (default 0: off): before every launch, wait (up to E16_MEMWAIT_S s, default 900) until MemAvailable is at least
# that on every node of the allocation, logging the per-node values.  2026-10-04 (job 12195): the record run at 9.16e11 on 10 nodes
# died on the memory guard at 283 s on the one node that started with MemAvailable 442 GB (the others 463-519 GB; the layout is
# 454 GB per node) -- the nodes were just out of B's job and a reboot; the same node showed 512 GB 15 min later.
memwait() {
    local need=${E16_MEMWAIT_GB:-0} left=${E16_MEMWAIT_S:-900} v mn
    [ "$need" = 0 ] && return 0
    while :; do
        v=$(srun --jobid="$SLURM_JOB_ID" -N "$G" --ntasks="$G" --ntasks-per-node=1 -c 4 --overlap --export=ALL bash -c 'echo "$(hostname -s) $(awk "/MemAvailable/ { printf \"%.0f\", \$2 / 1e6 }" /proc/meminfo)"' 2>/dev/null | sort)
        mn=$(echo "$v" | awk 'NF == 2 { if (m == "" || $2 < m) m = $2 } END { print m + 0 }')
        [ "$(echo "$v" | grep -c .)" = "$G" ] && [ "$mn" -ge "$need" ] && { say "memwait: min MemAvailable $mn GB >= $need on all $G nodes"; return 0; }
        [ "$left" -le 0 ] && { say "memwait: min $mn GB < $need GB after the wait: $(echo "$v" | paste -sd' '); launching anyway"; return 0; }
        say "memwait: min $mn GB < $need GB ($(echo "$v" | awk '{ printf "%s=%s ", $1, $2 }')), waiting"; sleep 60; left=$((left - 60))
    done
}
launch() {
    local tag=$1 to=$2 d=$3 of=$4; shift 4; local try rc t0 el tot lg
    for try in 1 2; do
        lg=$OUT/log/$tag$([ $try = 2 ] && echo .retry).log
        memwait
        say "launch $tag try $try: mnrun.sh $G env $LINE $* ./ecalc $d $of (timeout $to)"
        t0=$SECONDS
        SLURM_JOB_ID=$SLURM_JOB_ID timeout "$to" ./mnrun.sh "$G" env $LINE "$@" ./ecalc "$d" "$of" > "$lg" 2>&1; rc=$?
        el=$((SECONDS - t0))
        tot=$(grep -m1 -E '^total +[0-9.]+ s' "$lg" | sed -E 's/^total +([0-9.]+) s.*/\1/')
        echo "$tag try $try rc $rc total ${tot:-none} s elapsed $el s nodes $G digits $d $(grep -c -E 'VERIFY OK|RECHECK OK' "$lg") ok-lines $(grep -m1 -E 'mn: all [0-9]+ nodes: (VERIFY|RECHECK) [A-Z]+' "$lg") | $MNRUN_MODULES" | tee -a "$OUT/log/walls.txt" | tee -a "$LOG"
        if [ $rc = 0 ]; then return 0; fi
        if [ -z "$tot" ] && { [ $rc = 124 ] || grep -q -E 'Segmentation fault|_pmi_network_allgather failed|inet_recv: unexpected socket EOF' "$lg"; }; then
            say "launch $tag: the init segfault / PMI hang (A16 4), counted; retrying once"; [ $try = 1 ] && continue
        fi
        return $rc
    done
    return 1
}
cleanup_parts() { [ "$KEEP" = 1 ] && return 0; rm -f "$1".part???? "$1".t1; say "deleted $1.part* ($2)"; }

# ---- plan: the login-node checks (no device) -------------------------------------------------------------------------------
if [ "$STEP" = plan ] || [ "$STEP" = all ]; then
    env $LINE MN_PLAN_ONLY="$DIGITS:$G" ./ecalc > "$OUT/log/plan_$DIGITS.txt" 2>&1
    env $LINE MN_PLAN_ONLY="$BELOW:$G" ./ecalc > "$OUT/log/plan_$BELOW.txt" 2>&1
    env $LINE BS_LAYOUT_ONLY="$((DIGITS / G)):$G" ./ecalc "$DIGITS" /dev/null > "$OUT/log/layout_$DIGITS.txt" 2>&1
    for f in "$OUT/log/plan_$DIGITS.txt" "$OUT/log/plan_$BELOW.txt"; do
        grep -E '^plan (check|summary|pool)' "$f" | cut -c1-200 | tee -a "$LOG"
        grep -q '^plan check .*: OK' "$f" || { say "plan check NOT OK in $f: stop"; exit 3; }
    done
    grep '^room:' "$OUT/log/layout_$DIGITS.txt" | tee -a "$LOG"
    node=$(grep '^room:' "$OUT/log/layout_$DIGITS.txt" | sed -E 's/.*node ([0-9]+) budget.*/\1/')
    [ -n "$node" ] && [ "$node" -gt 480000000000 ] && { say "layout node $node B > 480 GB: stop"; exit 3; }
    say "plan OK: $(grep '^plan pool' "$OUT/log/plan_$DIGITS.txt" | sed -E 's/.*(COMM_SHMEM_POOL_MB=[0-9]+).*/\1/'), node $node B"
    touch "$OUT/PLAN_DONE"
fi

# ---- record: no checkpoint, no top set ----------------------------------------------------------------------------------------
if [ "$STEP" = record ] || [ "$STEP" = all ]; then
    if [ -f "$OUT/RECORD_DONE" ]; then say "record: done already"; else
        df -h "$OUT" | tail -1 | tee -a "$LOG"
        launch rec "$TO_RUN" "$DIGITS" "$OUT/rec/e.out" && touch "$OUT/RECORD_DONE" || { say "record failed"; exit 4; }
        grep -E 'transform cache:|^mn: node 0 level|mem\[0\]|^total|VmHWM|aac7env: node [^ ]+ clocks t=' "$OUT/log/rec.log" | head -40 | cut -c1-200 >> "$LOG"
    fi
fi

# ---- dev: the development form (ECALC_CHECKPOINT=1: the budgeted top set) ----------------------------------------------------
if [ "$STEP" = dev ] || [ "$STEP" = all ]; then
    if [ -f "$OUT/DEV_DONE" ]; then say "dev: done already"; else
        launch dev "$TO_RUN" "$DIGITS" "$OUT/dev/e.out" ECALC_CHECKPOINT=1 || { say "dev failed"; exit 4; }
        if [ -f "$OUT/RECORD_DONE" ]; then        # the packed parts of two runs of the same size and nodes are byte-identical
            # (the header's binary fields [0, 1024) and the limbs after the 4096-byte header; the text at ECP_TEXT_OFF names the run)
            same=1; for p in "$OUT"/rec/e.out.part????; do q=$OUT/dev/${p##*/}; { cmp -s -n 1024 "$p" "$q" && cmp -s -i 4096 "$p" "$q"; } || { same=0; say "dev vs rec: ${p##*/} DIFFERS"; }; done
            [ $same = 1 ] && say "dev vs rec: all $(ls "$OUT"/rec/e.out.part???? | wc -l) packed parts identical" && cleanup_parts "$OUT/dev/e.out" "dev, identical to rec"
        fi
        touch "$OUT/DEV_DONE"
    fi
fi

# ---- chain: off the clock on the record's parts --------------------------------------------------------------------------------
if [ "$STEP" = chain ] || [ "$STEP" = all ]; then
    [ -f "$OUT/RECORD_DONE" ] || { say "chain: no record run"; exit 5; }
    # 1. RECHECK the packed parts (the residues, the windows and T1 in the residue form; no pools; the nodes in parallel)
    if [ -f "$OUT/CHAIN1_DONE" ]; then say "chain 1 done already"; else
        launch recheck_packed "$TO_CHAIN" "$DIGITS" "$OUT/rec/e.out" ECALC_RECHECK=1 || { say "RECHECK (packed) failed"; exit 6; }
        grep -q "mn: all $G nodes: RECHECK OK" "$OUT/log/recheck_packed.log" && touch "$OUT/CHAIN1_DONE" || { say "no 'all $G nodes: RECHECK OK'"; exit 6; }
    fi
    # 2. the per-part conversion, two parts per node in parallel (MN_OUT_DKM_HI: 2G parts; each checked mod the T1 primes as it converts)
    if [ -f "$OUT/CHAIN2_DONE" ]; then say "chain 2 done already"; else
        t0=$SECONDS
        timeout "$TO_CHAIN" srun --jobid="$SLURM_JOB_ID" -N "$G" --ntasks="$((2 * G))" --ntasks-per-node=2 -c 64 --overlap --export=ALL \
            bash -c 'k=$(printf %04d $SLURM_PROCID); t=$SECONDS; '"$HERE"'/../tools/unpack_digits -q -o '"$OUT"'/rec/e.txt.part$k '"$OUT"'/rec/e.out.part$k; rc=$?; echo "unpack part $k rc $rc $((SECONDS - t)) s on $(hostname -s)"; exit $rc' \
            > "$OUT/log/unpack.log" 2>&1; rc=$?
        say "chain 2 unpack: rc $rc, $((SECONDS - t0)) s elapsed for $((2 * G)) parts; per part: $(grep -c 'rc 0' "$OUT/log/unpack.log") ok"
        [ $rc = 0 ] && touch "$OUT/CHAIN2_DONE" || { say "a conversion failed (its residue check or I/O): see $OUT/log/unpack.log"; exit 6; }
        [ -e "$OUT/rec/e.txt.t1" ] || ln -s e.out.t1 "$OUT/rec/e.txt.t1"     # the sidecar the recheck reads beside the ASCII parts
    fi
    # 3a. RECHECK the ASCII parts
    if [ -f "$OUT/CHAIN3_DONE" ]; then say "chain 3 done already"; else
        launch recheck_ascii "$TO_CHAIN" "$DIGITS" "$OUT/rec/e.txt" ECALC_RECHECK=1 || { say "RECHECK (ASCII) failed"; exit 6; }
        grep -q "mn: all $G nodes: RECHECK OK" "$OUT/log/recheck_ascii.log" && touch "$OUT/CHAIN3_DONE" || { say "no 'all $G nodes: RECHECK OK' (ASCII)"; exit 6; }
    fi
    # 3b. the leading 1e11 digits against the reference ("2." + 1e11 digits = 1e11 + 2 bytes; part 0000 holds ~4.6e10 of them)
    if [ -f "$OUT/CHAIN4_DONE" ]; then say "chain 4 done already"; else
        t0=$SECONDS
        if cat "$OUT"/rec/e.txt.part0000 "$OUT"/rec/e.txt.part0001 "$OUT"/rec/e.txt.part0002 | head -c 100000000002 | cmp - <(head -c 100000000002 "$REF11"); then
            say "the 1e11 prefix: identical to $REF11 ($((SECONDS - t0)) s)"; touch "$OUT/CHAIN4_DONE"
        else say "the 1e11 prefix DIFFERS from $REF11"; exit 6; fi
        [ "$KEEP" = 1 ] || { rm -f "$OUT"/rec/e.txt.part????; say "deleted the ASCII parts (RECHECK OK, prefix identical; the packed parts stay)"; }
    fi
    touch "$OUT/CHAIN_DONE"
fi

# ---- below: one grid step below, no checkpoint; RECHECK after it -----------------------------------------------------------------
if [ "$STEP" = below ] || [ "$STEP" = all ]; then
    if [ -f "$OUT/BELOW_DONE" ]; then say "below: done already"; else
        launch below "$TO_RUN" "$BELOW" "$OUT/below/e.out" || { say "below failed"; exit 4; }
        launch recheck_below "$TO_CHAIN" "$BELOW" "$OUT/below/e.out" ECALC_RECHECK=1 || { say "RECHECK (below) failed"; exit 6; }
        grep -q "mn: all $G nodes: RECHECK OK" "$OUT/log/recheck_below.log" && { touch "$OUT/BELOW_DONE"; cleanup_parts "$OUT/below/e.out" "below, RECHECK OK"; } || { say "no RECHECK OK (below)"; exit 6; }
    fi
fi

say "e16_headline.sh $STEP: finished; walls in $OUT/log/walls.txt"
cat "$OUT/log/walls.txt" | tee -a "$LOG"
touch "$OUT/E16_DONE"
[ "${E16_CANCEL:-0}" = 1 ] && scancel "$SLURM_JOB_ID"
exit 0

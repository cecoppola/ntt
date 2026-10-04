#!/bin/bash
# d16_pairs.sh - Phase 16 D (results/D16.md): the interleaved A/B pairs on aac7 (the same nodes, A B A B), under timeout, the init
# segfault (A16 4) retried once and counted, both walls per launch (`total` = the run's clock; elapsed = with the parts written).
#   SLURM_JOB_ID=<J> ./tests/d16_pairs.sh <set>        from ecalc/, after `source aac7env.sh`
# Sets (the nodes = the allocation's):
#   s1   4 nodes, 4e10 total (1e10 per node), the parts on NFS, every run's digits against ~/p16/D/ref_4e10.txt (digcmp.sh):
#        (b) MN_OUT_DKM_HI 1 / 0 (the wall with the NFS write), (d) MN_T_CHUNK_MB 1024 / 2048, (e) COMM_SHMEM_SERIAL 0 / 1,
#        SHMEM_OFI_NUM_NICS default / 4, (c) one run with COMM_LAYER_STATS=1 COMM_XGMI_STATS=1 per form (DKM_HI 1 and 0)
#   s2   4 nodes, 1.6e11 total (4e10 per node: the first size with mn grid products at g = 4 -- the cache has nothing to do at 1e10
#        per node: `plan cache: not wanted`), the parts in the nodes' /tmp (RAM; 17.8 GB per node per run, two runs kept at most):
#        (a) RNS_DIST_CACHE_PARTIAL 1 / 0, (d) MN_T_CHUNK_MB 1024 / 2048; digits: VERIFY OK on every node, every run's parts
#        byte-identical to the set's first run (the limbs after the header and the header's binary fields), and that run's top part
#        against the 1e11 reference's leading digits
#   s8   8 nodes, 8e10 total: MN_GROUPS default (2,4,8) / 2,8 / 8 (A B C A B C), MN_T_CHUNK_MB 1024 / 2048; NFS, against ref_8e10.txt
#   s8c  8 nodes, 3.2e11 total (4e10 per node), /tmp: the cache pair again
# D16_REPS (default 2) pairs per switch; D16_SIZE / D16_NODES override a set's size.
set -u
SET=${1:?set}
cd "$(dirname "$0")/.."; HERE=$(pwd)
[ -n "${SLURM_JOB_ID:-}" ] || { echo "set SLURM_JOB_ID"; exit 1; }
source ./aac7env.sh; export MNRUN_CPUS_PER_TASK=${MNRUN_CPUS_PER_TASK:-auto}
O=${D16_OUT:-$HOME/p16/D}/$SET; mkdir -p "$O"; LOG=$O/pairs.log; W=$O/walls.txt
REPS=${D16_REPS:-2}
say() { echo "$(date '+%Y-%m-%dT%H:%M:%S%z') $*" | tee -a "$LOG"; }
nodes=$(scontrol show hostnames "$(squeue -j "$SLURM_JOB_ID" -h -o %N)"); NN=$(echo "$nodes" | wc -l)
BASE="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_CHECKPOINT=1 ECALC_VERBOSE=2 ECALC_LOG_CLOCKS=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6 MEM_REPORT_DEVS=1"
TMPD=/tmp/p16D
case $SET in
    s1)  G=${D16_NODES:-4}; D=${D16_SIZE:-40000000000};  MODE=nfs; REF=$HOME/p16/D/ref_4e10.txt;;
    s2)  G=${D16_NODES:-4}; D=${D16_SIZE:-160000000000}; MODE=tmp; REF=$HOME/ntt/ecalc/results/e_1e11.out;;
    s8)  G=${D16_NODES:-8}; D=${D16_SIZE:-80000000000};  MODE=nfs; REF=$HOME/p16/D/ref_8e10.txt;;
    s8c) G=${D16_NODES:-8}; D=${D16_SIZE:-320000000000}; MODE=tmp; REF=$HOME/ntt/ecalc/results/e_1e11.out;;
    *) echo "set: s1 s2 s8 s8c"; exit 1;;
esac
[ "$NN" -ge "$G" ] || { say "the allocation has $NN nodes, the set needs $G"; exit 1; }
[ "$MODE" = nfs ] && [ ! -s "$REF" ] && { say "no reference $REF"; exit 1; }
onall() { srun --jobid="$SLURM_JOB_ID" -N "$G" --ntasks="$G" --ntasks-per-node=1 -c 8 --overlap --export=ALL bash -c "$1"; }
[ "$MODE" = tmp ] && onall "mkdir -p $TMPD; df -h /tmp | tail -1"
BASERUN=""   # tmp mode: the first run's directory (the byte baseline)
LAUNCHES=0; SEGV=0

# run <tag> <VAR=value ...>: one launch of D digits on G nodes with BASE + the words (a later word overrides an earlier one in env)
run() {
    local tag=$1; shift; local try rc t0 el tot lg of ver
    if [ "$MODE" = nfs ]; then of=$O/$tag/e.out; mkdir -p "$O/$tag"; else of=$TMPD/$tag/e.out; onall "mkdir -p $TMPD/$tag"; fi
    for try in 1 2; do
        lg=$O/$tag$([ $try = 2 ] && echo .retry).log; LAUNCHES=$((LAUNCHES + 1))
        say "run $tag try $try: mnrun.sh $G env $BASE $* ./ecalc $D $of"
        t0=$SECONDS; timeout "${D16_TIMEOUT:-1800}" ./mnrun.sh "$G" env $BASE "$@" ./ecalc "$D" "$of" > "$lg" 2>&1; rc=$?; el=$((SECONDS - t0))
        tot=$(grep -m1 -E '^total +[0-9.]+ s' "$lg" | sed -E 's/^total +([0-9.]+) s.*/\1/')
        ver=$(grep -m1 -oE "mn: all $G nodes: VERIFY [A-Z]+" "$lg")
        if [ $rc != 0 ] && [ -z "$tot" ] && { [ $rc = 124 ] || grep -q -E 'Segmentation fault|_pmi_network_allgather failed|inet_recv: unexpected socket EOF' "$lg"; }; then
            SEGV=$((SEGV + 1)); say "run $tag try $try: the init segfault / PMI hang (rc $rc, $el s), counted ($SEGV of $LAUNCHES launches)"; [ $try = 1 ] && continue
        fi
        break
    done
    # the digits
    local dig=
    if [ "$MODE" = nfs ]; then
        dig=$(./digcmp.sh "$of" "$REF" 2>&1 | tail -1)
    else
        if [ -z "$BASERUN" ]; then
            BASERUN=$TMPD/$tag
            local np; np=$(onall "ls $TMPD/$tag/e.out.part* 2>/dev/null | wc -l" | awk '{ s += $1 } END { print s }')
            # the top part (part0000, on the node that holds it) against the reference's leading digits (part0000 = the top node's X_hi share)
            dig="baseline ($np parts); top part vs $(basename "$REF"): $(onall "p=$TMPD/$tag/e.out.part0000; [ -f \$p ] || exit 0; n=\$($HERE/../tools/unpack_digits -q \$p | head -c 10000000002 | wc -c); $HERE/../tools/unpack_digits -q \$p | head -c \$n | cmp -s - <(head -c \$n $REF) && echo \"identical (\$n bytes)\" || echo DIFFERS" | grep -v '^$' | paste -sd' ')"
        else
            dig=$(onall "cd $TMPD/$tag 2>/dev/null || exit 0; bad=0; for p in e.out.part*; do q=$BASERUN/\$p; [ -f \$q ] || { bad=1; continue; }; cmp -s -n 1024 \$p \$q && cmp -s -i 4096 \$p \$q || bad=1; done; [ \$bad = 0 ] && echo identical || echo DIFFERS" | sort | uniq -c | paste -sd' ')
            dig="vs baseline: $dig"
        fi
    fi
    echo "$tag rc $rc total ${tot:-none} s elapsed $el s | ${ver:-no VERIFY line} | $dig | $*" | tee -a "$W" | tee -a "$LOG"
    grep -E '^ +transform cache:|^plan cache|^mn: node 0 level|^divmod\(mn|^recip\(mn|^scratch\(mn|^layer-stats|^xgmi-stats|comm_shmem: pe 0: all-to-all|mem\[0\]|VmHWM|^mn_out|^total|dist_mn node 0:' "$lg" | cut -c1-300 > "$O/$tag.key"
    [ "${D16_KEEP:-0}" = 1 ] && return 0
    if [ "$MODE" = nfs ]; then rm -f "$of".part???? "$of".t1; else [ "$TMPD/$tag" = "$BASERUN" ] || onall "rm -rf $TMPD/$tag"; fi
    return 0
}
pair() {   # pair <name> <A words> <B words> [<C words>]: A B [C] interleaved, REPS times
    local name=$1 a=$2 b=$3 c=${4:-}; local i
    for i in $(seq 1 "$REPS"); do run "${name}_A$i" $a; run "${name}_B$i" $b; [ -n "$c" ] && run "${name}_C$i" $c; done
}
say "set $SET: $G nodes ($(echo "$nodes" | head -n "$G" | paste -sd,)), $D digits, $MODE, $REPS pairs per switch"
case $SET in
    s1)
        pair dkm  "MN_OUT_DKM_HI=1" "MN_OUT_DKM_HI=0"
        pair tch  "MN_T_CHUNK_MB=1024" "MN_T_CHUNK_MB=2048"
        pair ser  "COMM_SHMEM_SERIAL=0" "COMM_SHMEM_SERIAL=1"
        pair nic  "SHMEM_OFI_NUM_NICS=1" "SHMEM_OFI_NUM_NICS=4"
        run stats_dkm1 COMM_LAYER_STATS=1 COMM_XGMI_STATS=1 MN_OUT_DKM_HI=1
        run stats_dkm0 COMM_LAYER_STATS=1 COMM_XGMI_STATS=1 MN_OUT_DKM_HI=0;;
    s2|s8c)
        pair cache "RNS_DIST_CACHE_PARTIAL=1" "RNS_DIST_CACHE_PARTIAL=0"
        pair tch   "MN_T_CHUNK_MB=1024" "MN_T_CHUNK_MB=2048"
        run stats COMM_LAYER_STATS=1 COMM_XGMI_STATS=1;;
    s8)
        pair grp "MN_GROUPS=2,4,8" "MN_GROUPS=2,8" "MN_GROUPS=8"
        pair tch "MN_T_CHUNK_MB=1024" "MN_T_CHUNK_MB=2048"
        run stats COMM_LAYER_STATS=1 COMM_XGMI_STATS=1;;
esac
[ "$MODE" = tmp ] && [ "${D16_KEEP:-0}" != 1 ] && onall "rm -rf $TMPD"
say "set $SET done: $LAUNCHES launches, $SEGV init failures; walls in $W"
touch "$O/DONE"

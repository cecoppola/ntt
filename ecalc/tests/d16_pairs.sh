#!/bin/bash
# d16_pairs.sh - Phase 16 D (results/D16.md): the interleaved A/B pairs on aac7 -- one fresh sbatch job per pair (B16 measured the
# walls drifting up run after run inside one allocation, and C16 found a node refusing every later SHMEM launch once a step was
# killed: "Error configuring interconnect"), the same nodes within a pair (A B [C]), under timeout; a launch failure (the init
# segfault of A16 4, a timeout) cancels the job and the pair is resubmitted once (counted).  Both walls per launch (`total` = the
# run's clock; elapsed = with the parts written), the job's nodes and their state at the start logged per pair.
#   ./tests/d16_pairs.sh <set>        from ecalc/, after `source aac7env.sh` (no SLURM_JOB_ID: the script allocates)
# Sets:
#   s1   4 nodes, 4e10 total (1e10 per node), the parts on NFS, every run's digits against ~/p16/D/ref_4e10.txt (digcmp.sh):
#        (b) MN_OUT_DKM_HI 1 / 0, (d) MN_T_CHUNK_MB 1024 / 2048, (e) COMM_SHMEM_SERIAL 0 / 1, SHMEM_OFI_NUM_NICS 1 / 4,
#        (c) one run with COMM_LAYER_STATS=1 COMM_XGMI_STATS=1 per form (DKM_HI 1 and 0)
#   s2   4 nodes, 1.6e11 total (4e10 per node: the first size with mn grid products at g = 4 -- the cache has nothing to do at 1e10
#        per node, `plan cache: not wanted`), the parts in the nodes' /tmp (RAM; 17.8 GB per node per run, deleted after the hashes):
#        (a) RNS_DIST_CACHE_PARTIAL 1 / 0, (d) MN_T_CHUNK_MB 1024 / 2048; digits: VERIFY OK on every node, every part's sha1 (the
#        header's binary fields + the limbs) equal to the set's first run's, that run's top part against the 1e11 reference's prefix
#   s8   8 nodes, 8e10 total: MN_GROUPS default (2,4,8) / 2,8 / 8 (A B C), MN_T_CHUNK_MB 1024 / 2048; NFS, against ref_8e10.txt
#   s8c  8 nodes, 3.2e11 total (4e10 per node), /tmp: the cache pair again
#   pe   2 nodes, 2e10 total: one node-process per node (`mnrun.sh 2`, four contexts on one NIC) vs four per node (`mnrun.sh 8`,
#        one per APU, a NIC each -- C16's NIC spread), identical digits (ref: the 1e11 prefix, 2e10 + 2 bytes)
# D16_REPS (default 2) pairs per switch; D16_SIZE / D16_NODES override a set's size; D16_WALLTIME the job's limit (0:45:00).
set -u
SET=${1:?set}
cd "$(dirname "$0")/.."; HERE=$(pwd)
source ./aac7env.sh; export MNRUN_CPUS_PER_TASK=${MNRUN_CPUS_PER_TASK:-auto}
O=${D16_OUT:-$HOME/p16/D}/$SET; mkdir -p "$O"; LOG=$O/pairs.log; W=$O/walls.txt
REPS=${D16_REPS:-2}
say() { echo "$(date '+%Y-%m-%dT%H:%M:%S%z') $*" | tee -a "$LOG"; }
BASE="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_CHECKPOINT=1 ECALC_VERBOSE=2 ECALC_LOG_CLOCKS=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6 MEM_REPORT_DEVS=1"
TMPD=/tmp/p16D
case $SET in
    s1)  G=${D16_NODES:-4}; D=${D16_SIZE:-40000000000};  MODE=nfs; REF=$HOME/p16/D/ref_4e10.txt;;
    s2)  G=${D16_NODES:-4}; D=${D16_SIZE:-160000000000}; MODE=tmp; REF=$HOME/ntt/ecalc/results/e_1e11.out;;
    s8)  G=${D16_NODES:-8}; D=${D16_SIZE:-80000000000};  MODE=nfs; REF=$HOME/p16/D/ref_8e10.txt;;
    s8c) G=${D16_NODES:-8}; D=${D16_SIZE:-320000000000}; MODE=tmp; REF=$HOME/ntt/ecalc/results/e_1e11.out;;
    pe)  G=${D16_NODES:-2}; D=${D16_SIZE:-20000000000};  MODE=nfs; REF=$HOME/p16/D/ref_2e10.txt;;
    *) echo "set: s1 s2 s8 s8c pe"; exit 1;;
esac
[ "$MODE" = nfs ] && [ ! -s "$REF" ] && { say "no reference $REF"; exit 1; }
WALL=${D16_WALLTIME:-0:45:00}
J=; NODES=
onall() { srun --jobid="$J" -N "$G" --ntasks="$G" --ntasks-per-node=1 -c 4 --overlap --export=ALL bash -c "$1"; }
LAUNCHES=0; SEGV=0; BASEHASH=$O/baseline.sha1

job_start() {   # a fresh exclusive allocation of G nodes; waits for it (D16_JOBWAIT s, default 2 h); the nodes' state logged
    local s; J=$(sbatch -p "$AAC7_PART" -N "$G" --exclusive -c 192 --gpus-per-node=4 -t "$WALL" -J D16 --parsable --wrap "sleep $(echo "$WALL" | awk -F: '{ print $1 * 3600 + $2 * 60 + $3 }')")
    echo "$J $(date '+%FT%T') $SET $1" >> "$HOME/p16/D/jobs.txt"
    local w=${D16_JOBWAIT:-7200}; while [ "$w" -gt 0 ]; do s=$(squeue -j "$J" -h -o %T 2>/dev/null); [ "$s" = RUNNING ] && break; [ -z "$s" ] && { say "job $J vanished"; return 1; }; sleep 15; w=$((w - 15)); done
    [ "$s" = RUNNING ] || { say "job $J did not start in time"; scancel "$J"; return 1; }
    NODES=$(scontrol show hostnames "$(squeue -j "$J" -h -o %N)" | paste -sd,)
    say "pair $1: job $J nodes $NODES"
    onall 'h=$(hostname -s); echo "$h load $(cut -d" " -f1-3 /proc/loadavg) memavail $(awk "/MemAvailable/ { printf \"%.0f GB\", \$2 / 1e6 }" /proc/meminfo) tmp $(df -h /tmp | awk "NR == 2 { print \$3 }") gpu-procs $(rocm-smi --showpids 2>/dev/null | grep -cE "^[0-9]+ ") $(rocm-smi --showclocks 2>/dev/null | grep -oE "GPU\[[0-9]\].*sclk[^(]*\([0-9]+Mhz\)" | sed -E "s/.*GPU\[([0-9])\].*\(([0-9]+)Mhz\)/sclk\1=\2/" | paste -sd" ")"' 2>&1 | sort | tee "$O/nodes_$J.txt" >> "$LOG"
    [ "$MODE" = tmp ] && onall "mkdir -p $TMPD" > /dev/null
    return 0
}
job_end() { [ -n "$J" ] && { [ "$MODE" = tmp ] && [ "${D16_KEEP:-0}" != 1 ] && onall "rm -rf $TMPD" > /dev/null 2>&1; scancel "$J"; }; J=; }

# run <tag> <VAR=value ... | procs=N>: one launch of D digits over the job's G nodes (procs=N: N node-processes instead of G);
# returns 0 ok, 1 digits / verify problem, 2 a launch failure (the job is poisoned: the caller resubmits the pair)
run() {
    local tag=$1; shift; local rc t0 el tot lg of ver P=$G w; local -a words=()
    for w in "$@"; do case $w in procs=*) P=${w#procs=};; *) words+=("$w");; esac; done
    if [ "$MODE" = nfs ]; then of=$O/$tag/e.out; mkdir -p "$O/$tag"; else of=$TMPD/$tag/e.out; onall "mkdir -p $TMPD/$tag" > /dev/null; fi
    lg=$O/$tag.log; LAUNCHES=$((LAUNCHES + 1))
    say "run $tag (job $J, $NODES): mnrun.sh $P env $BASE ${words[*]} ./ecalc $D $of"
    t0=$SECONDS; SLURM_JOB_ID=$J timeout "${D16_TIMEOUT:-1500}" ./mnrun.sh "$P" env $BASE "${words[@]}" ./ecalc "$D" "$of" > "$lg" 2>&1; rc=$?; el=$((SECONDS - t0))
    tot=$(grep -m1 -E '^total +[0-9.]+ s' "$lg" | sed -E 's/^total +([0-9.]+) s.*/\1/')
    ver=$(grep -m1 -oE "mn: all $P nodes: VERIFY [A-Z]+" "$lg")
    grep -E '^ +transform cache:|^plan cache|^mn: node 0 level|^divmod\(mn|^recip\(mn|^scratch\(mn|^layer-stats|^xgmi-stats|comm_shmem: pe [0-9]+: all-to-all|mem\[0\]|VmHWM|^mn_out|^total|dist_mn node 0:|aac7env: node [^ ]+ clocks' "$lg" | cut -c1-300 > "$O/$tag.key"
    if [ $rc != 0 ] && [ -z "$tot" ]; then
        SEGV=$((SEGV + 1)); say "run $tag: launch failure rc $rc after $el s ($(grep -m1 -oE 'Segmentation fault|_pmi_network_allgather failed|Error configuring interconnect|PMI2_Init failed' "$lg" || echo no known signature)); $SEGV of $LAUNCHES launches"
        echo "$tag rc $rc total none s elapsed $el s | LAUNCH FAILURE | job $J nodes $NODES | ${words[*]} procs $P | $MNRUN_MODULES" | tee -a "$W" >> "$LOG"; return 2
    fi
    # the digits (digcmp.sh of a 4e10 NFS file took 14 min on the login node): every part's sha1 over the header's binary fields and
    # the limbs (not the text at 1024, which names the run), taken on the nodes (their page cache holds what they just wrote) and
    # compared with the set's first run's; that first run's top part (part0000: the top node's X_hi share) against the reference's
    # leading digits through unpack_digits
    local dig= pd=$(dirname "$of")
    onall "cd $pd 2>/dev/null || exit 0; for p in e.out.part*; do [ -f \$p ] || continue; echo \"\$p \$( (head -c 1024 \$p; tail -c +4097 \$p) | sha1sum | cut -c1-40)\"; done" | sort -u > "$O/$tag.sha1"
    if [ ! -s "$BASEHASH" ]; then
        cp "$O/$tag.sha1" "$BASEHASH"
        local top=/tmp/p16D_top_$$.txt
        if [ "$MODE" = nfs ]; then "$HERE/../tools/unpack_digits" -q "$pd/e.out.part0000" > "$top"; n=$(stat -c %s "$top"); head -c "$n" "$REF" | cmp -s - "$top" && t="identical ($n bytes)" || t=DIFFERS; rm -f "$top"
        else t=$(onall "p=$pd/e.out.part0000; [ -f \$p ] || exit 0; $HERE/../tools/unpack_digits -q \$p > $top; n=\$(stat -c %s $top); head -c \$n $REF | cmp -s - $top && echo \"identical (\$n bytes)\" || echo DIFFERS; rm -f $top" | grep -v '^$' | paste -sd' '); fi
        dig="baseline ($(wc -l < "$BASEHASH") parts hashed); top part vs $(basename "$REF"): $t"
    else cmp -s "$O/$tag.sha1" "$BASEHASH" && dig="identical to the baseline ($(wc -l < "$O/$tag.sha1") parts)" || dig="DIFFERS from the baseline"; fi
    [ "$MODE" = tmp ] && [ "${D16_KEEP:-0}" != 1 ] && onall "rm -rf $pd" > /dev/null
    echo "$tag rc $rc total ${tot:-none} s elapsed $el s | ${ver:-no VERIFY line} | $dig | job $J nodes $NODES | ${words[*]} procs $P | $MNRUN_MODULES" | tee -a "$W" >> "$LOG"
    [ "$MODE" = nfs ] && [ "${D16_KEEP:-0}" != 1 ] && rm -f "$of".part???? "$of".t1
    [ $rc = 0 ] && [[ "$dig" != *DIFFERS* ]] && [[ "$ver" == *"VERIFY OK" ]]; return $?
}
pair() {   # pair <name> <A words> <B words> [<C words>]: one job per repetition: A B [C] on the same nodes; a launch failure -> a new job, once
    local name=$1 a=$2 b=$3 c=${4:-}; local i try r
    for i in $(seq 1 "$REPS"); do
        for try in 1 2; do
            job_start "${name}$i" || return 1
            r=0; run "${name}_A$i$([ $try = 2 ] && echo r)" $a; [ $? = 2 ] && r=2
            [ $r = 0 ] && { run "${name}_B$i$([ $try = 2 ] && echo r)" $b; [ $? = 2 ] && r=2; }
            [ $r = 0 ] && [ -n "$c" ] && { run "${name}_C$i$([ $try = 2 ] && echo r)" $c; [ $? = 2 ] && r=2; }
            job_end
            [ $r = 0 ] && break; say "pair ${name}$i try $try: a launch failed, the job is dropped; resubmitting the pair"
        done
    done
}
single() { job_start "$1" || return 1; run "$@"; job_end; }
say "set $SET: $G nodes per job, $D digits, $MODE, $REPS pairs per switch, one job per pair"
case $SET in
    s1)
        pair dkm  "MN_OUT_DKM_HI=1" "MN_OUT_DKM_HI=0"
        pair tch  "MN_T_CHUNK_MB=1024" "MN_T_CHUNK_MB=2048"
        pair ser  "COMM_SHMEM_SERIAL=0" "COMM_SHMEM_SERIAL=1"
        pair nic  "SHMEM_OFI_NUM_NICS=1" "SHMEM_OFI_NUM_NICS=4"
        pair stats "COMM_LAYER_STATS=1 COMM_XGMI_STATS=1 MN_OUT_DKM_HI=1" "COMM_LAYER_STATS=1 COMM_XGMI_STATS=1 MN_OUT_DKM_HI=0";;
    s2|s8c)
        pair cache "RNS_DIST_CACHE_PARTIAL=1" "RNS_DIST_CACHE_PARTIAL=0"
        pair tch   "MN_T_CHUNK_MB=1024" "MN_T_CHUNK_MB=2048"
        single stats COMM_LAYER_STATS=1 COMM_XGMI_STATS=1;;
    s8)
        pair grp "MN_GROUPS=2,4,8" "MN_GROUPS=2,8" "MN_GROUPS=8"
        pair tch "MN_T_CHUNK_MB=1024" "MN_T_CHUNK_MB=2048"
        single stats COMM_LAYER_STATS=1 COMM_XGMI_STATS=1;;
    pe)
        pair pe "procs=2" "procs=8";;
esac
say "set $SET done: $LAUNCHES launches, $SEGV launch failures; walls in $W"
touch "$O/DONE"

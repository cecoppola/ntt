#!/bin/bash
# dl15_batch.sh <stage> <expected short sha> - agent DL (Phase 15 Batch 3, results/DL15.md): one unattended node batch.
#   A  10^11 at size 1, the layout that follows NEWTON_DKM (this clone) against today's (~/ntt-DL15base = ab8daa2), NEWTON_DKM=1 on both
#      (the default), interleaved new / base x 2 with ECALC_LIVE=1 MEM_REPORT_DEVS=1 DB_POOL_VERBOSE=1 (the arena, the pool's peak, remaps,
#      hipMalloc); the packed files cmp'd against the first run's, the first against the reference at the end (digcmp.sh)
#   B  1.3 x 10^11 at size 1 (new, then base) with the same switches, the file discarded (VERIFY is internal); 4 x 10^10 (new) against
#      results/e_4e10.out; 10^10 at sizes 2, 3, 4 on one node (mnrun.sh, POOL_LOG=29) with ECALC_LIVE=1 MEM_REPORT_DEVS=1, the parts against
#      the first 10^10 digits of e_4e10.out
#   C  ./mnaccept.sh <job> --stress --only unit,e9,mn,corr,stress
# One 1-node job (-J DL, 45 min; NODE=<host> pins it), cancelled at the end.  Runs are strictly one after another (no run maps VMM
# memory while another's kernels run).  Logs in ~/dl15tmp/<stage>/.
ST=$1 SHA=$2
cd ~/ntt-DL15/ecalc || exit 1
L=~/dl15tmp/$ST; mkdir -p "$L"; exec > "$L/batch.log" 2>&1
[ "$(git rev-parse --short=7 HEAD)" = "${SHA:0:7}" ] || { echo "WRONG COMMIT $(git log --oneline -1)"; exit 1; }
echo "== DL15 batch $ST $(date) $(git log --oneline -1); base $(git -C ~/ntt-DL15base log --oneline -1)"
X=; [ -n "${NODE:-}" ] && X="-w $NODE"
J=$(sbatch -p PPAC_MI300A_SPX -N1 $X --gpus=4 -t 0:45:00 -J DL --parsable --wrap "sleep 2700") || exit 1
trap 'scancel $J' EXIT
echo "$J" > ~/dl15tmp/"$ST".job
until [ "$(squeue -j "$J" -h -o %T)" = RUNNING ]; do [ -z "$(squeue -j "$J" -h -o %T)" ] && { echo "job $J gone"; exit 1; }; sleep 20; done
NODE=$(squeue -j "$J" -h -o %N); echo "job $J on $NODE $(date)"
R() { local d=$1; shift; srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $d; $*"; }
N() { srun --jobid="$J" -N1 --overlap bash -c "$*"; }
NEW=~/ntt-DL15/ecalc BASE=~/ntt-DL15base/ecalc T=/tmp/dl15_$J
N "mkdir -p $T"
cmpref() { N "$NEW/digcmp.sh $1 $2"; }
evict() { N "python3 -c \"import os,sys
for p in sys.argv[1:]:
    fd=os.open(p,os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED); os.close(fd)\" $*"; }
MEMSW="ECALC_LIVE=1 MEM_REPORT_DEVS=1 DB_POOL_VERBOSE=1 ECALC_VERBOSE=2 RNS_VERBOSE=1"
summ() { # log: the lines the report reads
    local log=$1
    echo "   $(grep -a '^total' "$log" | awk '{print "total", $2}'); $(grep -a '^recip ' "$log" | awk '{print "recip", $2}'); $(grep -a '^dm ' "$log" | awk '{print "dm", $2}'); $(grep -a '^bs ' "$log" | awk '{print "bs", $2}'); $(grep -a '^init ' "$log" | awk '{print "init", $2}'); $(grep -ac 'VERIFY OK' "$log") VERIFY OK, $(grep -ac 'VERIFY FAILED' "$log") FAILED"
    grep -a 'bs: dm layout\|bs: arenas\|VMM arena [0-9]\|VMM remap\|VMM growth\|hipMalloc [0-9.]* GB inside\|VMM arena released\|tail [0-9.]* GB:\|BS_ARENA_ROOM\|divmod(dev\|divmod(mn\|^mem \(init\|bs\|recip\|division\|dm\|summary\)\|^live:.*divmod\|budget:' "$log" | cut -c1-330 | sed 's/^/   /'
}
case $ST in
A)
    E11=~/ntt/ecalc/results/e_1e11.out; K=$T/keep.txt
    run11() { # tag dir env...
        local tag=$1 d=$2; shift 2; local log=$L/$tag.log f=$T/e11.txt c
        N "rm -rf $f $f.*"; evict $E11
        local t0; t0=$(date +%s.%N)
        R "$d" "env $* $MEMSW ./ecalc 100000000000 $f" > "$log" 2>&1; local rc=$?
        local wall; wall=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b - a}')
        if N "test -e $K"; then c=$(N "cmp -s $f $K && echo 'same bytes as the first run' || echo 'DIFFERS from the first run'; rm -rf $f $f.*"); else N "mv $f $K; rm -rf $f.*"; c="kept (compared to the reference at the end)"; fi
        echo "RUN11 $tag: rc $rc wall ${wall} s; $c"; summ "$log"
    }
    run11 new_1 $NEW; run11 base_1 $BASE; run11 new_2 $NEW; run11 base_2 $BASE
    echo "RUN keep (new_1) against the reference: $(cmpref $K $E11) $(date)"
    ;;
B)
    run13() { local tag=$1 d=$2; local log=$L/$tag.log f=$T/e13.txt
        N "rm -rf $f $f.*"
        R "$d" "env $MEMSW ./ecalc 130000000000 $f" > "$log" 2>&1; local rc=$?
        N "rm -rf $f $f.*"; echo "RUN13 $tag: rc $rc"; summ "$log"; }
    run13 new13 $NEW; run13 base13 $BASE
    E4=~/ntt/ecalc/results/e_4e10.out
    R $NEW "env $MEMSW ./ecalc 40000000000 $T/e4.txt" > "$L/e4_new.log" 2>&1; rc=$?
    echo "RUN e4_new: rc $rc; $(cmpref $T/e4.txt $E4)"; summ "$L/e4_new.log"; N "rm -rf $T/e4.txt $T/e4.txt.*"
    N "head -c 10000000002 $E4 > $T/ref_1e10.txt; printf '\n' >> $T/ref_1e10.txt"
    for p in 2 3 4; do
        f=$T/e10_$p.txt; log=$L/e10_p$p.log
        SLURM_JOB_ID=$J timeout 900 ./mnrun.sh "$p" env POOL_LOG=29 $MEMSW ./ecalc 10000000000 "$f" > "$log" 2>&1; rc=$?
        echo "RUN e10 size $p: rc $rc; $(cmpref $f $T/ref_1e10.txt)"; summ "$log"; N "rm -rf $f $f.*"
    done
    ;;
C)
    ./mnaccept.sh "$J" --stress --only unit,e9,mn,corr,stress 2>&1 | grep -a '^PASS\|^FAIL\|passed\|^=='
    ;;
esac
N "rm -rf $T"
echo "== DL15 batch $ST done $(date)"

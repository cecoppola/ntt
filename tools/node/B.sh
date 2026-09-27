# IO15: the helpers with the time limit inside (timeout cannot run a shell function: job.sh's first step() ran nothing, rc 127)
G() { timeout ${STO:-1500} srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm; cd $HOME/ntt-IO15/ecalc; $*" < /dev/null; }
N() { timeout ${STO:-1500} srun --jobid=$J -N1 --overlap bash -c "cd $HOME/ntt-IO15/ecalc; $*" < /dev/null; }
M() { local p=$1; shift; SLURM_JOB_ID=$J timeout ${STO:-1500} ./mnrun.sh $p "$@" < /dev/null; }
step() { local tag=$1; shift; local el=$(( $(date +%s) - T0 )); [ $el -gt ${TMAX:-2400} ] && { echo "SKIP $tag (time $el)"; return; }
         echo "== $tag $(date +%T)"; local t1=$(date +%s); "$@" > $D/$tag.log 2>&1; local rc=$?
         echo "   rc $rc, $(( $(date +%s) - t1 )) s; $(grep -a '^total\|VERIFY\|IDENTICAL\|identical\|differ\|RECHECK OK\|RECHECK FAILED\|FATAL\|^rc=\|^wrote\|unpack_digits:\|^wall' $D/$tag.log | head -8 | cut -c1-220 | tr '\n' '|')"; }
# Batch B: W1 -- the digit writer on aac6's NFS /shared (the stand-in for a network file system) at 1e10 digits: the write
# modes (O_DIRECT, buffered, buffered + fsync + DONTNEED at close, fdatasync + DONTNEED per chunk), the pwrite threads, the
# chunk size, packed; a local /tmp run for the reference wall; dd for the path's ceiling.  Every run: VERIFY in the run, the
# memory sampler's summary (Cached / Dirty: the page cache is HBM), the wall of the process (with the write).
P=$HOME/IO15/w; mkdir -p $P
F='^VERIFY\|^total\|^wrote\|^mn_out\|^dc \|^T2 \|FATAL'
N "hostname; df -h /tmp $P | tail -2; free -g | head -2" > $D/node.txt 2>&1
step dd N "for i in 1 2; do dd if=/dev/zero of=$P/dd.bin bs=64M count=64 oflag=direct 2>&1 | tail -1; rm -f $P/dd.bin; dd if=/dev/zero of=$P/dd.bin bs=64M count=64 conv=fsync 2>&1 | tail -1; rm -f $P/dd.bin; done"
step wb N "cd $HOME/ntt-IO15/tools; for a in '-m direct -t 8' '-m direct -t 32' '-m direct -t 2' '-m sync -t 8' '-m drop -t 8' '-m direct -t 8'; do ./wbench \$a -s 1 $P/wb.%h; done; ip -br link | grep UP; cat /sys/class/net/*/speed 2>/dev/null | sort | uniq -c"
w1() { local tag=$1 dir=$2; shift 2
       step $tag G "t0=\$(date +%s.%N); env $* ECALC_MEM_SAMPLE=2 ECALC_MEM_SAMPLE_FILE=$D/$tag.mem ECALC_VERBOSE=2 ./ecalc 10000000000 $dir/io_$tag.txt | grep -a '$F'; echo wall \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s; ls -l $dir/io_$tag.txt; tail -1 $D/$tag.mem; rm -rf $dir/io_$tag.txt*"; }
w1 loc   /tmp
w1 d8    $P
w1 d32   $P MN_OUT_THREADS=32
w1 d32c  $P MN_OUT_THREADS=32 MN_OUT_CHUNK_MB=1024
w1 buf   $P ECALC_OUT_MODE=buffered
w1 sync  $P ECALC_OUT_MODE=sync
w1 drop  $P ECALC_OUT_MODE=drop
w1 d2    $P MN_OUT_THREADS=2
w1 pkd   $P ECALC_OUT_PACKED=1
w1 pks   $P ECALC_OUT_PACKED=1 ECALC_OUT_MODE=sync
w1 sync32 $P ECALC_OUT_MODE=sync MN_OUT_THREADS=32
N "rm -rf $P"

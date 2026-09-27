# Batch B: W1 -- the digit writer on aac6's NFS /shared (the stand-in for a network file system) at 1e10 digits: the write
# modes (O_DIRECT, buffered, buffered + fsync + DONTNEED at close, fdatasync + DONTNEED per chunk), the pwrite threads, the
# chunk size, packed; a local /tmp run for the reference wall; dd for the path's ceiling.  Every run: VERIFY in the run, the
# memory sampler's summary (Cached / Dirty: the page cache is HBM), the wall of the process (with the write).
P=$HOME/IO15/w; mkdir -p $P
F='^VERIFY\|^total\|^wrote\|^mn_out\|^dc \|^T2 \|FATAL'
N "hostname; df -h /tmp $P | tail -2; free -g | head -2" > $D/node.txt 2>&1
step dd N "for i in 1 2; do dd if=/dev/zero of=$P/dd.bin bs=64M count=64 oflag=direct 2>&1 | tail -1; rm -f $P/dd.bin; dd if=/dev/zero of=$P/dd.bin bs=64M count=64 conv=fsync 2>&1 | tail -1; rm -f $P/dd.bin; done"
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

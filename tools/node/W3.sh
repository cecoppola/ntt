# Batch W3 (2 nodes, NN=2): at 3e9 digits (2 nodes over aac6's 1 GbE: 1e9 took 180 s, 1e10 would take ~30 min) -- the part
# files to NFS: without MN_OUT_EARLY, with it, and with MN_OUT_WAVES=2; the three runs' parts compared by sha1 (VERIFY in each).
P=$HOME/IO15/w3; mkdir -p $P
N2() { timeout ${STO:-1500} srun --jobid=$J -N2 --ntasks-per-node=1 --overlap bash -c "cd $HOME/ntt-IO15/ecalc; $*" < /dev/null; }
N2 "hostname; ls -la /tmp | grep io_; rm -rf /tmp/io_*" > $D/node.txt 2>&1
for v in "n3:" "e3:MN_OUT_EARLY=1" "w3:MN_OUT_WAVES=2"; do
  tag=${v%%:*}; env=${v#*:}
  step $tag M 2 env $env ECALC_VERBOSE=2 ./ecalc 3000000000 $P/io_$tag.txt
  N "cat $P/io_$tag.txt.part* | sha1sum; ls -l $P/io_$tag.txt*; rm -rf $P/io_$tag.txt*" >> $D/$tag.log 2>&1
done
N "rm -rf $P"

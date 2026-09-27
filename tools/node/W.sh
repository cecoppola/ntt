# Batch W (2 nodes, NN=2): the waves across real nodes (ASCII and packed, to NFS so the parts meet), MN_OUT_EARLY at 1e10
# (part files on each node's /tmp; each part's sha1 on its node, compared between the runs), the NFS aggregate with 2 nodes
# writing at once vs in 2 waves.
R=$HOME/ntt/ecalc/ref/e_1000000000.txt; P=$HOME/IO15/w2; mkdir -p $P
N2() { timeout ${STO:-1500} srun --jobid=$J -N2 --ntasks-per-node=1 --overlap bash -c "cd $HOME/ntt-IO15/ecalc; $*" < /dev/null; }
N2 "hostname; df -h /tmp | tail -1" > $D/node.txt 2>&1
step w2a M 2 env MN_OUT_WAVES=2 ECALC_VERBOSE=2 ./ecalc 1000000000 $P/io_w2a.txt
step w2ac N "cat $P/io_w2a.txt.part* | cmp - $R && echo IDENTICAL; rm -rf $P/io_w2a.txt*"
step w2p M 2 env MN_OUT_WAVES=2 ECALC_OUT_PACKED=1 ./ecalc 1000000000 $P/io_w2p.txt
step w2pc N "$U --cmp $R $P/io_w2p.txt.part*; rm -rf $P/io_w2p.txt*"
step e2p M 2 env MN_OUT_EARLY=1 ECALC_OUT_PACKED=1 ECALC_VERBOSE=2 ./ecalc 1000000000 $P/io_e2p.txt
step e2pc N "$U --cmp $R $P/io_e2p.txt.part*; rm -rf $P/io_e2p.txt*"
step n10 M 2 env ECALC_VERBOSE=2 ./ecalc 10000000000 /tmp/io_n10.txt
step n10s N2 "sha1sum /tmp/io_n10.txt.part*; rm -rf /tmp/io_n10.txt*"
step e10 M 2 env MN_OUT_EARLY=1 ECALC_VERBOSE=2 ./ecalc 10000000000 /tmp/io_e10.txt
step e10s N2 "sha1sum /tmp/io_e10.txt.part*; rm -rf /tmp/io_e10.txt*"
step e10p M 2 env MN_OUT_EARLY=1 ECALC_OUT_PACKED=1 ECALC_VERBOSE=2 ./ecalc 10000000000 /tmp/io_e10p.txt
N2 "ls -l /tmp/io_e10p.txt.part*; rm -rf /tmp/io_e10p.txt*" >> $D/e10p.log 2>&1
step nfs1 M 2 env ECALC_VERBOSE=2 ./ecalc 10000000000 $P/io_nfs1.txt
N "ls -l $P/io_nfs1.txt*; rm -rf $P/io_nfs1.txt*" >> $D/nfs1.log 2>&1
step nfs2 M 2 env MN_OUT_WAVES=2 ECALC_VERBOSE=2 ./ecalc 10000000000 $P/io_nfs2.txt
N "ls -l $P/io_nfs2.txt*; rm -rf $P/io_nfs2.txt*" >> $D/nfs2.log 2>&1
N "rm -rf $P"

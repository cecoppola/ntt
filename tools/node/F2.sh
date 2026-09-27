# Batch F (1 node): the gates (t_newton, t_mul, mnaccept unit,e9), size 4 on one node with waves / MN_OUT_EARLY (POOL_LOG=29
# as mnaccept runs size 4 at 1e9: the default 2^31 planes over 4 ranks exceed the node budget, rc 8 in batch E), stale files.
R=$HOME/ntt/ecalc/ref/e_1000000000.txt
N "hostname; ls -la /tmp | grep io_; rm -rf /tmp/io_*" > $D/node.txt 2>&1
step s4w M 4 env POOL_LOG=29 MN_OUT_WAVES=2 ./ecalc 1000000000 /tmp/io_s4.txt
step s4wc N "cat /tmp/io_s4.txt.part* | cmp - $R && echo IDENTICAL; rm -rf /tmp/io_s4.txt*"
step s4ep M 4 env POOL_LOG=29 MN_OUT_EARLY=1 ECALC_OUT_PACKED=1 ./ecalc 1000000000 /tmp/io_s4e.txt
step s4epc N "$U --cmp $R /tmp/io_s4e.txt.part*; rm -rf /tmp/io_s4e.txt*"
step s4wp M 4 env POOL_LOG=29 MN_OUT_WAVES=4 ECALC_OUT_PACKED=1 ./ecalc 1000000000 /tmp/io_s4p.txt
step s4wpr M 4 env POOL_LOG=29 ECALC_RECHECK=1 ./ecalc 1000000000 /tmp/io_s4p.txt
step s4wpc N "$U --cmp $R /tmp/io_s4p.txt.part*; rm -rf /tmp/io_s4p.txt*"
N "ls -la /tmp | grep io_; rm -rf /tmp/io_*" >> $D/node.txt 2>&1

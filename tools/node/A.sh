# Batch A: functional -- gates at 1e9 (both bases), packed at 1e9 (direct, sync, small chunks) vs ASCII, RECHECK on packed,
# size 2-4 on one node (packed, waves), the stripe command path (fake lfs), W7 auto on /tmp and NFS, the memory guard
R=$HOME/ntt/ecalc/ref/e_1000000000.txt; F='^VERIFY\|^total\|^wrote\|^mn_out\|^T2 \|^dc \|^mn: node .*: T2\|^mn: all\|^mn: node .*: wrote\|MN_OUT_WAVES\|stripe\|FATAL\|^memsample: ECALC'
N "hostname; df -h /tmp | tail -1; free -g | head -2" > $D/node.txt 2>&1
step e9b10 G "./ecalc 1000000000 /tmp/io_a.txt | grep -a '$F'; cmp /tmp/io_a.txt $R && echo IDENTICAL; rm -f /tmp/io_a.txt*"
step e9b2 G "LIMB_BASE=2 ./ecalc 1000000000 /tmp/io_b.txt | grep -a '$F'; cmp /tmp/io_b.txt $R && echo IDENTICAL; rm -f /tmp/io_b.txt*"
step pk9 G "ECALC_OUT_PACKED=1 ECALC_CKPT_TOP=1 ./ecalc 1000000000 /tmp/io_p.txt | grep -a '$F'; ls -l /tmp/io_p.txt; $U --cmp $R /tmp/io_p.txt; ECALC_RECHECK=1 ./ecalc 1000000000 /tmp/io_p.txt | tail -3; rm -rf /tmp/io_p.txt*"
step pk9s G "ECALC_OUT_PACKED=1 ECALC_OUT_MODE=sync ./ecalc 1000000000 /tmp/io_q.txt | grep -a '$F'; $U --cmp $R /tmp/io_q.txt; $U /tmp/io_q.txt | sha1sum; cat $R.sha1; ECALC_RECHECK=1 ./ecalc 1000000000 /tmp/io_q.txt | tail -2; rm -rf /tmp/io_q.txt*"
step ch1a G "MN_OUT_CHUNK_MB=1 ECALC_VERBOSE=2 ./ecalc 1000000000 /tmp/io_c.txt | grep -a 'T2 \|VERIFY'; cmp /tmp/io_c.txt $R && echo IDENTICAL; rm -rf /tmp/io_c.txt*"
step ch1p G "MN_OUT_CHUNK_MB=1 ECALC_VERBOSE=2 ECALC_OUT_PACKED=1 ./ecalc 1000000000 /tmp/io_d.txt | grep -a 'T2 \|VERIFY'; $U --cmp $R /tmp/io_d.txt; rm -rf /tmp/io_d.txt*"
step e6p G "for n in 1000000 10000000 100000000; do ECALC_OUT_PACKED=1 ./ecalc \$n /tmp/io_e.txt | grep -a 'VERIFY'; $U -q --cmp \$HOME/ntt/ecalc/ref/e_\$n.txt /tmp/io_e.txt; rm -f /tmp/io_e.txt*; done"
step s2p M 2 env ECALC_OUT_PACKED=1 ECALC_CKPT_TOP=1 ./ecalc 1000000000 /tmp/io_s2.txt
step s2pc N "ls -l /tmp/io_s2.txt*; $U --cmp $R /tmp/io_s2.txt.part*"
step s2pr M 2 env ECALC_RECHECK=1 ./ecalc 1000000000 /tmp/io_s2.txt
N "rm -rf /tmp/io_s2.txt*"
step s4w M 4 env MN_OUT_WAVES=2 ./ecalc 1000000000 /tmp/io_s4.txt
step s4wc N "cat /tmp/io_s4.txt.part* | cmp - $R && echo IDENTICAL; rm -rf /tmp/io_s4.txt*"
step s3wp M 3 env MN_OUT_WAVES=3 ECALC_OUT_PACKED=1 ./ecalc 1000000000 /tmp/io_s3.txt
step s3wpc N "$U --cmp $R /tmp/io_s3.txt.part*; rm -rf /tmp/io_s3.txt*"
rm -f $HOME/IO15/fakelfs.log
step stripe M 2 env PATH=$HOME/IO15/fakebin:$PATH MN_OUT_STRIPE=4:16:8 MN_OUT_STRIPE_FORCE=1 ECALC_VERBOSE=2 ./ecalc 100000000 /tmp/io_st.txt
cat $HOME/IO15/fakelfs.log >> $D/stripe.log 2>&1
N "cat /tmp/io_st.txt.part* | cmp - $HOME/ntt/ecalc/ref/e_100000000.txt && echo IDENTICAL; rm -rf /tmp/io_st.txt*" >> $D/stripe.log 2>&1
step auto G "for f in /tmp/io_w.txt $HOME/IO15/io_w.txt; do ECALC_ODIRECT=auto ECALC_CKPT_TOP=1 ./ecalc 100000000 \$f | grep -a '$F\|checkpoint'; cmp \$f \$HOME/ntt/ecalc/ref/e_100000000.txt && echo IDENTICAL; rm -rf \$f*; done"
step guard1 G "ECALC_MEM_GUARD_GB=100000 ./ecalc 1000000000 /tmp/io_g.txt; echo rc=\$?; rm -rf /tmp/io_g.txt*"
step guard2 G "A=\$(awk '/MemAvailable/{print int(\$2/1e6)}' /proc/meminfo); echo MemAvailable \$A GB; ECALC_MEM_GUARD_GB=\$((A-40)) ECALC_MEM_SAMPLE=1 ./ecalc 10000000000 /tmp/io_g.txt 2>&1 | grep -a 'memsample: ECALC\|FATAL\|^bs \|^dm \|^total\|VERIFY' ; echo rc=\${PIPESTATUS[0]}; rm -rf /tmp/io_g.txt*"
step guard3 M 2 env ECALC_MEM_GUARD_GB=100000 ./ecalc 1000000000 /tmp/io_g2.txt
N "rm -rf /tmp/io_g*"

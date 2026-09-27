# Batch D: one 1e11 to aac6's NFS -- packed, O_DIRECT (W1's better mode from the login node) -- both walls, the writer's rate,
# then the packed file converted from NFS and hashed against e_1e11.out's sha1, and RECHECK of the packed file on NFS.
SHA=$(cut -d' ' -f1 $HOME/V214/e_1e11.sha1); P=$HOME/IO15/w11; mkdir -p $P
F='^VERIFY\|^total\|^wrote\|^mn_out\|^dc \|^T2 \|^T1 \|FATAL'
N "hostname; df -h $P | tail -1; echo want $SHA" > $D/node.txt 2>&1
step p11n G "t0=\$(date +%s.%N); ECALC_OUT_PACKED=1 ECALC_VERBOSE=2 ECALC_MEM_SAMPLE=5 ECALC_MEM_SAMPLE_FILE=$D/p11n.mem ./ecalc 100000000000 $P/io_p11n.txt | grep -a '$F'; echo wall \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s; ls -l $P/io_p11n.txt*; tail -1 $D/p11n.mem"
step u11n N "t0=\$(date +%s.%N); $U $P/io_p11n.txt | sha1sum; echo want $SHA; echo wall \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s"
step r11n G "t0=\$(date +%s); ECALC_RECHECK=1 ./ecalc 100000000000 $P/io_p11n.txt | tail -4; echo \$(( \$(date +%s) - t0 )) s"
N "rm -rf $P"

# Batch C: W2 at 1e11 on the node's local /tmp -- the ASCII run and the packed run paired on one node (both walls: the run's
# `total` without the write, the process wall with it), the packed file -> converter -> sha1 against e_1e11.out's
# (~/V214/e_1e11.sha1), the converter's speed to a pipe and to a file, RECHECK on both forms.
SHA=$(cut -d' ' -f1 $HOME/V214/e_1e11.sha1)
F='^VERIFY\|^total\|^wrote\|^mn_out\|^dc \|^T2 \|^T1 \|^dm \|^bs \|FATAL\|digits:'
N "hostname; df -h /tmp | tail -1; free -g | head -2; echo want $SHA" > $D/node.txt 2>&1
c11() { local tag=$1; shift
        step $tag G "t0=\$(date +%s.%N); env $* ECALC_MEM_SAMPLE=5 ECALC_MEM_SAMPLE_FILE=$D/$tag.mem ECALC_VERBOSE=2 ./ecalc 100000000000 /tmp/io_$tag.txt | grep -a '$F'; echo wall \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s; ls -l /tmp/io_$tag.txt*; tail -1 $D/$tag.mem"; }
c11 a11
step a11s N "t0=\$(date +%s); dd if=/tmp/io_a11.txt iflag=direct bs=64M status=none | sha1sum; echo want $SHA; echo \$(( \$(date +%s) - t0 )) s"
step a11r G "t0=\$(date +%s); ECALC_RECHECK=1 ./ecalc 100000000000 /tmp/io_a11.txt | tail -3; echo \$(( \$(date +%s) - t0 )) s"
N "rm -rf /tmp/io_a11.txt*"
c11 p11 ECALC_OUT_PACKED=1
step u11s N "t0=\$(date +%s.%N); $U /tmp/io_p11.txt | sha1sum; echo want $SHA; echo wall \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s"
step u11n N "t0=\$(date +%s.%N); $U -n /tmp/io_p11.txt; echo wall \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s"
step u11o N "t0=\$(date +%s.%N); $U -o /tmp/io_u11.txt /tmp/io_p11.txt; echo wall \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s; ls -l /tmp/io_u11.txt; rm -f /tmp/io_u11.txt"
step p11r G "t0=\$(date +%s); ECALC_RECHECK=1 ./ecalc 100000000000 /tmp/io_p11.txt | tail -3; echo \$(( \$(date +%s) - t0 )) s"
N "rm -rf /tmp/io_p11.txt* /tmp/io_u11.txt"

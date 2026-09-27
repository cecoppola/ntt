# Batch P (IO2, 1 node): per-part conversion.  Packed output is the default (int15b).  Sizes 2 and 4 at 1e9: each part
# converted alone, the outputs concatenated and cmp'd with the reference, each part --cmp'd alone; 1e10 at size 2: one part
# alone and both parts in parallel (the per-part rate, as each target node would convert its own); 1e11 at size 1: the run,
# the converter to a pipe against e_1e11.out's sha1 and to a local file (the rate).
R=$HOME/ntt/ecalc/ref/e_1000000000.txt; SHA=$(cut -d' ' -f1 $HOME/V214/e_1e11.sha1)
N "hostname; ls -la /tmp | grep io_; rm -rf /tmp/io_*; df -h /tmp | tail -1" > $D/node.txt 2>&1
pp() { local tag=$1; step ${tag}c N "for p in /tmp/io_$tag.txt.part*; do $U -o \${p/.txt./.asc.} \$p || echo FAIL \$p; $U --cmp $R \$p | tail -1; done; ls -l /tmp/io_$tag.asc.part*; cat /tmp/io_$tag.asc.part* | cmp - $R && echo IDENTICAL concatenated; rm -rf /tmp/io_$tag.*"; }
step s2 M 2 env ./ecalc 1000000000 /tmp/io_s2.txt
pp s2
step s4 M 4 env POOL_LOG=29 ./ecalc 1000000000 /tmp/io_s4.txt
pp s4
step t10 M 2 env ./ecalc 10000000000 /tmp/io_t10.txt
step t10c N "ls -l /tmp/io_t10.txt*; t0=\$(date +%s.%N); $U -o /tmp/io_t10.asc.part0000 /tmp/io_t10.txt.part0000; echo alone \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s; rm -f /tmp/io_t10.asc.*; t0=\$(date +%s.%N); for p in /tmp/io_t10.txt.part*; do $U -o \${p/.txt./.asc.} \$p & done; wait; echo parallel \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s; cat /tmp/io_t10.asc.part* | sha1sum; echo want 3fc025473586ed222be1605df7fac8774f951f3a; rm -rf /tmp/io_t10.*"
step p11 G "t0=\$(date +%s.%N); ECALC_VERBOSE=2 ./ecalc 100000000000 /tmp/io_p11.txt | grep -a '^total\|^wrote\|VERIFY\|^mn_out'; echo wall \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s; ls -l /tmp/io_p11.txt*"
step u11s N "t0=\$(date +%s.%N); $U /tmp/io_p11.txt | sha1sum; echo want $SHA; echo wall \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s"
step u11o N "t0=\$(date +%s.%N); $U -o /tmp/io_u11.txt /tmp/io_p11.txt; echo wall \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s; ls -l /tmp/io_u11.txt; rm -f /tmp/io_u11.txt"
N "rm -rf /tmp/io_*; ls -la /tmp | grep io_" >> $D/node.txt 2>&1

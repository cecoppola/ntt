# Batch E: V2's unfinished item D (~/V214/D.steps) -- checkpoints with O_DIRECT (the default since Phase 14): ECALC_CKPT_TOP
# off / 1 / 2 at 1e11 on the local /tmp, and size 2 at 1e10 without / with the tree sets; each digit file hashed.
SHA=$(cut -d' ' -f1 $HOME/V214/e_1e11.sha1)
F='^VERIFY\|^total\|^wrote\|^dm \|^bs \|checkpoint\|ckpt\|top set\|tree set\|FATAL\|^mn: all'
N "hostname; df -h /tmp | tail -1; free -g | head -2; echo want $SHA" > $D/node.txt 2>&1
ck() { local tag=$1; shift
       step $tag G "t0=\$(date +%s.%N); env $* ECALC_VERBOSE=2 ./ecalc 100000000000 /tmp/io_$tag.txt | grep -a '$F'; echo wall \$(awk \"BEGIN{print \$(date +%s.%N) - \$t0}\") s; du -sh /tmp/io_$tag.txt*; dd if=/tmp/io_$tag.txt iflag=direct bs=64M status=none | sha1sum; echo want $SHA; rm -rf /tmp/io_$tag.txt*"; }
ck ck_off
ck ck_top1 ECALC_CKPT_TOP=1
ck ck_top2 ECALC_CKPT_TOP=2
s2() { local tag=$1; shift
       step $tag M 2 env $* ECALC_VERBOSE=2 ./ecalc 10000000000 /tmp/io_$tag.txt
       N "du -sh /tmp/io_$tag.txt* /tmp/io_$tag.ck 2>/dev/null; cat /tmp/io_$tag.txt.part* | sha1sum; rm -rf /tmp/io_$tag.txt* /tmp/io_$tag.ck" >> $D/$tag.log 2>&1; }
N "sha1sum $HOME/ntt/ecalc/results/e_1e10.out" > $D/e1e10.sha 2>&1 &
s2 ck2_none
s2 ck2_tree BS_CKPT_DIR=/tmp/io_ck2_tree.ck BS_CKPT_MIN_LEVEL=99
s2 ck2_all BS_CKPT_DIR=/tmp/io_ck2_all.ck
wait

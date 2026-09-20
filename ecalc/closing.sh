#!/bin/bash
# Phase 10 closing series: N runs of 4e10 on the final main, the reference evicted before each run,
# digits cmp'd against results/e_4e10.out after each (then evicted again).  srun --jobid=<id> -N1 --gpus=4 bash closing.sh [N]
module load rocm; cd "$(dirname "$0")" || exit 1
N=${1:-5}; OUT=results/close10; mkdir -p $OUT; REF=results/e_4e10.out
evict() { python3 -c "import os,sys; fd=os.open(sys.argv[1],os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)" "$1" 2>/dev/null; }
for i in $(seq 1 $N); do
  evict $REF; evict /tmp/e_close.out; rm -f /tmp/e_close.out
  ./ecalc 40000000000 /tmp/e_close.out > $OUT/run$i.log 2>&1
  if cmp -s /tmp/e_close.out $REF; then D=identical; else D=DIFFERS; fi
  evict $REF; evict /tmp/e_close.out
  echo "run $i: $(grep -aE '^total' $OUT/run$i.log | head -1); $(grep -ao 'VmHWM [0-9.]* GB' $OUT/run$i.log | tail -1); $(grep -a VERIFY $OUT/run$i.log | head -1); digits $D"
done
rm -f /tmp/e_close.out; echo done

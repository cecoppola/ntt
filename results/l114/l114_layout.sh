#!/bin/bash
# L1 W1: BS_LAYOUT_ONLY at six points for every DM_TIGHT x DM_TAIL_DEAD combination, checked by mem_model.py --check-c (login node, no GPU)
module load rocm >/dev/null 2>&1; cd ~/ntt-L114/ecalc || exit 1; mkdir -p ~/L114
for t in 0 1; do for d in 0 1 2; do
  f=~/L114/layout_t${t}_d${d}.txt
  env DM_TIGHT=$t DM_TAIL_DEAD=$d BS_LAYOUT_ONLY=1e10,4e10,1e11,1.3e11,1.68e11,7.38e10:576 ./ecalc 1000000 x > $f 2>&1; rc=$?
  echo "== tight $t tail_dead $d: rc $rc, $(grep -c '^layout:' $f) layout lines"
  python3 mem_model.py --check-c $f > $f.chk 2>&1; grep -v '^   ' $f.chk | grep -v '^D ' ; grep '^   ' $f.chk | grep -v ' +0.0000 %' | head -5
done; done
echo; grep -h '^layout:' ~/L114/layout_t0_d0.txt | sed -n '3p;6p' | cut -c1-330
grep -h '^layout:' ~/L114/layout_t1_d1.txt | sed -n '3p;6p' | cut -c1-330
grep -h '^planes:' ~/L114/layout_t1_d0.txt | sed -n 3p | cut -c1-300

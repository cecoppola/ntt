#!/bin/bash
# wait for batch 1's mnaccept ef1, stop batch 1 (its trap scancels its job), update + rebuild, launch batch 2
S=~/T114/batch.txt
until grep -q "mnaccept ef1 rc" $S; do sleep 30; if ! pgrep -f '^/bin/bash .*/T114_batch\.sh$' > /dev/null; then echo "$(date) batch 1 gone before ef1 finished" >> ~/T114/chain.txt; break; fi; done
sleep 5
for p in $(pgrep -f '^/bin/bash .*/T114_batch\.sh$'); do kill $p; done
sleep 10
[ -d ~/ntt-T114/ecalc/results/mnaccept/21218 ] && mv ~/ntt-T114/ecalc/results/mnaccept/21218 ~/ntt-T114/ecalc/results/mnaccept/21218_ef1
squeue -j 21218 -h | grep -q . && scancel 21218
cd ~/ntt-T114 && git pull -q ~/T114.bundle p14-T1 && echo "$(date) updated to $(git log --oneline -1)" >> ~/T114/chain.txt
bash -lc "module load rocm >/dev/null 2>&1; cd ~/ntt-T114/ecalc && make -s -j16" >> ~/T114/chain.txt 2>&1 || { echo "$(date) build failed" >> ~/T114/chain.txt; exit 1; }
echo "$(date) built; launching batch 2" >> ~/T114/chain.txt
exec ~/T114_batch2.sh

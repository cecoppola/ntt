#!/bin/bash
# L4 (2026-10-11): the groups_table search for one model configuration on an aac6 MI250 CPU node (pure Python, no GPU).
#   usage (login node):  l4_search.sh <tag> "<VAR=val ...>"      -> sbatch -p MI250 (one node, 40 min), output ~/l4/out_<tag>/ and marker ~/l4/out_<tag>/DONE
# The tree of the model (ecalc/*.py, results/S13_e0.txt, D13b_strategy_e0.txt) and tools/groups_table.py are unpacked in ~/l4/src.
TAG=$1; ENVS=$2; D=$HOME/l4; OUT=$D/out_$TAG; mkdir -p $OUT
cat > $OUT/run.sh <<EOF
#!/bin/bash
cd $D/src
export COMM_LAYER_VSLOT_SHARE=1 $ENVS
python3 -I tools/groups_table.py search --digits 3.71e13 3.82e13 --nmin 554 --nmax 576 --keep 5 --jobs \$(nproc) --out $OUT > $OUT/search.log 2>&1
python3 -I tools/groups_table.py table --out $OUT > $OUT/table.log 2>&1
date > $OUT/DONE
EOF
chmod +x $OUT/run.sh
sbatch -p MI250 -N1 --exclusive -t 0:40:00 -J l4_$TAG --parsable -o $OUT/slurm.out --wrap "$OUT/run.sh" < /dev/null

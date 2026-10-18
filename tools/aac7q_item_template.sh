#!/bin/bash
# aac7q queue item.  Copy to ~/aac7q/queue/<NN>_<owner>_<what>.sh (filename order breaks ties); the daemon only reads the "# KEY=" lines.
# OWNER=a99
# NODES=2
# EST_MIN=30                     (worst-case run time: hold and kill at 1.5*EST_MIN+5 min)
# EXPECT_MIN=20                  (optional, informational: expected run time; used only to fit a backfill before a waiting larger item, never to kill)
# APPROVED=<who>/<date>          (items without it are never run; the coordinator/user fills it in)
# NOT_BEFORE=                    (optional epoch)
# NOT_AFTER=                     (optional epoch; the item must be able to end before it)
# EXCLUSIVE=0                    (1 = needs a quiet aac7: none of our other jobs; nothing else starts meanwhile)
# ALLOW_SLOW=0                   (1 = may use the slow nodes listed in ~/aac7q/exclude_slow)
# GROUP=                        (optional name: items with the same GROUP start only when all of them fit at once, e.g. concurrent A/B arms)
#
# Environment given by the daemon: SLURM_JOB_ID (= your hold, NODES nodes, already RUNNING), AQ_HOLD, AQ_NODES, AQ_ITEM.
# OUT and HOLD are removed from the environment.  cwd = ~/aac7q/work.  Write your own output under ~/<task>/.
# Steps MUST use --overlap --mem=0 (plain "-c 192" steps hung); no -w.  Leave no step behind (squeue -s -j $SLURM_JOB_ID).
# The daemon kills the item (single PID tree) at 1.5*EST_MIN+5 min and scancels the hold afterwards.
set -u
J=$SLURM_JOB_ID
cd ~/ntt-acc/ecalc || exit 2
source aac7env.sh >/dev/null 2>&1
# example: one 1-node-per-step command on every node of the hold
# srun --jobid=$J -N$AQ_NODES -n$AQ_NODES --ntasks-per-node=1 --overlap --mem=0 --gpus-per-node=4 ./some_cmd
# or: SLURM_JOB_ID=$J ./mnrun.sh $AQ_NODES env <vars> ./ecalc <digits>
echo "replace me"; exit 1

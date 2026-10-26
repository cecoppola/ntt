---
name: aac7-exclude-stale
description: "aac7 runner's ~/aac7q/exclude list goes stale; re-admit idle excluded nodes for a rescreen at session start"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-24T21:31:14.415Z
---

The aac7 runner passes `sbatch -x` from `~/aac7q/exclude` (NIC-failed nodes) and `exclude_slow`. On 2026-10-24 the list still held 9 nodes from screens of 10-20..10-22, leaving only 2 usable nodes while 6 sat idle; holds stayed pending ("Resources") and work slipped by hours.

**Why:** the user asked "Do you really need so many hours?" and to check every node. The rescreen showed the NICs really were dead; the real gain was using the dead nodes for NIC-free work and keeping the only 2 NIC-good nodes for network work.

**How to apply:** when a hold stays pending while `sinfo` shows idle nodes, check `scontrol show job <id>` ExcNodeList against `~/aac7q/exclude`; comment out idle excluded nodes (keep a dated backup and note) and let the hold's start-of-hold NIC check (`nics start; bad && STOP`) rescreen them; re-exclude any that fail. Rescreen of 2026-10-24: only s1b0 and s5b0 had cxi NICs; the 6 NIC-dead nodes went to `exclude_slow` and serve NIC-free 1-node work (soaks, network-free benches) through a hold with `# ALLOW_SLOW=1` and no NIC stop (template items/9zu_session23.sh). Old hold templates ran the NIC check with -N3 on 2-node holds, so the check silently passed on dead nodes (fixed in items/9zu_session19.sh to use $SLURM_JOB_NUM_NODES). Related: [[aac7-packing]], [[aac7-never-idle]].

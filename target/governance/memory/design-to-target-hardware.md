---
name: design-to-target-hardware
description: "Always shape ecalc code to the target node (4 GPUs, 8 NICs; one PE per GPU, 2 NICs each), not to the dev cluster's quirks"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-21T05:27:49.307Z
---

Design every piece of ecalc code to fit the target hardware structure naturally: 4 GPUs (MI300A APUs) and 8 Slingshot-400 NICs per node, Cray SHMEM 12.1; unit = one PE/process per GPU with its own GPU-memory heap, nearest NICs (node NICs / 4), CPU cores and NUMA memory. NIC and GPU counts are start-up parameters, so aac7 (4 NICs) runs the same code.

**Why:** user, 2026-10-21: "Ultimately we will have 4 GPUs per node, and 8 NICs per node. You should have been anticipating this structure from the beginning, and you should have designed the code to naturally fit into this hardware structure." The aac7-specific one-process-per-node + helper-process design had to be abandoned.

**How to apply:** when proposing designs or experiments, check them against the target layout first; flag dev-cluster-only workarounds as such. Related: [[target-redirect]].

---
name: two-paths-open
description: "user 2026-10-23: keep both the fastest libfabric path and the fastest SHMEM path open; SHMEM work must never regress the libfabric path"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-23T05:04:37.813Z
---

Keep two code paths open and working at all times: (1) the most-performant libfabric path (default build XSHMEM=ofi, COMM_TRANSPORT=xshmem, default layout) and (2) the most-performant SHMEM path (XSHMEM=cray build, best layout/switches). Recorded as Q85 in docs/DECISIONS.md.

**Why:** the user does other development on path (1) independently of SHMEM issues; Cray/SHMEM problems must not block or slow it.

**How to apply:** any change to shared code (comm_xshmem.c, mnrun.sh, planner) needs path (1) digits identical and no slowdown (same-hold A/A) before merge; Cray-only defaults stay scoped to Cray builds; docs name both paths and how to build/run each. Related: [[target-redirect]], [[design-to-target-hardware]].

---
name: target-redirect
description: "2026-10-21 project redirect — build target-style (Cray SHMEM 12.1, 8x Slingshot-400/node) communication on aac7 with libfabric behind a 1:1 SHMEM-named header"
metadata:
  node_type: memory
  type: project
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-21T05:20:42.762Z
---

On 2026-10-21 the user ended Cray-SHMEM-on-aac7 tuning and redirected ecalc: the target has 8x Slingshot-400 NICs per node and Cray SHMEM 12.1.0; build data structures, code and messaging on aac7 exactly as on the target but with libfabric functions, so the swap is simple. Decisions Q66-Q69 (all (a)): 1:1 SHMEM-named header xshmem.h (libfabric backend on aac7, shmem_* on target); one PE per APU, 2 NICs per PE on target; freeze comm_shmem.c (delete its proxy path after the new path matches the reference), comm_ofi.c stays the reference; research Cray SHMEM 12.1 online. Reference speed: libfabric with one layered exchange in flight (COMM_LAYER_INTER2=0 default since ed63f9d).

**Why:** the aac7 Cray SHMEM 11.8 path (host heap, helper processes) could not reach libfabric speed and does not resemble the target.

**How to apply:** plan in day/multi/teams/TARGET1_PLAN.md (phases P1 research -> P7 cleanup); do not resume Cray SHMEM 11.8 tuning. Related: [[aac7-fewer-reboots]].

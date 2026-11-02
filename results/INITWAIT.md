# INITWAIT — the seed thread's wait for the region pools, and the layered self-test line (2026-11-01)

**Question.** In the 2026-10-29 run (2e13 digits, 576 nodes, 206 s) init was 36.6 s and the seed thread waited 8.6 s for the region pools; the layered transport self-test took 4-13 s. Can the wait be shortened, and can the self-test time be seen on its own?

**S3 (print only, always on, multi-node).** Rank 0 prints `init: layered transport self-test <s> s (rank 0; part of the init figure; mn_selftest <s> s)`. The `ECALC_INIT_TL=1` line `init = ...` now has a `layered self-test` term (before, that time was hidden in `other`). Digits and behavior unchanged. Measured at 2 nodes on aac7: 0.16-0.30 s (the 4-13 s was a 576-node figure; the user's rule, keep it while under 15 s, is not touched).

**S2, why the seed thread waits (code reading).** `rns_init` maps staging, then the plane pools, then calls the hook `binsplit_seeds_begin`. The hook starts the seed thread and then the main thread builds the region pools (`binsplit_pregrow`, first half of each VMM arena synchronously, the rest in the background). The seed thread has two pinned buffers of one chunk each (8 GiB with VMM arenas); it fills both in about 2.7 s and then must wait for the pools, because a VMM range takes no CPU stores and the DMAs need the pool addresses. So the wait is the mapping time of the first arena halves, done by the main thread through the HIP runtime. The pools are created by the main thread, after the plane pools, in `rns_init`.

**Measured switch `ECALC_INIT_EARLY_POOLS=1` (default 0; measured at commit 2b4462f and not kept, removed by the coordinator's decision of 2026-11-01).** The hook ran before the plane pools (same bytes, same order inside the hook). Risk: plane pools mapped last may get small pieces near the node memory edge (N414 effect; the earlier `RNS_REGIONS_FIRST` at 1 node 1e11 digits made the run 2 x slower, archive:results_merged_2026-10-24/TESTING_VERIF.md). Not tested near the edge here. Earlier experiments with the other two ways (plane pools in a helper thread, a seed prebuffer) gave nothing (results/APUMULT.md, section INIT).

**Method.** aac7, 2026-11-01, branch initwait 161bf16, items A_1n and B_2n, aac7 launch line + `ECALC_INIT_TL=1`, no digit file, 3 runs per arm, arms interleaved off/on, all VERIFY OK. Gates: 1 node (unset, set), 2 nodes path 1 (unset, set), 2 nodes path 2 (set): all PASS, digits identical.

**Results (measured; init in s, rank 0, median of 3 with the range).**

| nodes | digits | switch unset | switch set | seed thread ends (tl s) unset / set |
|---|---|---|---|---|
| 2 | 1000000000 | 8.43 (7.95-8.97) | 7.89 (7.82-8.27) | 6.0 / 1.6 |
| 2 | 16000000000 | 13.20 (13.07-13.53) | 12.49 (12.01-12.54) | 10.6 / 7.6 |
| 1 | 1000000000 | 5.14 (4.85-5.30) | 6.17 (5.91-6.23) | 5.2 / 2.2 |
| 1 | 16000000000 | 15.02 (12.45-20.95) | 16.61 (15.80-16.68) | 16.5 / 13.6 |

Reading. At 2 nodes the switch lowers init by 0.5-0.7 s (4-5 %) and the seed thread ends 3-4 s earlier, but the seed does not bind init there. At 1 node init gets 1 s worse at 1000000000 digits (ranges do not overlap) and about 1.6 s worse at 16000000000 (ranges overlap; the unset arm is bimodal, 12.5-21 s). At 576 nodes the seed work is far larger, so the earlier seed end could matter, but it was not run at that scale; the node memory there (416 of 480 GB) is near the edge where the fragmentation risk applies.

**Conclusion.** The switch is not kept (no gain at 1 node, under 1 s at 2 nodes; fewer switches). The wait is the VMM mapping of the region pools through the HIP runtime in one process driving four GPUs; it overlaps only partly, and the real fix is per-device allocator processes (plan item S1).

**RESUME.** Branch initwait: S3 kept, switch removed. Cleanup done: /shareddata folder and aac7 worktrees removed.

# SESSIONS — aac7 measurement sessions of September/October

Owner of the items below (condensed 2026-10-19; full originals in `archive:results_merged_2026-10-19/`, raw data and drivers in `archive:results_raw_2026-10-19/`; old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Labels: (m) measured, (d) derived by arithmetic from measured numbers, (i) inferred, (mod) modelled, (a) assumed. Times Eastern.

## Topic summary

These are the aac7 10-node (6.441e10 digits per node, no write) and one/two-node sessions of 2026-10-07/09 that decided four code items and measured where the 10-node wall goes. Results that still hold:

- **`MN_T_CHUNK_MB=2048` (D2, S22) is faster on aac7**: B - A = -40.6 s +- 11.1 (95 % CI), 16 of 16 rounds negative (pooled 24 rounds -42.3 s mean, 22 of 24 negative; mean total 436 -> 396 s, dm sd 23.2 -> 10.3 s); memory unchanged. The code default is 1024 MB (pool law, needed by the target); 2048 stays an aac7 profile-line choice; no target A/B was made.
- **Three prospective items were closed NO-GO on measurement**: S-2 division/reciprocal overlap (ceiling 25-34 s at 576 (mod), realistic 6-17 s, below the 30 s bar; the `ready` counter cannot separate skew from transfer), A37-R1 GPU seed (seed and mapping co-bind on aac7 by +0.55 s; at the target the seed ends at about 16 s against a 16.5 s init floor: gain 0-1 s (mod)), and the 3-pass 2^31 NTT (13/9/9 forward 90.6 ms (m) against today's 89.9 ms; gate was <= 80 ms; the fat 9-stage passes cost 3.2-3.6 ms per stage, not the modelled 2.5). A37-R5 batch pipeline "not worth it" here (later built anyway as `BS_BATCH_PIPE`, default 1 since D20; `results/KERNELS.md §N3`).
- **Where the 10-node wall goes (S27)**: direct exchanges cost 101 s per thread of 376 s (27 %), of which 61 % is skew (waiting for a slower node), not transport; transport variants (rot order, NIC rebinding, OFI-native signal) are dead ends (rot null on wall: +3.1 s, CI -1.0..+7.2). The one large measured, structural item is node 1 (a specific host) arriving about 20 s late at the reciprocal: S32 showed it follows the host (aac7-n01, stall in db_from_bi upload, 21-25 s), not the rank.
- **X2 (INTER2 + VSLOT_POOL, S31)** is -32 s (-8 %, clean rounds; -34 s all rounds, CI to +1.5) at 10 nodes with +0.8 GB (m) after the pool fragmentation fix; both are now built-in defaults (ecalc/README.md, since 2026-10-12). Its 576-node effect is unmeasured.
- **A rare crash exists** (segfault in libamdhip64 from the VMM background mapper, about 3.5 % of 10-node runs on 2026-10-08, 0.14 % per 2-node run (m)); `ECALC_VMM_SAFE=2` neither cured nor worsened it: not adopted (follow-up in `results/CRASHES_HANGS.md`).
- **Scaling data**: share ladder at 10 nodes: 410.6 s at 6.441e10, 464.4 s at 7.00e10, 528.5 s at 7.64e10 (time exponent about 1.5-1.9, above n log n; unexplained); memory linear at 28.4 GB per 1e10 digits per node (fixed about 173 GB on rank 0); the 8.1e10 share is stopped by the vslot rule (427.6 GB needed against 0.95 x the smallest MemAvailable 440.7 GB).
- **Superseded**: S22 §2 absolute wait levels (extra instruments cost 3-4 %); the S22 "S-2 GO" of S20 (flipped to NO-GO); S27's rank 3 X2 candidate (now adopted) and rank 1 (reciprocal chain lateness, diagnosed as a host defect, not a code item).


The per-item sections (tables, drivers, run lists) were moved to `archive:repo_cl6_2026-10-25/results/SESSIONS.md` (2026-10-25); section anchors that cited them now point to this summary.

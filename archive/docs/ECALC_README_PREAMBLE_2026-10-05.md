# ecalc/README.md preamble as of 2026-10-05 (HISTORICAL)

> Moved here verbatim from the top of `ecalc/README.md` on 2026-10-10 (documentation consolidation). It is the phase-by-phase
> history of the defaults and of the target size up to 2026-10-06. Current facts: `ecalc/README.md` (defaults table,
> switch table), `docs/code/00_OVERVIEW.md`, `docs/TARGET.md` §1.

# ecalc — e to 4 × 10¹⁰ digits on one MI300A node, and over several (PLAN.md §8, §15, §17, §25)

The default configuration at size 1 (one process, four APUs) is the Phase 11 result (RESULTS.md §76):
decimal limbs of 10¹⁸, the binary-splitting levels and the Newton division on device-resident numbers
through the four-APU distributed transform, 3·2ᵏ transform lengths, the paired batch tier and the
register-blocked transform body, the seeds and T1's recurrence overlapped with init and bs, the digits
streamed to the file in chunks — **4 × 10¹⁰ digits in 81.5 ± 1.4 s wall at 11.7 GB of host memory, 10¹¹
digits on one node in 263 s (445 GB)**, digits verified against the reference (Phase 9: 86.4 s / 48.8 GB;
Phase 7's 134 s / 154 GB and Phase 8's 112 s are in RESULTS §63–72). `LIMB_BASE=2` reproduces the
paper's binary-limb pipeline; `main` carries this code, the Phase 4 reproduction is tag `phase4-accepted`.
Several node-processes run the same program over the TCP or SHMEM communicator (`mnrun.sh`, below): the
leaf tree per node, the top levels, the division and the output distributed. Every environment switch
the code reads is listed once, with its default, in **Switches** at the end.

**The defaults (as of 2026-10-05; nothing to set).** Phase 13c's design (`RNS_STRATEGY=auto`, `ECALC_PLANE_CAP=2^31`,
`MDB_SHIFT_CHUNK_MB=1024`, `COMM_ALLTOALLV_DEPTH=2`, `NTT_B1R=3`, `NTT_PLAN=1`, three primes and `NTT_MODMUL=1` since step 0);
Phase 14's nine (2026-09-26: `RNS_PLANES_FIRST`, `NEWTON_RECIP_CUT`, `ECALC_ODIRECT`, `DB_POOL_VMM` with `DM_TIGHT`,
`MN_TREE_EARLY_FREE`, `MN_T_CHUNK_MB=1024`, `ECALC_BUDGET_CHECK`, `COMM_LAYER_TKERNEL`, `COMM_SHMEM_POOL_AUTO`); and **Phase 15's,
the user's decisions of 2026-09-27**: `RNS_AUTO_PIECE_COST=1` (D1), `ECALC_CORR_PATCH=2`, `NEWTON_RECIP_MID=1`,
`BS_SEED_FILL=128` (0, or a `BS_SEED_TERMS` in the environment, keeps the fixed span), `BI_MUL1_FAST=1`, `DIST_TWREC=1`,
**`ECALC_OUT_PACKED=1`** (the digits written packed, 0.444 B/digit; `tools/unpack_digits` makes the ASCII file off the clock),
`MN_OUT_EARLY=1`, `ECALC_ODIRECT=auto`; **Phase 15 Batch 2, the user's decisions of 2026-09-28** (main B2): `BS_ARENA_ROOM=0.16`
(was 0), `DIST_TWREC_G=1` (was 0), `RNS_POOL1_4Q=1` (PS; it changes only `ECALC_NP=4` runs). The top set (`ECALC_CKPT_TOP`) is **off**
by default (record timing runs); `ECALC_CHECKPOINT=1` turns on its budgeted form for development and testing. Not code defaults but
on the **target's launch line** (`docs/TARGET.md` §4; **CURRENT: the target is 3.71 × 10¹³ digits on 576 nodes**, `ecalc 37100000000000`,
since the user's decision of 2026-10-06 ≈ 19:30 EDT (TGTBENCH2/s18-target: "3.71e13 is fine") — chosen with margin against B7ACCT's
v-exchange-slot accounting (device layout 363.53 GB vs the 373.44 GB measured edge, 9.91 GB margin; node total 405.61 GB with v-slots
vs 480 GB, 74.39 GB margin; results/S18TGT.md Part 1); raise later if the memory configuration is lifted; *history*: it was **4.08 × 10¹³
for a few hours on 2026-10-06 (TGT17)** (`ecalc 40800000000000`) before B7ACCT's v-slots were counted and found it only 0.88 GB under
the edge — it was **5.276 × 10¹³ from the user's Batch 3 decision of 2026-09-29 to 2026-10-06** (`ecalc
52760000000000`; CAP17 found it 32.42 GB over the raw 373 GB device edge — does not fit) — **5.167 × 10¹³ was its test size**, int15k,
2026-10-03; before that, 4.25 × 10¹³ from 2026-09-27, 23:50 EDT until P24 (the user's decision 11 of 2026-09-28), then 5.1 × 10¹³ until B3):
**`ECALC_NP=auto`** (four primes only for the products over the three-prime bound; the decision of 2026-09-28 — it was `ECALC_NP=4`,
which is now +17.2 GB per node and over 480 GB at the target, modelled), **`RNS_DIST_CACHE_FIT=1`** (the mn transform cache bounded by
the budget: 0 slots at the target; without it the code's default takes 2 × 68.7 GB per node that no budget holds — never run the target
without FIT or `RNS_DIST_CACHE_MN=0`), `ECALC_MEM_GUARD_GB=6`, `COMM_SHMEM_ROUND_MB=1024` (D2). Off and staying off (2026-09-28):
`NTT_R3_FUSE`, `RNS_R3_MINK`. Not adopted: `DB_POOL_VMM_PAR`, `DB_POOL_VMM_EXTEND` (agent RL, not merged), E11 / `DM_BAND` (dropped
for now), MAP's `DB_POOL_VMM_STREAM` (dropped: not merged; `ECALC_INIT_TL` stays). Every report gives two walls (D3): without and with
the disk write. **Phase 15 Batch 3 (B3, 2026-09-29)**: defaults `MN_P24=2`, `NEWTON_DKM=1`; the target was **5.276 × 10¹³ digits**
(`ecalc 52760000000000`) until 2026-10-06, when TGT17 (the user's decision) moved it briefly to **4.08 × 10¹³** (`ecalc 40800000000000`),
then TGTBENCH2/s18-target (the same day) moved it to **3.71 × 10¹³** (`ecalc 37100000000000`), the current target — see the banner
above. **int15j (the user's decisions of 2026-09-29)**: on the target's launch line, not defaults, **`MN_OUT_DKM_HI=1`**
(two part files per node) and **`RNS_DIST_CACHE_PARTIAL=1`** (RESULTS §93: measured identical at 10¹¹ and at 3–4 node-processes);
`ECALC_NP_AUTO_MIN` (MPB) stays off; new and off: `ECALC_FAST_EXIT` (the user's decision of 2026-10-03: an option, not on the launch line) and
`DM_MN_LEAN` (int15k: the multi-node division's dead copies removed and the arena counted without them, −20 GB per node at the target, modelled;
on the target's launch line only since the user's decision of 2026-10-05, not a code default). **Phase 16, the user's decisions of
2026-10-05**: `BS_POOL_RULE=1` (R: the sizing pass's lower-bound rule for which bs levels take the batch tier vs the mdev tier), `COMM_INIT_EARLY=1`
(S: the transport opened before `rns_init`, diagnosed against Cray OpenSHMEMX's intermittent `shmem_init_thread` segfault — 0 of 55 vs 8 of 78),
`MN_SELFTEST_GROW=1` (P: the layered self-test's rows grow until R / ranks ≥ 32, needed for any 32-or-more-rank launch) are all defaults; see
their rows in **Switches** below. **Phase 17, the user's decision of 2026-10-06** (results/OFI17.md, RESULTS §106): **`COMM_OFI`
defaults to 1 wherever it applies** — the SHMEM transport's device exchanges go over libfabric on the calling APU's cxi NICs instead
of `shmem_putmem[_signal]_nbi`, when a cxi NIC is present (Cray Slingshot; a build without `OFI=1`/libfabric, or a system with no cxi
device such as aac6's TCP/SOS, stays on the old path with no code change needed); `COMM_OFI=0` always restores it.

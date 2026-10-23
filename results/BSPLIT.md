# BSPLIT — own-device split of oversized batch products (one process per GPU)
**Question.** Under `ECALC_PE_PER_GPU=1` the batch tier has no striped tier, so a batch product larger than the local tier fatals (S2c). Can the product be cut into pieces that fit one device, with identical digits?
**Change / branch.** Branch `batchsplit`. Switch `RNS_BATCH_SPLIT` (0 off, byte-identical default; 1 = on under P when the local tier cannot hold the product; 20..29 = forced in any mode, pieces of at most 2^k limbs, for tests). The operands are cut into a ka x kb grid; pieces sharing one B piece go through the local tier as one batch; outputs are added into C with carries in own kernels. With the switch at 1 under P the staging is raised to 2 GiB per PE (pieces up to 2^28 limbs).
**Method.** aac7 items 9zd_bsplit1c (1 n, commit fb09295), then 2 n and 3 n items. Gates per TESTING.md.
**Results.** (measured unless noted)

| check | result |
|---|---|
| `t_mul 0 split` (6 forced-split cases vs unsplit tier, limbs, lengths, residues), fb09295 | VERIFY OK, 10 split lines |
| planner, switch off, 12 outputs (1/2/3/6/576 n, with and without P) vs main | 0 differ |
| plan PG 3 n 1.9323e11 cap 2^30, switch on | FITS (device 327.23 / 373.44 GB, node 392.65 / 420 GB); off: REFUSED |
| plan PG cap 2^31 | does not fit even on main (node 564 / 420 GB) |
| split shapes at cap 2^30 (modelled) | level 20: 2x1, x0.90; level 21: 4x2, x1.70; level 22: 4x4 pieces of 2^28 points, x3.40 transform points |

Earlier run at 4e94a12: mixed small + large case failed on a wrong output length (striped tier leaves ncn 0); fixed in fb09295.
Cost: real pinned host memory grows by about 4.3 GB per node with the switch at 1 (not in the fit model). With one product per chunk the B transform is not shared across A pieces.

## 2 n gate (9zc_bsplit2y, commit 0414d1d, measured)
v2off2 PASS; 1e9 D vs forced pieces of 2^20 (49 split lines): digits identical; 1e11 D vs PGS (PG chunk settings, cap 2^30, RNS_BATCH_SPLIT=1, XSHMEM_HEAP_MB=8704; 16 split lines): digits identical (sha 578f5efb0ff2b9af), total 318.72 s against D 264.73 s (one run each, not a speed claim). The xshmem DATA heap peaked at 6416 MiB of 8704, below the new plan's 6912 MiB (the plan's value that was refused before: 4864).

## Planner heap model (xshmem DATA heap under one process per GPU)
Gate 9zc_bsplit2x (2 n 1e11, PG chunk settings, cap 2^30, RNS_BATCH_SPLIT=1, commit 0414d1d, measured): the run died after init with `the DATA heap of PE 0 (6144 MiB) cannot hold 1241 MiB more (in use 4965 MiB): XSHMEM_HEAP_MB >= 6209 needed`; the plan had given 4864 MiB (shortfall 1345 MiB, 28 %; the first try at the plan's value stopped at 4864 with 4967 needed). The two blocks are the tree level's child-share exchange (xex_post): the PE's share of P, 650820636 limbs = 4965 MiB, and a quarter of it, 1241 MiB, both staged at once and whole (one process per GPU has no rounds). `binsplit_shmem_pool_need` modelled only the products' internal exchanges (4542 MiB). Fix (commit 967d43c): for COMM_TRANSPORT=xshmem without rounds the model adds nq_leaf / NL limbs plus a quarter of it per level; 2 n 1e11 now plans 6622.7 MiB, XSHMEM_HEAP_MB=6912 (modelled; measured need 6206 + alignment). Planner outputs before and after (aac7 login node, 42 cases: 1e9 to 3.82e13 digits at 2, 3, 6, 576 nodes and 1 node, each with default, one process per GPU, xshmem with and without rounds, shmem, and the PG chunk settings): 2 move, both the PG chunk settings under xshmem (2 n 1.288e11: heap 4608 to 8960 MiB; 3 n 1.9323e11: 8704 to 8960 MiB); both still FITS (node 367.31 and 393.73 of 420 GB). D (default layout) and every shmem or rounds-bounded plan are byte-identical.

## 1 n 3e10 and the P digit bug (9zd_bsplit1w, commit a040a7b, measured)
D, P and PS30 (P, cap 2^30, RNS_BATCH_SPLIT=1; 4 split lines) all give sha e050c5ae142ae995. An earlier PS30 failure was not the split: plain P at 3e10 failed the same way (NST wrote newton_x_dx on PE 0 only; fixed by a040a7b, cherry-picked from tgtshape). Times 89.44 s D, 148.80 s P, 161.81 s PS30 (single runs; the P cost is P's, not the split's). Earlier 1 n gates (65d76ec): 1e9 D/P/PS20 identical, 1e10 D/PS22 identical (133 split lines), planner vs main 12 outputs 0 differ.

## Planner heap model, second fix (9zd_bsplit3zb died, measured; model f34abe1, a8a87c6, 501a6af)
3 n 1.9323e11 PGS with the first fix (heap 8960 MiB): PEs 4-7 stopped with `cannot hold 2304 MiB more (in use 6912 MiB): XSHMEM_HEAP_MB >= 9218`. The 1/4 receive term was a fit to the 2 n case. From xex_post: a whole exchange stages sst (the PE's part of the operand) and rst (its part of the receiving sequence) at once; a sending PE stages s = nq_leaf / NL, a receiver s prev / S (S nodes in the level's group, prev nodes per child), the running Q of c children 2 s prev c / S. Only the node that holds the sent child stages the full s (node 1 in the 3-way level). 3 n: modelled 4/3 s = 9100 MiB against measured 9216 (s is 1.3 % above nq_leaf / NL, unexplained); 2 n: modelled 7947 against measured 6206 (conservative; rst is sst/4 there, unexplained). Planned heaps: 2 n 1e11 8448, 3 n 9472 (covers 9218), 6 n 3.86e11 15104, 576 n 30720 MiB (the last two REFUSED on host fit under the PG settings). The comm_ofi / xshmem pool law also adds the control words per PE and 1 MiB (Cray backend 2 n 1e10 D needed 514 of 512): D xshmem heaps rise by 256 MiB at 1.288e11 on 2 n and 576 n, no fit verdict changes (42 cases, 8 moved).

## 3 n gate (9zd_bsplit3zc and 3zd, commit 501a6af, measured)
1.9323e11 digits on 3 n: PGS (PG chunk settings, cap 2^30, RNS_BATCH_SPLIT=1, planner heap 9472 MiB, no pin) 437.72 s, 13 VERIFY OK, sha e38f16dac94299b8. D (ECALC_NODE_GB=480, as D has always run this size; under 420 it is refused by the plan) 378.07 s, sha e38f16dac94299b8: digits identical. Single runs, no ABBA pairs (D runs without digit output were about 160 s; the P and split cost at 3 n is large, not a speed claim).

## Review fixes and 2 n re-gate (9zd_bsplit2z, commit ec005a5, measured)
Heap model from the largest node's share (lgamma tail of the term ranges; node shares are not uniform: PE 0 sat 6.4 % below the average at 2 n, node 1 1.3 % above at 3 n), a margin of max(256 MiB, staged/16) and the allgather scratch under P (the control words are in the CTRL heap, not the DATA heap), the 2 GiB staging in the node fit. Planned heaps (modelled): 2 n 1e11 8960 MiB, 3 n 1.9323e11 10240. 42-case planner comparison: D plans unchanged; only PG + RNS_BATCH_SPLIT=1 plans move (6 cases), no verdict changes (3 n node 403.39 of 420 GB). 2 n 1e11 PGS with the plan's heap and no pin: digits identical to D (sha 578f5efb0ff2b9af, 16 split lines), 307.85 s, DATA heap peak 6416 MiB of 8960 on the reporting PE (single run).

## RESUME
Gates done (1 n, 2 n, 3 n digits identical). Follow-ups: single-plane arm, mdev route, Karatsuba, larger staging or writing even pieces directly into C, ABBA timing pairs, and the unexplained 1.3 % and 2 n receive ratio in the heap model.

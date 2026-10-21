# SHMEM — the SHMEM proxy mode against libfabric at 6 nodes: one summary (2026-10-17)

Owner of the SHMEM-vs-libfabric story. Per-item records: results/SHMEM_ITEMS.md §N7B to N7E.md (proxy mode, NUMA, copy kernel,
send and init variants), results/SHMEM_ITEMS.md §N7G (huge pages, items 37-51), results/SHMEM_STALLS.md (the full account of the compaction-stall work, 2026-10-14 to 10-18, for whoever returns to it), this file (items 52-89 and 72j-75d, to 2026-10-18 11:45 ET). Labels: (m) measured,
(mod) modelled, (a) assumed. Unless a section says otherwise: aac7, 6 nodes, 3.8646e11 digits, the aac7 launch line, SHMEM arm `COMM_OFI=0`
with the defaults (huge-page proxy pools, D36), libfabric arm `COMM_OFI` unset. Raw data stay on aac7 in the day
directories named per item (`summary.txt`, `digest/`).

**Question and current answer.** Why is the SHMEM proxy mode slower than libfabric, and by how much? Libfabric takes
184.5 s (4 runs, item 45, sd 6.4, m). The SHMEM default takes 286.0 s on average over 24 runs (items 47, 48, 51-54;
sd 50.1; range 212-398 s, m). The four best SHMEM runs average 220.5 s (sd 5.9, m): the steady gap is about 36 s (m,
best 4 vs libfabric), and run-to-run variance adds about 65 s on average (m, mean of 24 minus best 4). The variance is
not a fixed slow node or step: a slow run loses its time in node-local steps on one or a few nodes that change from run
to run (see "Variance findings"). No switch tried so far closes either part significantly, except the huge-page default
already adopted (D36). Item 46 (3 default runs, 254-289 s) and the 8-run slow-mode set of 2026-10-16 (219-391 s, mean
292.0, m) are consistent with the 24-run figures and are not pooled above.

## Phase breakdown (s, means; recip is inside bs; m unless marked)

| arm | n | total | sd | bs | dm | recip | init | gap to libfabric |
|---|---|---|---|---|---|---|---|---|
| libfabric (item 45) | 4 | 184.5 | 6.4 | 72.8 | 75.2 | 19.5 | 32.6 | 0 |
| SHMEM best 4 (of the 24) | 4 | 220.5 | 5.9 | 85.9 | 96.0 | 24.6 | 34.0 | +36.0 (bs +13, dm +21, init +1) |
| SHMEM all 24 (items 47, 48, 51-54) | 24 | 286.0 | 50.1 | 116.2 | 117.2 | 35.0 | 45.7 | +101.5 (bs +43, dm +42, init +13) |

Per-item means of the 24-run set (total / bs / dm / recip / init, m): item 47 314.5 / 123.4 / 133.4 / 50.1 / 50.8
(4 runs, 221-397); item 48 285.6 / 125.4 / 116.3 / 34.7 / 37.6 (240-339); item 51 259.6 / 103.4 / 111.1 / 31.7 / 37.3
(212-291); item 52 268.8 / 108.2 / 105.9 / 29.7 / 46.7 (223-326); item 53 (commit 098f216, no timestamp switch)
285.9 / 118.1 / 117.0 / 29.3 / 46.0 (263-318); item 54 301.8 / 118.7 / 119.6 / 34.7 / 56.0 (234-398). Items 51, 52
and 54 ran with `COMM_PHASE_TS=1`, item 53 without: the means do not separate (m). The steady gap sits in the exchanges
(bs and dm, about +13 and +21 s), not in init (mod: best-4 difference).

Runs of items 52-54 (m; total / bs / recip / dm / init, s):

| item | run 1 | run 2 | run 3 | run 4 |
|---|---|---|---|---|
| 52 (ts2) | 300.1 / 116.5 / 25.9 / 100.9 / 73.3 | 226.2 / 88.9 / 24.9 / 97.7 / 35.7 | 326.3 / 142.0 / 43.6 / 129.2 / 41.7 | 222.6 / 85.5 / 24.2 / 95.9 / 36.0 |
| 53 (var) | 317.6 / 128.0 / 33.2 / 128.6 / 55.6 | 262.6 / 120.6 / 27.0 / 103.4 / 35.3 | 284.8 / 108.3 / 33.6 / 124.3 / 45.7 | 278.5 / 115.4 / 23.4 / 111.7 / 47.4 |
| 54 (ts3) | 397.9 / 142.9 / 49.8 / 149.7 / 91.6 | 233.7 / 92.4 / 26.9 / 103.8 / 33.8 | 313.3 / 128.6 / 33.9 / 119.5 / 56.7 | 262.2 / 110.7 / 28.2 / 105.3 / 41.9 |

The per-PE offset-word wait (roffmax) tracks the total within each item (Spearman 0.80-1.00, n = 4 per item, m): slow
runs wait on peers, they do not compute more.

## Switches tried on the proxy mode (6 nodes, 3.8646e11; B − A, 95 % CI, m)

| switch | result | status | default |
|---|---|---|---|
| `COMM_SHMEM_PX_THP` (huge-page proxy pools) | item 37, 4 pairs: −287.8 s [−507, −68], faster 4/4; items 38, 39, 41 (default on vs off): +207.7 (2 pairs), +28.4 [−27, +84], +163.5 [−45, +372] | adopted, D36 (a) | 1 |
| `COMM_SHMEM_NP_HEAP_THP` (node-process heap in 2 MiB pages) | 3 n items 97q-97t: stalls in 0 of 16 runs on (and 0 of 6 at 2 n, 97v) vs 9 of 24 off; see "Compaction stalls" and results/SHMEM_STALLS.md | adopted, D48 | 1 |
| `COMM_SHMEM_PX_PIN` (pinned pools) | N7G, 3 pairs with THP: −124.9 s [−460.6, +210.8]; never separated from THP | removed 2026-10-18 (not significant, no gain at 3 n in items 97e/97f; review date passed) | removed |
| `COMM_SHMEM_HEAP_THP` (huge-page SHMEM heap) | item 40, 2 pairs: +24.8 s [−179, +229]; gates PASS | dropped (D39), not merged | — |
| `COMM_SHMEM_OFFS_WITH_DATA=64` + `COMM_SHMEM_MESH_PAR=1` (N7F fixes A and C) | item 40 task-3 arm, 2 pairs: +49 s | off, no gain; `COMM_SHMEM_OFFS_WITH_DATA` removed 2026-10-18 (D49), `COMM_SHMEM_MESH_PAR` removed 2026-10-19 (task 4) | 0 / 0 |
| `COMM_SHMEM_PX_BLOCK_MB` (N7F fix B, put chunk) | not timed; default 4 MiB equals the comm_ofi chunk | removed 2026-10-19 (D54; 1/16/64 MiB did not improve PX3, item 99c) | fixed 4 MiB |
| `COMM_SHMEM_LAYV_ANY` (layered exchange, any-order push) | items 49-50, 4 pairs: −45.0 s [−116, +26], faster 4/4; item 56, 4 pairs: +4.6 s [−174, +183]; pooled over 8 pairs: no effect; gates PASS 1 and 2 n, 6-n 1e10 identical (item 55) | merged c3a469d, no effect; removed 2026-10-18 (gates PASS 1 and 2 n, item 93g) | — |
| `COMM_SHMEM_PE_PER_APU=2` | item 58 found an abort, not a timing: Cray SHMEM kills the job when `SHMEM_OFI_NUM_NICS` exceeds the NICs present (aac7: 4; the default 4 x PE_PER_APU asked for 8). Fixed in bbf471e/f67baf7 (mnrun.sh caps the default at the node's NIC count); item 63 gates PASS (1 and 2 n with value 2, 2 n default). Timing at 2 n: item 80 (ppa2s, 4 pairs, 2026-10-17): value 2 +5.6 s [+2.5, +8.6] slower than 1 (116.8 vs 122.4 s, CV 1.0 % both, m) | fixed; value 2 slower at 2 n, off | 1 |
| `ECALC_HOST_THP=0` (transparent huge pages off for the node process; branch hthp, unmerged) | item 65 (pb1, 2 n, 2026-10-17 22:07): D 124.6, C 125.8, H 146.0 s; H − D +21.4 (rounds +31, -6, +17, +44), C − D +1.2 (m) | not adopted; branch kept for reference | 1 |
| `COMM_SHMEM_POOL_MB=6144` (symmetric pool 6144 MiB vs the default) | item 78 (pool2, 2 n, 2 pairs): +18.6 s [-93, +130] (pairs +9.8, +27.4); init 46-61 vs 37-39 s (m) | not adopted | default |
| `COMM_SHMEM_PX_WORKERS=<n>` (n worker threads per proxy, each on a private SHMEM context; branch pxpeer, unmerged) | gates PASS on adb1343 (item 87: 1 and 2 n with 8, 2 n off). t_comm 4 n (item 72c): per-proxy busy rate 22.6 vs 18.5 GB/s/NIC, node throughput W/D 1.00-1.05, W/L 0.71-0.78 (m) | not adopted: raises the proxy rate, not the node throughput | 0 |
| `COMM_SHMEM_PX_SEND=2` + `COMM_SHMEM_PEER_ORDER=shift` (SS) | t_comm 4 n: SS/L 1.00-1.05 (item 72b). ecalc 4 n (item 72d, 4 pairs): SS − D −52.8 s [-158, +52], faster 4/4, nodes stall-heavy (D up to 280 s) (m) | candidate; mpss4 (72j, 4 n, memprep both arms): SM − DM +11.5 s [−50, +73]; mpss4b (72l) −56.3 and mpss4c (72p) −15.6 s per 4-pair set (12 pairs in all); SM saves 4-7 s of dm only (m); both switches removed 2026-10-19 (`PX_SEND=2` task 4, `PEER_ORDER` D54) | not adopted: no separation from the noise | 0 / 0 |
| `COMM_SHMEM_PX_SEND=3` (PX3: plain `shmem_ctx_putmem_nbi` chunks on a context per peer node, one fence + signal word per command; branch pxgap, merged ead367d) | gates PASS at 1 and 2 n (72q pxgate3, 75c). t_comm 4 n, 64 MiB exchange (72o tcomm4fix, T64 = PX3 + shift + BLOCK_MB=64): T64 17.46 ms vs libfabric 18.38 vs D 19.85 (m). ecalc 4 n (72r pxgap4, TM = PX3 + memprep): TM − DM −21.5 s [−94, +51]. ecalc 2 n (75c, T6 = PX3 + shift + BLOCK_MB=64 + memprep, 4 pairs): T6 − DM +24.1 s [−34.3, +82.5], clean pairs (2) +0.1 s; the two slow-mode pairs hit either arm (m) | default since D53 (user 2026-10-19), see "PX3 as the default" below | 0 |
| `MEM_HSTAGE_THP=0` + `MEM_PRECOMPACT_GB` (host staging without huge pages; in-process precompaction; branch fragfix, merged ead367d) | item 72g (2 n, a99feb5): gates PASS; after the fragmenter F 504 vs FX 308 s, init still about 165 s (m) | partial; see "Phase B results"; removed 2026-10-19 (task 4) | 0 |
| `MEM_DEV_COMPACT` (per-allocation compaction of device arenas; branch fragfix2, 2a6e867, unmerged) | item 73d (fix2g2, 2 n): after the fragmenter FY 1220 s and one run killed at the 1800 s limit, against F 244 / 306 s; on a node with 0-90 free 2 MiB blocks each populate takes 25-869 s with no huge pages. Item 73e (fix2c2, clean nodes): DY − D −3.0 s (m) | dropped (the compaction cannot create blocks on an exhausted node); branch deleted | — |
| `ECALC_MEMPREP_ONLY=1` + `MNRUN_MEMPREP_GB` (memory preparation as a separate step per node before the launch; branch fragfix, merged ead367d) | items 72i, 72h, 72n: see "Phase B results" | partial; design confirmed by the allocation bench (item 73c); removed 2026-10-19 (task 4) | 0 |
| `COMM_PHASE_TS` (per-PE timestamps) | diagnostics only; items 51, 52, 54 vs 53: no measurable cost (m, 12 vs 4 runs) | merged c3a469d; removed 2026-10-18 (D49), `COMM_TRACE` v2 replaces it | 0 |
| earlier: `PX_NUMA`, `PX_KCOPY`, `PX_INIT`, `PX_SEND`, `PEER_ORDER` | results/SHMEM_ITEMS.md §N7C to N7E.md: N7E 28 pairs +129.3 s [+92.9, +165.7] against comm_ofi; none adopted | off; `PX_INIT` and `PX_SEND=2` removed 2026-10-19 (task 4), `PEER_ORDER` removed 2026-10-19 (D54) | 0 |

Item 55 (commit 3ff1c22): gates PASS at 1 and 2 nodes with the switches off and on, comm_ofi 2 nodes PASS, 6-node 1e10
compare identical with `COMM_SHMEM_LAYV_ANY` off and on (m). The switch was removed (eight pooled pairs showed no effect, nothing depends on it; gates PASS 1 and 2 n, item 93g).

## Node-count results (items 57, 66, 67; m unless marked)

| item | nodes | digits | result |
|---|---|---|---|
| 57 (paired) | 6 | 3.8646e11 | SHMEM − libfabric +39.9 s [+2.5, +77.4] (dm +23, bs +13, recip +7, init +3.5) |
| 67 gap2 (4 rounds x 2 slots) | 2 | 1.2882e11 | SHMEM default D 120.5 s (n 4, sd 5.1, CV 4.2 %, 114.9-126.2) vs libfabric L 123.3 s (n 4, sd 8.1, CV 6.5 %, 117.1-135.2); paired D − L by round −17.5, +5.1, −5.0, +6.0 (mean −2.9 s): no gap, no stalls. Slot b ran about 8 s slower than slot a in both arms |
| 66 sn1 (six independent items, 3 runs each) | 1 | 6.441e10 | SHMEM default 65.9 s (n 18, sd 1.6, CV 2.5 %, 63.5-69.7; items 1-4 alone 66.3, sd 1.8, n 12): no stalls |
| 71 gap4 (4 pairs, 21:26 ET) | 4 | 2.5764e11 | L 256.2 (first run, stalled) / 143.9 / 145.9 / 164.6 s vs D 171.0 / 206.8 / 182.9 / 172.8 s: D − L gap about +30 s without L's first run; D − L by pair −85.2, +62.9, +37.0, +8.2 (mean +5.7 s over all four, +36.0 s over pairs 2-4). Layered-exchange rate (per APU, GB / t_post): D 11.72 vs L 14.53 GB/s, D/L 0.81 at 4 n (2 n, items 67: 16.08 vs 16.51, D/L 0.97) |

Item 57 agrees with the steady gap above (about +36 to +40 s at 6 n). Observation (mod): the fitted per-node stall
rate below (p 0.41 per node per run, variant i) predicts at 2 nodes a stall in about 65 % of runs of 40+ s; item 67
(4 runs per arm) and item 66 (18 runs at 1 n) show none and no gap. So the stall and the gap depend on the node
count, not on per-node work alone. Item 71 (4 n): gap about +30 to +36 s excluding the stalled run; 2 n −3 s, 6 n +36 to +40 s, so the steady gap grows with the node count (the layered-exchange rate D/L 0.97 at 2 n, 0.81 at 4 n). Libfabric also had a stalled first run at 4 n, so stalls are not specific to the proxy mode.

Tooling: item 60 (hangtest) passed with the patched driver library (re-entrancy-guarded `nl_on_exit`: DONE, then rc 0,
52 s). Item 61 (hthp) stopped after its gates passed; the pb1 campaign (item 65) replaces it and is running.

## Phase B results: the stall is host-memory fragmentation (items 65, 78, 76, 72g, 72i, 72h, 72n, 73c-73e; m unless marked)

- Reproduced (item 76, frag2, 2 n, 2026-10-17 22:58): a tmpfs fragmenter run before the job turns a 115-125 s 2 n run into a
  stall run. Arms (total s, n): X no fragmenter 143.8 (1); F right after the fragmenter 336.3 / 284.8 (init 194 / 146 vs
  49-71); Q after a quiet wait 192.6 / 184.5; C control afterwards 158.2 / 162.4. The F run on one node showed
  compact_stall 8707 and thp_fault_fallback 4083. The node does not recover after F (Q, C stay above the 2 n baseline).
  So host-memory fragmentation produces the node-local stalls (the first run after a busy period is the slowest, as seen
  at 6 n).
- Mechanism (from the item 76 logs): start-up steps that touch host memory slow down: arena allocation 23-27 s vs 7,
  rns_init 35-37 vs 14-15, the layered self-test 77 / 124 vs 9 / 14. Host staging buffers request huge pages
  (madvise); with the kernel's defrag setting this means direct compaction per fault, and the huge-page request falls
  back to small pages when compaction fails. The device pools (hipMalloc, about 97 GB per NUMA domain) are inferred
  to land in small pages as well (a, GPU-side allocations suspected). Not a SHMEM-only effect (libfabric stalled at 4 n).
- Not the cause or not a cure: node-process THP off (item 65: +21.4 s, slower); a larger symmetric pool (item 78: +18.6 s).
- Fixes tried (fragmenter then run, total s per run; m): item 72g (fragfix, 2 n, a99feb5, precompaction in the node
  process): F 504 vs FX 308, init still about 165 s. Item 72i (memprep4, 4 n, e05ce42, separate preparation step): F 634 /
  891, FX 560 / 417, D 414 / 309, DX 569 / 229 (the nodes were left in a bad state by the item's own fragmenter, so
  the comparison is weak). Item 72h (memprep2, 2 n): F 396.7 (one run killed at its 900 s limit), FX 445.7 / 295.2, FP
  289.3 / 347.4, C 118.1 / 214.0; in two runs (FP r6, FX r7) init was 40 s but bs 222 / 160 s: the cost moves to the
  next memory-touching step.
- Item 72n (mpl4, 4 n, memprep in both arms, DM vs LM): DM 207-250 s (4 runs, mean 230), libfabric LM 162.6 / 165.2 s
  (2 ok runs, mean 164) (m). The other two LM runs were BAD (rc 8, no crash): the program's host-memory budget check stopped
  every rank before compute because node n1 held 1.2-1.4 GB less MemAvailable than the 33.8 GB needed (LM needs about
  10 GB more host than DM for its pools). Not a code fault; it shows the margin: LM runs sat 7-40 GB from the limit, DM
  passed by 2.9 GB in one run. Memprep ran in every run (huge 100 %) but took 4-158 s per node (slowest where only 80-88 GiB
  per NUMA node could be populated). Diagnosis, doubt moderate: the node, not libfabric.
- Item 73d (fix2g2, 2 n, MEM_DEV_COMPACT=2, branch fragfix2): after the fragmenter FY took 1220 s (init 1072 s) in one run
  and was killed at the 1800 s limit in another, against F 244 / 306 s, C 172 / 255 s and the clean X 142 s. The first ~40
  calls are fast (17 GiB in 1.1-1.7 s), but on a node with 0-90 free 2 MiB blocks each populate takes 25-60 s (one 13 GiB
  call 869 s) with 0 % huge pages and gives back no blocks; the calls repeat for every 2.15 GB chunk and the five
  processes of a node queue on the mapper lock. Compaction cannot create blocks on an exhausted node, so the switch was
  dropped and the branch deleted. Item 73e (fix2c2, clean nodes): DY − D −3.0 s (no cost when blocks are plentiful).
- Item 73c (fragbench1, 1 n, 64 GiB first touch, clean vs fragmented host memory, 2 repeats): the only lever is threading.
  thp_pop_mt4 (4 pinned threads, MADV_HUGEPAGE + MADV_POPULATE_WRITE) is best on fragmented memory: 7.1 s, 9.7 GB/s,
  100 % huge pages (clean 10.3 GB/s); 16 threads 8.7 GB/s on fragmented memory (more compaction stalls); single-thread
  patterns 6.0-7.4 GB/s; MADV_COLLAPSE no faster and lost 448 pages to fallback. No pattern repairs the fragmentation.
  Limit: the fragmented state still held 31-45k free 2 MiB blocks (32768 needed); exhausted memory was not tested.
  This matches the design of the memory preparation step (4 threads per NUMA node). Inference (a): prepare once before the
  large allocations, take the huge-page-sensitive regions first (device arenas, seed buffers, plane pools), never repeat
  the preparation per allocation.
- Status: partial. The preparation step removes about a third of the loss; the remainder reappears in the next step
  that touches host or GPU memory. MEM_DEV_COMPACT is dropped. Problem A is now the own-allocation design (the process
  reserves and prepares its large regions itself in a good order); design in progress, nothing measured yet.

## Phase C results: the proxy path against libfabric (items 72, 73, 72b, 72c, 72d, 87, 72j-72r, 75c; m unless marked)

- t_comm bandwidth probe (proxy D vs libfabric L, sizes 16-256 MiB): at 2 n D/L about 1.0 and proxy put rate 23.2
  GB/s per NIC. At 4 n with every peer in flight D/L 0.71-0.93 across probes (0.77-0.78 at 16-64 MiB in item 73, 0.88-0.93
  in item 72b, 0.71-0.74 in item 72c), with pairs only 0.94-1.03; the proxy's busy put rate falls to 17.5-18.8 GB/s per NIC against
  23.2 with one peer. The put chunk size and the host copy are not the cause (per the probes).
- Mechanism: the one-loop proxy posts a put and blocks in shmem_quiet on one destination at a time, so with several
  peers the first batch holds one peer and, in rank order, every node's first put hits rank 0 (incast).
- Worker threads (COMM_SHMEM_PX_WORKERS=8, branch pxpeer): per-proxy busy rate 22.6 vs 18.5 GB/s per NIC, but node throughput
  unchanged (W/D 1.00-1.05, W/L 0.71-0.78): the bottleneck is elsewhere. Not adopted, branch unmerged.
- PX_SEND=2 with PEER_ORDER=shift (SS): matches libfabric in the probe (SS/L 1.00-1.05, item 72b) and was faster in
  4 of 4 ecalc pairs at 4 n (item 72d: −52.8 s [−158, +52]; the nodes were stall-heavy, D up to 280 s, so the
  interval is wide and part of the difference may be stall luck). Whether SS is real is the pending question.
- SS against D with the memory preparation step in both arms (items 72j, 72l, 72p, 4 n; 12 pairs in three sets): SM − DM
  +11.5 s [−50, +73], −56.3 and −15.6 s per set; SM saves 4-7 s of the dm phase only, the totals do not separate. Item
  72k (mpss2, 2 n): +6.3 s [−82, +95], stalls in both arms. SS is not confirmed.
- PX3 (COMM_SHMEM_PX_SEND=3, plain put chunks on a context per peer node, one fence + signal per command). Why: the default
  proxy posts at 19-20 GB/s per NIC while busy but idles inside the exchange, and PX_SEND=2's put-with-signal chunks post
  at only 14.9 GB/s (item 72b-era probes). Gates PASS at 1 and 2 n (items 72q, 75c). t_comm probe at 4 n (item 72o): the
  64 MiB exchange takes 17.46 ms with T64 (PX3 + shift + 64 MiB chunks) against 18.38 ms libfabric and 19.85 ms default
  (m), i.e. the probe gap closes. ecalc at 4 n (item 72r, 4 pairs, memprep in both arms): TM − DM −21.5 s [−94, +51], not
  significant. ecalc at 2 n (item 75c, 4 pairs): T6 − DM +24.1 s [−34.3, +82.5]; the two clean pairs differ by +0.1 s
  (dm phase 38.3-38.6 s in both arms, same comm time), the two slow-mode pairs (roffmax 112-143 s) hit either arm, so one
  pair does not attribute the loss to the arm. Neutral at 2 n is expected (one peer node).
- Pending: 4 n statistics for PX3 (items 75a t64m4 with T6 against DM, 75b t64m4b, 75d px64g2b at 2 n); a 6 n confirmation
  only after at least 8 pairs at 4 n with a CI that excludes 0.

## Scaling model to 576 nodes (mod)

Fit on the 24 SHMEM runs (16 per-run, 8 partly assumed): base 212.3 s (fastest run, a), a node stalls with probability p
per opportunity with Exponential(mu) size, the run loses the slowest node's stall. Variant (i), one opportunity per
node per run: p 0.41, mu 42 s. Variant (ii), 3 node-local phases: p 0.17, mu 28 s. Both reproduce mean, sd and
P(< 30 s excess) of the data; 24 runs cannot separate them. Libfabric: 184.5 s base plus the max of N per-node jitters
(Normal, 9.9 s). Steady gap 36 s at 6 n scaled to 576 n as low x 0.30 (exposed fabric time ratio) or high x 2.25 (NIC
bytes per node). Target 3.82e13 digits, 576 nodes x 4 APUs; estimate.py inputs `aac7_l4` g 6 and `target_l4` g 576.

| N | libfabric | gap low..high | stall excess (i) / (ii) | SHMEM (i) / (ii) | SHMEM / libfabric |
|---|---|---|---|---|---|
| 6 | 184.5 | 36 | 67 / 69 | 288 / 290 | 1.56-1.57 |
| 24 | 191.3 | 28..50 | 122 / 166 | 342 / 407 | 1.79-2.13 |
| 96 | 196.8 | 21..63 | 182 / 278 | 399 / 538 | 2.03-2.74 |
| 288 | 200.5 | 15..74 | 227 / 370 | 442 / 645 | 2.20-3.22 |
| 576 | 202.5 | 11..81 | 256 / 426 | 469 / 709 | 2.32-3.50 |

The excess grows as mu ln(N p). Sensitivity at 576 n: halving p buys only 29-58 s; p x 0.1 gives x 1.84-2.57; p x 0.01
gives x 1.36-1.72; stalls removed leaves x 1.05 (low gap) to x 1.40 (high gap); halving mu gives x 1.69-2.45.
Conclusion (mod): at 576 n the stall variance (+256 to +427 s) dominates the steady gap (11-81 s); staying within 10 %
of libfabric needs the stall mechanism removed (about 0.1 % of today's rate), not reduced, so phase B precedes phase C.
Caveat: the model assumes the stall rate per node is node-count independent. The stall depends on node state (host
memory fragmentation, reproduced in item 76) and on how many nodes are in a run, not on node count alone: the 1 n and 2 n runs on clean
nodes showed none, while a fragmented node stalls at 2 n. The fitted p is therefore a property of the nodes' recent
history, and the 576-n excess is an upper-leaning estimate (mod). Uncertainty: 24 runs
and an assumed size law; the absolute 576-n excess is about ±40 %.

## Variance findings (items 43, 48, 51, 52, 54; m unless marked)

- No fixed straggler: the late node changes every run (node 0, 1, 2, 3, the pair 4+5, or all six), each PE on its own
  NIC and NUMA domain, pools 2 MiB everywhere.
- Slow runs lose 80-180 s in node-local steps with no inter-node exchange (12 runs of items 51, 52, 54): number-system
  init 13 → 31 s, the layered self-test 10 → 53 s (held by its slowest member), the first leaf binary-splitting
  step 28 → 97 s. Spearman with the total: slowest leaf +0.92, self-test +0.82, init max +0.76, GPU ntt-step stalls
  in leaf levels +0.73. The layered exchange rate (5.9-8.3 GB/s mean) correlates −0.71 and is worth about 15 s.
- Not correlated with the total: transport open 6.7-10.5 s, shmem_init 4-8.6 s, huge-page collapse 2-3.6 s, the
  start barrier 5.5-9.8 s. The staggered first-bs starts seen earlier (pairs of nodes up to about 48 s late) are a
  consequence: the other nodes wait for the slowest node's leaf step.
- In the slowest run of item 54 (397.9 s) four of the six nodes report init 90-92 s against 27-28 s on the other two
  (fast runs: 32-36 s on all nodes).
- Compaction stalls before and after runs are not linked to the slow runs (item 48, n = 4: largest per-run increase
  6770, 4170, 708, 501). The first run of each of items 52-54 was its slowest and had the highest pre-run load1
  (35-58 vs 18-23, m, n = 3).
- Mechanism (hypothesis, a): transient memory-subsystem slowness on a node (first-touch allocation and GPU kernels),
  plausibly kernel reclaim or compaction under five pinned 3 GiB symmetric heaps per node plus the huge-page pools,
  which the libfabric path does not create.
- Clock sync error between nodes is about 0.4 ms (inferred, not measured).

## Compaction stalls and `COMM_SHMEM_NP_HEAP_THP` (3 nodes, 4.8e10, items 97a-97t, 2026-10-18; m unless marked)

- Mechanism (items 97k-97n, heartbeat counters `COMM_TRACE_HB_MEM`, per-mapping smaps capture): the kernel's proactive
  compaction (`compaction_proactiveness` 20) starts on a NUMA node whose free memory is > 90 % in blocks < 2 MiB. After
  start-up this is NUMA node 0 of the job's first node in nearly every run (score 91-99; other nodes 0-11). A pass isolates
  the node-process's Cray SHMEM symmetric heap (`/dev/zero` shared mapping at the library base, all 4 KiB: 3096 MiB under
  SHMEM, 1048 MiB under libfabric), fails to migrate the NIC-registered pages and retries; every unmap evicts the process's
  GPU queues (KFD `evicted_ms` +42 s summed in an 8 s storm), so all 4 APU threads stall. Failed-page counts per storm match
  the heap sizes (740k pages = 2.9 GiB SHMEM, 241k pages = 0.94 GiB libfabric).
- Fix: `COMM_SHMEM_NP_HEAP_THP=1` pre-faults and collapses that heap to 2 MiB pages before registration (compaction skips
  compound pages); 3096 MiB fully PMD-mapped, 1.9-2.0 s per process, overlapped (init unchanged). Gates 1 n, 2 n SHMEM and
  libfabric, off and on: PASS, digits identical (97q, 97s; main-based build in 97w).
- Runs, 3 n (97q/97r/97s/97t): with the switch 0 of 16 runs stalled (SHMEM DT 47.4-50.9 s, libfabric 45.9-48.0 s;
  2 n, 97v: 0 of 6); without it 9 of 24 stalled (SHMEM up to 70.3 s). Same nodes: SHMEM with the switch about 6 % behind libfabric when
  libfabric runs well (49.6 vs 46.6 s, 97q/97r). Default 1 since D48 (user 2026-10-18).
- Not adopted: `MEM_FRAG_RESERVE_GB` (branch fragres): a 6 GiB reserve on NUMA node 0 lowers the score after start-up but
  costs 4-8 s of init and, without the heap fix, makes the storms worse (97s/97t). `MNRUN_MEMPREP_GB=100` (97g/97h),
  `MEM_HSTAGE_THP=0` (97b3/97b4), `COMM_SHMEM_PX_INIT_THREADS=16` (97i/97j), `COMM_SHMEM_PX_CONC=1` (97c/97d): no effect.
  `COMM_SHMEM_PX_INIT_THREADS` removed 2026-10-19 (a constant 4): 98d (2 n, measured) heap collapse about 3 s at 4, 8 and 16 threads (node-process heap mean 2.7-3.4 s, helper heaps 2.9-3.4 s).
- Remaining (m): successful compaction passes on that node still evict the GPU queues for 1.4-12 s summed per run; root
  access (`compaction_proactiveness=0` per job, hugetlb heaps) would remove them (private list of settings kept outside the
  repository).

## Four-node breakdown, start-up cost and dropped experiments (items 98a-98o, 2026-10-18/19; m unless marked)

- **4 n pooled** (6.4e10 digits, build with the NP heap fix on, first run of each queue item excluded, 23 runs): libfabric 53.18 s (n 7), SHMEM DT 54.34 s
  (n 12, +2.2 %), DTIX (DT + `COMM_LAYER_INTER2=0`) 53.54 s (n 4, +0.7 %). SHMEM is within 5 % of libfabric at 4 n; the
  `LX`/`DTX` arms are A/A controls since the fix is the default.
- **DT against DTIX** (item 98n, 4 n 6.4e10, ABBA, 8 runs each): DT 53.8 s (sd 1.0), DTIX 53.8 s (sd 0.8), S (SHMEM
  defaults) 55.8 s (n 2), A/A control 54.4 s (n 1). `COMM_LAYER_INTER2=0` gives no gain on SHMEM. DT is faster than S
  by 2.0 s, mostly in dm (14.8 against 16.6 s) and bs (12.9 against 13.6 s). No first-run init excess in this item
  (first run init 23.1 s).
- **Trace compare** (`trace_report --compare`, libfabric 2 runs against DT 4 runs, per-process medians): exchange window
  36.6 against 35.0 s; exchange in flight equal (21.9 against 22.1 s); DT has +2.5 s in copy-in and +1.0 s in quiet
  (m). The compaction-stall counter was 977 (libfabric) against 27 (DT).
- **Start-up** (2 n, 3.2e10, warm SHMEM, init 23.65 s on node 0): `mn_init` 6.1 s (heap collapse 3.0, library init
  1.7, proxy pool registration 1.0) + `rns_init` 11.0 s (device memory commit about 17-18 GB/s per node; planes about
  6.5 s (mod), arena half 3.6 s) + self-tests 3.0 + 3.3 s (m, parts by difference marked mod in the source run).
  Libfabric init is about 1.5-1.9 s shorter (smaller heap, no proxy pools); 4 n init is 0.5 s above 2 n (m). The first
  run of an item takes 33-35 s of init against 23-25 s, with the lowest MemFree before the run and the extra time in
  the device memory commit (m; cause assumed: page-cache reclaim).
- **Heap-collapse threads**: 4, 8 and 16 threads give about 3 s each (node-process heap 2.7-3.4 s, helper heaps 2.9-3.4
  s, 2 runs per setting, m): kernel-bound, not thread-bound. `COMM_SHMEM_PX_INIT_THREADS` was removed (constant 4).
- **Dropped: `ECALC_INIT_OVERLAP`** (plane-pool allocation in threads during `mn_init`, upper bound -5.5 s, mod): 1 n
  passed, 2 n failed; the proxy heap collapse got ENOMEM for 2 MiB pages while the overlapped allocation ran, so the
  overlap competes with the stall fix for free 2 MiB blocks. Not merged.
- Residual eviction figures were corrected: results/SHMEM_STALLS.md §6.5.

## Six-node confirmation and the gap against the off-node peers (items 98f, 98r, 98s, 2026-10-19; m unless marked)

- **6 n confirmation** (98f, 3.8646e11 digits, build 75a9bb8, gates g6/g6l PASS): libfabric 183.2 s (sd 1.5, n 3), SHMEM
  defaults 211.7 s (sd 2.4, n 3, +15.6 %), DT 215.1 s (sd 0.7, n 4, +17.4 %); paired L - DT -32.2 s, 95 % CI [-37.3,
  -27.1]. No stalls: the spread is 0.7-2.4 s against sd 111 s before the NP heap fix. S - L by phase: dm +20.8, bs
  +8.4, recip +5.5, init -1.5 s.
- **Same digits per node at 2 and 4 n** (6.44e10 per node): 2 n (98s, 1.2882e11) S 119.2 against L 118.2 s (n 4 each,
  paired +0.9 s, CI [-3.0, +4.8]); 4 n (98r, 2.5764e11) S 168.2 (sd 2.0), DT 161.6 (sd 1.0), L 151.6 s (sd 7.6, bimodal
  145 / 154-161 s), paired S - L +16.6 s, CI [+3.8, +29.4]. The gap follows the number of off-node peers: +0.8 % with 1,
  +11 % (DT +6.6 %) with 3, +16 % (DT +17 %) with 5. At 6.4e10 on 4 n the total hid it (libfabric's longer start-up and
  a small exchange share), though bs + dm were already about 16 % slower.
- **What the excess is** (traces and exchange counters, 4 and 6 n): per byte, not per message (libfabric gets cheaper
  per byte with larger messages, SHMEM stays flat: 6 n unequal exchange 0.086 against 0.118 s/GB); about 74 % of the
  SHMEM excess is waiting on peers (offset word and signal; signal one-way at 4 n mean 629 against 124 µs, p99 7.8
  against 1.0 ms); no straggler node or slow link. Likely cause (not proven): each peer's chain (offset publish, put,
  signal) ends at the slowest of the N chains.
- **Early offset publish** (candidate) is not possible as built: the receiver publishes right after its intra stage and
  transpose, and the intra receive buffer shares the inter receive region until the transpose; publishing earlier needs a
  separate buffer (+A per slot per APU thread of device memory).

## PX3 as the default (D53, user 2026-10-19; items 98v, 99b, 98r, 99n, 99p; m)

- **Rule.** `COMM_SHMEM_PX_SEND` unset = 3 (PX3) under `COMM_SHMEM_PE_PER_APU=1` with an OpenSHMEM 1.5 build, else 1; an explicit
  value keeps its checks. `COMM_SHMEM_PX_SEND=1` is the fallback.
- **4 n** (2.5764e11 digits, item 98v, n 3 each): PX3 162.0 s against `PX_SEND=1` 169.1 s; DT 161.6 s (98r).
- **On the default build** (main a6cc1b3 / 66e34d0, 2026-10-19): 4 n 2.5764e11 (item 99n, gates of the default, `PX_SEND=1`
  and libfabric PASS): default 163.7 s (n 4, sd 1.1) against libfabric 156.7 s (n 5; bimodal 148.5-168.7 s), paired +9.6 s, CI
  [-4.4, +23.7]. 2 n 1.2882e11 (item 99p, one off-node peer): default 114.6, `PX_SEND=1` 116.3, libfabric 115.4 s (n 3-4
  each, without the slow first run of the item, 136.3 s, init 48 against 37 s): parity, the default costs nothing at 1 peer.
- **6 n** (3.8646e11, build d1a1f277, items 99b + 99g pooled): PX3 against `PX_SEND=1` in 7 pairs, every pair faster, mean
  -5.9 s (-2.8 %), 95 % CI [-9.4, -2.4]; per item PX3 205.7 / 208.1 s against 213.1 / 212.8 s; libfabric 190.0 / 190.7 s
  (99g: 181.5-184.1 s after a slow first run). The rule of D53 (CI excluding 0, digits identical) is met.
- **Tried and not adopted** (4 n 2.5764e11 sweep 99c, n 2 each, and 6 n): chunk size (1, 4, 16, 64 MiB) and
  `COMM_SHMEM_PEER_ORDER=shift` change PX3 by less than the spread (P3 161.4, 1 MiB 164.3, shift 164.1 s at 4 n; 64 MiB +
  shift (DT) 215.1 s at 6 n, worse than `PX_SEND=1`). `PX_SEND=4` (put-with-signal on the last put, branch pxsig, archived):
  6 n 246.6 s (99b); with shift 149.2 s at 4 n (libfabric level) but 221.9 s at 6 n, +9.0 s against `PX_SEND=1`, CI [+5.0,
  +13.1] (99g): whole puts with one-to-one pairing help at 3 peers and hurt at 5.
  `COMM_SHMEM_PEER_ORDER` and `COMM_SHMEM_PX_BLOCK_MB` were removed 2026-10-19 (D54; the chunk is the fixed 4 MiB): every peer loop walks in rank order; the trace header keeps `shift` = 0.
- **Remaining gap** at 6 n: PX3 against libfabric about +9 % (208 against 190 s); see "Six-node confirmation and the gap
  against the off-node peers" above.

## Round size and send schedule (items 9aa, 9ad, 9ae, 2026-10-19; m unless marked)

- **Round size** (`COMM_SHMEM_ROUND_MB`, 1024 on the launch line): no division-multiply exchange is split into rounds at 4 n
  2.5764e11 (each is about 134 MB per peer pair against a 341 MiB round share; 660 of 16000 exchanges are rounded, none in
  dm). 2048 adds 10.7 GB per node and failed the runtime budget check in 2 of 3 runs; 4096 tripped the host memory guard;
  the one 2048 run that fitted took 175.2 s against 175.0 s for 1024. Not a lever.
- **Send schedule:** whole puts to one peer at a time with put-with-signal (archived `PX_SEND=4` + shift order, P4S, build
  d1a1f277) is steady at 4 n: 150.9 s (n 4, sd 0.8; dm 54.5-55.5 s) against PX3 162.5 s (n 3; dm 63.1-64.4 s) and libfabric
  147.4 s (n 3, all fast launches; dm 52.0-52.3 s) (item 9ae). PX3's dm sits at libfabric's slow-launch level in about 45
  runs at 4 n and never at the fast one, so it is not the per-launch draw; likely cause: incast from PX3's round-robin
  chunks to all peers at once (derived). At 6 n P4S lost 9 s (item 99g); a schedule that keeps the 4 n gain is in design.
- **Schedule switch `COMM_SHMEM_PX_SCHED=W|auto`** (on main, default off): whole put-with-signal per command, a
  shifted ring for both the sender and the receiver loops, a baton window W, and a blocking receive-offset wait when
  W >= n-1. A first version that shifted only the sender did not reproduce the gain (SC99 162-165 s, dm 65 s, sender offset
  wait 24.5 s): both ends must walk the ring and the offset wait must be the library's blocking one.
  - Item 9am (4 n 2.5764e11): gates 4/4 PASS. SC1 150.6 / 149.4 s, SC99 152.5 / 153.5 s (first run 157.8 s), PX3 167.5 /
    166.9 s. dm 53-55 s for SC against 67 s. Mechanism: 11354 whole puts against 196369 4 MiB chunks; proxy rate while busy
    19.5 against 17.1 GB/s; dm post-to-complete 22.5 against 32.1 ms per exchange; sender offset wait 0.8 against 7.4 s.
    SC1 and SC99 are not separable at n 2.
  - Item 9aj (6 n 3.8646e11): PX3 217.6 / 215.4 s, SC99 223.4 / 221.3 s, SC1 246.0 / 249.3 s, libfabric 207.5 / 201.3 s.
    The schedule loses at 6 n (SC99 +5.8 s, SC1 +31 s against PX3 means), the same pattern as P4S.
  - Review fixes (1aeabae1: with the switch off the control block is the same as main): gates 5/5 PASS (item 9an; 1 n and
    2 n default path, 2 n W=99, 2 n W=1 with rounds).
  - PLAN16 items (2026-10-20, m, all VERIFY=OK): 4 n 2.5764e11 SC99 149.1 (item 9hb), 152.9 / 152.8 (9ht), 150.3 / 148.4 /
    149.2 / 152.1 s (9hx): n 7, mean 150.7, dm 53.4-53.8 s, against PX3 165.0, 165.7 / 166.2, 163.7 / 163.2 / 164.8 s (warm,
    n 6, mean 164.8, dm 64.0-64.7 s); every SC99 run is below every PX3 run (-14.1 s, -8.6 %); libfabric in the same items
    145.9 s (fast launch) and 165.0-175.0 s (slow launches). 3 n 1.9323e11 (9hu, 9hv): SC99 157.6 / 157.7 / 158.3 s (first
    run 166.0) against PX3 158.5 s (n 4) and libfabric 144.7 s (3 fast launches): neutral. 2 n 1.2882e11: 115.4 against
    115.9 s (neutral). 6 n 1.93e11 (9hi, 9ho): SC99 97-100 s against PX3 92.9-94.5 s (loses). The gain is a 4-node (power
    of two, mostly equal-block exchanges) effect; at 3 and 6 n the unequal all-to-all-v exchanges dominate and no schedule
    closes the gap to libfabric (traces and staging-size probes in this file's PLAN16 notes).
  - Merged default off with `COMM_SHMEM_PX_SCHED=auto` (99 up to 4 nodes, else 0; the banner prints the resolved value),
    branch pxauto 8f254a4b: gates 7/7 PASS (item 9hz: 1 n and 2 n default, both protocols, 2 n `=99`, 2 n `=auto` with the
    banner "resolved to 99", 2 n `=99` with COMM_TRACE). Adopting `auto` as the default is the user's decision. Item 9ao (6 n 3.8646e11, branch 1aeabae1): PX3 222.6 (first run) / 204.9 s, W=2 234.0 / 233.5 s, W=3 232.1 / 231.8 s (m): every window loses at 6 n.
- **NIC policy** (item 9af, 4 n 2.5764e11): `SHMEM_OFI_NIC_POLICY=NUMA` aborts at init (rc 134, 2 of 2 runs), so it is not
  usable. Controls in the same item: PX3 168.3 s warm (the first run 183.0 s), libfabric 162.0 and 157.9 s (m).

## PLAN16 (2026-10-20): the remaining gap and what closes it (items 9hi-9iz; m measured, mod modelled, d derived)

The send schedule itself is described above ("Schedule switch `COMM_SHMEM_PX_SCHED=W|auto`"); this section records the
state per node count and why the rest stays open on aac7.

- **Per node count (ecalc, m).**
  - 2 n 1.2882e11: `PX_SCHED=99` + `COMM_SHMEM_ROUND_MB=128` 112.4 s (n 5) against libfabric 116.1 s (n 5, warm): SHMEM
    ahead (items 9iq, 9iv).
  - 4 n 2.5764e11: the same configuration 148.3 s (n 10; 146.9 s, sd 0.4, in 9iu alone) against libfabric 157.0 s (n 8;
    fast launches 145.0-149.4 s, slow launches 160.5-167.7 s): closed (items 9il, 9ip, 9iu).
  - 3 n 1.9323e11: no SHMEM variant closes the gap (PX3 about 158 s, SC99 about 158 s, SH1 156 s, libfabric about 145 s;
    items 9hu, 9hv, 9in).
  - 6 n 1.93e11: PX3 about 94-98 s, libfabric 84-92 s; no schedule helps (items 9hi, 9ho, 9hp, 9hw, 9io).
- **Cause at 3 and 6 n** (non-power-of-two counts, unequal all-to-all-v "layv" exchanges). Traces (items 9ij, 9in) show
  bulk transfers in flight for about 59-61 s in every SHMEM variant against 51 s in libfabric for the same bytes (d);
  the node-process `shmem_ctx_quiet` at the end of layv exchanges is 13.8 against 0.46 s per node, with copies equal.
  Control-path fixes (NOARRIVEQ, SC99) only move the wait into the put tail. Libfabric with a host staging pool is no
  faster at 6 n (item 9hr) and far slower at 3 n (item 9ir, no THP on that pool). The cxi driver gives huge NIC
  translation entries only for hugetlb or dmabuf memory (driver source), so SHMEM's host staging always uses 4 KiB NIC
  translations on aac7 (`nr_hugepages` 0): fact from the driver source. That the gap shrinks on the target, where hugetlb
  pages exist, is an inference (target items 29 and 31 test it).
- **Closed levers.** Hidden libsma fence and signal knobs (item 9ic); library environment flags and NIC maps (9ea, 9ed,
  9hs MCU bank); ATS and ODP (9it, 9is); device-source puts (DEVSRC: libsma 11.8 rejects a device-memory source, -14 Bad address); global objects (needs symmetric
  memory, 9id); small staging rounds for translation reuse at 6 n (9hw); `COMM_SHMEM_LAYV_ROUND` (branch layvr, not
  merged: dm +5 s at round 128 and +10 s at round 16 against PX3 at 3 n, items 9ix, 9iz); SC99 with `MN_NONPOW2_CAP` at
  3 n (9ih, 9ii: paired -1.1 s, n.s.); the shx, shx2 and shx3 switches (benchmark builds, not merged).
- **`COMM_SHMEM_ROUND_MB` sweep and memory.** 2 n 1.2882e11 (item 9im; total s, two runs each / init s / dm s / proxy pool
  MiB, four pools per node):

  | ROUND_MB | total | init | dm | pool |
  |---|---|---|---|---|
  | 64 | 113.3, 113.7 | 35.1 | 39.3 | 770 |
  | 128 | 114.3, 114.0 | 36.0 | 39.1 | 770 |
  | 256 | 112.5, 116.1 | 37.5 | 38.3 | 1026 |
  | 512 | 114.8, 117.2 | 36.8 | 38.3 | 1538 |
  | 1024 (default) | 116.2, 115.3 | 37.7 | 38.2 | 2562 |

  The pool floor is 770 MiB at 128 and below; 128 saves 7.2 GB per node at 2 n with no time lost. 4 n (item 9il):
  -2.5 and -4.1 s against the 1024 arms; 6 n 1.93e11 (item 9io): neutral (1024 97.0 s, 128 97.5 s, 256 94.9 s, n 2
  each). Planner fit (mod): at 6 n 3.8646e11 with `MN_NONPOW2_CAP` the node margin is +46.25 GB for libfabric and, for
  SHMEM, -3.68 GB at ROUND_MB 1024 against +3.84 GB at 128.
- **Defaults (user Q59, Q60, 2026-10-20).** `COMM_SHMEM_PX_SCHED` unset = `auto` (99 up to 4 nodes, PX3 above;
  `=0` restores PX3); the aac7 profile sets `COMM_SHMEM_ROUND_MB=128`. At the plain launch line, 2 n 1.2882e11 (item
  9jj): SHMEM 116.2, 113.5, 111.1 s (mean 113.6) against libfabric 119.3, 121.3, 113.5 s (118.0).
  Merged main 3fa7555, plain launch line (items 9jo, 9jn, 9jd): 2 n 1.2882e11 SHMEM 112.0, 112.0, 113.8 s (112.6) against
  libfabric 113.4, 114.8, 113.6 s (114.0); 4 n 2.5764e11 SHMEM 146.8, 144.8, 145.9 s (145.8) against libfabric 146.7,
  165.5, 147.3 s (153.1, bimodal); 3 n 1.9323e11 (`MN_NONPOW2_CAP` auto: on; item 9jw) SHMEM 155.0 (first), 142.6,
  147.4 s (148.3) against libfabric 136.2, 141.0, 143.9 s (140.4); 6 n 3.8646e11 (`MN_NONPOW2_CAP` on) SHMEM 193.7 (first), 182.3, 184.0 s (186.7)
  against libfabric 174.2, 173.5, 174.7 s (174.1).

## Open questions and next experiments

- Stalls (cause and fix recorded above): kernel proactive compaction on a 4 KiB pinned node-process heap evicted the GPU
  queues; `COMM_SHMEM_NP_HEAP_THP=1` (default, D48) removes it. Open:
  1. Done: 6-node confirmation (items 98f, 99b, 99g; no stalls).
  2. Hugetlb backing for the proxy and node-process heaps: switch `COMM_SHMEM_HUGETLB` merged, default off; the 4-node
     paired test is open (the target has hugetlb); it would also remove the residual evictions below.
  3. The remaining gap grows with the off-node peers (sections above); with PX3 about +7 % at 4 n and +9 % at 6 n.
  4. Residual GPU-queue evictions from successful compaction on NUMA node 0 of the job's first node (1.4-12 s summed per
     run, m).
- Problem B (steady gap): PX3 statistics at 4 n were running (items 75a, 75b, 75d). SS is not confirmed (12 pairs, no
  separation). If PX3 + shift + 64 MiB chunks separates from the default at 4 n (at least 8 pairs, CI excluding 0), a
  6-n confirmation against the item 45 and 57 baselines follows; if it does not, probe the hand-off between the node
  process and the proxy.
- Item 51's wall times in results/SHMEM_ITEMS.md §N7G (223, 264, 186, 235 s, from the timestamp lines) differ from the
  `summary.txt` totals used here (259.8, 291.2, 212.3, 275.2 s); the totals are the comparable figure.

## COMM_LAYER_INTER2 default under SHMEM (2026-10-21, user: "Adopt SHMEM one in flight over two in flight")

- **3 n, 1.9323e11 digits** (item 9m5 on aac7, build eb0c5f0e, 14 runs, all VERIFY OK; m): SHMEM default (two in flight) 149.9 s (n 5, sd 2.1);
  SHMEM + `COMM_LAYER_INTER2=0` 143.2 s (n 3, sd 5.5); libfabric 140.2 s (n 3); libfabric + `COMM_LAYER_INTER2=0` 137.2 s (n 3). The layv summed
  wait fell from 264-266 s (SHMEM) to 183 s (SHMEM + INTER2=0). The earlier 4 n test (older build, items 98a-98o above) showed no gain; no 6 n measurement yet.
- **Change**: `COMM_LAYER_INTER2` unset is now 0 under SHMEM and 1 under libfabric (`comm_layered_inter2_on()` reads `comm_ofi_planned()`, the same
  hardware/env decision on every rank); an explicit 0/1 wins. The `config:` eff line shows `layer_inter2:N`. Digits unchanged (exchange order only).

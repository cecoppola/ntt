# SHMEM — the SHMEM proxy mode against libfabric at 6 nodes: one summary (2026-10-17)

Owner of the SHMEM-vs-libfabric story. Per-item records: results/N7B.md to N7E.md (proxy mode, NUMA, copy kernel,
send and init variants), results/N7G.md (huge pages, items 37-51), this file (items 52-67). Labels: (m) measured,
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
| `COMM_SHMEM_PX_PIN` (pinned pools) | N7G, 3 pairs with THP: −124.9 s [−460.6, +210.8]; never separated from THP | off, not significant | 0 |
| `COMM_SHMEM_HEAP_THP` (huge-page SHMEM heap) | item 40, 2 pairs: +24.8 s [−179, +229]; gates PASS | dropped (D39), not merged | — |
| `COMM_SHMEM_OFFS_WITH_DATA=64` + `COMM_SHMEM_MESH_PAR=1` (N7F fixes A and C) | item 40 task-3 arm, 2 pairs: +49 s | off, no gain | 0 / 0 |
| `COMM_SHMEM_PX_BLOCK_MB` (N7F fix B, put chunk) | not timed; default 4 MiB equals the comm_ofi chunk | off (unchanged) | 4 |
| `COMM_SHMEM_LAYV_ANY` (layered exchange, any-order push) | items 49-50, 4 pairs: −45.0 s [−116, +26], faster 4/4; item 56, 4 pairs: +4.6 s [−174, +183]; pooled over 8 pairs: no effect; gates PASS 1 and 2 n, 6-n 1e10 identical (item 55) | merged c3a469d, no effect | 0 |
| `COMM_SHMEM_PE_PER_APU=2` | item 58 found an abort, not a timing: Cray SHMEM kills the job when `SHMEM_OFI_NUM_NICS` exceeds the NICs present (aac7: 4; the default 4 x PE_PER_APU asked for 8). Fixed in bbf471e/f67baf7 (mnrun.sh caps the default at the node's NIC count); item 63 gates PASS (1 and 2 n with value 2, 2 n default). Timing at 2 n: item 70 (pending) | fixed; timing pending | 1 |
| `COMM_PHASE_TS` (per-PE timestamps) | diagnostics only; items 51, 52, 54 vs 53: no measurable cost (m, 12 vs 4 runs) | merged c3a469d | 0 |
| earlier: `PX_NUMA`, `PX_KCOPY`, `PX_INIT`, `PX_SEND`, `PEER_ORDER` | results/N7C.md to N7E.md: N7E 28 pairs +129.3 s [+92.9, +165.7] against comm_ofi; none adopted | off | 0 |

Item 55 (commit 3ff1c22): gates PASS at 1 and 2 nodes with the switches off and on, comm_ofi 2 nodes PASS, 6-node 1e10
compare identical with `COMM_SHMEM_LAYV_ANY` off and on (m).

## Node-count results (items 57, 66, 67; m unless marked)

| item | nodes | digits | result |
|---|---|---|---|
| 57 (paired) | 6 | 3.8646e11 | SHMEM − libfabric +39.9 s [+2.5, +77.4] (dm +23, bs +13, recip +7, init +3.5) |
| 67 gap2 (4 rounds x 2 slots) | 2 | 1.2882e11 | SHMEM default D 120.5 s (n 4, sd 5.1, CV 4.2 %, 114.9-126.2) vs libfabric L 123.3 s (n 4, sd 8.1, CV 6.5 %, 117.1-135.2); paired D − L by round −17.5, +5.1, −5.0, +6.0 (mean −2.9 s): no gap, no stalls. Slot b ran about 8 s slower than slot a in both arms |
| 66 sn1 (six independent items, 3 runs each) | 1 | 6.441e10 | SHMEM default 65.9 s (n 18, sd 1.6, CV 2.5 %, 63.5-69.7; items 1-4 alone 66.3, sd 1.8, n 12): no stalls |
| 71 gap4 | 4 | pending | queued (first attempt 68/69 failed in 4-26 s on a driver check, not a code fault) |

Item 57 agrees with the steady gap above (about +36 to +40 s at 6 n). Observation (mod): the fitted per-node stall
rate below (p 0.41 per node per run, variant i) predicts at 2 nodes a stall in about 65 % of runs of 40+ s; item 67
(4 runs per arm) and item 66 (18 runs at 1 n) show none and no gap. So the stall and the gap depend on the node
count, not on per-node work alone. Item 71 (4 n) will place the onset. This is a finding, not a conclusion about cause.

Tooling: item 60 (hangtest) passed with the patched driver library (re-entrancy-guarded `nl_on_exit`: DONE, then rc 0,
52 s). Item 61 (hthp) stopped after its gates passed; the pb1 campaign (item 65) replaces it and is running.

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
Caveat: the model assumes the stall rate per node is node-count independent; the 1 n and 2 n results above do not
support that, so the 576-n excess is an upper-leaning estimate until item 71 places the onset (mod). Uncertainty: 24 runs
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

## Open questions and next experiments

- Phase B (variance, about 65 s on average): test the memory hypothesis. Experiment: 4 ABBA pairs at 6 n with
  `COMM_PHASE_TS=1`, A default vs B with transparent huge pages off for the node process only (proposed switch
  `ECALC_HOST_THP=0`, `prctl(PR_SET_THP_DISABLE)` before `mn_init()`; the proxies keep `COMM_SHMEM_PX_THP=1`), with
  per-node vmstat compaction/allocstall deltas, buddyinfo and a 1 s memory and GPU sampler.
- Phase C (steady gap, about 36 s): the bs and dm exchanges are 13 and 21 s slower than libfabric in the best runs.
  Candidates: the extra host copy of the proxy path against libfabric's direct device puts (zero-copy design, a), and
  the put chunk (`COMM_SHMEM_PX_BLOCK_MB` above 4, untimed). First step: a t_comm bandwidth probe of the proxy path
  against comm_ofi at 6 nodes, then a zero-copy design note before any code.
- Item 51's wall times in results/N7G.md (223, 264, 186, 235 s, from the timestamp lines) differ from the
  `summary.txt` totals used here (259.8, 291.2, 212.3, 275.2 s); the totals are the comparable figure.

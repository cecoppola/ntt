# TUNE17 — comm_ofi tuning: chunk size × writes in flight, and two NICs per device (Phase 17, aac7)

Times Eastern (aac7 logs are Pacific: +3 h). Numbers labelled **measured** / **modelled** / **assumed**.
Driver: `ecalc/tests/ofitune17_drive.sh` (fire-and-forget). Design of comm_ofi: docs/code/07_COMM_OFI.md.

## Why

comm_ofi's all-to-all holds 44–48 GB/s per node at 4–10 nodes (**measured**, results/OFI17.md §3), about 11–12 GB/s per NIC,
while one pair of NICs reaches 23.3 GB/s (**measured**, results/NIC16_experiments.md). The question: is the gap the knobs
(write size `COMM_OFI_CHUNK_MB`, default 4; writes in flight per NIC `COMM_OFI_WINDOW`, default 64) or the fabric
(many-to-many contention)? And does the two-NICs-per-device form (`COMM_OFI_NICS`, the target's `0,4;1,5;…`) work, and at what cost
on aac7, which has one NIC per APU (so the second NIC is a neighbor's, across NUMA)? No new switches; only the existing env knobs.

## Plan (what the driver runs, in order)

Gate: starts only after `~/ofimem17/R/STD17_DONE` exists (the timed 10-node STD17 production run in job 12287 — no fabric traffic
may overlap it), then waits ≤ 1 h for job 12287's other steps to end. If the marker is not there by **16:00 EDT** the verdict is
`SKIPPED`. It **always** writes `~/ofitune17/DONE` (first line = verdict). Never cancels 12287 / 12294.

| step | what | nodes |
|---|---|---|
| sw4 | `t_comm --bw 4 5 16` (all-to-all, 4 threads/node, slabs 64 KiB–16 MiB), `COMM_OFI_CHUNK_MB` ∈ {1, 4, 8} × `COMM_OFI_WINDOW` ∈ {16, 64, 128} (9 runs; 4 × 64 = the default) | first 4 of 12287 |
| sw10 | the best two of sw4 (mean per-node aggregate GB/s at the 16 MiB slab) and the default, same test | all 10 |
| nic2 | default NICs `--bw`; then for `COMM_OFI_NICS="0,1;1,2;2,3;3,0"` (A) and `"0,0;1,1;2,2;3,3"` (B, duplicate entries: two domains on one cxi): `t_comm` correctness (2 VERIFY OK), then `t_comm --bw 4 5 64` | first 2 |
| ec2 | `ecalc 2e10` on 2 nodes (ofi17's launch line, `COMM_OFI=1`), default NICs vs the better of A/B: VERIFY OK, parts' sha1 identical, top part identical to the `e_1e11.out` prefix (`~/ref`, else `~/ntt/ecalc/results`) | first 2 |

Every `--bw` run records per-PE per-thread and aggregate GB/s per slab (`<tag>.log`), the four cxi counters per node before/after
(`hni_sts_{tx,rx}_ok_octets`, `drive.log`), and a row in `bw.tsv`: tag, nodes, chunk, window, nics, rc, aggregate at the largest slab
(mean and min over PEs), per-thread mean, aggregate at 4 MiB.

Clone: `~/ofimem17` on aac7 at `main` (the build of `0c77c4c`; `0c77c4c..8e03ba0` and this commit change no code, so no rebuild —
the STD17 driver runs from the same clone's binaries).

## COLLECT

```
R="sshpass -p <cluster-password> ssh -o ConnectTimeout=20 chcoppola@aac7.amd.com"
$R 'head -1 ~/ofitune17/DONE; column -t -s$'"'"'\t'"'"' ~/ofitune17/bw.tsv; cat ~/ofitune17/walls.txt'
$R 'cut -c1-400 ~/ofitune17/drive.log | tail -120'
$R 'grep -E ": pe [0-9]+ .*16777216 B per slab" ~/ofitune17/sw4_*.log ~/ofitune17/sw10_*.log | cut -c1-260'
$R 'pgrep -af "^bash tests/ofitune17_drive.sh"; squeue -s -j 12287 -h -o "%i %j %M"'     # kill only by that PID
```
Re-arm (e.g. after `SKIPPED`), with a later deadline in epoch seconds:
```
$R 'cd ~/ofimem17/ecalc && TUNE17_DEADLINE=<epoch> setsid nohup bash tests/ofitune17_drive.sh 12287 > ~/ofitune17/driver.out 2>&1 < /dev/null &'
```
A subset: `TUNE17_STEPS=nic2,ec2` (steps sw4, sw10, nic2, ec2; sw10 without sw4 runs the default only).

## Results

(to be filled from the COLLECT output)

## RESUME

- Armed on the aac7 login node (see COLLECT for the PID check); the verdict lands in `~/ofitune17/DONE`.

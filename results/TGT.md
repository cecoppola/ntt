# TGT — target facts (user-supplied 2026-10-12) and what they change

**Question.** The user supplied facts about the target machine. Where do they go, which backlog rows (OPPORTUNITIES.md)
change, what does the digit write cost against the file system's measured rate, and does any planned launch value change?

**Change / branch.** Branch `tgt`. No code or default changed. Model constants only: `ecalc/mn_model.py` gains the
user-supplied target facts (`TARGET_NICS_PER_APU=2`, `TARGET_NIC_GBS=50`, so `TARGET_BW_APU` = 100 GB/s, the same value
as before; `TARGET_FS_SINGLE_FILE_GBS=1000` at `TARGET_FS_WRITERS_PER_NODE=32`, `TARGET_WRITE_BW_FS` = 1.736 GB/s per
node, now added to the write rates every target estimate prints). `TARGET_WRITE_BW` (1.0 GB/s per node, ecalc's writer)
is unchanged, so the standing estimate does not move. `profiles/target.conf.template`: the keys these facts determine are
filled. PLAN.md §1 lists what is known.

**Facts (user-supplied, a).** 576 nodes × 4 MI300A = 2304 APUs; 8 Slingshot NICs per node at 400 Gb/s (2 per APU,
100 GB/s injection peak per APU, 400 GB/s per node); Rocky Linux 9.6; Slurm 25.11.8; ROCm 7.14.0 (may be upgraded later);
cray-shmem 12.1.0; CCE 21.0.0; libfabric 2.3.1; file system ≈ 1 TB/s cumulative write into one file at 32 processes per
node. Compared with aac7 (m): CCE 21.0.0 and libfabric 2.3.1 are the same, Slurm is the same 25.11 series (25.11.4),
OpenSHMEMX is 11.8.0 there (11.7.4 the only other), and aac7 has one NIC per APU.

**Method.**
- ROCm 7.14.0 build: aac7 login node, a scratch clone of `main`, `module unload rocm/7.0.3; module load rocm/7.14.0
  cray-dsmml cray-openshmemx`, `make -B -j16 SHMEM_CRAY=1 GMP_HOME=~/gmp ecalc`, then `make all tests/t_comm tests/t_edge`.
- Write time: `ecalc/estimate.py --fabric target_l4 --g 576 --D 6.632e10 --write-bw W` for W = 0.434, 0.6, 1.0, 1.736,
  2.0 GB/s per node; `--bw 50` for one effective NIC per APU.
- Plan values: `tools/plan_values.py --profile <the template with the site keys filled by placeholders> 38200000000000
  576` on the aac7 login node (`~/ntt-acc` build), also with `COMM_OFI_NICS` unset and with `COMM_OFI=0`.
- ecalc's writer read from `ALGORITHM.md` §9 and `ecalc/mn_out.c`.

**Results.**

ROCm 7.14.0 (m, 2026-10-12): `ecalc` and `make all` (tests, tools) build with `-DCOMM_SHMEM`, linked against the 7.14.0
`libamdhip64`; with or without `-fPIC` the `ld.lld` relocation failure of `results/V16.md` does not reproduce. Not run on
a compute node: no digits and no time yet.

ecalc's writer today: one process per node; a background writer thread with 8 pwrite threads per 256 MB chunk
(`MN_OUT_THREADS=8`, `MN_OUT_CHUNK_MB=256`), O_DIRECT on Lustre (`ECALC_ODIRECT=auto`); two part files per node
(`MN_OUT_DKM_HI`: X_hi from the division's step 1, then X_lo), 1152 files in all; striping by the directory unless
`MN_OUT_STRIPE`. So 8 concurrent writers per node into per-node files, not 32 writers into one file.

| write rate per node | aggregate | raw write of 15.92 TB (27.63 GB per node) | wall with the write (no-write 185.5 s) | exposed write |
|---|---|---|---|---|
| 1.736 GB/s, the file system's 1 TB/s (u) | 1000 GB/s | 15.9 s | 186.5 s | 1.0 s (mod) |
| 1.0 GB/s, the model's prior for ecalc's writer (a) | 576 GB/s | 27.6 s | 198.3 s | 12.8 s (mod) |
| 0.6 GB/s, the old single-stream prior (a) | 346 GB/s | 46.1 s | 216.7 s | 31.2 s (mod) |
| 0.434 GB/s, 8 of 32 writers if the rate scales with writers (a) | 250 GB/s | 63.7 s | 234.3 s | 48.8 s (mod) |

Bytes: 3.82 × 10¹³ digits × 7.5 B / 18 digits = 15.92 TB (packed 60-bit form). The writer design gap is 11.8 s against the
1.0 GB/s prior and up to 47.8 s against the writer-scaled case (mod); new row N8.

Fabric: 100 GB/s per APU was the assumed design value and is now the user-supplied peak, so the estimate is unchanged
(185.5 s without the write at 3.82 × 10¹³ on 576 nodes in this run, mod). With one effective NIC per APU (50 GB/s): 222.7 s,
+37.2 s (mod). `comm_ofi` takes the cxi devices on each APU's NUMA node and stripes over them when `COMM_OFI_NICS` is unset,
so two NICs per APU are used without a setting; the template no longer sets `COMM_OFI_NICS`.

Plan values (m, planner on the aac7 login node): unchanged by these facts. `COMM_SHMEM_POOL_MB` 2304, heap 2816 MiB,
`FI_UNIVERSE_SIZE` 4096, `MN_GROUPS` 2,4,8,16,32,64,192,576, pool_log 31, `plan check` OK with 1236 products. With
`COMM_OFI=0` (SHMEM carries the device exchanges): pool 10240 MiB, heap 10752 MiB, plan check OK.

OPPORTUNITIES changes: M7c re-scored (the two-NIC path is the code's default; the open item is to verify it, +37 s at stake);
M7d re-worded (ROCm 7.14.0 builds; measure its speed on aac7, re-measure at target upgrades); M7b folded into N8's test;
N4 notes the Slurm series; new N7 (SHMEM as the primary transport, cray-shmem 12.1.0 untested) and N8 (writer design gap).
Top 5 for the target: M7c, N8, N7, N1, M7d.

**Verdict.** No default changes. Three items for the target work: verify two NICs per APU (M7c), benchmark ecalc's writer
shape against the file system's measured mode (N8), measure the SHMEM path and 12.1.0 (N7). One aac7 item is new: ROCm
7.14.0 against 7.2.4 at 1 node (M7d).

**Open issues.**
- The module name behind "cray-shmem 12.1.0" is assumed to be `cray-openshmemx/12.1.0`; the kit's auto-detection looks
  only for `cray-openshmemx/*`.
- `tools/plan_values.py --profile` refuses the template's meta keys `TARGET_PARTITION`, `TARGET_NODES` and `ECALC_OUT_DIR`
  ("unknown switch": no consumer in `ecalc/` or `tools/`); a filled `profiles/target.conf` cannot be passed to it until a
  script reads them or the loader accepts meta keys. The plan run above dropped those three lines.
- `estimate.py --target` still prints 3.71 × 10¹³ as "the target" (the size before 2026-10-12).

**RESUME.** Branch `tgt` merged to `main`. Next: the M7d ABBA on aac7 (1 node, `rocm/7.14.0` vs `rocm/7.2.4`), the N7
`COMM_OFI=0` A/B at 2–6 nodes, and for the target kit a `tools/wbench` run at scale with `-t 8` and `-t 32`.

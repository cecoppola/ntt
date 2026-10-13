# NIC — node screen catches nodes with fewer NICs than the launch asks for

Status: done 2026-10-13 (Eastern). Branch nicchk, merged to main.

## Problem (measured)
2026-10-13 12:55: three SHMEM-path runs (`COMM_OFI=0`, `SHMEM_OFI_NUM_NICS=4`) aborted within seconds (rc 141) on one node:
`LIBSMA ERROR: error in SHMEM_OFI_NUM_NICS parsing: An invalid number of nics 4 was found in "4". There is only 1 NIC available.`
Later runs on the same allocation passed (21 of 24). The node screen checked only times, not the NIC count.

## Change
`tools/nodescreen.sh` local stage prints `NS nics have=<n> want=<w>` per node: `have` = distinct `fi_info -p cxi` domains seen
by the runtime (after the same module load as the SHMEM launch); `want` = `--want-nics N`, default `$SHMEM_OFI_NUM_NICS`, 0 = report only.
`tools/nodescreen_tab.py` flags a node with `have < want` as a failed step, so it lands in EXCLUDE like any other bad node.
No new setting (an option of the tool). `ecalc/README.md` tool row updated.

## Verification (measured, 4 nodes, one queue item, local stage only)
| want | per-node `have` | verdict | rc |
|---|---|---|---|
| 4 | 4, 4, 4, 4 (`/sys/class/cxi` also 4 on each) | all ok, EXCLUDE empty | 0 |
| 8 (positive control) | 4, 4, 4, 4 | all four flagged `NICs 4 < 8 wanted` | 3 |

Not reproduced on a real 1-NIC node (none in the allocation); the flag path is exercised by the want=8 control.
The login node has no cxi device (count 0), so the check is only meaningful on compute nodes.

## RESUME
Use `tools/nodescreen.sh --hold JOB --want-nics 4` (or export `SHMEM_OFI_NUM_NICS=4`) before SHMEM runs that set 4 NICs.

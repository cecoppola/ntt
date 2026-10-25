#!/bin/bash
# prm05 - BS_ARENA_ROOM 0.12 / 0.14 / 0.16 and BS_SEED_FILL 128 vs 256 at 1e11 on 1 node (TQ48); one run per arm (long runs), so differences under the A/A spread (ntt03) are not conclusive.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 1
ARMV[R14]="BS_ARENA_ROOM=0.14"; ARMV[R12]="BS_ARENA_ROOM=0.12"; ARMV[R16]="BS_ARENA_ROOM=0.16"; ARMV[F256]="BS_ARENA_ROOM=0.14 BS_SEED_FILL=256"
D=$(fitd 1 D "$(dig 1e11)" BS_ARENA_ROOM=0.16)
for w in R12 R14 R16 F256; do ecalc_plan 1 "$D" D ${ARMV[$w]}; tgt_kv m.plan_dev_${w}_gb "$P_DEV_GB GB"; done
runseq D 1 "$D" "R14 R12 R16 F256" || true
arms_report R14
tgt_kv q.TQ48 "${AB_LINE:-no counted run} at $D digits (single runs)"
finish

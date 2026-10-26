#!/bin/bash
# prm09 - ECALC_PLANE_CAP 2^30 (B, the Q122 launch setting) against 2^31 (A, the code default) at 1 node(s), layout D, the campaign share (TQ82).
# aac7 1 n 8e9 (2026-10-25, m): total -2.64 s [-2.98, -2.30], init -2.70 s, compute +0.06 s [-0.07, +0.21], digits identical.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
ARMV[A]="ECALC_PLANE_CAP=2^31"; ARMV[B]="ECALC_PLANE_CAP=2^30"
need_path 1
ab_main D 1 "$(fitd 1 D "$(dig $TGT_SHARE 1)")" "A B B A A B" A TQ82

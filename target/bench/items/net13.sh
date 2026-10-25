#!/bin/bash
# net13 - SHMEM library NIC controls on 12.1 (TQ41): t_xshmem stream, path 2, 2 nodes; arms: defaults (SHMEM_ENV_DISPLAY), SHMEM_OFI_NIC_POLICY values, SHMEM_OFI_NUM_NICS values.
# An arm whose run fails is a result (value not accepted); the library reports in the raw logs.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 2
declare -A V=([def]="SHMEM_ENV_DISPLAY=1" [polblock]="SHMEM_OFI_NIC_POLICY=BLOCK" [polrr]="SHMEM_OFI_NIC_POLICY=ROUND_ROBIN" [polnuma]="SHMEM_OFI_NIC_POLICY=NUMA" [nic1]="SHMEM_OFI_NUM_NICS=1" [nic2]="SHMEM_OFI_NUM_NICS=2")
LINES=; i=0
for a in def polblock polrr polnuma nic1 nic2 def; do
    i=$((i + 1)); tleft_ok 0.85 || break
    t=xt_${a}_$i
    XT_TMO=4 xt_run "$t" 2 2 stream ${V[$a]} XT_MAXMB=64; rc=$?; xt_kv "$t" stream || true
    P=$(xt_peak "$t" stream); tgt_kv m.peak_${t}_node_gbs "${P:-NA} GB/s"; LINES="$LINES $a=${P:-fail(rc$rc)}"
done
tgt_kv q.TQ41 "stream peak node GB/s:$LINES"
OPT_NO_RUNS_OK=1; finish

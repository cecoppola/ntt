#!/bin/bash
# net07 - COMM_OFI_NICS arms on path 1 (TQ39): unset (NUMA-local), two NICs per APU, one NIC per APU; t_xshmem stream on 2 nodes and ecalc 1e10 (shape P).
# The unset arm passes COMM_OFI_NICS= (empty); check the raw logs that the launcher treated it as unset.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 1
ARMV[U]="COMM_OFI_NICS="; ARMV[T]="COMM_OFI_NICS=0,4;1,5;2,6;3,7"; ARMV[O]="COMM_OFI_NICS=0;1;2;3"
for a in U T O; do
    XT_TMO=5 xt_run "xt_$a" 1 2 stream ${ARMV[$a]} XT_MAXMB=64 || note "t_xshmem $a rc $?"; xt_kv "xt_$a" stream || true
    tgt_kv m.stream_peak_${a}_node_gbs "$(xt_peak xt_$a stream) GB/s"
done
runseq P 2 "$(dig 1e10 2)" "U T O O T U" || true
arms_report U
tgt_kv q.TQ39 "stream peak node GB/s U/T/O $(xt_peak xt_U stream)/$(xt_peak xt_T stream)/$(xt_peak xt_O stream); ecalc ${AB_LINE:-none}"
finish

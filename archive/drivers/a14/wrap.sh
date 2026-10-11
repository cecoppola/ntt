#!/bin/bash
# wrap.sh TAG cmd... : per node (one task per node): quick NIC MCU/fgfc snapshot, run cmd, snapshot again. Files in $SNAPDIR/TAG.{pre,post}.<host>
d=${SNAPDIR:-$HOME/sess1011c/a14/nic}; mkdir -p $d; t=$1; shift
snap() { { date +%s.%N; for c in 0 1 2 3; do for n in $(seq 10 40) ; do x=/sys/class/cxi/cxi$c/device/telemetry/oxe_mcu_meas_$n; [ -r $x ] && echo "cxi$c/oxe_mcu_meas_$n $(cat $x | cut -d@ -f1)"; done
  x=/sys/class/cxi/cxi$c/device/telemetry/oxe_stall_fgfc_start_0; echo "cxi$c/fgfc_start_0 $(cat $x | cut -d@ -f1)"; done; } > $d/$t.$1.$(hostname -s); }
snap pre; "$@"; rc=$?; snap post; exit $rc

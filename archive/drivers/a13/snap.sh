#!/bin/bash
# snap.sh TAG : on each node, dump Slingshot NIC telemetry + ethtool counters of cxi0-3/hsn0-3 to ~/sess1011c/a13/nic/TAG.<host>.txt
d=$HOME/sess1011c/a13/nic; mkdir -p $d; f=$d/$1.$(hostname -s).txt
{ date +%s.%N; for c in 0 1 2 3; do for x in /sys/class/cxi/cxi$c/device/telemetry/*; do echo "cxi$c/$(basename $x) $(cat $x 2>/dev/null | tr '\n' ' ')"; done
  ethtool -S hsn$c 2>/dev/null | sed "s/^ *\([a-z_0-9]*\): /hsn$c\/\1 /"; done; } > $f

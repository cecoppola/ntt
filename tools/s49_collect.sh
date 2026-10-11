#!/bin/bash
# T4 (S49) collection: fetch every NODECHK line (aac7 + aac6) and print the host table
# (host, MemAvailable, Cached, upload s per APU, flags).  Usage: bash tools/s49_collect.sh [> results/S49_table.txt]
# Flags: slowest-APU upload > 3x the median of that cluster's hosts, MemAvailable < 90 % of that median (kit rules);
# uses the LATEST line per host.  Also prints the driver markers.
P='sshpass -p <REMOVED> ssh -o ConnectTimeout=20'
get() { $P $1 'cd ~/batch1011/T4 && for f in out/*/logs/06_nodechk.log; do [ -f "$f" ] && grep -a "^NODECHK " "$f" | sed "s|^|'$1' $f |"; done; echo "--markers--"; cat DONE_adhoc DONE_aac6 2>/dev/null; tail -4 driver.log timer.log 2>/dev/null' </dev/null; }
{ get aac7; get aac6; } | python3 -I "$(dirname "$0")/s49_table.py"

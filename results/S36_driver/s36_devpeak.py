#!/usr/bin/env python3
# s36_devpeak.py <plain-log> - per-node device memory of one ecalc run from the MEM_REPORT_DEVS blocks: for each rank and phase the SUM over the
# node's 4 APUs of "(driver: X of Y GB used)" (the driver's view of the device, everything in it); the rank's peak is the max over phases.
# Prints one line: devmax=<GB, max over ranks> devmean=<GB, mean over ranks> pool=<GB, largest 'comm pool' of an APU line> vslot=<GB, largest hipMalloc'd-now v-slots of an APU line> ranks=<n>
# (devmax=NA when the log has no complete block.)
import sys, re
apu = {}; pool = 0.0; vsl = 0.0
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r"mem\[(\d+)\] \[(\S+)\]\s+APU(\d+): .*\(driver: ([0-9.]+) of", line)
    if not m: continue
    apu.setdefault((m.group(1), m.group(2)), {})[int(m.group(3))] = float(m.group(4))
    p = re.search(r"comm pool ([0-9.]+) GB", line); v = re.search(r"hipMalloc'd now ([0-9.]+)", line)
    if p: pool = max(pool, float(p.group(1)))
    if v: vsl = max(vsl, float(v.group(1)))
peak = {}
for (rk, ph), a in apu.items():
    if len(a) < 4: continue
    t = sum(a.values())
    if rk not in peak or t > peak[rk]: peak[rk] = t
if not peak: print("devmax=NA devmean=NA pool=%.2f vslot=%.2f ranks=0" % (pool, vsl)); sys.exit(0)
print("devmax=%.2f devmean=%.2f pool=%.2f vslot=%.2f ranks=%d" % (max(peak.values()), sum(peak.values()) / len(peak), pool, vsl, len(peak)))

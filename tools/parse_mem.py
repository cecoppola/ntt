#!/usr/bin/env python3
# parse_mem.py <logfile> - per-rank (node) max over the run of (sum of 4 APU "driver used" GB) + host RSS GB,
# from the same mem[<rank>] [<phase>] report block.  Prints one line per rank: "rank R host=<hostname|?> peak=<GB> phase=<phase>".
# Ported byte-for-byte from ~/b7v17/parse_mem.py on aac7 (read-only there) for s18w's rundriver.sh (rd_peaks).
import sys, re

def main(path):
    host_rss = {}
    apu_driver = {}
    rank_host = {}
    with open(path, 'r', errors='replace') as f:
        for line in f:
            m = re.match(r'aac7env: node (\S+) task (\d+) ', line)
            if m:
                rank_host[m.group(2)] = m.group(1)
                continue
            m = re.match(r'mem\[(\d+)\] \[(\S+)\] host ([0-9.]+) GB RSS', line)
            if m:
                host_rss[(m.group(1), m.group(2))] = float(m.group(3))
                continue
            m = re.match(r"mem\[(\d+)\] \[(\S+)\]\s+APU(\d+): .*\(driver: ([0-9.]+) of", line)
            if m:
                key = (m.group(1), m.group(2))
                apu_driver.setdefault(key, {})[int(m.group(3))] = float(m.group(4))
                continue
    node_peak = {}
    for key, apus in apu_driver.items():
        if len(apus) < 4:
            continue
        rank, phase = key
        if key not in host_rss:
            continue
        total = sum(apus.values()) + host_rss[key]
        if rank not in node_peak or total > node_peak[rank][0]:
            node_peak[rank] = (total, phase)
    if not node_peak:
        print("NO_MEM_REPORT_DEVS_DATA")
        return
    for rank in sorted(node_peak, key=int):
        total, phase = node_peak[rank]
        host = rank_host.get(rank, "?")
        print(f"rank {rank} host={host} peak={total:.2f}GB phase={phase}")

if __name__ == "__main__":
    main(sys.argv[1])

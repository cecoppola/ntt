#!/usr/bin/env python3
"""topo_check.py - launch-time switch-group check (report only; it never changes a run, the exit code is 0 unless the input is unusable).

  topo_check.py [--procs P] [--nodes-file F | --nodes a,b,c]  (no node option: names from stdin) [--group-size 64] [--topology FILE] [--sched 2,4,..]

The tree runs over node sets of the sizes of the schedule (rns_dist.c mn_groups_parse; this uses its mirror
mem_model.mn_groups, MN_GROUPS honoured), each set a block of consecutive ranks.  A set of up to one switch group
(default 64 nodes) is cheap only if all its nodes sit under one switch group.  The tool prints the rank -> switch-group map
(runs of ranks) and one WARN line for every set of up to the group size whose nodes span two groups.

Switch-group rule: default group = (node number - 1) div 64, the number being the trailing digits of the host name
(TOPO_GROUP_SIZE or --group-size changes the 64); --topology reads `scontrol show topology` text (SwitchName=.. Level=0
Nodes=host[001-064],..), the leaf switches then define the groups.  The node list is taken in rank order: the order the launcher
hands to srun (block distribution: rank r on node r div (P / nodes)).  Pure Python 3, standard library."""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def expand(expr):
    """host[001-003,7],other -> list of host names (one bracket per item)"""
    out = []
    for item in re.findall(r"[^,\[]+(?:\[[^\]]*\])?[^,\[]*", expr):
        m = re.match(r"^(.*?)\[([^\]]*)\](.*)$", item)
        if not m:
            out.append(item.strip())
            continue
        pre, body, post = m.groups()
        for part in body.split(","):
            a, _, b = part.partition("-")
            w = len(a)
            for v in range(int(a), int(b or a) + 1):
                out.append("%s%0*d%s" % (pre, w, v, post))
    return [h for h in out if h]


def leaf_map(text):
    """scontrol show topology text -> {host: leaf switch name} (Level=0 lines)"""
    g = {}
    for ln in text.splitlines():
        m = re.search(r"SwitchName=(\S+)\s+Level=0\b.*?\bNodes=(\S+)", ln)
        if m:
            for h in expand(m.group(2)):
                g[h] = m.group(1)
    return g


def group_of(host, size, leaves):
    if leaves is not None:
        return leaves.get(host, "?")
    m = re.search(r"(\d+)$", host)
    return "g%d" % ((int(m.group(1)) - 1) // size) if m else "?"


def schedule(p):
    import mem_model
    return mem_model.mn_groups(p, os.environ.get("MN_GROUPS") or None)


def check(nodes, procs=None, size=64, leaves=None, sched=None):
    """-> (lines to print, number of warnings).  nodes: host per node in rank order; procs: PEs (default len(nodes))"""
    p = procs or len(nodes)
    per = max(1, p // len(nodes)) if len(nodes) else 1
    pe = [nodes[min(r // per, len(nodes) - 1)] for r in range(p)]
    grp = [group_of(h, size, leaves) for h in pe]
    lines = ["topo_check: %d PEs on %d nodes, switch group %s" % (p, len(nodes), "from topology" if leaves is not None else "= (node number - 1) div %d" % size)]
    runs, a = [], 0
    for r in range(1, p + 1):
        if r == p or grp[r] != grp[a]:
            runs.append("%d-%d:%s" % (a, r - 1, grp[a]) if r - 1 > a else "%d:%s" % (a, grp[a]))
            a = r
    lines.append("topo_check: rank map " + " ".join(runs))
    warns = 0
    for s in (sched if sched is not None else schedule(p)):
        if s >= p or s > size:
            continue
        bad = [k for k in range(0, p, s) if len(set(grp[k:k + s])) > 1]
        if bad:
            warns += len(bad)
            lines.append("topo_check: WARN tree level %d: %d of %d sets span two switch groups (first at ranks %d-%d: %s)"
                         % (s, len(bad), p // s, bad[0], min(bad[0] + s, p) - 1, ",".join(sorted(set(grp[bad[0]:bad[0] + s])))))
    if not warns:
        lines.append("topo_check: OK, every tree set of up to %d ranks is inside one switch group" % size)
    return lines, warns


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--procs", type=int)
    ap.add_argument("--nodes-file")
    ap.add_argument("--nodes")
    ap.add_argument("--group-size", type=int, default=int(os.environ.get("TOPO_GROUP_SIZE") or 64))
    ap.add_argument("--topology", default=os.environ.get("TOPO_TOPOLOGY_FILE"))
    ap.add_argument("--sched")
    a = ap.parse_args(argv)
    if a.nodes:
        nodes = expand(a.nodes)
    elif a.nodes_file:
        nodes = open(a.nodes_file).read().split()
    else:
        nodes = sys.stdin.read().replace(",", " ").split()
    if not nodes:
        print("topo_check: empty node list", file=sys.stderr)
        return 1
    leaves = leaf_map(open(a.topology).read()) if a.topology else None
    sched = [int(x) for x in a.sched.split(",")] if a.sched else None
    lines, _ = check(nodes, a.procs, a.group_size, leaves, sched)
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())

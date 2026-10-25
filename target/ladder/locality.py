#!/usr/bin/env python3
"""locality.py - physical location of Slurm nodes and groups of physically adjacent nodes (target/FORMATS.md section 9).

  locality.py nodes [--from live|FILE] [--map FILE] [--save [PATH]] [--partition P]
        table of every node that has an HPE xname: host xname cab chassis slot blade node state partitions (stdout);
        the summary (nodes per chassis and cabinet, states, geometry) goes to stderr; --save writes the table to PATH
        (default results/probe/topology.tsv below the target/ folder)
  locality.py pick --size N [--count K] [--state idle|alloc|any] [--exclude FILE] [--partition P] [--only-hosts FILE] [--from ..] [--map FILE]
        K disjoint groups of N physically adjacent nodes, one group per line, comma separated, in physical order.
        Exit 3 if fewer than K groups exist (the groups found are still printed).  stderr: one info line per group and
        `cross=<unit>` when the group could not lie inside the smallest unit that holds N nodes.
  locality.py order HOSTLIST [--from ..] [--map FILE] [--lines]
        the hosts (a Slurm hostlist expression is accepted) sorted by physical location, comma separated

Physical order is (cabinet, chassis, slot, blade, node) from the xname x<cab>c<chassis>s<slot>b<blade>n<node>.  The xname is
taken from NodeName / NodeAddr / NodeHostName / Comment / Features of `scontrol show node -o`, or from a host->xname map file
(--map; two whitespace separated columns).  Units, smallest first: blade (node card) < slot < chassis < cabinet < system.
Input --from: `live` (default; one `scontrol show node -o` call), a file with that output (e.g. the probe's
raw/scontrol_nodes.txt), or a topology.tsv written by `nodes --save` (states are then as saved).

Pure Python 3.6+, standard library only.  Deterministic: the same input gives the same groups.
"""
import argparse
import os
import re
import subprocess
import sys

XNAME_RE = re.compile(r"x(\d+)c(\d+)s(\d+)b(\d+)n(\d+)")
UNITS = ("blade", "slot", "chassis", "cabinet", "system")
FIELDS = ("host", "xname", "cab", "chassis", "slot", "blade", "node", "state", "partitions")
DOWN_FLAGS = ("DOWN", "DRAIN", "DRAINING", "DRAINED", "FAIL", "FAILING", "NOT_RESPONDING", "INVALID_REG", "POWERED_DOWN",
              "POWERING_DOWN", "POWER_DOWN", "UNKNOWN", "FUTURE", "CLOUD", "MAINTENANCE", "REBOOT_REQUESTED", "REBOOT_ISSUED")
BUSY_BASE = ("ALLOCATED", "MIXED", "COMPLETING")


class Node(object):
    __slots__ = ("host", "xname", "cab", "chassis", "slot", "blade", "node", "state", "partitions")

    def __init__(self, host, xname, state="", partitions=""):
        self.host, self.xname, self.state, self.partitions = host, xname, state, partitions
        m = XNAME_RE.search(xname)
        self.cab, self.chassis, self.slot, self.blade, self.node = (int(g) for g in m.groups())

    def key(self):
        return (self.cab, self.chassis, self.slot, self.blade, self.node)

    def unit(self, level):      # id of the unit of this node at `level` (0 blade .. 4 system)
        return self.key()[:4 - level] if level < 4 else ()

    def flags(self):
        s = re.sub(r"[*~#!%$@^-]+$", "", self.state.upper())
        parts = s.split("+")
        return parts[0], set(parts[1:])

    def usable(self):
        base, fl = self.flags()
        return not (base in DOWN_FLAGS or fl & set(DOWN_FLAGS) or base == "")

    def idle(self):
        base, fl = self.flags()
        return self.usable() and base not in BUSY_BASE and base == "IDLE" and not (fl & {"RESERVED"})

    def busy(self):
        base, _ = self.flags()
        return self.usable() and base in BUSY_BASE


# ---------------------------------------------------------------------------------------------------- input
def kv(line, key):
    m = re.search(r"(?:^|\s)%s=(\S*)" % re.escape(key), line)
    return m.group(1) if m else ""


def read_map(path):
    m = {}
    if not path:
        return m
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if len(line) >= 2:
                m[line[0]] = line[1]
    return m


def parse_scontrol(text, hostmap=None):
    """Nodes from `scontrol show node -o` text.  Entries without an xname are skipped (counted in parse_scontrol.skipped)."""
    hostmap = hostmap or {}
    nodes, skipped = [], []
    for line in text.splitlines():
        if not line.startswith("NodeName="):
            continue
        host = kv(line, "NodeName")
        xn = hostmap.get(host, "")
        if not XNAME_RE.fullmatch(xn or ""):
            xn = ""
            for k in ("NodeName", "NodeAddr", "NodeHostName", "Comment", "ActiveFeatures", "AvailableFeatures"):
                m = XNAME_RE.search(kv(line, k))
                if m:
                    xn = m.group(0)
                    break
        if not xn:
            skipped.append(host)
            continue
        nodes.append(Node(host, xn, kv(line, "State"), kv(line, "Partitions")))
    parse_scontrol.skipped = skipped
    return nodes


def parse_tsv(text):
    nodes = []
    for line in text.splitlines():
        f = line.rstrip("\n").split("\t")
        if len(f) < 8 or f[0] == "host" or not XNAME_RE.fullmatch(f[1]):
            continue
        nodes.append(Node(f[0], f[1], f[7], f[8] if len(f) > 8 else ""))
    return nodes


def load_nodes(src, mapfile=None, topo_default=None):
    hostmap = read_map(mapfile)
    if src in (None, "", "live", "sinfo", "scontrol"):
        try:
            text = subprocess.check_output(["scontrol", "show", "node", "-o"], universal_newlines=True,
                                           stderr=subprocess.STDOUT, timeout=60)
        except (OSError, subprocess.SubprocessError) as e:
            if topo_default and os.path.exists(topo_default):
                sys.stderr.write("locality: scontrol failed (%s); using the saved %s\n" % (e, topo_default))
                src = topo_default
                text = None
            else:
                sys.exit("locality: scontrol show node failed (%s); give --from FILE" % e)
        else:
            return parse_scontrol(text, hostmap)
    with open(src) as f:
        text = f.read()
    if text.lstrip().startswith("host\t") or ("\t" in text.split("\n", 1)[0] and "NodeName=" not in text[:2000]):
        return parse_tsv(text)
    return parse_scontrol(text, hostmap)


def read_exclude(path):
    s = set()
    if path:
        with open(path) as f:
            for line in f:
                for t in re.split(r"[,\s]+", line.split("#", 1)[0]):
                    if t:
                        s.add(t)
    return s


# ---------------------------------------------------------------------------------------------------- hostlists
def _split_top(expr):
    out, depth, cur = [], 0, ""
    for ch in expr:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur:
        out.append(cur)
    return [x.strip() for x in out if x.strip()]


def _expand_one(term):
    m = re.search(r"\[([^\]]*)\]", term)
    if not m:
        return [term]
    pre, post = term[:m.start()], term[m.end():]
    vals = []
    for part in m.group(1).split(","):
        r = re.fullmatch(r"(\d+)(?:-(\d+))?", part.strip())
        if not r:
            raise ValueError("bad range %r in %r" % (part, term))
        a, b = r.group(1), r.group(2) or r.group(1)
        w = len(a) if a.startswith("0") and len(a) > 1 else 0
        vals += [str(i).zfill(w) for i in range(int(a), int(b) + 1)]
    res = []
    for v in vals:
        for tail in _expand_one(post):
            res.append(pre + v + tail)
    return res


def expand_hostlist(expr, use_scontrol=True):
    """Hostnames of a Slurm hostlist expression (scontrol show hostnames when available, else a small expander)."""
    expr = expr.strip()
    if not expr:
        return []
    if use_scontrol:
        try:
            out = subprocess.check_output(["scontrol", "show", "hostnames", expr], universal_newlines=True,
                                          stderr=subprocess.DEVNULL, timeout=20)
            hosts = out.split()
            if hosts:
                return hosts
        except (OSError, subprocess.SubprocessError):
            pass
    hosts = []
    for t in _split_top(expr):
        hosts += _expand_one(t)
    return hosts


# ---------------------------------------------------------------------------------------------------- picking
def smallest_unit_level(nodes, n):
    """Index into UNITS of the smallest unit that holds n of these nodes (system when none smaller does)."""
    for lv in range(4):
        cnt = {}
        for x in nodes:
            cnt[x.unit(lv)] = cnt.get(x.unit(lv), 0) + 1
        if max(cnt.values()) >= n:
            return lv
    return 4


def pick_groups(nodes, size, count=1, state="idle", exclude=(), partition=None):
    """-> (groups, notes).  groups: list of lists of Node (each in physical order); notes: one dict per group
    {unit, unit_id, level, ideal, cross, span}."""
    pool = [x for x in nodes if (not partition or partition in x.partitions.split(","))]
    pool.sort(key=Node.key)
    rank = {x.host: i for i, x in enumerate(pool)}           # position in the physical order of ALL known nodes
    allowed = {"idle": Node.idle, "alloc": Node.busy, "any": Node.usable}[state]
    avail = [x for x in pool if allowed(x) and x.host not in exclude]
    avail0 = list(avail)                                      # "fully usable" is judged before any group is taken
    ideal = smallest_unit_level(pool, size) if pool else 4
    groups, notes = [], []
    for _ in range(count):
        chosen = None
        for lv in range(ideal if ideal < 4 else 4, 5):
            units = {}
            for x in avail:
                units.setdefault(x.unit(lv), []).append(x)
            full, free0 = {}, {}
            for x in pool:
                full[x.unit(lv)] = full.get(x.unit(lv), 0) + 1
            for x in avail0:
                free0[x.unit(lv)] = free0.get(x.unit(lv), 0) + 1
            cands = [(u, m) for u, m in units.items() if len(m) >= size]
            if not cands:
                continue
            # a fully usable unit first, then the best fit (least left over), then the earliest in physical order
            cands.sort(key=lambda c: (0 if free0[c[0]] == full[c[0]] else 1, len(c[1]) - size, c[1][0].key()))
            u, members = cands[0]
            members = sorted(members, key=Node.key)
            best = None                                      # the window of `size` consecutive members with the smallest physical span
            for i in range(len(members) - size + 1):
                span = rank[members[i + size - 1].host] - rank[members[i].host]
                if best is None or span < best[0]:
                    best = (span, i)
            g = members[best[1]:best[1] + size]
            chosen = (g, {"unit": UNITS[lv], "unit_id": u, "level": lv, "ideal": ideal, "cross": lv > ideal,
                          "span": best[0] + 1 - size})
            break
        if chosen is None:
            break
        groups.append(chosen[0])
        notes.append(chosen[1])
        gone = set(x.host for x in chosen[0])
        avail = [x for x in avail if x.host not in gone]
    return groups, notes


def unit_label(note):
    u = note["unit_id"]
    if not u:
        return "system"
    names = "xcsb"
    ids = "".join("%s%d" % (names[i], v) for i, v in enumerate(u))
    return "%s %s" % (note["unit"], ids)


# ---------------------------------------------------------------------------------------------------- commands
def summary(nodes, skipped=()):
    lines = []
    st = {}
    for x in nodes:
        b, fl = x.flags()
        k = b + ("+" + "+".join(sorted(fl)) if fl else "")
        st[k] = st.get(k, 0) + 1
    lines.append("nodes with an xname: %d (skipped without: %d%s)" % (len(nodes), len(skipped),
                 (": " + ",".join(skipped[:6]) + (",..." if len(skipped) > 6 else "")) if skipped else ""))
    lines.append("states: " + " ".join("%s=%d" % kv_ for kv_ in sorted(st.items())))
    cabs, chs = {}, {}
    for x in nodes:
        cabs.setdefault(x.cab, []).append(x)
        chs.setdefault((x.cab, x.chassis), []).append(x)
    lines.append("cabinets: %d  chassis: %d" % (len(cabs), len(chs)))
    for c in sorted(cabs):
        lines.append("cabinet %d: %d nodes (%d idle, %d usable) in %d chassis" % (
            c, len(cabs[c]), sum(1 for x in cabs[c] if x.idle()), sum(1 for x in cabs[c] if x.usable()),
            len(set(x.chassis for x in cabs[c]))))
    for k in sorted(chs):
        m = chs[k]
        lines.append("chassis c%d/%d: %d nodes (%d idle, %d usable)" % (k[0], k[1], len(m), sum(1 for x in m if x.idle()),
                                                                       sum(1 for x in m if x.usable())))
    def per(keyf):
        d = {}
        for x in nodes:
            d.setdefault(keyf(x), 0)
            d[keyf(x)] += 1
        return (min(d.values()), max(d.values())) if d else (0, 0)
    pb, ps, pc = per(lambda x: x.key()[:4]), per(lambda x: x.key()[:3]), per(lambda x: x.key()[:2])
    lines.append("geometry (min-max nodes per unit): blade %d-%d, slot %d-%d, chassis %d-%d" % (pb + ps + pc))
    return lines


def cmd_nodes(a):
    topo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "results", "probe", "topology.tsv")
    nodes = load_nodes(a.src, a.map, topo)
    if a.partition:
        nodes = [x for x in nodes if a.partition in x.partitions.split(",")]
    nodes.sort(key=Node.key)
    rows = ["\t".join(FIELDS)] + ["\t".join(str(v) for v in (x.host, x.xname, x.cab, x.chassis, x.slot, x.blade, x.node,
                                                              x.state, x.partitions)) for x in nodes]
    sys.stdout.write("\n".join(rows) + "\n")
    skipped = getattr(parse_scontrol, "skipped", [])
    for l in summary(nodes, skipped):
        sys.stderr.write("locality: " + l + "\n")
    if a.save is not None:
        path = a.save or os.path.normpath(topo)
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w") as f:
            f.write("\n".join(rows) + "\n")
        sys.stderr.write("locality: saved %s\n" % path)
    return 0 if nodes else 1


def cmd_pick(a):
    topo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "results", "probe", "topology.tsv")
    nodes = load_nodes(a.src, a.map, topo)
    if not nodes:
        sys.stderr.write("locality: no node with an xname in the input\n")
        return 1
    if a.only_hosts:       # restrict the pool to these hosts (an allocation's nodes): one per line, comma or space separated
        keep = read_exclude(a.only_hosts)
        nodes = [x for x in nodes if x.host in keep]
        if not nodes:
            sys.stderr.write("locality: none of the --only-hosts has an xname in the input\n")
            return 1
    groups, notes = pick_groups(nodes, a.size, a.count, a.state, read_exclude(a.exclude), a.partition)
    for g, n in zip(groups, notes):
        sys.stdout.write(",".join(x.host for x in g) + "\n")
    for i, (g, n) in enumerate(zip(groups, notes), 1):
        sys.stderr.write("locality: group %d: %d nodes in %s, physical gap %d%s\n" % (
            i, len(g), unit_label(n), n["span"], ("  cross=%s" % n["unit"]) if n["cross"] else ""))
    if len(groups) < a.count:
        sys.stderr.write("locality: only %d of %d groups of %d (%s nodes%s) exist\n" % (
            len(groups), a.count, a.size, a.state, (" in partition " + a.partition) if a.partition else ""))
        return 3
    return 0


def cmd_order(a):
    topo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "results", "probe", "topology.tsv")
    hosts = expand_hostlist(" ".join(a.hostlist), not a.no_scontrol)
    by = {x.host: x for x in load_nodes(a.src, a.map, topo)}
    missing = [h for h in hosts if h not in by]
    if missing:
        sys.stderr.write("locality: no xname for %s; they go last in the given order\n" % ",".join(missing))
    known = sorted((by[h] for h in hosts if h in by), key=Node.key)
    out = [x.host for x in known] + missing
    sys.stdout.write(("\n" if a.lines else ",").join(out) + "\n")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog=__doc__)
    sub = p.add_subparsers(dest="cmd")
    sub.required = True

    def common(s):
        s.add_argument("--from", dest="src", default="live", help="live (scontrol), a scontrol -o file or a topology.tsv")
        s.add_argument("--map", help="host -> xname map file")
    s = sub.add_parser("nodes")
    common(s)
    s.add_argument("--save", nargs="?", const="", default=None, help="write the table (default results/probe/topology.tsv)")
    s.add_argument("--partition")
    s.set_defaults(fn=cmd_nodes)
    s = sub.add_parser("pick")
    common(s)
    s.add_argument("--size", type=int, required=True)
    s.add_argument("--count", type=int, default=1)
    s.add_argument("--state", choices=("idle", "alloc", "any"), default="idle")
    s.add_argument("--exclude", help="file with hosts to leave out (one per line or comma separated)")
    s.add_argument("--partition")
    s.add_argument("--only-hosts", help="file: use only these hosts (the nodes of an allocation; combine with --state any)")
    s.set_defaults(fn=cmd_pick)
    s = sub.add_parser("order")
    common(s)
    s.add_argument("hostlist", nargs="+")
    s.add_argument("--lines", action="store_true", help="one host per line")
    s.add_argument("--no-scontrol", action="store_true", help="expand the hostlist without scontrol")
    s.set_defaults(fn=cmd_order)
    a = p.parse_args(argv)
    if getattr(a, "size", 1) < 1 or getattr(a, "count", 1) < 1:
        p.error("--size and --count must be >= 1")
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())

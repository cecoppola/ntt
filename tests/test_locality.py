"""Unit tests of target/ladder/locality.py with synthetic topologies (names are built here, no real names).
Run: python3 -m unittest discover -s tests   (from the repository root), or python3 tests/test_locality.py"""
import importlib.util
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("locality", os.path.join(HERE, "..", "target", "ladder", "locality.py"))
loc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(loc)


def xname(cab, ch, sl, bl, nd):
    return f"x{cab}c{ch}s{sl}b{bl}n{nd}"


def synth(cabs=2, chassis=8, slots=8, blades=2, nodes=2, cab0=1000, states=None, host=lambda x: x, part="gpu"):
    """scontrol show node -o text of a regular system; states: {xname: State string}."""
    states = states or {}
    lines = []
    for c in range(cabs):
        for ch in range(chassis):
            for s in range(slots):
                for b in range(blades):
                    for n in range(nodes):
                        xn = xname(cab0 + c, ch, s, b, n)
                        lines.append(f"NodeName={host(xn)} Arch=x86_64 CPUTot=192 NodeAddr={xn} NodeHostName={xn} "
                                     f"OS=Linux 5.14 #1 SMP Fri Apr 4 RealMemory=500000 State={states.get(xn, 'IDLE')} "
                                     f"ThreadsPerCore=2 Partitions={part} BootTime=None")
    return "\n".join(lines) + "\n"


def run(argv):
    o, e = io.StringIO(), io.StringIO()
    with redirect_stdout(o), redirect_stderr(e):
        rc = loc.main(argv)
    return rc, o.getvalue(), e.getvalue()


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def write(self, name, text):
        p = os.path.join(self.tmp, name)
        with open(p, "w") as f:
            f.write(text)
        return p

    def groups(self, argv, text):
        rc, out, err = run(argv + ["--from", self.write("n.txt", text)])
        return rc, [l.split(",") for l in out.splitlines()], err


class TestParse(Base):
    def test_xname_and_fields(self):
        nodes = loc.parse_scontrol(synth(1, 1, 2, 2, 2))
        self.assertEqual(len(nodes), 8)
        x = nodes[5]
        self.assertEqual(x.key(), (1000, 0, 1, 0, 1))
        self.assertEqual(x.partitions, "gpu")

    def test_xname_from_nodeaddr_when_hostname_is_nid(self):
        t = synth(1, 1, 1, 2, 1, host=lambda x: "nid%06d" % (int(x[-4:].replace("b", "").replace("n", "")) + 1))
        nodes = loc.parse_scontrol(t)
        self.assertTrue(all(x.host.startswith("nid") for x in nodes))
        self.assertEqual(nodes[1].xname, xname(1000, 0, 0, 1, 0))

    def test_map_file(self):
        xn = xname(1004, 3, 5, 1, 0)
        t = f"NodeName=nid000001 NodeAddr=nid000001 State=IDLE Partitions=gpu\nNodeName=login1 State=IDLE\n"
        m = self.write("map", f"nid000001 {xn}\n")
        nodes = loc.parse_scontrol(t, loc.read_map(m))
        self.assertEqual([n.xname for n in nodes], [xn])
        self.assertEqual(loc.parse_scontrol.skipped, ["login1"])

    def test_state_flags(self):
        mk = lambda s: loc.Node("h", xname(1, 0, 0, 0, 0), s)
        self.assertTrue(mk("IDLE").idle())
        self.assertTrue(mk("IDLE+PLANNED").idle())
        self.assertFalse(mk("IDLE+DRAIN").usable())
        self.assertFalse(mk("DOWN*").usable())
        self.assertFalse(mk("IDLE+NOT_RESPONDING").usable())
        self.assertTrue(mk("MIXED").busy() and mk("MIXED").usable() and not mk("MIXED").idle())
        self.assertTrue(mk("ALLOCATED").busy())


class TestHostlist(unittest.TestCase):
    def test_expand(self):
        e = lambda s: loc.expand_hostlist(s, use_scontrol=False)
        self.assertEqual(e("n[1-3,7]"), ["n1", "n2", "n3", "n7"])
        self.assertEqual(e("n[08-10],m1"), ["n08", "n09", "n10", "m1"])
        self.assertEqual(e("a,b"), ["a", "b"])
        self.assertEqual(e("x[1-2]c[0-1]"), ["x1c0", "x1c1", "x2c0", "x2c1"])
        self.assertEqual(e(""), [])
        self.assertEqual(e("h[3]-p"), ["h3-p"])

    def test_order_cli(self):
        t = synth(1, 2, 2, 1, 1)
        d = tempfile.mkdtemp()
        f = os.path.join(d, "n.txt")
        with open(f, "w") as fh:
            fh.write(t)
        a, b, c, e = (xname(1000, 1, 1, 0, 0), xname(1000, 0, 0, 0, 0), xname(1000, 1, 0, 0, 0), xname(1000, 0, 1, 0, 0))
        rc, out, err = run(["order", "--no-scontrol", "--from", f, f"{a},{b},{c},{e}"])
        self.assertEqual(rc, 0)
        self.assertEqual(out.strip().split(","), [b, e, c, a])
        rc, out, err = run(["order", "--no-scontrol", "--from", f, "--lines", f"{a},{b}"])
        self.assertEqual(out.split(), [b, a])


class TestPick(Base):
    def setUp(self):
        super().setUp()
        self.t = synth()          # 2 cabinets x 8 chassis x 8 slots x 2 blades x 2 nodes = 512 nodes, 32 per chassis

    def check_adjacent(self, g, unit_len):
        keys = [loc.XNAME_RE.search(h).groups() for h in g]
        self.assertEqual(len(set(k[:unit_len] for k in keys)), 1, g)

    def test_sizes_inside_smallest_unit(self):
        # 1 node -> blade; 8 -> slot holds 4 so chassis; 32 -> chassis; 64 -> cabinet (256 nodes)
        for size, unit_len in ((1, 4), (2, 4), (4, 3), (8, 2), (32, 2), (64, 1), (256, 1)):
            rc, gs, err = self.groups(["pick", "--size", str(size), "--count", "1"], self.t)
            self.assertEqual(rc, 0)
            self.assertEqual(len(gs[0]), size)
            self.check_adjacent(gs[0], unit_len)
            self.assertNotIn("cross=", err)

    def test_size_equal_to_all(self):
        rc, gs, err = self.groups(["pick", "--size", "512"], self.t)
        self.assertEqual((rc, len(gs[0])), (0, 512))
        self.assertNotIn("cross=", err)

    def test_disjoint_and_physical_order(self):
        rc, gs, err = self.groups(["pick", "--size", "8", "--count", "6"], self.t)
        self.assertEqual(rc, 0)
        flat = [h for g in gs for h in g]
        self.assertEqual(len(flat), len(set(flat)))
        for g in gs:
            self.assertEqual(g, sorted(g, key=lambda h: tuple(int(v) for v in loc.XNAME_RE.search(h).groups())))
            self.check_adjacent(g, 2)
        # best fit: the six groups pack the first chassis (4 groups of 8) and then the next one
        self.assertEqual(len(set(loc.XNAME_RE.search(g[0]).groups()[:2] for g in gs)), 2)

    def test_deterministic(self):
        a = self.groups(["pick", "--size", "8", "--count", "3"], self.t)
        b = self.groups(["pick", "--size", "8", "--count", "3"], self.t)
        self.assertEqual(a[1], b[1])

    def test_prefers_fully_idle_unit(self):
        # the first chassis has one busy node: a group of 32 goes to the next, intact chassis
        bad = xname(1000, 0, 3, 1, 1)
        t = synth(states={bad: "ALLOCATED"})
        rc, gs, err = self.groups(["pick", "--size", "32"], t)
        self.assertEqual(rc, 0)
        self.assertTrue(all(h.startswith("x1000c1s") for h in gs[0]), gs[0][:2])
        self.assertNotIn("cross=", err)

    def test_group_of_8_prefers_intact_chassis(self):
        # slot 0 of chassis 0 busy (28 idle left there): the group of 8 goes to the intact chassis 1
        st = {xname(1000, 0, 0, b, n): "ALLOCATED" for b in range(2) for n in range(2)}
        rc, gs, err = self.groups(["pick", "--size", "8"], synth(states=st))
        self.assertTrue(all(h.startswith("x1000c1s") for h in gs[0]))
        # when every chassis has a busy slot, the best fit inside a chassis is used
        st = {xname(1000 + c, ch, 0, b, n): "ALLOCATED" for c in range(2) for ch in range(8) for b in range(2) for n in range(2)}
        rc, gs, err = self.groups(["pick", "--size", "8"], synth(states=st))
        self.assertTrue(all(h.startswith("x1000c0s") and not h.startswith("x1000c0s0b") for h in gs[0]), gs[0])
        self.assertNotIn("cross=", err)

    def test_down_nodes_cross_to_next_unit(self):
        # every chassis loses one node: no chassis holds 32 idle nodes -> the group of 32 crosses to the cabinet
        st = {xname(1000 + c, ch, 0, 0, 0): "DOWN" for c in range(2) for ch in range(8)}
        rc, gs, err = self.groups(["pick", "--size", "32"], synth(states=st))
        self.assertEqual(rc, 0)
        self.assertEqual(len(gs[0]), 32)
        self.assertIn("cross=cabinet", err)
        self.assertTrue(all(h not in st for h in gs[0]))

    def test_state_any_and_alloc(self):
        st = {xname(1000, 0, 0, 0, 0): "ALLOCATED", xname(1000, 0, 0, 0, 1): "MIXED"}
        t = synth(1, 1, 1, 1, 2, states=st)
        self.assertEqual(self.groups(["pick", "--size", "1", "--state", "idle"], t)[0], 3)
        rc, gs, _ = self.groups(["pick", "--size", "2", "--state", "alloc"], t)
        self.assertEqual((rc, len(gs[0])), (0, 2))
        rc, gs, _ = self.groups(["pick", "--size", "2", "--state", "any"], t)
        self.assertEqual((rc, len(gs[0])), (0, 2))

    def test_fewer_groups_exit_3(self):
        rc, gs, err = self.groups(["pick", "--size", "256", "--count", "3"], self.t)
        self.assertEqual(rc, 3)
        self.assertEqual(len(gs), 2)
        self.assertIn("only 2 of 3", err)
        rc, gs, err = self.groups(["pick", "--size", "513"], self.t)
        self.assertEqual((rc, gs), (3, []))

    def test_exclude_file_and_partition(self):
        ex = self.write("ex", f"# slow\n{xname(1000, 0, 0, 0, 0)},{xname(1000, 0, 0, 0, 1)}\n")
        rc, out, err = run(["pick", "--size", "4", "--exclude", ex, "--from", self.write("n.txt", self.t)])
        self.assertNotIn(xname(1000, 0, 0, 0, 0), out.split(","))
        self.assertTrue(out.startswith(xname(1000, 0, 1, 0, 0)), out)
        t2 = synth(1, 1, 1, 1, 2, part="a") + synth(1, 1, 1, 1, 2, cab0=2000, part="b")
        rc, gs, err = self.groups(["pick", "--size", "2", "--partition", "b"], t2)
        self.assertTrue(gs[0][0].startswith("x2000"))

    def test_odd_shape(self):
        # 3 chassis of 5 nodes (uneven slots), one node down: size 5 -> an intact chassis; size 7 -> two chassis, cross
        lines = []
        for ch, cnt in ((0, 5), (1, 5), (2, 4)):
            for i in range(cnt):
                lines.append(f"NodeName={xname(7, ch, i // 2, i % 2, 0)} State={'DOWN' if (ch, i) == (1, 2) else 'IDLE'} Partitions=p")
        t = "\n".join(lines) + "\n"
        rc, gs, err = self.groups(["pick", "--size", "5"], t)
        self.assertTrue(all(h.startswith(f"x7c0s") for h in gs[0]), gs)
        rc, gs, err = self.groups(["pick", "--size", "7"], t)
        self.assertEqual((rc, len(gs[0])), (0, 7))
        self.assertNotIn("cross=", err)         # no chassis holds 7 even when intact: the cabinet is the smallest unit
        # the 7 are consecutive in the physical order of the usable nodes
        self.assertEqual(sorted(gs[0], key=lambda h: tuple(int(v) for v in loc.XNAME_RE.search(h).groups())), gs[0])

    def test_window_with_hole_prefers_small_gap(self):
        # one chassis of 8 single nodes, node 3 down: size 3 -> nodes 0-2 (gap 0), not a window across the hole
        t = "\n".join(f"NodeName={xname(1, 0, i, 0, 0)} State={'DOWN' if i == 3 else 'IDLE'}" for i in range(8)) + "\n"
        rc, gs, err = self.groups(["pick", "--size", "3"], t)
        self.assertEqual(gs[0], [xname(1, 0, i, 0, 0) for i in range(3)])


class TestNodes(Base):
    def test_table_summary_save(self):
        t = synth(2, 2, 2, 2, 1, states={xname(1001, 1, 1, 1, 0): "DOWN"}) + "NodeName=login1 State=IDLE\n"
        f = self.write("n.txt", t)
        sv = os.path.join(self.tmp, "sub", "topology.tsv")
        rc, out, err = run(["nodes", "--from", f, "--save", sv])
        self.assertEqual(rc, 0)
        rows = out.splitlines()
        self.assertEqual(rows[0].split("\t"), list(loc.FIELDS))
        self.assertEqual(len(rows), 1 + 16)
        self.assertEqual(rows[1].split("\t")[1:7], [xname(1000, 0, 0, 0, 0), "1000", "0", "0", "0", "0"])
        self.assertIn("cabinets: 2  chassis: 4", err)
        self.assertIn("skipped without: 1", err)
        self.assertIn("DOWN=1", err)
        with open(sv) as fh:
            self.assertEqual(fh.read(), out)
        # the saved table is accepted as input
        rc, out2, _ = run(["pick", "--size", "4", "--from", sv])
        self.assertEqual(rc, 0)
        self.assertEqual(len(out2.strip().split(",")), 4)


if __name__ == "__main__":
    unittest.main()

"""Unit tests of ecalc/topo_check.py (the report-only switch-group check) with synthetic node lists.
Run: python3 -m unittest discover -s tests   (from the repository root)"""
import importlib.util
import io
import os
import random
import unittest
from contextlib import redirect_stdout
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("topo_check", os.path.join(HERE, "..", "ecalc", "topo_check.py"))
tc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tc)


def names(nums):
    return ["nd%03d" % i for i in nums]


class TopoCheck(unittest.TestCase):
    def test_contiguous_576_clean(self):
        lines, w = tc.check(names(range(1, 577)))
        self.assertEqual(w, 0)
        self.assertIn("0-63:g0", lines[1])
        self.assertIn("512-575:g8", lines[1])

    def test_one_node_excluded(self):
        lines, w = tc.check(names([i for i in range(1, 578) if i != 3]), 576)
        self.assertEqual(w, 9 * 6)                      # 9 sets at each of the levels 2, 4, 8, 16, 32, 64
        self.assertTrue(any("level 2:" in l and "ranks 62-63" in l for l in lines))
        self.assertTrue(any("level 64:" in l for l in lines))

    def test_shuffled(self):
        n = names(range(1, 577))
        random.Random(1).shuffle(n)
        lines, w = tc.check(n)
        self.assertGreater(w, 0)
        self.assertTrue(any("WARN tree level 2:" in l for l in lines))

    def test_ppn_blocks(self):                           # 8 PEs on 4 nodes: two per node, rank r on node r div 2
        _, w = tc.check(names([1, 2, 65, 66]), 8, 64, sched=[2, 4, 8])
        self.assertEqual(w, 0)                          # the 4-sets are nodes 1,2 (g0) and 65,66 (g1)
        _, w = tc.check(names([1, 65, 2, 66]), 4, 64, sched=[2, 4])
        self.assertEqual(w, 2)                           # level 2 sets {1,65} and {2,66}

    def test_topology_text_and_hostlist(self):
        self.assertEqual(tc.expand("nd[001-003,007],x9"), ["nd001", "nd002", "nd003", "nd007", "x9"])
        txt = "SwitchName=s0 Level=0 LinkSpeed=1 Nodes=nd[001-002]\nSwitchName=s1 Level=0 LinkSpeed=1 Nodes=nd[003-004]\n"
        lines, w = tc.check(names([1, 2, 3, 4]), leaves=tc.leaf_map(txt), sched=[2, 4])
        self.assertEqual(w, 0)
        lines, w = tc.check(names([1, 3, 2, 4]), leaves=tc.leaf_map(txt), sched=[2, 4])
        self.assertEqual(w, 2)

    def test_mn_groups_env(self):
        with mock.patch.dict(os.environ, {"MN_GROUPS": "2,128,576"}):
            lines, w = tc.check(names([i for i in range(1, 578) if i != 3]), 576)
        self.assertEqual(w, 9)                           # only level 2 is <= 64 (128 is above the group size)
        with mock.patch.dict(os.environ, {"MN_GROUPS": "3,2"}):
            lines, w = tc.check(names(range(1, 65)))     # invalid: one line, no exception
        self.assertTrue(any("not checked" in l for l in lines))

    def test_group_size_not_dividing(self):
        lines, w = tc.check(names(range(1, 25)), size=10, sched=[2, 4, 8])   # groups 1-10, 11-20, 21-24
        self.assertEqual(w, 3)                           # the 4-set at ranks 8-11 and the 8-sets at ranks 8-15, 16-23
        self.assertTrue(any("level 8:" in l and "ranks 8-15" in l for l in lines))

    def test_unknown_hosts_no_ok(self):
        lines, w = tc.check(["nd001", "gateway", "nd003", "nd004"], sched=[2, 4])
        self.assertTrue(any("unknown switch group for 1 hosts" in l for l in lines))
        self.assertFalse(any(" OK" in l for l in lines))
        txt = "SwitchName=s0 Level=0 Nodes=nd[001-002]\n"
        lines, _ = tc.check(names([1, 2, 3, 4]), leaves=tc.leaf_map(txt), sched=[2, 4])
        self.assertTrue(any("unknown switch group for 2 hosts" in l for l in lines))
        self.assertFalse(any(" OK" in l for l in lines))

    def test_multi_bracket_nodes(self):
        txt = "SwitchName=s0 Level=0 LinkSpeed=1 Nodes=x[1-3,5],y[7-8]\nSwitchName=top Level=1 Switches=s0\n"
        self.assertEqual(tc.leaf_map(txt), {"x1": "s0", "x2": "s0", "x3": "s0", "x5": "s0", "y7": "s0", "y8": "s0"})

    def test_sort_and_main_stdin(self):
        shuffled = names([66, 1, 65, 2])
        out = io.StringIO()
        with mock.patch("sys.stdin", io.StringIO("\n".join(shuffled))), redirect_stdout(out):
            self.assertEqual(tc.main(["--procs", "4", "--sched", "2,4", "--sort"]), 0)
        self.assertIn("OK", out.getvalue())              # sorted: g0 g0 g1 g1
        out = io.StringIO()
        with mock.patch("sys.stdin", io.StringIO("\n".join(shuffled))), redirect_stdout(out):
            tc.main(["--procs", "4", "--sched", "2,4"])
        self.assertIn("WARN tree level 2", out.getvalue())   # as given: no sort


if __name__ == "__main__":
    unittest.main()

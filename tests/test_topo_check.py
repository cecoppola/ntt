"""Unit tests of ecalc/topo_check.py (the report-only switch-group check) with synthetic node lists.
Run: python3 -m unittest discover -s tests   (from the repository root)"""
import importlib.util
import os
import random
import unittest

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


if __name__ == "__main__":
    unittest.main()

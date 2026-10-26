"""Unit tests of target/ladder/parse_run.py on synthetic ecalc logs (names built here)."""
import importlib.util
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("parse_run", os.path.join(HERE, "..", "target", "ladder", "parse_run.py"))
pr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pr)

HEAD = "27182818284590452353602874713526624977572470936999595749669676"


def log(nodes=2, verify="OK", total=100.0, dc=(1.0, 1.0), head=HEAD, host=lambda i: f"h{i}"):
    L = ["mnrun.sh: plan fit    FITS: 5e+10 digits on 2: device 301.90 / 373.44 GB (+71.54), node 412.48 / 420 GB (+7.52), DM_MN_WRAP=1 (auto)",
         "RESULT ecalc init s 10.5", "RESULT ecalc bs s 40.0", "RESULT ecalc dm s 30.0",
         "mem init            300.0    75.0 |   103.1     0.0     0.0   180.4     0.0     0.0     0.0   0.61 |    25.4    21.5     0.0     0.0     0.0     3.9    25.4",
         "mem bs              350.2    87.6 |   103.1     0.0     0.0   180.4     0.0    58.7   119.8   0.61 |     9.6     4.3     0.1     0.0     0.0     5.2    26.4"]
    for i in range(nodes):
        L.append(f"mn: {host(i)} {i}: dc {dc[i]:8.2f} s   digits [0, 5) of 5 formatted")
        L.append(f"mn: {host(i)} {i}: T1        0.01 s   ok")
        L.append(f"mn: {host(i)} {i}: digits: {head if i == 0 else '5' * 20}... (this node's range)")
        L.append(f"mn: {host(i)} {i}: VERIFY {verify}")
    L.append(f"mn: all {nodes} nodes: VERIFY {verify}")
    L.append(f"total {total:8.2f} s   (bs 40.0 + ...)")
    L.append(f"RESULT ecalc total s {total}")
    return "\n".join(L) + "\n"


class T(unittest.TestCase):
    def test_basic(self):
        r = pr.parse(log(), 2)
        self.assertEqual((r["total_s"], r["verify"], r["init_s"]), ("100.00", "OK", "10.50"))
        self.assertEqual((r["dev_peak_gb"], r["dev_peak_apu_gb"], r["host_hwm_gb"]), ("350.2", "87.6", "26.4"))
        self.assertEqual(r["plan_device_gb"], "301.90")
        self.assertTrue(r["e_prefix_check"].startswith("PASS"))
        self.assertEqual(r["slow_hosts"], "none")

    def test_hash_independent_of_host_names(self):
        a = pr.parse(log(host=lambda i: f"a{i}"), 2)["digits_hash"]
        b = pr.parse(log(host=lambda i: f"b{i}"), 2)["digits_hash"]
        self.assertEqual(a, b)
        self.assertNotEqual(a, pr.parse(log(head="2718281828459045235360287471352662497757247093699959574966967" + "1"), 2)["digits_hash"])

    def test_verify_fail_and_missing(self):
        self.assertEqual(pr.parse(log(verify="FAILED"), 2)["verify"], "FAIL")
        self.assertEqual(pr.parse("total 5 s\n", 2)["verify"], "NONE")

    def test_per_pe_phases_take_max(self):
        t = "RESULT ecalc bs s 40.0\nRESULT ecalc bs s 44.0\nRESULT ecalc bs s 41.0\nRESULT ecalc total s 100.0\nRESULT ecalc total s 120.0\n"
        r = pr.parse(t, 1)
        self.assertEqual(float(r["bs_s"]), 44.0)
        self.assertEqual(float(r["total_s"]), 100.0)

    def test_wrong_prefix(self):
        r = pr.parse(log(head="2718281828459045235360287471352662497757247093699959574966968"), 2)
        self.assertTrue(r["e_prefix_check"].startswith("FAIL"))

    def test_slow_node(self):
        r = pr.parse(log(nodes=4, dc=(1.0, 1.1, 9.0, 1.0)), 4)
        self.assertEqual(r["slow_hosts"], "h2")

    def test_pe_lines_map_to_nodes(self):
        t = "mn: pe 0 of 4 (node 0 of 2, local 0 of 2): x\nmn: pe 3 of 4 (node 1 of 2, local 1 of 2): x\n" \
            "mn: pe 0: dc   1.00 s   d\nmn: pe 3: dc   2.00 s   d\nmn: all 4 PEs: VERIFY OK\n"
        r = pr.parse(t, 2, ["ha", "hb"])
        self.assertEqual(r["dc_node_max_s"], "2.00 at hb")
        self.assertEqual(r["verify"], "OK")

    def test_collect(self):
        d = tempfile.mkdtemp()
        dirs = []
        for k, tot, h in ((1, 100.0, "aa"), (2, 104.0, "aa"), (3, 101.0, "bb")):
            p = os.path.join(d, f"r{k}")
            os.makedirs(p)
            with open(os.path.join(p, "summary.txt"), "w") as f:
                f.write(f"level=L0\nnodes=1\ndigits=7\np1r1.total_s={tot}\np1r1.verify=OK\np1r1.digits_hash={h}\np1r1.e_head=2718\n")
            dirs.append(p)
        lines, ok = pr.collect(dirs, 90.0, None)
        self.assertFalse(ok)                                   # the hashes differ
        self.assertIn("DIFFER", lines[0])
        self.assertIn("spread 3.9", lines[0])
        self.assertIn("measured/model 1.13", lines[0])
        lines, ok = pr.collect(dirs[:2])
        self.assertTrue(ok)


if __name__ == "__main__":
    unittest.main()

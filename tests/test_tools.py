"""Unit tests of the Python tools (stdlib unittest; numpy only for the trace_report tests, skipped without it).

    python3 -m unittest discover -s tests          # from the repository root; every test < 5 s
"""
import os, subprocess, sys, tempfile, shutil, unittest, importlib.util

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")


def run(script, *args, cwd=ROOT):
    return subprocess.run([sys.executable, os.path.join(TOOLS, script), *args], capture_output=True, text=True, cwd=cwd, timeout=60)


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(TOOLS, name + ".py"))
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod


try:
    import numpy  # noqa: F401
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False


_FIX = {}


def fixture():
    """the synthetic trace, written once per test run"""
    if not _FIX:
        tr = load("trace_report"); tmp = tempfile.mkdtemp(prefix="test_trace_")
        tr.synth(os.path.join(tmp, "run"), nodes=3, nex=30)
        _FIX.update(tr=tr, tmp=tmp, d=os.path.join(tmp, "run"))
    return _FIX


def tearDownModule():
    if _FIX:
        shutil.rmtree(_FIX["tmp"], ignore_errors=True)


class TraceReport(unittest.TestCase):
    def setUp(self):
        if not HAVE_NUMPY:
            self.skipTest("needs numpy")
        f = fixture(); self.tr, self.tmp, self.d = f["tr"], f["tmp"], f["d"]

    def test_header_and_load(self):
        run_ = self.tr.load_run(self.d, "syn")
        self.assertEqual(run_.nodes, 3)
        self.assertEqual(len(run_.nodeproc), 3)
        self.assertEqual(run_.files[0]["meta"]["rec_bytes"], 40)
        self.assertIn("E_BEGIN", run_.ev)

    def test_bad_magic(self):
        p = os.path.join(self.tmp, "bad"); os.makedirs(p)
        with open(os.path.join(p, "trace_pe0.bin"), "wb") as f:
            f.write(b"NOTATRACE" + b"\0" * 100)
        with self.assertRaises(SystemExit):
            self.tr.read_head(os.path.join(p, "trace_pe0.bin"))

    def test_selftest(self):
        r = run("trace_report.py", "--selftest")
        self.assertEqual(r.returncode, 0, r.stdout[-500:] + r.stderr[-500:]); self.assertIn("selftest: report", r.stdout)

    def test_node_metrics(self):
        run_ = self.tr.load_run(self.d, "syn")
        by = {h["meta"]["node"]: h for h in run_.nodeproc}
        m = self.tr.node_metrics(run_, by[0]); m2 = self.tr.node_metrics(run_, by[2])
        self.assertEqual(m["n_ex"], 30)                          # 30 exchanges, kind 5 = layeq
        self.assertEqual(m["n_layeq"], 30)
        self.assertGreater(m["ex"], 0); self.assertLessEqual(m["busy"], m["window"] + 1e-9)
        self.assertEqual(m["n_roff"], 60)                        # 2 peers x 30 exchanges
        self.assertGreater(m["rounds"], 0)
        self.assertEqual(m["gap_n"], 0)
        self.assertEqual(m2["gap_n"], 1); self.assertAlmostEqual(m2["gap_s"], 0.04, places=6)     # the synthetic 40 ms heartbeat gap and 30 ms watchdog firing of node 2
        self.assertEqual(m2["wd_n"], 1); self.assertAlmostEqual(m2["wd_max"], 0.03, places=6)

    def test_pair_repeated_keys(self):
        # a key that repeats (sequence restart): the k-th A pairs with the k-th B in time order
        np = numpy; R = np.zeros(4, self.tr.REC)
        R["ev"] = [1, 2, 1, 2]; R["t"] = [100, 130, 200, 250]; R["seq"] = 7
        A, d = self.tr._pair(R, 1, 2, self.tr._FX)
        self.assertEqual(list(d), [30, 50])

    def test_compare_cli(self):
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "trace_report.py"), "--compare", self.d, "--vs", self.d, self.d, "--label", "A", "B"],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("COMPARE  A (1 run", r.stdout); self.assertIn("B (2 runs", r.stdout)
        self.assertIn("exchange in flight", r.stdout)
        # identical sides: every B-A is 0
        line = next(l for l in r.stdout.split("\n") if "start-up" in l)
        self.assertIn("+0.0%", line)


class SwitchRegistry(unittest.TestCase):
    def test_check_on_repo(self):
        r = run("switch_registry.py", "--check")
        self.assertEqual(r.returncode, 0, r.stdout[-500:] + r.stderr[-500:])
        self.assertIn("0 problems", r.stdout)

    def test_parser_literal_defaults(self):
        sr = load("switch_registry")
        self.assertEqual(sr.clean_lit("8.0"), "8")
        self.assertEqual(sr.clean_lit("16UL"), "16")
        self.assertEqual(sr.clean_lit('"abc"'), "abc")
        self.assertEqual(sr.ternary_default(" 5 : 3;"), "3")
        self.assertEqual(sr.ternary_default(" a ? b : c : d;"), "derived")
        self.assertTrue(sr.in_comment(" * getenv(\"X\")", 5))
        self.assertTrue(sr.in_comment("int a; // getenv(\"X\")", 12))
        self.assertFalse(sr.in_comment("int a = getenv(\"X\");", 8))


class DocCheck(unittest.TestCase):
    def test_repo_clean(self):
        r = run("doccheck.py")
        self.assertEqual(r.returncode, 0, r.stdout[-800:])
        self.assertIn("0 problem(s)", r.stdout)

    def test_negative_cited_path(self):
        tmp = tempfile.mkdtemp(prefix="test_doccheck_")
        try:
            with open(os.path.join(tmp, "README.md"), "w") as f:
                f.write("Scope: test\nsee `tools/does_not_exist_zz.py`\n")
            r = run("doccheck.py", "--root", tmp)
            self.assertEqual(r.returncode, 1)
            self.assertIn("[path] cited path does not exist: tools/does_not_exist_zz.py", r.stdout)
            self.assertEqual(run("doccheck.py", "--root", tmp, "--warn").returncode, 0)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class ArenaTrim(unittest.TestCase):
    """mem_model.arena_trim (BS_ARENA_TRIM_GB's twin): off = identity, a trim subtracts, the clamp stops at max(v2, v3)"""
    def test_trim(self):
        sys.path.insert(0, os.path.join(ROOT, "ecalc")); import mem_model as M
        G = 10**9
        def need(t):
            L = dict(need_dev=100 * G, v2=60 * G, v3=50 * G); r = M.arena_trim(L, 10 * G, t); return r // G, L["trim"] // G
        self.assertEqual(need(0), (110, 0)); self.assertEqual(need(25), (85, 25)); self.assertEqual(need(99), (60, 50))


if __name__ == "__main__":
    unittest.main()

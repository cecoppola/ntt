"""Unit tests of the Python tools (stdlib unittest).

    python3 -m unittest discover -s tests          # from the repository root; every test < 5 s
"""
import os, subprocess, sys, tempfile, shutil, unittest, importlib.util

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")


def run(script, *args, cwd=ROOT):
    return subprocess.run([sys.executable, os.path.join(TOOLS, script), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, cwd=cwd, timeout=60)


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(TOOLS, name + ".py"))
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod


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

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


class SettingsGuard(unittest.TestCase):
    """ecalc/config_line.c: an unregistered ECALC_/XSHMEM_/... name in the environment is refused (rc 2)"""
    def test_guard(self):
        tmp = tempfile.mkdtemp(prefix="test_guard_")
        try:
            with open(os.path.join(tmp, "m.c"), "w") as f:
                f.write('#include "config_line.h"\nint main(int c, char **v){ if (c > 1) { cfg_settings(); return 0; } return cfg_guard() ? 2 : 0; }\n')
            exe = os.path.join(tmp, "m")
            e = os.path.join(ROOT, "ecalc")
            subprocess.check_call(["gcc", "-I" + e, "-o", exe, os.path.join(tmp, "m.c"), os.path.join(e, "config_line.c")])
            env = {"PATH": os.environ["PATH"]}
            self.assertEqual(subprocess.call([exe], env=env), 0)
            self.assertEqual(subprocess.call([exe], env=dict(env, ECALC_NP="3", MNRUN_PPN="1", PE_ENV="x")), 0)
            r = subprocess.run([exe], env=dict(env, ECALC_FOO="1"), stderr=subprocess.PIPE, universal_newlines=True)
            self.assertEqual(r.returncode, 2)
            self.assertIn("ECALC_FOO", r.stderr)
            for ok in ({"MN_MODEL_PROFILE": "x"}, {"MN_MODEL_ROUND_SYNC": "1"}, {"COMM_LAYER_VSLOT_MB_3": "1"}, {"COMM_ALLTOALLV_DEPTH": "2"}):
                self.assertEqual(subprocess.call([exe], env=dict(env, **ok)), 0, ok)      # tool names, a built name
            self.assertEqual(subprocess.call([exe], env=dict(env, COMM_OFI="1")), 2)      # a row with no reader is refused
            r = subprocess.run([exe, "s"], env=dict(env, ECALC_NP="3"), stdout=subprocess.PIPE, universal_newlines=True)
            self.assertRegex(r.stdout, r"ECALC_NP\s+\S+\s+3\n")
        finally:
            shutil.rmtree(tmp)


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


class NodeHealth(unittest.TestCase):
    """C4: tools/nodehealth.sh reads EDAC from a fake sysfs root; nodescreen_tab.py warns on ue > 0 and refuses (EXCLUDE) with --refuse-ue"""
    def fake(self, ue0, ce0, ue1, ce1):
        tmp = tempfile.mkdtemp(); self.addCleanup(shutil.rmtree, tmp, True)
        for i, (u, c) in enumerate(((ue0, ce0), (ue1, ce1))):
            d = os.path.join(tmp, "sys/devices/system/edac/mc/mc%d" % i); os.makedirs(d)
            open(os.path.join(d, "ue_count"), "w").write("%d\n" % u); open(os.path.join(d, "ce_count"), "w").write("%d\n" % c)
        return tmp

    def health(self, root):
        env = dict(os.environ, NODEHEALTH_SYSFS_ROOT=root)
        return subprocess.run(["bash", os.path.join(ROOT, "tools/nodehealth.sh")], capture_output=True, text=True, env=env).stdout

    def test_sums(self):
        l = self.health(self.fake(3, 5, 0, 2)); self.assertIn("ue=3 ce=7", l); self.assertIn("mc=mc0:3/5,mc1:0/2", l); self.assertIn("boot=", l)
        self.assertIn("ue=NA ce=NA", self.health(tempfile.mkdtemp()))

    def screen(self, line, refuse):
        out = tempfile.mkdtemp(); self.addCleanup(shutil.rmtree, out, True); os.makedirs(os.path.join(out, "local"))
        open(os.path.join(out, "nodes.txt"), "w").write("n1\n")
        open(os.path.join(out, "local/n1.log"), "w").write("NS host n1\n" + line + "\nNS step nodechk rc=0 wall=1.0\nNS done n1\n")
        a = [sys.executable, os.path.join(ROOT, "tools/nodescreen_tab.py"), "--out", out, "--nodes", os.path.join(out, "nodes.txt")] + (["--refuse-ue"] if refuse else [])
        r = subprocess.run(a, capture_output=True, text=True)
        return r.returncode, r.stdout, open(os.path.join(out, "EXCLUDE")).read()

    def test_refusal(self):
        h = self.health(self.fake(1, 0, 0, 0)).strip()
        rc, out, exc = self.screen(h, False); self.assertEqual((rc, exc), (0, "")); self.assertIn("WARN EDAC ue=1", out)
        rc, out, exc = self.screen(h, True); self.assertEqual((rc, exc), (3, "n1\n")); self.assertIn("EXCLUDE: EDAC ue=1", out)
        rc, out, exc = self.screen(self.health(self.fake(0, 9, 0, 0)).strip(), True); self.assertEqual((rc, exc), (0, "")); self.assertNotIn("WARN EDAC", out)


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

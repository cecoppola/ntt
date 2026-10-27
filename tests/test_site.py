"""Site-adaptation tests (stdlib unittest, no cluster, no compiler): MNRUN_GRES (ecalc/gres.sh), the Makefile's PMI / SHMEM / DSMML discovery
(`make -n` against fake install trees), gen_conf's output-directory pick.

    python3 -m unittest discover -s tests          # from the repository root
"""
import os, subprocess, sys, tempfile, shutil, stat, unittest, importlib.util

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sh(script, env=None, cwd=ROOT):
    e = dict(os.environ); e.pop("MNRUN_GRES", None); e.pop("MNRUN_GRES_RESOLVED", None)
    e.update(env or {})
    return subprocess.run(["bash", "-c", script], capture_output=True, text=True, env=e, cwd=cwd, timeout=60)


class Gres(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def fake(self, scontrol_out, sinfo_out):
        for name, out in (("scontrol", scontrol_out), ("sinfo", sinfo_out)):
            p = os.path.join(self.d, name)
            with open(p, "w") as f:
                f.write("#!/bin/sh\n" + ("echo '%s'\n" % out if out is not None else "exit 1\n"))
            os.chmod(p, 0o755)
        return {"PATH": self.d + ":" + os.environ["PATH"]}

    def on(self, env, node="n01", extra=""):
        r = sh('. ecalc/gres.sh; %s mn_gres_on %s && echo ON || echo OFF' % (extra, node), env)
        return r.stdout.strip()

    def test_listed_gres_keeps_options(self):
        self.assertEqual(self.on(self.fake("NodeName=n01 Gres=gpu:4(S:0-3) CPUTot=192", "gpu:4")), "ON")

    def test_no_gres_drops_options(self):
        self.assertEqual(self.on(self.fake("NodeName=n01 Gres=(null) CPUTot=192", "(null)")), "OFF")

    def test_partition_query_without_node(self):
        self.assertEqual(self.on(self.fake("", "(null)"), node=""), "OFF")
        self.assertEqual(self.on(self.fake("", "gpu:4"), node=""), "ON")

    def test_unknown_keeps_options(self):
        self.assertEqual(self.on(self.fake(None, None)), "ON")

    def test_switch_wins(self):
        env = self.fake("NodeName=n01 Gres=(null)", "(null)")
        self.assertEqual(self.on(dict(env, MNRUN_GRES="on")), "ON")
        env = self.fake("NodeName=n01 Gres=gpu:4", "gpu:4")
        self.assertEqual(self.on(dict(env, MNRUN_GRES="off")), "OFF")

    def test_cache_in_env(self):
        env = self.fake("NodeName=n01 Gres=(null)", "(null)")
        r = sh('. ecalc/gres.sh; mn_gres_on n01; echo "$MNRUN_GRES_RESOLVED"; PATH=/nonexistent:/usr/bin:/bin; mn_gres_on n01 && echo ON || echo OFF', env)
        self.assertEqual(r.stdout.split(), ["off", "OFF"])

    def test_strip(self):
        env = self.fake("NodeName=n01 Gres=(null)", "(null)")
        r = sh('. ecalc/gres.sh; mn_gres_strip "--exclusive -c 192 --gpus-per-node=4 --gpu-bind=none --gpus=4 -N2" n01', env)
        self.assertEqual(r.stdout.strip(), "--exclusive -c 192 -N2")
        env = self.fake("NodeName=n01 Gres=gpu:4", "gpu:4")
        r = sh('. ecalc/gres.sh; mn_gres_strip "--exclusive --gpus-per-node=4" n01', env)
        self.assertEqual(r.stdout.strip(), "--exclusive --gpus-per-node=4")


class MakefileSite(unittest.TestCase):
    def setUpClass(cls):
        cls.d = tempfile.mkdtemp()
        d = cls.d
        for sub in ("pmi/6.1.17/lib", "pmi/6.1.17/include", "sma/12.1.0/ofi/include", "sma/12.1.0/ofi/lib64", "ds/include", "ds/lib", "lf/include/rdma", "stale/include"):
            os.makedirs(os.path.join(d, sub))
        for f in ("pmi/6.1.17/lib/libpmi2.so", "pmi/6.1.17/include/pmi2.h", "sma/12.1.0/ofi/include/shmem.h", "lf/include/rdma/fabric.h"):
            open(os.path.join(d, f), "w").close()

    def tearDownClass(cls):
        shutil.rmtree(cls.d, ignore_errors=True)

    setUpClass = classmethod(setUpClass)
    tearDownClass = classmethod(tearDownClass)

    def setUp(self):
        if not shutil.which("make"):
            self.skipTest("no make")

    def line(self, *args, **env):
        e = dict(os.environ)
        for k in ("CRAY_PMI_PREFIX", "PMI_HOME", "PMI_LIBDIR", "CRAY_OPENSHMEMX_PREFIX", "CRAY_SHMEM_PREFIX", "CRAY_SHMEM_DIR", "CRAY_SMA_PREFIX", "SMA_HOME", "CRAY_DSMML_PREFIX", "CRAY_SMA", "CRAY_DSMML"):
            e.pop(k, None)
        e.update(env)
        r = subprocess.run(["make", "-n", "-B", *args, "tests/t_xshmem"], capture_output=True, text=True, env=e, cwd=os.path.join(ROOT, "ecalc"), timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr[-400:])
        return r.stdout.replace(self.d, "F")

    def test_cray_site_12_1(self):
        s = self.line("XSHMEM=cray", CRAY_PMI_PREFIX=self.d + "/pmi/6.1.17", CRAY_SHMEM_DIR=self.d + "/sma/12.1.0/ofi", CRAY_DSMML_PREFIX=self.d + "/ds")
        for w in ("-IF/sma/12.1.0/ofi/include", "-LF/sma/12.1.0/ofi/lib64", "-LF/pmi/6.1.17/lib", "-Wl,-rpath,F/pmi/6.1.17/lib", "-LF/ds/lib"):
            self.assertIn(w, s)
        self.assertNotIn("11.8.0", s)

    def test_openshmemx_prefix_wins(self):
        s = self.line("XSHMEM=cray", CRAY_OPENSHMEMX_PREFIX="/x/y", CRAY_SHMEM_DIR=self.d + "/sma/12.1.0/ofi")
        self.assertIn("-I/x/y/include", s)

    def test_ofi_pmi(self):
        s = self.line("XSHMEM=ofi", "OFI=1", "LIBFABRIC=" + self.d + "/lf", CRAY_PMI_PREFIX=self.d + "/pmi/6.1.17")
        self.assertIn("-IF/pmi/6.1.17/include", s)
        self.assertIn("-LF/pmi/6.1.17/lib -lpmi2 -Wl,-rpath,F/pmi/6.1.17/lib", s)

    def test_defaults_unchanged(self):
        """No site variable and no /opt/cray here: the old default link line."""
        if os.path.isdir("/opt/cray"):
            self.skipTest("a Cray tree exists here")
        s = self.line("XSHMEM=cray")
        self.assertIn("-L/opt/cray/pe/sma/11.8.0/ofi/sma/lib64 -L/opt/cray/pe/dsmml/0.3.1/dsmml/lib -L/opt/cray/pe/lib64 -lsma -lpmi -lpmi2 -ldsmml "
                      "-Wl,-rpath,/opt/cray/pe/sma/11.8.0/ofi/sma/lib64 -Wl,-rpath,/opt/cray/pe/dsmml/0.3.1/dsmml/lib -Wl,-rpath,/opt/cray/pe/lib64 -lpthread", s)


class GenConfOutDir(unittest.TestCase):
    def conf(self, facts):
        d = tempfile.mkdtemp()
        try:
            with open(os.path.join(d, "facts.tsv"), "w") as f:
                for k, v in facts.items():
                    f.write("%s\t%s\tt\n" % (k, v))
            out = os.path.join(d, "t.conf")
            r = subprocess.run([sys.executable, os.path.join(ROOT, "target/probe/gen_conf.py"), d, "-o", out], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stderr[-400:])
            with open(out) as fh:
                return {k.strip(): v.split("#")[0].strip() for k, _, v in (l.partition("=") for l in fh) if k.strip() and not k.startswith("#")}
        finally:
            shutil.rmtree(d, ignore_errors=True)

    FS = {"fs.mount.lus": "type=lustre mount=/lus size_kB=9000000000 avail_kB=8000000000",
          "fs.mount.nfs": "type=nfs mount=/nfsh size_kB=900000000 avail_kB=800000000"}

    def test_unwritable_root_falls_back_to_home(self):
        c = self.conf(dict(self.FS, **{"fs.writable_mounts": "none", "fs.dir.HOME": "/nfsh/u /dev/x nfs 1 2 3 4% /nfsh"}))
        self.assertEqual(c.get("ECALC_OUT_DIR"), "/nfsh/u/ecalc_out")

    def test_writable_mount_chosen(self):
        c = self.conf(dict(self.FS, **{"fs.writable_mounts": "/lus /nfsh"}))
        self.assertEqual(c.get("ECALC_OUT_DIR"), "/lus/ecalc_out")

    def test_scratch_below_unwritable_root(self):
        c = self.conf(dict(self.FS, **{"fs.writable_mounts": "/nfsh", "fs.dir.SCRATCH": "/lus/scratch/u x"}))
        self.assertEqual(c.get("ECALC_OUT_DIR"), "/lus/scratch/u/ecalc_out")

    def test_no_fact_unchanged(self):
        c = self.conf(dict(self.FS))
        self.assertEqual(c.get("ECALC_OUT_DIR"), "/lus/ecalc_out")


if __name__ == "__main__":
    unittest.main()

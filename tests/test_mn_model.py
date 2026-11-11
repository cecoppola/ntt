"""mn_model.py regression tests (stdlib unittest, no cluster): the existing profiles' estimate rows stay as they were (A3, 2026-10-27: only the
round-size default changed, which moves the memory columns only), and the target_fit profile reproduces the target's measured weak-scaling
phases (bs, dm at 2 / 64 / 576 nodes, 8e9 digits per node) within +-20 %.

    python3 -m unittest tests.test_mn_model        # from the repository root; a few seconds
"""
import json, os, subprocess, sys, unittest
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ECALC = os.path.join(ROOT, "ecalc")

# (target and target_l4 rows re-pinned 2026-11-01: the part-file write rate is 4.0 GB/s per node, was 1.0)
# Table rows (g = 2 and 64, 8e9 digits per node) printed by `mn_model.py estimate --fabric <profile> --g 2 64 --D 8e9` of main 460bed6 (XSHMEM_ROUND_MB=1024 assumed).
PINS = {'aac7': ['2    8.0e+09  1.600e+10  |     24.2    50.4    0.8 |    6.3   29.1    12 |    178     27      6    205 | 256.8 GB  32.1 GB   0.0 GB | yes / yes',
          '64   8.0e+09  5.120e+11  |     45.9    70.4    1.2 |   25.3   27.4    36 |    178     30      9    209 |   1.0 TB 125.3 GB   0.0 GB | yes / yes'],
 'aac7_l4': ['2    8.0e+09  1.600e+10  |     24.0    50.3    0.8 |    6.1   29.1    12 |    178     27      6    205 | 256.8 GB  32.1 GB   0.0 GB | yes / yes',
             '64   8.0e+09  5.120e+11  |     45.2    69.8    1.2 |   24.6   27.5    35 |    178     30      9    209 |   1.0 TB 125.3 GB   0.0 GB | yes / yes'],
 'target': ['2    8.0e+09  1.600e+10  |     20.5    20.0    0.3 |    0.7    2.4     3 |    178     27      6    205 | 256.8 GB  32.1 GB   0.0 GB | yes / yes',
            '64   8.0e+09  5.120e+11  |     25.8    25.3    0.4 |    2.5    2.4    10 |    178     30      9    209 |   1.0 TB 125.1 GB   0.0 GB | yes / yes'],
 'target_l4': ['2    8.0e+09  1.600e+10  |     20.5    20.0    0.3 |    0.7    2.4     3 |    178     27      6    205 | 256.8 GB  32.1 GB   0.0 GB | yes / yes',
               '64   8.0e+09  5.120e+11  |     25.8    25.3    0.4 |    2.5    2.4    10 |    178     30      9    209 |   1.0 TB 125.1 GB   0.0 GB | yes / yes']}

# the six measured phase times of the target (m, BS_SEED_THREADS=144, 8e9 digits per node): bs = leaf + tree levels, dm = reciprocal + division;
# totals without an output file (init, bs, dm, dc residue): 2 and 64 nodes measured, 576 nodes at the trend init (~162 s)
POINTS = {(2, "bs"): 4.6, (2, "dm"): 7.6, (64, "bs"): 23.7, (64, "dm"): 30.8, (576, "bs"): 56.5, (576, "dm"): 73.3}
TOTALS = {2: 39.1, 64: 84.6, 576: 162.0}

FIT_SCRIPT = r"""
import json, sys
sys.path.insert(0, %r)
import mn_model as M, mem_model
fab = M.apply_profile("target_fit")
d = M.Design(np=3, strategy="auto", cap=mem_model.CAPS["2^31"], chunk="both", depth=2, modmul=1, p15=True, round_mb=128, out_overlap="none", p15b=True,
             np_mn="auto", packed=True, p15c=True, arena_room=None, cache_fit=True)
out = {}
for g in (2, 64, 576):
    e = M.estimate(g, 8e9, "grid", None, fab, "model", design=d)
    out[g] = dict(bs=e["batch"] + e["top"] + e["levels"], dm=e["recip"] + e["div"], total=e["nowrite_s"])
print(json.dumps(out))
""" % ECALC


def rows(profile, extra):
    r = subprocess.run([sys.executable, os.path.join(ECALC, "mn_model.py"), "estimate", "--fabric", profile, "--g", "2", "64", "--D", "8e9"] + extra,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=600, cwd=ECALC)
    return [l.rstrip() for l in r.stdout.splitlines() if l.split()[:1] in (["2"], ["64"]) and "|" in l]


class ProfilesPinned(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with ThreadPoolExecutor(8) as ex:
            f1 = {p: ex.submit(rows, p, ["--round-mb", "1024"]) for p in PINS}
            f2 = {p: ex.submit(rows, p, []) for p in PINS}
            cls.old = {p: f.result() for p, f in f1.items()}
            cls.new = {p: f.result() for p, f in f2.items()}

    def test_rows_unchanged_with_the_old_round_size(self):
        for p in PINS:
            self.assertEqual(self.old[p], PINS[p], p)

    def test_default_round_size_moves_only_memory_columns(self):
        for p in PINS:
            self.assertEqual(len(self.new[p]), 2, p)
            for a, b in zip(self.new[p], PINS[p]):
                self.assertEqual(a.split("|")[:3], b.split("|")[:3], p)    # g, digits, the walls, the exposed times: the time model does not read round_mb
                self.assertEqual(a.split("|")[4:], b.split("|")[4:], p)    # the fabric volumes too


class TargetFit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        r = subprocess.run([sys.executable, "-c", FIT_SCRIPT], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=600, cwd=ECALC)
        assert r.returncode == 0, r.stderr[-2000:]
        cls.m = {int(k): v for k, v in json.loads(r.stdout.splitlines()[-1]).items()}

    def test_six_points_within_20_percent(self):
        for (g, k), meas in POINTS.items():
            self.assertLess(abs(self.m[g][k] / meas - 1), 0.20, (g, k, meas, self.m[g][k]))

    def test_totals_within_20_percent(self):
        for g, meas in TOTALS.items():
            self.assertLess(abs(self.m[g]["total"] / meas - 1), 0.20, (g, meas, self.m[g]["total"]))

    def test_other_profiles_do_not_see_the_sync_term(self):
        s = "import sys; sys.path.insert(0, %r); import mn_model as M; M.apply_profile('target_fit'); M.apply_profile('target_l4'); print(M.ROUND_SYNC, M.INIT_DOUBLING, M.DC_FMT_MN, M.NODE_SCALE, M.INIT_SCALE)" % ECALC
        r = subprocess.run([sys.executable, "-c", s], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=120)
        self.assertEqual(r.stdout.split(), ["0.0", "0.0", "0.061", "1.0", "1.0"], r.stderr[-500:])


OLD_SCRIPT = r"""
import sys
sys.path.insert(0, %r)
import mn_model as M
e = M.estimate(576, 3.4722e10, fabric=M.apply_profile("target_fit"))
print(round(e["nowrite_s"], 1), round(e["nic_bytes"] / 1e12, 2), round(e["init"], 1))
""" % ECALC


class V12(unittest.TestCase):
    """D2 (2026-11-01): the model recalibrated on the measured decomposition of the 2e13 run (576 nodes).  Pins say why."""
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, ECALC)
        import mn_model
        cls.M = mn_model

    def test_terms_close_to_the_measured_wall(self):
        # the six terms at 2e13 against the run's measured wall 206.27 s (an independent constant); the buckets close to 0.2 s in the run's own table, so 0.25 s
        e = self.M.v12_estimate(2e13)
        self.assertEqual(set(e["terms"]), {"init", "wire", "polls", "engine", "gpu", "host"})
        self.assertAlmostEqual(sum(e["terms"].values()), 206.27, delta=0.25)
        # the measured ratios the terms are built from: 165,300 rounds, 9.33 TB, per-round time, wire rate, the round-sizing rule (128 MiB / 575 = 233 KB)
        self.assertEqual(round(e["rounds"]), 165300)
        self.assertAlmostEqual(e["fabric_tb"], 9.33, places=2)
        self.assertAlmostEqual(e["round_s"] * 1e3, 0.355, places=3)
        self.assertAlmostEqual(e["wire_gbs"], 184.4, delta=0.1)
        self.assertAlmostEqual(self.M.xshmem_chunk_bytes(576) / 1e3, 233.0, delta=1.0)

    def test_other_sizes_within_twelve_percent(self):
        # NOT fitted: the 10-29 series and the second 2e13 run; 1.8e13 ran with other settings (diagnostic words) and is kept for that reason.  Excluded: the reference
        # run (2e13, 206.27, the calibration) and 3.6e13 (the fitted row).  Known residuals -11.6 % ... +8.0 %: linear scaling is too steep above 2e13, too flat below.
        rows = [r for r in self.M.v12_table() if r[0] != 3.6e13 and r[1] != 206.27]
        self.assertEqual(len(rows), 7)
        for d, meas, w, res in rows:
            self.assertLess(abs(res), 0.12, (d, meas, w))

    def test_write_term_off_is_unchanged_and_on_is_wall_plus_exposed(self):
        # default OFF: v12_estimate has no write key and keeps the 2e13 / 3.6e13 numbers; v12_write (MODELED, not measured) = wall + exposed
        e = self.M.v12_estimate(3.82e13)
        self.assertNotIn("wall_write_s", e)
        self.assertAlmostEqual(e["wall_s"], 415.7, delta=0.1)
        self.assertAlmostEqual(self.M.v12_estimate(2e13)["wall_s"], 206.1, delta=0.1)
        for bw in (1.736, 1.0, 0.6, 0.434):
            w = self.M.v12_write(3.82e13, bw)
            self.assertAlmostEqual(w["nowrite_s"], e["wall_s"], places=9)
            self.assertAlmostEqual(w["wall_write_s"], e["wall_s"] + w["exposed_s"], places=9)
            self.assertAlmostEqual(w["exposed_s"], max(w["raw_s"], w["fmt_s"]) - w["hidden_s"], places=9)
        raw = {bw: self.M.v12_write(3.82e13, bw) for bw in (1.736, 1.0, 0.6, 0.434)}
        self.assertAlmostEqual(raw[1.0]["raw_s"], 27.6, delta=0.05)          # 3.82e13 / 576 x 0.4167 B / 1 GB/s
        self.assertEqual(raw[1.0]["exposed_s"], 0.0)                         # hidden under the division (81.7 s x ~0.5)
        self.assertAlmostEqual(raw[0.6]["exposed_s"], 5.2, delta=0.1)
        self.assertAlmostEqual(raw[0.434]["exposed_s"], 22.8, delta=0.1)
        self.assertAlmostEqual(raw[0.434]["wall_write_s"], 438.6, delta=0.1)
        self.assertLessEqual(raw[1.736]["wall_write_s"], raw[0.434]["wall_write_s"])

    def test_phi_is_the_only_fit(self):
        # phi is solved on the 3.6e13 passes (mean of three plain runs); the pin keeps the fit visible: 0.34
        self.assertAlmostEqual(self.M.V12_PHI, 0.341, places=3)
        # piece index from results/MODEL.md section PIECES (1 / 2 / 1 up to 2.6e13; 1/2/2 at 2.7e13; 2/3/2 at 2.9e13; 2/4/2 at 3.6e13; 3/5/3 at 4.0e13); the form is an assumption (a)
        for d, want in ((2.6e13, 1.0), (2.7e13, 1.33), (2.9e13, 1.83), (3.6e13, 2.0), (4.0e13, 2.83)):
            self.assertAlmostEqual(self.M.v12_piece_index(d), want, places=2, msg=d)

    def test_monotone_and_576_only(self):
        w = [self.M.v12_estimate(d)["wall_s"] for d in (1e13, 2e13, 3e13, 3.6e13, 4e13)]
        self.assertEqual(w, sorted(w))
        with self.assertRaises(ValueError): self.M.v12_estimate(2e13, nodes=64)

    def test_risk_rows(self):
        r = self.M.v12_risk(3.6e13)
        self.assertEqual([(x[1], x[2]) for x in r], [(7, 3), (4, 0)])
        self.assertEqual(self.M.v12_risk(3e13), [])                        # no runs between 2.4e13 and 3.6e13
        self.assertEqual(self.M.v12_risk(4e13)[0][3], 1.0)

    def test_cli(self):
        r = subprocess.run([sys.executable, os.path.join(ECALC, "mn_model.py"), "v12", "--digits", "2e13"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr[-500:])
        self.assertIn("206.1", r.stdout)

    def test_old_target_fit_estimate_pinned_as_stale(self):
        # target_fit stays selectable and unchanged: 527.5 s / 6.72 TB / init 22.4 s for the 2e13 run that took 206.27 s with 9.33 TB and init 36.6 s (the D2 comparison)
        r = subprocess.run([sys.executable, "-c", OLD_SCRIPT], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=300, cwd=ECALC)
        self.assertEqual(r.stdout.split(), ["527.5", "6.72", "22.4"], r.stderr[-500:])


if __name__ == "__main__":
    unittest.main()

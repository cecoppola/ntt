"""mn_model.py regression tests (stdlib unittest, no cluster): the existing profiles' estimate rows stay as they were (A3, 2026-10-27: only the
round-size default changed, which moves the memory columns only), and the target_fit profile reproduces the target's measured weak-scaling
phases (bs, dm at 2 / 64 / 576 nodes, 8e9 digits per node) within +-30 %.

    python3 -m unittest tests.test_mn_model        # from the repository root; a few seconds
"""
import json, os, subprocess, sys, unittest
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ECALC = os.path.join(ROOT, "ecalc")

# Table rows (g = 2 and 64, 8e9 digits per node) printed by `mn_model.py estimate --fabric <profile> --g 2 64 --D 8e9` of main 460bed6 (XSHMEM_ROUND_MB=1024 assumed).
PINS = {'aac7': ['2    8.0e+09  1.600e+10  |     24.2    50.4    0.8 |    6.3   29.1    12 |    178     27      6    205 | 256.8 GB  32.1 GB   0.0 GB | yes / yes',
          '64   8.0e+09  5.120e+11  |     45.9    70.4    1.2 |   25.3   27.4    36 |    178     30      9    209 |   1.0 TB 125.3 GB   0.0 GB | yes / yes'],
 'aac7_l4': ['2    8.0e+09  1.600e+10  |     24.0    50.3    0.8 |    6.1   29.1    12 |    178     27      6    205 | 256.8 GB  32.1 GB   0.0 GB | yes / yes',
             '64   8.0e+09  5.120e+11  |     45.2    69.8    1.2 |   24.6   27.5    35 |    178     30      9    209 |   1.0 TB 125.3 GB   0.0 GB | yes / yes'],
 'target': ['2    8.0e+09  1.600e+10  |     20.5    22.5    0.4 |    0.7    4.9     3 |    178     27      6    205 | 256.8 GB  32.1 GB   0.0 GB | yes / yes',
            '64   8.0e+09  5.120e+11  |     25.8    27.6    0.5 |    2.5    4.7     9 |    178     30      9    209 |   1.0 TB 125.1 GB   0.0 GB | yes / yes'],
 'target_l4': ['2    8.0e+09  1.600e+10  |     20.5    22.5    0.4 |    0.7    4.9     3 |    178     27      6    205 | 256.8 GB  32.1 GB   0.0 GB | yes / yes',
               '64   8.0e+09  5.120e+11  |     25.8    27.6    0.5 |    2.5    4.7     9 |    178     30      9    209 |   1.0 TB 125.1 GB   0.0 GB | yes / yes']}

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
                       capture_output=True, text=True, timeout=600, cwd=ECALC)
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
        r = subprocess.run([sys.executable, "-c", FIT_SCRIPT], capture_output=True, text=True, timeout=600, cwd=ECALC)
        assert r.returncode == 0, r.stderr[-2000:]
        cls.m = {int(k): v for k, v in json.loads(r.stdout.splitlines()[-1]).items()}

    def test_six_points_within_30_percent(self):
        for (g, k), meas in POINTS.items():
            self.assertLess(abs(self.m[g][k] / meas - 1), 0.30, (g, k, meas, self.m[g][k]))

    def test_totals_within_30_percent(self):
        for g, meas in TOTALS.items():
            self.assertLess(abs(self.m[g]["total"] / meas - 1), 0.30, (g, meas, self.m[g]["total"]))

    def test_other_profiles_do_not_see_the_sync_term(self):
        s = "import sys; sys.path.insert(0, %r); import mn_model as M; M.apply_profile('target_fit'); M.apply_profile('target_l4'); print(M.ROUND_SYNC, M.INIT_DOUBLING, M.DC_FMT_MN)" % ECALC
        r = subprocess.run([sys.executable, "-c", s], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.stdout.split(), ["0.0", "0.0", "0.061"], r.stderr[-500:])


if __name__ == "__main__":
    unittest.main()

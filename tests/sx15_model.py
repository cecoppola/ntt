#!/usr/bin/env python3
"""sx15_model.py - agent SX (Phase 15 task 0a, results/SX15.md 4): the target's two walls with and without the division's last low
product (X_lo Q mod B^w, step 2 of NEWTON_DKM=1), on the standing estimate's defaults (estimate.py: B2 + DKM, ECALC_NP=auto, the cache at
the slots RNS_DIST_CACHE_FIT allows = 0, the packed part file from the division's hook, write at 1 GB/s unless --write-bw).

Variants (each a monkeypatch of mn_model.division_cost_dkm; mn_model.py itself is not changed):
  base      the model as it stands (step 2's low product + the corrections + R's residues; the writer hidden under them)
  skip      step 2's low product, its corrections and R's residues dropped; one small op for the certificate (the guard limb read);
            the early writer (hook before step 2's low product) then hides only what is left after the hook (~ nothing)
  skip+band skip, and step 1's X_hi Q mod B^w formed as the band [nq - 1 - s - 1, w) only (the part of R1 step 2 reads; a
            NOT BUILT idea, results/SX15.md 2.5)
  *_hi      the same with MN_MODEL_DKM_HI=1 (agent EW's X_hi writer, being built: the writer hides all of step 2)
Every number printed is modelled.
    ./tests/sx15_model.py [--T 5.167e13] [--g 576] [--write-bw 1.0]
"""
import argparse, os, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "..", "ecalc"))
import mn_model as M
import mem_model
import estimate as E

ORIG = M.division_cost_dkm
MODE = {"skip": False, "band": False}

def patched(fab, nq, dl, npn, g, rule, form, sn=None):
    rc, c, groups = ORIG(fab, nq, dl, npn, g, rule, form, sn)
    L = dict(M.DKM_LAST)
    if not MODE["skip"] and not MODE["band"]:
        return rc, c, groups
    if sn is None: sn = npn
    na = sn + dl; k = na - nq + 1; w = nq + 2
    s = min(k // 2, dl); k1 = k - s
    t = c.t; hide = L["hide"]; hide_hi = L["hide_hi"]; d_band = 0.0
    if MODE["band"]:                                                    # step 1's product as the band R1's top needs
        c2b = M.product_cost(fab, k1, nq, g, lowcut=max(0, nq - 1 - s - 1), highcut=w, form=form)
        d_band = L["c2"] - c2b.t
        t -= d_band
    if MODE["skip"]:
        rest = M.Cost(); rest.add(M.small_cost(fab, g, 6))
        cert = M.Cost(); cert.add(M.small_cost(fab, g, 1))              # the guard limb read, the certificate
        t = t - L["c4"] - rest.t + cert.t
        hide = cert.t                                                   # the hook is where the low product was: nothing left to hide under
        hide_hi = hide_hi - L["c4"] - rest.t + cert.t
    nc = M.Cost(); nc.add(c); nc.t = t
    M.DKM_LAST.update(hide=hide, hide_hi=hide_hi, total=t, c2_band_saved=d_band)
    return rc, nc, groups

M.division_cost_dkm = patched

def one(T, g, fab, design, skip, band, hi):
    MODE["skip"], MODE["band"] = skip, band
    M.DKM_HI = hi
    M._PC.clear()
    e = E.estimate(g, T / g, "grid", "2,4,8,16,32,64,192,576", fab, "model", staging="code", design=design)
    return e, dict(M.DKM_LAST)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--T", type=float, default=5.167e13)
    ap.add_argument("--g", type=int, default=576)
    ap.add_argument("--write-bw", type=float, default=M.TARGET_WRITE_BW)
    a = ap.parse_args()
    M.TWREC_G = True; M.CACHE_FORCE = None
    design = M.Design(np=3, strategy="auto", cap=mem_model.CAPS["2^31"], chunk="both", depth=2, modmul=1, chunk_mb=M.CHUNK_MB, p15=True, round_mb=1024,
                      out_overlap="none", p15b=True, np_mn="auto", packed=True, p15c=True, arena_room=None, cache_fit=True)
    fab = M.Fabric(M.TARGET.name, 100.0, 2e-6, group=64, layers=2, taper=1.0, write_bw=a.write_bw)
    print("sx15_model: T %.4g digits on %d nodes, part file at %.2f GB/s per node, NEWTON_DKM=%d (all numbers modelled)" % (a.T, a.g, a.write_bw, M.DKM))
    print("%-34s | %8s %8s | %8s %8s | %s" % ("variant", "no write", "write", "division", "hidden", "step products c1 / c2 / c3 / c4 (s)"))
    base = None
    for name, skip, band, hi in (("base (today)", False, False, False), ("skip X_lo Q", True, False, False), ("skip + band X_hi Q", True, True, False),
                                 ("base + X_hi writer (EW)", False, False, True), ("skip + X_hi writer", True, False, True), ("skip + band + X_hi writer", True, True, True)):
        e, L = one(a.T, a.g, fab, design, skip, band, hi)
        if base is None: base = e
        hid = L.get("hide_hi" if hi else "hide", 0.0) * e["div"] / L["total"] if L.get("total") else 0.0
        print("%-34s | %6.1f s %6.1f s | %6.1f s %6.1f s | %.1f / %.1f / %.1f / %.1f%s   (vs base: %+.1f / %+.1f s)" % (
            name, e["nowrite_s"], e["wall_s"], e["div"], hid, L["c1"], L["c2"], L["c3"], L["c4"],
            "  band saves %.1f of c2" % L["c2_band_saved"] if L.get("c2_band_saved") else "", e["nowrite_s"] - base["nowrite_s"], e["wall_s"] - base["wall_s"]))
    print("(c1 = X_hi's A mu, c2 = X_hi Q mod B^w, c3 = X_lo's A mu, c4 = X_lo Q mod B^w: the fabric model's raw costs; 'division' is scaled by the calibration)")

if __name__ == "__main__":
    main()

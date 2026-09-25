"""Figures for docs/PAPER.md.  Run: python3 docs/fig/make_figs.py  (matplotlib, numpy).
Every number plotted is either computed here from the stated formula or copied from RESULTS.md (cited in the paper)."""
import math, os
import numpy as np
import matplotlib
matplotlib.use("svg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, FancyArrowPatch

OUT = os.path.dirname(os.path.abspath(__file__))
SURF, INK, INK2, GRID, MUTED = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df", "#c9c8c2"
C1, C2, C3, C4 = "#2a78d6", "#eb6834", "#1baf7a", "#eda100"      # categorical slots 1-4, fixed order (validated)
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "text.color": INK, "axes.edgecolor": MUTED,
                     "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2, "axes.grid": True,
                     "grid.color": GRID, "grid.linewidth": 0.8, "axes.spines.top": False, "axes.spines.right": False,
                     "figure.facecolor": SURF, "axes.facecolor": SURF, "svg.fonttype": "none", "lines.linewidth": 2})


def save(fig, name):
    fig.savefig(os.path.join(OUT, name), bbox_inches="tight", facecolor=SURF)
    plt.close(fig)


# 1. binary-splitting tree: node width = log2 Q(a,b); every level sums to log2 N!
def fig_bs_tree(N=16):
    lg = lambda a, b: sum(math.log2(k) for k in range(a + 1, b + 1))
    total = lg(0, N)

    def split(a, b):                     # size-balanced split: equal log Q on both sides
        best, bm = None, a + 1
        for m in range(a + 1, b):
            d = abs(lg(a, m) - lg(m, b))
            if best is None or d < best: best, bm = d, m
        return bm
    levels, frontier = [], [(0, N)]
    while frontier:
        levels.append(frontier)
        nxt = []
        if all(b - a == 1 for a, b in frontier): break
        for a, b in frontier:
            if b - a > 1:
                m = split(a, b); nxt += [(a, m), (m, b)]
            else: nxt.append((a, b))
        frontier = nxt
    fig, ax = plt.subplots(figsize=(8.2, 3.4)); ax.grid(False)
    for depth, lev in enumerate(levels):
        x = 0.0
        for a, b in sorted(lev):
            w = lg(a, b)
            done = depth > 0 and b - a == 1 and (a, b) in levels[depth - 1]
            ax.add_patch(Rectangle((x + 0.15, -depth - 0.8), max(w - 0.3, 0.05), 0.6, color=MUTED if done else (C1 if depth % 2 == 0 else "#86b6ef"), lw=0))
            if w > 6: ax.text(x + w / 2, -depth - 0.5, f"({a},{b})", ha="center", va="center", fontsize=7.5, color="white" if depth % 2 == 0 else INK)
            x += w
        ax.text(total + 1, -depth - 0.5, f"level {depth}: {sum(1 for a, b in lev if not (depth and b - a == 1 and (a, b) in levels[depth - 1]))} new", va="center", fontsize=8.5, color=INK2)
    ax.set_xlim(0, total + 14); ax.set_ylim(-len(levels) - 0.1, 0.1); ax.set_yticks([])
    ax.set_xlabel(f"size in bits: box width = log₂ Q(a,b); gray = leaves finished at an earlier level; every level sums to log₂ {N}! = {total:.1f}")
    ax.set_title(f"Size-balanced binary splitting of Σ 1/k!, N = {N}", fontsize=10, loc="left")
    save(fig, "bs_tree.svg")


# 2. padding waste: smallest admissible length >= n, 2^k only vs {2^k, 3*2^k}
def fig_padding():
    n = np.unique(np.round(np.logspace(np.log10(1100), np.log10(66000), 3000)).astype(int))
    p2 = 2 ** np.ceil(np.log2(n))
    p3 = np.minimum(p2, 3 * 2 ** np.ceil(np.log2(n / 3)))
    fig, ax = plt.subplots(figsize=(8.2, 3.0))
    ax.plot(n, 100 * (p2 / n - 1), color=C2, label="lengths 2ᵏ only")
    ax.plot(n, 100 * (p3 / n - 1), color=C1, label="lengths 2ᵏ and 3·2ᵏ")
    ax.set_xscale("log", base=2); ax.set_ylabel("padding waste, %"); ax.set_xlabel("points needed by the product (log scale)")
    ax.set_title("waste: 2ᵏ only up to 100 % (mean 39 %); with 3·2ᵏ at most 50 % (mean 19 %)", fontsize=9.5, loc="left")
    ax.legend(frameon=False, loc="upper center", bbox_to_anchor=(0.5, -0.22), ncol=2); ax.set_ylim(0, 105)
    save(fig, "padding.svg")


# 3. DIF (Gentleman-Sande) signal-flow graph, n = 8: natural order in, bit-reversed out
def fig_dif():
    n, S = 8, 3
    fig, ax = plt.subplots(figsize=(8.2, 3.6)); ax.grid(False); ax.axis("off")
    for s in range(S):
        h = n >> (s + 1)
        for blk in range(0, n, 2 * h):
            for t in range(h):
                u, v = blk + t, blk + t + h
                for a, b in ((u, u), (u, v), (v, u), (v, v)):
                    ax.plot([s, s + 1], [-a, -b], color=C1 if a == b else "#86b6ef", lw=1.3, zorder=1)
                e = t * (n // (2 * h))
                ax.text(s + 0.93, -v + 0.18, f"ω{''.join('⁰¹²³⁴⁵⁶⁷'[int(c)] for c in str(e))}", fontsize=8, color=C2, ha="right")
    for s in range(S + 1):
        ax.scatter([s] * n, [-i for i in range(n)], s=18, color=INK, zorder=2)
    for i in range(n):
        ax.text(-0.12, -i, f"x[{i}]", ha="right", va="center", fontsize=9)
        r = int(f"{i:03b}"[::-1], 2)
        ax.text(S + 0.12, -i, f"X[{r}]", ha="left", va="center", fontsize=9)
    for s in range(S): ax.text(s + 0.5, 0.75, f"stage {s + 1}: span {n >> (s + 1)}", ha="center", fontsize=8.5, color=INK2)
    ax.text(1.5, -8.0, "butterfly (u, v) → (u + v, (u − v)·ω);  the output index is the input index bit-reversed", ha="center", fontsize=8.5, color=INK2)
    ax.set_xlim(-0.6, S + 0.6); ax.set_ylim(-8.3, 1.1)
    save(fig, "dif8.svg")


# 4. LDS bank-conflict class of a stride-16 column read, before/after the XOR swizzle
def fig_swizzle(c=5):
    t = np.arange(16); e = 16 * t + c
    before = e % 16; after = (e ^ ((e >> 4) & 15)) % 16
    fig, axs = plt.subplots(1, 2, figsize=(8.2, 2.8), sharey=True)
    for ax, y, ttl, col in ((axs[0], before, "plain index e: 16 threads → 1 class (16-way conflict)", C2),
                            (axs[1], after, "e ⊕ ((e ≫ 4) & 15): 16 threads → 16 classes", C1)):
        ax.scatter(t, y, s=40, color=col, zorder=3); ax.set_title(ttl, fontsize=9, loc="left")
        ax.set_xlabel("thread t (reads e = 16t + 5)"); ax.set_xticks(range(0, 16, 3)); ax.set_yticks(range(0, 16, 3))
    axs[0].set_ylabel("bank class (e mod 16)")
    save(fig, "swizzle.svg")


# 5. four-step ownership: R x C grid, point m = i + R j, rows owned by ranks -> block-cyclic limbs
def fig_fourstep(R=8, C=8, P=4):
    cols = [C1, C2, C3, C4]
    fig = plt.figure(figsize=(8.4, 3.9))
    ax = fig.add_axes([0.0, 0.08, 0.42, 0.84]); ax.axis("off")
    for i in range(R):
        for j in range(C):
            ax.add_patch(Rectangle((j, -i - 1), 0.94, 0.94, color=cols[i // (R // P)], lw=0, alpha=0.9))
            ax.text(j + 0.47, -i - 0.53, str(i + R * j), ha="center", va="center", fontsize=7.5, color="white")
    for i in range(0, R, R // P): ax.text(-0.2, -i - (R // P) / 2, f"rank {i // (R // P)}", ha="right", va="center", fontsize=8.5)
    ax.text(C / 2, 0.5, "row i, column j holds limb m = i + R·j", ha="center", fontsize=9)
    ax.text(C / 2, -R - 0.7, "① length-C transforms along rows (local)   ② twiddle ωₙ^(i·v)", ha="center", fontsize=8.5, color=INK2)
    ax.set_xlim(-2.2, C + 0.2); ax.set_ylim(-R - 1.1, 1.0)
    bx = fig.add_axes([0.47, 0.30, 0.52, 0.40]); bx.axis("off")
    n = R * C
    for m in range(n):
        i = m % R
        bx.add_patch(Rectangle((m, 0), 0.92, 1, color=cols[i // (R // P)], lw=0, alpha=0.9))
    for m in range(0, n, R): bx.text(m, -0.6, str(m), fontsize=7.5, color=INK2)
    bx.text(0, 1.6, "the same ownership along the integer's limbs 0 … 63:", fontsize=9)
    bx.text(0, -1.7, "each rank owns C runs of R/P limbs: block-cyclic, not contiguous.\n③ one all-to-all (transpose)   ④ length-R transforms along columns (local)",
            fontsize=8.5, color=INK2, va="top")
    bx.set_xlim(-0.5, n + 0.5); bx.set_ylim(-3.2, 2.2)
    save(fig, "fourstep.svg")


# 6. grid split: pieces in the (offset of A, offset of B) plane; low cut and high cut
def fig_grid():
    fig, axs = plt.subplots(1, 2, figsize=(8.4, 3.8))
    la, lb, ka, kb = 1.0, 1.0, 4, 3
    for ax, mode in zip(axs, ("band", "low")):
        ax.set_aspect("equal"); ax.grid(False)
        for i in range(ka):
            for l in range(kb):
                oa, ob = i * la, l * lb
                lo_end, lo_start = oa + ob + la + lb, oa + ob
                if mode == "band": skip = lo_end <= 2.0     # everything below the cut c = 2 is never read
                else: skip = lo_start >= 3.0                 # only the low w = 3 piece-units are needed
                ax.add_patch(Rectangle((oa + 0.04, ob + 0.04), la - 0.08, lb - 0.08, color=MUTED if skip else C1, lw=0))
                ax.text(oa + 0.5, ob + 0.5, "skip" if skip else f"A{i}·B{l}", ha="center", va="center", fontsize=8.5, color=INK2 if skip else "white")
        xs = np.linspace(0, ka, 50)
        if mode == "band":
            ax.plot(xs, 2.0 - xs, color=C2, lw=1.5)
            ax.set_title("high part only (Newton's u, corr; A_h·μ)\nline: the cut c; pieces wholly below it are skipped", fontsize=8.5, loc="left")
        else:
            ax.plot(xs, 3.0 - xs, color=C2, lw=1.5)
            ax.set_title("low part only (X·Q mod Bʷ, for the remainder)\nline: w; pieces starting at or above it are skipped", fontsize=8.5, loc="left")
        ax.set_xlim(0, ka); ax.set_ylim(0, kb); ax.set_xlabel("offset of the A piece (piece units)"); ax.set_ylabel("offset of the B piece")
    save(fig, "grid.svg")


# 7. Newton precision schedule: cost of power-of-two doubling vs k-anchored doubling, M(n) = n log2 n
def fig_newton(j0=4):
    M = lambda x: x * math.log2(max(x, 2))

    def doubling(k):
        j, c = j0, 0.0
        while j < k:
            jn = 2 * j; c += M(jn); j = jn        # the last step is computed at 2j even when k < 2j
        return c

    def anchored(k):
        j, c = j0, 0.0
        while j < k:
            jn = k
            while (jn + 1) // 2 > j: jn = (jn + 1) // 2
            c += M(jn); j = jn
        return c
    ks = np.arange(40, 4200)
    r = [doubling(k) / anchored(k) for k in ks]
    fig, ax = plt.subplots(figsize=(8.2, 2.9))
    ax.plot(ks, r, color=C1, lw=1.5); ax.set_xscale("log", base=2)
    ax.set_xlabel("target precision k (limbs), log scale"); ax.set_ylabel("cost ratio\ndoubling / anchored")
    ax.set_ylim(0.95, 2.05); ax.axhline(1, color=MUTED, lw=1)
    ax.set_title("just above a power of two, plain doubling computes the last step at almost twice the precision needed", fontsize=9, loc="left")
    save(fig, "newton.svg")


# 8. FP64 error-free product: hi = fl(ab) holds the top 53 bits, lo = fma(a, b, -hi) the rest
def fig_twoprod():
    fig, ax = plt.subplots(figsize=(8.2, 1.9)); ax.grid(False); ax.axis("off")
    ax.add_patch(Rectangle((0, 1.2), 105, 0.8, color=GRID, lw=0)); ax.text(52.5, 1.6, "exact product a·b: up to 105 bits (a < 2p, b < p, p < 2⁵²)", ha="center", va="center", fontsize=9)
    ax.add_patch(Rectangle((0, 0), 53, 0.8, color=C1, lw=0)); ax.text(26.5, 0.4, "hi = fl(a·b): 53-bit significand", ha="center", va="center", fontsize=9, color="white")
    ax.add_patch(Rectangle((53, 0), 52, 0.8, color=C2, lw=0)); ax.text(79, 0.4, "lo = fma(a, b, −hi): the exact rounding error", ha="center", va="center", fontsize=9, color="white")
    ax.text(0, -0.5, "most significant", fontsize=8, color=INK2); ax.text(105, -0.5, "least significant", fontsize=8, color=INK2, ha="right")
    ax.set_xlim(-1, 106); ax.set_ylim(-0.8, 2.2)
    save(fig, "twoprod.svg")


# 9. 4e10 digits on one node: phase times through the project (RESULTS §63, §80)
def fig_phases():
    stages = ["Phase 4\n(paper reproduced)", "binary final\n(LIMB_BASE=2)", "decimal final\n(Phase 8)", "Phase 13c\ndefaults"]
    bs = [74.7, 43.2, 61.1, 23.7]; dm = [51.1, 26.6, 37.0, 22.5]; dc = [82.5, 79.3, 4.2, 0.0]
    oth = [9.3 + 7.3, 12.2 + 5.7, 1.7 + 4.9, 0.1]
    fig, ax = plt.subplots(figsize=(8.2, 3.6)); ax.grid(axis="x", visible=False)
    x = np.arange(len(stages)); bottom = np.zeros(len(stages))
    for vals, name, col in ((bs, "bs (binary splitting)", C1), (dm, "dm (division)", C2), (dc, "dc (radix conversion)", C3), (oth, "10dP + T1 + T2", C4)):
        v = np.array(vals); ax.bar(x, v, bottom=bottom, color=col, width=0.55, label=name, edgecolor=SURF, linewidth=2)
        for xi, (b, h) in enumerate(zip(bottom, v)):
            if h >= 12: ax.text(xi, b + h / 2, f"{h:.1f}", ha="center", va="center", fontsize=8, color="white" if col in (C1, C2) else INK)
        bottom += v
    for xi, tot in enumerate(bottom): ax.text(xi, tot + 3, f"{tot:.1f} s", ha="center", fontsize=9)
    ax.set_xticks(x, stages, fontsize=8.5); ax.set_ylabel("seconds (sum of phases)"); ax.set_ylim(0, 245)
    ax.legend(frameon=False, fontsize=8.5, loc="upper right")
    save(fig, "phases.svg")


# 10. register blocking: a DIF stage acts on one index bit; groups of 3 bits live in one thread's registers
def fig_regblock():
    fig, ax = plt.subplots(figsize=(8.2, 2.5)); ax.grid(False); ax.axis("off")
    groups = [(6, 5, 4), (3, 2, 1), (0,)]
    cols = [C1, C2, C3]
    for g, (bits, col) in enumerate(zip(groups, cols)):
        for b in bits:
            x = 6 - b
            ax.add_patch(Rectangle((x * 1.1, 0), 1.0, 1.0, color=col, lw=0))
            ax.text(x * 1.1 + 0.5, 0.5, f"b{b}", ha="center", va="center", color="white" if col != C3 else INK, fontsize=10)
            ax.text(x * 1.1 + 0.5, 1.25, f"stage {7 - b}", ha="center", fontsize=8, color=INK2)
    ax.text(1.1 * 1.5, -0.45, "A: 3 stages, registers", ha="center", fontsize=8.5, color=INK2)
    ax.text(1.1 * 4.5, -0.45, "B: 3 stages, registers", ha="center", fontsize=8.5, color=INK2)
    ax.text(1.1 * 6 + 0.5, -0.45, "C: 1 stage", ha="center", fontsize=8.5, color=INK2)
    for xb in (3, 6):
        ax.annotate("", xy=(xb * 1.1 - 0.05, 1.9), xytext=(xb * 1.1 - 0.05, 0.0), arrowprops=dict(arrowstyle="-", color=INK2, lw=1.2, ls="--"))
        ax.text(xb * 1.1 - 0.05, 2.05, "LDS exchange", ha="center", fontsize=8, color=INK2)
    ax.text(8.6, 0.5, "index m = (b6 b5 b4 b3 b2 b1 b0)₂\nDIF stage s pairs m with m ⊕ 2^(7−s)", fontsize=8.5, va="center")
    ax.set_xlim(-0.2, 13.2); ax.set_ylim(-0.8, 2.4)
    save(fig, "regblock.svg")


if __name__ == "__main__":
    for f in (fig_bs_tree, fig_padding, fig_dif, fig_swizzle, fig_fourstep, fig_grid, fig_newton, fig_twoprod, fig_phases, fig_regblock):
        f()
    print("ok")

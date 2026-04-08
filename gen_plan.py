#!/usr/bin/env python3
"""Generate PLAN.md with ANSI color and box-drawing formatting."""
import sys

E = chr(27)
RST  = f"{E}[0m"
WHT  = f"{E}[1;37m"   # bold white   — title, separators
MAG  = f"{E}[1;35m"   # bold magenta — segment headers
CYN  = f"{E}[1;36m"   # bold cyan    — table headers, subsections
GRN  = f"{E}[1;32m"   # bold green   — easy difficulty, good values
YLW  = f"{E}[1;33m"   # bold yellow  — moderate, instructions
RED  = f"{E}[1;31m"   # bold red     — hard difficulty

SEP  = WHT + "═" * 118 + RST

def diff(n):
    """Return colored difficulty dots string (n filled out of 5)."""
    filled = f"{GRN}●{RST}" * min(n, 2)
    if n == 3: filled = f"{GRN}●●{RST}{YLW}●{RST}"
    if n == 4: filled = f"{YLW}●●●{RST}{RED}●{RST}"
    if n == 5: filled = f"{RED}●●●●●{RST}"
    empty  = f"{WHT}○{RST}" * (5 - n)
    if n <= 2:
        filled = (f"{GRN}●{RST}" * n)
    return filled + empty

def box_row(cols, widths, sep="│"):
    """Render one table row with box-drawing."""
    row = "  │"
    for i, (c, w) in enumerate(zip(cols, widths)):
        row += f" {c:<{w}} │"
    return row

def box_rule(widths, left="├", mid="┼", right="┤", h="─"):
    parts = [h * (w + 2) for w in widths]
    return "  " + left + mid.join(parts) + right

def box_top(widths):
    parts = ["─" * (w + 2) for w in widths]
    return "  ┌" + "┬".join(parts) + "┐"

def box_bot(widths):
    parts = ["─" * (w + 2) for w in widths]
    return "  └" + "┴".join(parts) + "┘"

def table(headers, rows, col_colors=None):
    """Render a complete box-drawing table."""
    # Compute widths from visible text (strip ANSI for width calculation)
    import re
    ansi_re = re.compile(r'\x1b\[[0-9;]*m')
    def vlen(s): return len(ansi_re.sub('', s))

    widths = [vlen(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], vlen(str(cell)))

    lines = []
    lines.append(box_top(widths))
    # Header row
    hdr = "  │"
    for h, w in zip(headers, widths):
        hdr += f" {CYN}{h}{RST}{' ' * (w - vlen(h))} │"
    lines.append(hdr)
    lines.append(box_rule(widths))
    for row in rows:
        r = "  │"
        for i, (cell, w) in enumerate(zip(row, widths)):
            cell = str(cell)
            pad = w - vlen(cell)
            r += f" {cell}{' ' * pad} │"
        lines.append(r)
    lines.append(box_bot(widths))
    return "\n".join(lines)

lines = []

# ── Title ─────────────────────────────────────────────────────────────────────
title = "N T T   /   M I 3 0 0 A   —   I M P L E M E N T A T I O N   P L A N"
width = 118
pad = (width - len(title)) // 2
lines += [
    "",
    WHT + "╔" + "═" * width + "╗" + RST,
    WHT + "║" + " " * pad + title + " " * (width - pad - len(title)) + "║" + RST,
    WHT + "╚" + "═" * width + "╝" + RST,
    "",
    YLW + "  View with:  cat PLAN.md   or   less -R PLAN.md" + RST,
    "",
    "  This document tracks algorithm choices for each NTT implementation segment.",
    "  Chosen options are marked ✓. Tables are updated as units are completed.",
    "",
]

# ── Legend ────────────────────────────────────────────────────────────────────
lines += [
    f"  Difficulty:  {diff(1)} easy   {diff(3)} moderate   {diff(5)} expert",
    f"  Seg Perf:   ×1.0 = baseline option within that segment (lower = faster).",
    f"  Overall:    estimated effect on total GPU wall-time for n=2²⁰, MI300A.",
    f"  LOC:        projected lines of new C/HIP source code for that option.",
    "",
    SEP,
    "",
]

# ── Runtime distribution ───────────────────────────────────────────────────────
lines += [
    CYN + "  RUNTIME DISTRIBUTION  (estimated, n = 2²⁰, single NTT, MI300A)" + RST,
]
rt_headers = ["Seg", "Segment Name", "Est. Share", "Bottleneck", "Notes"]
rt_rows = [
    ["1", "Core NTT butterfly algorithm",   "~65 %", "compute",   "Radix-2 stages; Montgomery muls dominate"],
    ["2", "Modular arithmetic (reduction)", "~15 %", "compute",   "Folded into butterfly; reduction cost"],
    ["3", "Bit-reversal permutation",        " ~8 %", "bandwidth", "Bandwidth-bound; eliminated by Stockham"],
    ["4", "Twiddle generation & storage",    " ~5 %", "bandwidth", "HBM reads per butterfly; precomputed"],
    ["5", "Kernel dispatch & memory mgmt",   " ~4 %", "latency",   "MI300A: unified HBM, no PCIe overhead"],
    ["6", "Multi-NTT batching overhead",     " ~3 %", "latency",   "Amortized across batch; kernel reuse"],
]
lines.append(table(rt_headers, rt_rows))
lines += ["",
    "  Note: For n=256 (ML-KEM/ML-DSA), all data fits in LDS. Butterfly share rises",
    "  to ~85%; memory and dispatch costs fall to <5% combined.",
    "", SEP, "",
]

# ── Segment 1: Core NTT Algorithm ─────────────────────────────────────────────
lines += [
    MAG + "  SEGMENT 1 — CORE NTT BUTTERFLY ALGORITHM" + RST + "  (≈65% runtime)",
    "",
    "  The top-level algorithm choice determines butterfly order, memory access pattern,",
    "  whether bit-reversal is needed, and how well the kernel maps to MI300A wavefronts.",
    "",
]
s1_headers = ["Algorithm", "Diff", "LOC", "Seg Perf", "Overall", "Memory", "Notes"]
s1_rows = [
    ["Cooley-Tukey DIT",       diff(1), "~150", "×1.0",  "baseline", "O(n)",    "Standard DIT; requires bit-reversed input"],
    ["Gentleman-Sande DIF",    diff(1), "~160", "×1.0",  "~0%",      "O(n)",    "DIF; bit-reversed output; pairs with CT"],
    ["Stockham (self-sorting)", diff(2), "~260", "×0.92", "−5%",      "O(2n)",   "No bit-reversal; double-buffer; GPU-preferred"],
    ["Mixed-radix r=4",        diff(3), "~420", "×0.70", "−20%",     "O(n)",    "Fewer stages; better LDS reuse per wavefront"],
    ["Six-step FFT",           diff(4), "~520", "×0.55", "−29%",     "O(n)+T",  "Optimal n>2¹⁸; cache-oblivious; needs transpose"],
]
lines.append(table(s1_headers, s1_rows))
lines += [
    "",
    "  Recommendation: start with Cooley-Tukey DIT (lowest risk, fastest to implement",
    "  correctly). Add Stockham as the primary GPU path once CT is verified.",
    "  Six-step warrants investigation only after smaller-n paths are tuned.",
    "", SEP, "",
]

# ── Segment 2: Modular Arithmetic ─────────────────────────────────────────────
lines += [
    MAG + "  SEGMENT 2 — MODULAR ARITHMETIC & REDUCTION" + RST + "  (≈15% runtime)",
    "",
    "  Every butterfly requires two modular multiplications. Reduction strategy directly",
    "  sets the cost floor for the entire NTT. MI300A has no native 64-bit mod instruction.",
    "",
]
s2_headers = ["Method", "Diff", "LOC", "Seg Perf", "Overall", "Constraint", "Notes"]
s2_rows = [
    ["Lazy / deferred reduction", diff(1), "~40",  "×1.0",  "baseline", "64-bit headroom", "Reduce every 2–3 stages; simplest correct impl"],
    ["Barrett reduction",         diff(2), "~70",  "×0.82", "−2.7%",    "any q < 2³²",    "Precomputed reciprocal; no conversion needed"],
    ["Montgomery multiplication", diff(2), "~85",  "×0.75", "−3.75%",   "any odd q",       "Best for sustained chains; twiddles pre-converted"],
    ["Montgomery + lazy combo",   diff(3), "~130", "×0.60", "−6%",      "any odd q",       "Montgomery in body, lazy accumulation between stages"],
    ["Plantard reduction",        diff(4), "~105", "×0.65", "−5.25%",   "q < 2³¹",        "64→32 narrowing trick; faster on some pipelines"],
]
lines.append(table(s2_headers, s2_rows))
lines += [
    "",
    "  Recommendation: implement lazy reduction first (correctness baseline), then",
    "  Montgomery. ML-KEM q=3329 and ML-DSA q=8380417 both work with Montgomery.",
    "  Plantard worth benchmarking against Montgomery on actual MI300A hardware.",
    "", SEP, "",
]

# ── Segment 3: Bit-Reversal ────────────────────────────────────────────────────
lines += [
    MAG + "  SEGMENT 3 — BIT-REVERSAL PERMUTATION" + RST + "  (≈8% runtime)",
    "",
    "  Required by Cooley-Tukey DIT (input) and Gentleman-Sande DIF (output).",
    "  Eliminated entirely if Stockham is chosen for Segment 1.",
    "",
]
s3_headers = ["Method", "Diff", "LOC", "Seg Perf", "Overall", "Notes"]
s3_rows = [
    ["Eliminated (Stockham)",           "N/A",   "  0",  "—",     "−8%",   "Only if Seg 1 = Stockham; best outcome"],
    ["Separate kernel (index swap)",    diff(1), "~50",  "×1.0",  "baseline", "Simple; one read + one write per element"],
    ["Precomputed index LUT",           diff(1), "~65",  "×0.90", "−0.8%", "LUT eliminates bit-twiddling in hot loop"],
    ["Schatzman in-place",             diff(2), "~85",  "×0.82", "−1.4%", "Cache-friendlier; avoids LUT memory"],
]
lines.append(table(s3_headers, s3_rows))
lines += [
    "",
    "  Recommendation: implement the simple separate kernel alongside CT. If Stockham",
    "  is adopted, this segment is retired entirely.",
    "", SEP, "",
]

# ── Segment 4: Twiddle Factor Generation ──────────────────────────────────────
lines += [
    MAG + "  SEGMENT 4 — TWIDDLE FACTOR GENERATION & STORAGE" + RST + "  (≈5% runtime)",
    "",
    "  Twiddle factors ω^k must be in Montgomery form for the Montgomery butterfly.",
    "  MI300A HBM3 (900 GB/s) makes precomputed tables cheap to read.",
    "",
]
s4_headers = ["Method", "Diff", "LOC", "Seg Perf", "Overall", "HBM use", "Notes"]
s4_rows = [
    ["Precompute all ω^k, store HBM", diff(1), "~80",  "×1.0",  "baseline", "O(n) words",  "Best bandwidth; one read per butterfly"],
    ["Partial table + recurrence",    diff(2), "~120", "×1.08", "+0.4%",    "O(√n) words", "Recompute within stage using recurrence"],
    ["LDS-cached (small n ≤ 2¹⁴)",  diff(2), "~110", "×0.70", "−1.5%",    "LDS only",    "All twiddles fit in shared mem; zero HBM reads"],
    ["On-the-fly (repeated sqr)",    diff(3), "~65",  "×1.40", "+2%",      "O(1)",         "Minimal HBM; compute-bound; rarely worth it"],
]
lines.append(table(s4_headers, s4_rows))
lines += [
    "",
    "  Recommendation: precompute all ω^k in HBM for general n; switch to LDS-cached",
    "  for the n=256 ML-KEM/ML-DSA path. Both tables must be in Montgomery form.",
    "", SEP, "",
]

# ── Segment 5: Kernel Dispatch ─────────────────────────────────────────────────
lines += [
    MAG + "  SEGMENT 5 — KERNEL DISPATCH & MEMORY MANAGEMENT" + RST + "  (≈4% runtime)",
    "",
    "  MI300A is an APU: host and device share HBM3. There is no PCIe copy.",
    "  hipMalloc allocates from the same unified pool as malloc; pointers are shared.",
    "",
]
s5_headers = ["Method", "Diff", "LOC", "Seg Perf", "Overall", "Notes"]
s5_rows = [
    ["One kernel launch per NTT stage",    diff(1), "~80",  "×1.0",  "baseline", "Simple; barrier between stages = sync"],
    ["Fused multi-stage kernel (LDS)",     diff(3), "~220", "×0.60", "−1.6%",    "Tile log₂(LDS_elems) stages per launch"],
    ["Persistent grid (single launch)",    diff(4), "~320", "×0.50", "−2%",      "One launch total; sync via atomics in HBM"],
    ["MI300A unified ptr (zero-copy)",     diff(1), "~30",  "—",     "−4% vs dGPU","APU: allocate once; no hipMemcpy needed"],
]
lines.append(table(s5_headers, s5_rows))
lines += [
    "",
    "  Recommendation: begin with one kernel per stage. Implement fused multi-stage",
    "  once stage-level kernels are verified. Exploit unified memory from day one —",
    "  use hipHostMalloc with hipHostMallocDefault; pointer is valid on both sides.",
    "", SEP, "",
]

# ── Segment 6: Batching ────────────────────────────────────────────────────────
lines += [
    MAG + "  SEGMENT 6 — MULTI-NTT BATCHING" + RST + "  (≈3% overhead amortized)",
    "",
    "  ML-KEM requires 256-point NTTs; ML-DSA likewise. Batching many small NTTs",
    "  is critical for throughput: a single n=256 NTT uses <1% of MI300A capacity.",
    "",
]
s6_headers = ["Method", "Diff", "LOC", "Seg Perf", "Overall", "Target n", "Notes"]
s6_rows = [
    ["Serial: one NTT per launch",        diff(1), "~10",  "×1.0",  "baseline", "any",      "No batching; correct reference baseline"],
    ["Grid-stride batch (one kernel)",    diff(2), "~55",  "×0.55", "−1.4%",    "any",      "All NTTs in one launch; hides latency"],
    ["LDS-batched (small n in shmem)",    diff(3), "~160", "×0.20", "−2.4%",    "n ≤ 2¹⁴", "Pack many NTTs per CU; zero HBM spill"],
    ["Warp-level (1 NTT per wavefront)",  diff(4), "~210", "×0.15", "−2.55%",   "n = 256",  "wavefront=64 fits half of n=256; optimal ML-KEM"],
]
lines.append(table(s6_headers, s6_rows))
lines += [
    "",
    "  Recommendation: implement grid-stride batching from the start — it subsumes",
    "  the serial case at n_batch=1. Warp-level batching for n=256 is the highest-",
    "  value optimization for PQC workloads and should be a named development target.",
    "", SEP, "",
]

# ── Overall Assessment ─────────────────────────────────────────────────────────
lines += [
    CYN + "  OVERALL ASSESSMENT" + RST,
    "",
    "  Status: infrastructure complete; no source files written.",
    "",
    "  Optimal algorithm stack (aggressive, all segments at best option):",
]
stack_headers = ["Seg", "Choice", "Combined gain"]
stack_rows = [
    ["1", "Six-step FFT (large n) / Stockham (small n)", "−29% / −5%"],
    ["2", "Montgomery + lazy combo",                     "−6%"],
    ["3", "Eliminated (via Stockham)",                   "−8%"],
    ["4", "Precomputed HBM + LDS for n=256",            "−1.5%"],
    ["5", "Fused multi-stage + MI300A unified ptr",      "−2%"],
    ["6", "Warp-level for n=256, grid-stride otherwise", "−2.55%"],
    ["—", "Total estimated vs. naive baseline",          "≈ −49% wall-time"],
]
lines.append(table(stack_headers, stack_rows))
lines += [
    "",
    "  Conservative first-milestone stack (Phase 2 target):",
    "    Seg 1: Cooley-Tukey DIT   Seg 2: lazy reduction   Seg 3: separate kernel",
    "    Seg 4: precomputed HBM    Seg 5: per-stage launch  Seg 6: grid-stride batch",
    "  This is correct, buildable, and measurable — the right foundation.",
    "",
    SEP,
    "",
]

out = "\n".join(lines)
with open("PLAN.md", "w") as f:
    f.write(out)
print(f"Written {len(out)} bytes, {len(lines)} lines.")

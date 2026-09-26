"""PAPER.md -> paper_tex.md for the LaTeX build.
  * title/subtitle -> YAML; the hand-made contents and the horizontal rules dropped (LaTeX makes a TOC)
  * Mermaid blocks -> the pre-rendered build/mermaidN.pdf; SVG figures -> their PDF conversions
  * an image followed by a "*Figure N. ...*" paragraph -> one captioned figure
  * Unicode math in running text (10¹⁸, 2ᵏ, ≈, →, ⌊ ⌋, ε, ...) -> real LaTeX math, outside code and existing math"""
import re, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
m = re.match(r"# (.+?)\n\n\*(.+?)\*\n", s, re.S)
title, subtitle = m.group(1), " ".join(m.group(2).split())
s = s[m.end():]
s = re.sub(r"## Contents\n.*?\n---\n", "", s, flags=re.S)
s = re.sub(r"\n---\n", "\n", s)
k = iter(range(100))
s = re.sub(r"```mermaid\n.*?```\n", lambda _: f"![](build/mermaid{next(k)}.pdf)\n\n", s, flags=re.S)
def fig(mm):
    path, cap = mm.group(1).replace(".svg", ".pdf"), " ".join(mm.group(2).split())
    cap = re.sub(r"^(Figure [A0-9]+\.)", r"**\1**", cap)
    w = {"mermaid0": "38%"}.get(path.split("/")[-1][:-4], "100%")
    return f"![{cap}]({path}){{width={w}}}\n\n"
s = re.sub(r"!\[[^\]]*\]\(([^)]+)\)\n+\*(Figure .+?)\*\n\n", fig, s, flags=re.S)

SUP = dict(zip("⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻ᵏ", list("0123456789+-") + ["k"]))
SUB = dict(zip("₀₁₂ᵢₖ", "012ik"))
SYM = {"≈": r"\approx", "≤": r"\le", "≥": r"\ge", "→": r"\to", "←": r"\leftarrow", "↔": r"\leftrightarrow",
       "⊕": r"\oplus", "≫": r"\gg", "≪": r"\ll", "⌊": r"\lfloor", "⌋": r"\rfloor", "⌈": r"\lceil", "⌉": r"\rceil",
       "∣": r"\mid", "√": r"\surd", "∑": r"\sum", "∞": r"\infty", "ε": r"\varepsilon", "ω": r"\omega", "μ": r"\mu",
       "µ": r"\mu", "ρ": r"\rho", "θ": r"\theta", "φ": r"\varphi", "π": r"\pi", "δ": r"\delta", "Δ": r"\Delta",
       "ℤ": r"\mathbb{Z}", "ℓ": r"\ell", "⋯": r"\cdots", "−": "-", "·": r"\cdot", "×": r"\times", "±": r"\pm"}
def plain(t):                                             # running text only
    t = re.sub("[" + "".join(SUP) + "]+", lambda mm: "\\ensuremath{^{" + "".join(SUP[c] for c in mm.group()) + "}}", t)
    t = re.sub("[" + "".join(SUB) + "]+", lambda mm: "\\ensuremath{_{" + "".join(SUB[c] for c in mm.group()) + "}}", t)
    t = re.sub("|".join(map(re.escape, SYM)), lambda mm: "\\ensuremath{" + SYM[mm.group()] + "}", t)
    t = re.sub(r"([①②③④✔★⭐])", r"\\fb{\1}", t)
    return t
# split into protected (code blocks, inline code, $$..$$, $..$) and plain parts
tok = re.compile(r"(```.*?```|`[^`\n]*`|\$\$.*?\$\$|(?<![\\$])\$[^$\n]+?\$)", re.S)
parts = tok.split(s)
s = "".join(p if i % 2 else plain(p) for i, p in enumerate(parts))
title, subtitle = title.replace("10¹¹", "$10^{11}$"), plain(subtitle)
head = f"""---
title: "{title}"
subtitle: "{subtitle}"
documentclass: article
fontsize: 10pt
geometry: "margin=2.3cm"
mainfont: "TeX Gyre Pagella"
mathfont: "TeX Gyre Pagella Math"
monofont: "DejaVu Sans Mono"
monofontoptions: "Scale=0.82"
linestretch: 1.08
colorlinks: true
linkcolor: "blue!50!black"
urlcolor: "blue!50!black"
toc: true
toc-depth: 2
header-includes: |
  ```{{=latex}}
  \\usepackage{{caption}}
  \\captionsetup[figure]{{labelformat=empty,font=small,width=0.92\\textwidth}}
  \\usepackage{{float}}
  \\floatplacement{{figure}}{{htbp}}
  \\newfontfamily\\fallbackfont[Scale=0.85]{{DejaVu Sans}}
  \\newcommand\\fb[1]{{{{\\fallbackfont #1}}}}
  \\usepackage{{etoolbox}}
  \\AtBeginEnvironment{{longtable}}{{\\small}}
  \\setlength{{\\emergencystretch}}{{3em}}
  ```
---

"""
open(dst, "w").write(head + s)

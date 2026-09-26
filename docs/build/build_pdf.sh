#!/bin/sh
# Typeset docs/PAPER.md as docs/PAPER.pdf.  Needs pandoc, xelatex, rsvg-convert, TeX Gyre fonts; the two
# Mermaid diagrams are pre-rendered in build/mermaid*.pdf (npx @mermaid-js/mermaid-cli -i build/mermaidN.mmd -o ... --pdfFit).
set -e
cd "$(dirname "$0")/.."
for f in fig/*.svg; do rsvg-convert -f pdf -o "${f%.svg}.pdf" "$f"; done
python3 build/prep.py PAPER.md build/paper_tex.md
pandoc build/paper_tex.md -o PAPER.pdf --pdf-engine=xelatex --shift-heading-level-by=-1 --columns=80 \
  --resource-path=. -f markdown+tex_math_dollars+pipe_tables+implicit_figures -V block-headings

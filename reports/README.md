# Reports

LaTeX sources for the two project write-ups, plus the compiled PDFs.

| File                      | What it is                                                   |
|---------------------------|--------------------------------------------------------------|
| `academic_report.tex`     | Source for the university-submission report (18 pages).      |
| `academic_report.pdf`     | Compiled PDF.                                                |
| `internal_deep_dive.tex`  | Source for the team's no-holds-barred walkthrough (14 pages).|
| `internal_deep_dive.pdf`  | Compiled PDF.                                                |
| `build/`                  | Intermediate LaTeX artefacts (`.aux`, `.log`, `.toc`, …).    |

The Markdown originals live in `../docs/` (`academic_report.md`,
`internal_deep_dive.md`). The `.tex` sources mirror them but use TikZ for
the block diagrams and `listings` for code excerpts.

## Build

Requires TeX Live (any reasonably recent year). On macOS:

```sh
brew install --cask mactex-no-gui    # or use the BasicTeX package
```

On Debian/Ubuntu:

```sh
sudo apt install texlive-latex-recommended texlive-latex-extra \
                 texlive-fonts-recommended texlive-pictures latexmk
```

Then, from this directory:

```sh
latexmk -pdf -outdir=build -auxdir=build academic_report.tex
latexmk -pdf -outdir=build -auxdir=build internal_deep_dive.tex
cp build/academic_report.pdf academic_report.pdf
cp build/internal_deep_dive.pdf internal_deep_dive.pdf
```

`latexmk` re-runs `pdflatex` as many times as needed to settle the table of
contents and cross-references.

## Clean

```sh
latexmk -C -outdir=build -auxdir=build
rm -rf build
```

## Editing notes

* The `.tex` sources are hand-written, not generated from the Markdown. If
  you change content, update the `.tex` directly and rebuild; do not run
  pandoc over the Markdown.
* Custom listing styles for SystemVerilog (`style=sv`) and C (`style=c`) live
  in each preamble.
* Long file paths are kept tight with `\emergencystretch=4em`; very long
  literal paths in tables may still need to be broken manually.

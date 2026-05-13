# No Man's Land — CSEE 4840 Spring 2026 Final Submission

**Project:** No Man's Land — a WWI horde-survival game on the Terasic DE1-SoC
**Team:** Rohit Biswas (rb3908), Kambinachi Obioha (kno2117), Nicola Paparella (np2953)
**Instructor:** Prof. Stephen Edwards

## Deliverables in this folder

| File | Size | Pages | Description |
|---|---|---|---|
| `academic_report.pdf` | 768 KB | 99 | Final academic report — architecture, register map, build flow, testing, results, and a complete source-listings appendix covering all hardware (SystemVerilog/Verilog) and software (C) sources. |
| `slides.pdf` | 261 KB | 19 | Half-hour presentation deck. System overview, peripheral block diagram, register map, pixel pipeline, HUD, software architecture, results. Photo frames build with placeholders until JPGs are dropped into `img/`. |
| `nmlland_source.tar.gz` | 120 KB | — | Machine-readable source bundle. Contains everything needed to re-run Platform Designer and Quartus and rebuild the HPS-side game: `hw/`, `nml_gpu_hw/`, `sw/`, plus `DESIGN.md`, `SETUP.md`, `README.md`. Excludes generated databases, output bitstreams, QSYS-generated dirs, git history, and reports. |

## Supporting

| Path | Purpose |
|---|---|
| `img/` | Drop-folder for the three photographs the slide deck references via `\IfFileExists`. See `img/README.md`. |
| `img/README.md` | Lists the expected JPG filenames (`01_system_running.jpg`, `02_hud_closeup.jpg`, `03_gameplay.jpg`) and where each appears in the deck. |

## How the artifacts are built (for the record)

```bash
# PDFs
cd reports
latexmk -pdf -outdir=build -auxdir=build academic_report.tex
latexmk -pdf -outdir=build -auxdir=build slides.tex
cp build/academic_report.pdf ../submission/academic_report.pdf
cp build/slides.pdf          ../submission/slides.pdf

# Source tarball (from project root)
tar -czf submission/nmlland_source.tar.gz \
    --exclude='.git' --exclude='build' --exclude='lab3-reference' \
    --exclude='docs' --exclude='reports' --exclude='submission' \
    --exclude='hw/quartus/db' --exclude='hw/quartus/incremental_db' \
    --exclude='hw/quartus/output_files' \
    --exclude='nml_gpu_hw/db' --exclude='nml_gpu_hw/incremental_db' \
    --exclude='nml_gpu_hw/output_files' --exclude='nml_gpu_hw/soc_system' \
    --exclude='nml_gpu_hw/hps_isw_handoff' --exclude='nml_gpu_hw/ip' \
    --exclude='nml_gpu_hw/*.sopcinfo' --exclude='nml_gpu_hw/*.srf' \
    --exclude='nml_gpu_hw/*.qdf' --exclude='*~' --exclude='*.bak' \
    --exclude='sw/nml_game_term' \
    .
```

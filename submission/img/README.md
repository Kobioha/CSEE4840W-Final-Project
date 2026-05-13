# Submission Images

The slide deck (`reports/slides.tex` → `submission/slides.pdf`) references three
photographs of the system from this directory. Each is loaded with
`\IfFileExists`, so the deck builds with visible "PHOTO GOES HERE" placeholders
when the JPGs are missing. Drop the files in with these exact filenames and
re-run `latexmk` on `slides.tex` to embed them.

| Filename | Where it appears | What it should show |
|---|---|---|
| `01_system_running.jpg` | Frame 2, right after the title slide | Full-bleed photo of the DE1-SoC board + VGA monitor + SNES gamepad with the game running. Landscape orientation, the whole rig in one shot. |
| `02_hud_closeup.jpg` | Frame after the "HUD strip --- top 16 pixels of every frame" technical diagram | Closeup of the HUD strip on the VGA monitor showing the HP bar, AMMO, WAVE, ART, GAS, and SCORE readouts in actual rendered pixels. |
| `03_gameplay.jpg` | Final "What actually works" frame | Live gameplay shot on the VGA monitor (player, enemies, projectiles, HUD visible). Sharp enough that sprites are recognizable. |

Acceptable formats: `.jpg` or `.png`. JPG is preferred (smaller). Keep each
under ~2 MB; the deck does not need full-resolution photos.

After dropping the files in:

```bash
cd ../../reports
latexmk -pdf -outdir=build -auxdir=build slides.tex
cp build/slides.pdf ../submission/slides.pdf
```

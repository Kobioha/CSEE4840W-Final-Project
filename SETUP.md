# No Man's Land — Setup, Build, and Test Guide (DE1-SoC)

This guide takes you from a fresh `git pull` on the lab Linux machine to a
working VGA image on the monitor, and then on to running the C game on the
HPS. It is structured in three phases — do them in order.

| Phase | Goal | Time | Hardware needed |
|------:|------|------|-----------------|
| 0 | Sanity-check on a host laptop | 10 min | none |
| 1 | **Smoke-test bitstream — pixels on the monitor** | 60–90 min | DE1-SoC, VGA monitor, USB Blaster |
| 2 | HPS-driven C game on top of the FPGA | several hours | Phase 1 + DE1-SoC SD card with Linux |

> **Status of this checkpoint:** Phase 0 and Phase 1 are complete and ready
> to test today. Phase 2 requires Qsys integration of the HPS-to-FPGA
> Lightweight bridge — that work is documented in §6 below, but is **not**
> done yet. The C driver, render path, and Makefile are written and waiting
> for the bridge to land.

---

## 0. Sanity-check on a host laptop (no FPGA)

Run before getting anywhere near the lab machine to confirm the C side
compiles. From the repo root:

```bash
cd sw
make terminal           # builds nml_game_term, prints sprite slots to stdout
./nml_game_term | head  # press Ctrl-C after a few frames
make clean
```

If `make terminal` fails, fix the C side before going further. The terminal
build does not touch any hardware.

Also re-generate the ROM hex files so they're committed alongside any sprite
or tile-map changes:

```bash
cd hw
python3 gen_rom.py
```

This produces `sprite_rom.hex`, `tile_rom.hex`, `palette.hex`, and
`sprite_table.hex` — all four are required for the bitstream to show pixels.

---

## 1. Phase 1 — smoke-test bitstream on the DE1-SoC

**Goal:** confirm the FPGA fabric, PLL, VGA timing, and `nml_gpu` compositor
all work on real silicon. No HPS, no SD card, no Linux. The bitstream alone
produces a static frame containing a player, two enemies, and a bullet on a
grey tiled background, using the data baked into the hex files at synthesis
time.

### 1.1 Prerequisites on the lab Linux machine

- **Quartus Prime Lite** 20.1 or 22.1 (matches the version the GHRD targets;
  newer versions also work).
- **USB Blaster II** drivers installed and the user in the `plugdev` group
  (or run `quartus_pgm` as root).
- The DE1-SoC powered on, with a VGA monitor connected and the USB Blaster
  cable plugged into the **USB Blaster** port (not the UART one).

### 1.2 Pull the repo

```bash
git clone <repo-url> CSEE4840W-Final-Project
cd CSEE4840W-Final-Project
```

### 1.3 Generate the ROM/init hex files

The bitstream uses `$readmemh` to pre-load five memories. The hex files are
checked into the repo, but always regenerate after pulling so any teammate
edit to `hw/gen_rom.py` is reflected:

```bash
cd hw
python3 gen_rom.py
```

Expected output:

```
wrote sprite_rom.hex (16384 bytes)
wrote tile_rom.hex (4096 bytes)
wrote palette.hex (256 entries, 24b each)
wrote sprite_table.hex (32 entries, 64b each)
```

### 1.4 Compile the bitstream in Quartus

```bash
cd hw/quartus
quartus_sh --flow compile nml_gpu
```

Or open `nml_gpu.qpf` in the Quartus GUI and click **Processing → Start
Compilation**. Compilation should finish with no errors. Common warnings to
ignore:

- "Width mismatch" notes inside the SystemVerilog: harmless if the file
  compiles.
- "No exact match for clock constraint": expected — we don't ship an SDC
  file for the smoke-test bitstream. The 25 MHz divided clock is well below
  Cyclone V's slowest speed grade Fmax for the small logic in `nml_gpu`.

If compilation fails, the most likely causes are listed in §5
**Troubleshooting**.

The `.sof` (SRAM Object File) appears at:

```
hw/quartus/output_files/nml_gpu.sof
```

### 1.5 Load the bitstream over JTAG

Power-cycle the board, then:

```bash
cd hw/quartus
quartus_pgm -m JTAG -o "p;output_files/nml_gpu.sof@2"
```

`@2` is the chain index for the FPGA core on the DE1-SoC (HPS is `@1`). If
the program fails with "no hardware found", run `jtagconfig` first to list
detected adapters and chain indices.

### 1.6 What you should see

Within a second of `quartus_pgm` finishing:

- LEDR[0] is **on** (FPGA powered).
- LEDR[1] **flickers** at the VGA refresh rate (60 Hz, lit during active
  video).
- The VGA monitor shows:
  - A dark-grey background made of 8×8 tiles.
  - A green 16×16 square (player) near the centre.
  - Two red 16×16 squares (enemies) in the upper half.
  - A small yellow 4×4 dot (bullet) below the player.

If any of those are missing, jump to §5.

> **Reset:** press KEY[0] to reset the entire pipeline. The screen blanks
> briefly and reappears with the same content.

### 1.7 Interpreting the result

A correct image proves the entire dataflow:

- 25 MHz pixel clock from `pll_25mhz.v`.
- `vga_timing.sv` HSYNC/VSYNC.
- M10K `$readmemh` initialisation (palette, sprite table, sprite ROM, tile
  ROM, tile map default-zero = tile 0).
- `sprite_eval` (priority sort), `sprite_fetch` (line-buffer fill), and
  `compositor` (priority MUX + palette lookup).
- VGA DAC pin assignments in `nml_gpu.qsf`.

You're now ready for Phase 2.

---

## 2. Phase 2 — HPS-driven game on top of the bitstream

**Goal:** boot Linux on the HPS, load the FPGA from Linux, run the C game
binary, see the player respond to input.

> **As of this checkpoint:** the bitstream from Phase 1 has no HPS-to-FPGA
> bridge. Software writes from `/dev/mem` will land in unmapped territory
> and have no effect. To use the C driver, regenerate the bitstream from a
> top-level that integrates `nml_gpu` with a Qsys `soc_system` containing
> the HPS plus the LW bridge. The instructions in §6 below describe that
> work; the rest of this section assumes it has been done.

### 2.1 Boot the board

1. Insert the class-issued (or Terasic-provided) DE1-SoC Linux SD card.
2. Set MSEL switches per the GHRD documentation (default for DE1-SoC is
   `00000` — FPPx32 from HPS).
3. Power on.
4. Connect to the board. Two options:
   - **Serial (UART):** use the mini-USB UART port. `screen /dev/ttyUSB0
     115200` or PuTTY at 115 200 8N1. Login `root`, no password.
   - **SSH:** plug a network cable in. Find the board's IP from the UART
     console (`ip addr`), then `ssh root@<ip>`.

### 2.2 Build the C binary on the lab machine

```bash
cd sw
make             # cross-compiles for armhf using arm-linux-gnueabihf-gcc
ls -lh nml_game  # should be a static armhf ELF
```

If `arm-linux-gnueabihf-gcc` isn't installed:

```bash
sudo apt install gcc-arm-linux-gnueabihf
```

Alternatively, build natively on the board itself:

```bash
scp sw/*.c sw/*.h sw/Makefile root@<board>:/root/sw/
ssh root@<board>
cd sw && make native
```

### 2.3 Load the FPGA from Linux on the HPS

Copy the `.rbf` (Quartus generated this alongside the `.sof` because we
asked for it via `GENERATE_RBF_FILE ON`) to the board:

```bash
scp hw/quartus/output_files/nml_gpu.rbf root@<board>:/lib/firmware/
```

On the board, load it. The exact command depends on the kernel version on
the SD card; try in order until one works:

1. **Modern fpga-manager interface (Linux 4.x+):**

   ```bash
   echo 0 > /sys/class/fpga_manager/fpga0/flags
   echo nml_gpu.rbf > /sys/class/fpga_manager/fpga0/firmware
   dmesg | tail   # expect "fpga_manager fpga0: writing nml_gpu.rbf to ..."
   ```

2. **Terasic `fpga_config` script (older images):**

   ```bash
   cd /home/root && ./fpga_config /lib/firmware/nml_gpu.rbf
   ```

3. **Direct `dd` to the FPGA manager device (rare):**

   ```bash
   dd if=/lib/firmware/nml_gpu.rbf of=/dev/fpga0 bs=1M
   ```

Whichever succeeds, confirm by `cat /sys/class/fpga_manager/fpga0/state` —
should print `operating` (or similar "configured" string).

### 2.4 Copy and run the game

```bash
scp sw/nml_game root@<board>:/root/
ssh root@<board>
./nml_game        # must run as root for /dev/mem mmap
```

Expected behaviour:

- Monitor shows the same scene as Phase 1.
- The player sprite (green square) drifts right for ~1.3 s, then left for
  ~1.3 s, then back — that's `input_fake.c` running its hardcoded test
  pattern.
- Yellow bullets fire upward periodically.
- Red enemies spawn at the top and chase the player.
- After the player is hit enough times, `GAME OVER. Final score: N` prints
  on the SSH session and the binary exits.

### 2.5 Check frame timing

While the game is running, in another shell:

```bash
ssh root@<board> top -n 1 -b | head
```

CPU should be < 5 % — the loop spends most of its time in `nanosleep` and
the `nml_commit_frame` SWAP poll. If CPU is pegged, the SWAP poll is timing
out (likely cause: the bitstream isn't actually loaded — re-do §2.3).

---

## 3. File map

```
.
├── DESIGN.md                 -- requirements doc (Section 5 = register map)
├── SETUP.md                  -- this file
├── README.md
├── hw/
│   ├── nml_gpu.sv            -- top peripheral (Avalon slave + RAMs + pipeline)
│   ├── avalon_slave_iface.sv -- register decode
│   ├── vga_timing.sv         -- 640x480@60 timing
│   ├── sprite_eval.sv        -- per-scanline sprite priority sort
│   ├── sprite_fetch.sv       -- HBLANK sprite fetch into line buffer
│   ├── compositor.sv         -- tile + sprite priority MUX + palette lookup
│   ├── pll_25mhz.v           -- 50→25 MHz divide-by-2 (smoke-test PLL)
│   ├── de1soc_top.sv         -- DE1-SoC pins + nml_gpu instantiation (no HPS)
│   ├── gen_rom.py            -- emits the four hex files below
│   ├── sprite_rom.hex        -- 16 KB, $readmemh into sprite_rom
│   ├── tile_rom.hex          --  4 KB, $readmemh into tile_rom
│   ├── palette.hex           -- 256 × 24b, $readmemh into palette_ram
│   ├── sprite_table.hex      -- 32 × 64b, $readmemh into sprite_table_active/shadow
│   └── quartus/
│       ├── nml_gpu.qpf       -- Quartus project
│       └── nml_gpu.qsf       -- pin assignments + file list + settings
└── sw/
    ├── main.c                -- 60 Hz game loop (FPGA or terminal build)
    ├── game.c, game.h        -- entity pool, AI, collision, score
    ├── input.h               -- input bitmask abstraction
    ├── input_fake.c          -- hardcoded test pattern (Phase 2 needs libusb)
    ├── render.h              -- render_frame(const game_t*)
    ├── render.c              -- entity → nml_gpu sprite-table writer
    ├── render_terminal.c     -- alt: prints sprite slots to stdout
    ├── nml_gpu.h             -- driver: register offsets, sprite struct, prototypes
    ├── nml_gpu.c             -- driver: /dev/mem mmap + register writers
    └── Makefile              -- targets: all (default), native, terminal, clean
```

---

## 4. What's done vs. what's outstanding

### Done (this checkpoint)

- All compositor SystemVerilog modules.
- 50→25 MHz pixel clock (`pll_25mhz.v`).
- `$readmemh` pre-init of palette + initial sprite table — bitstream alone
  shows a complete frame.
- DE1-SoC top-level wrapper and Quartus project.
- C driver (`nml_gpu.h` / `nml_gpu.c`) covering all register regions.
- Render path (`render.c`) translating game entities to sprite-table writes.
- Makefile with cross-compile, native, and terminal targets.
- `gen_rom.py` so future art / level changes don't require manual hex
  editing.
- Compile-blocking bug fix in `nml_gpu.sv` (port name mismatch + missing
  `frame_ctr_in` connection on the Avalon slave).

### Outstanding (per `DESIGN.md`)

- **HPS-to-FPGA Lightweight bridge integration in Qsys.** Required before
  the C driver does anything. See §6.
- **HUD overlay** in `compositor.sv` (HP bar, wave, score). Reads from
  `player_pos`, `player_stats`, `score_reg`, `kill_count` already exposed
  by `avalon_slave_iface`.
- **Real input device.** Replace `input_fake.c` with a libusb thread that
  reads SNES (preferred) or USB mouse, per `DESIGN.md` §3.
- **Wave system / auto-attacks / state machine.** Needs the modules
  enumerated in `DESIGN.md` §6.2: `wave.c`, `autoatk.c`, `entity.c`, `ai.c`,
  `collision.c`, plus the `STATE_MENU/PLAYING/LEVELUP/DEAD` machine.
- **SDL simulator** (`sdl_sim.c`) so game logic can be developed without the
  board.
- **Real sprite art** replacing the placeholder solid squares in
  `gen_rom.py`. The format is documented in `DESIGN.md` §8.1–8.2.
- **`sprite_eval` semantics drift from spec.** The HW reads bit 47 of each
  64-bit sprite entry as an "active" bit (`sprite_eval.sv:32`); the design
  doc instead says sprites are hidden by writing sentinel coords. The C
  driver works around this by setting bit 7 of `flags` for visible sprites
  via `NML_FLAGS(...)`. Cleanup item: change `sprite_eval` to use the
  sentinel and drop the workaround, or formalise the active bit in the
  design doc.
- **Frame counter** in `STATUS[15:8]` is wired to a constant `8'd0` in
  `nml_gpu.sv:175` — needs a counter incremented on each VSYNC.

---

## 5. Troubleshooting

### Phase 1 — bitstream

| Symptom | Likely cause | Fix |
|---|---|---|
| `quartus_pgm` says "no hardware found" | USB Blaster not detected | `jtagconfig`; reconnect USB cable; check user is in `plugdev` |
| Compile error: "cannot find sprite_rom.hex" | hex files not generated, or `SEARCH_PATH` wrong | `cd hw && python3 gen_rom.py`; confirm `nml_gpu.qsf` has `set_global_assignment -name SEARCH_PATH ..` |
| Compile error: port name mismatch in `avalon_slave_iface` | older checkout missing the `score_reg` rename fix | git pull; the fix is in `nml_gpu.sv:167-178` |
| Monitor: "no signal" | wrong VGA pin assignments OR pixel clock not running | Verify pins in `nml_gpu.qsf` against the GHRD `.qsf` (DE1-SoC system CD); check LEDR[1] flickers; if not, the PLL register isn't being clocked — see next row |
| Monitor: "out of range" or constant black-and-white bars | pixel clock is at 50 MHz instead of 25 MHz | `pll_25mhz` not instantiated, or AUTO_GLOBAL_CLOCK_BUFFER off; check Quartus warnings for "register inferred from clock" |
| Monitor shows scrolling / tearing | reset is asserted continuously (KEY[0] held), or PLL is glitching | Confirm KEY[0] is high (released); upgrade `pll_25mhz.v` to an `altera_pll` IP if the divide-by-2 is unstable |
| Monitor shows correct sync but all-black | palette[0x20] not initialised or `$readmemh` failed silently | Check Quartus messages for "$readmemh: file not found"; regenerate hex; recompile |
| Monitor shows correct sync, grey background, but no sprites | sprite_table active bit (bit 47) is zero | Check `sprite_table.hex` contents — entries should start with `00008001…`; regenerate via `gen_rom.py` |

### Phase 2 — HPS / Linux

| Symptom | Likely cause | Fix |
|---|---|---|
| `nml_open: /dev/mem: Permission denied` | not root | `sudo ./nml_game` or `su -` first |
| `nml_open: mmap: Invalid argument` | Lightweight bridge isn't routed through Qsys | §6 |
| Game runs, no visual change on monitor | Bitstream loaded but Qsys isn't routing LW bridge to nml_gpu | §6; verify bridge base is `0xFF200000` and nml_gpu sits at offset 0 |
| `nml_commit_frame: timed out waiting for SWAP commit` | VBLANK/SWAP_PENDING not making it back through STATUS register read path | check that `vblank_in` and `swap_pending_in` are connected in nml_gpu.sv (they are, line 168) and that the slave iface read path returns them (line 188) |

---

## 6. Phase 2 prerequisite — adding the HPS-to-FPGA bridge

This is the largest outstanding work item. Because doing it correctly
requires interactive Qsys (Platform Designer) work, the steps below are a
walkthrough rather than a script. Plan ~3 hours the first time.

### 6.1 Start from the DE1-SoC Golden Hardware Reference Design (GHRD)

Don't build the HPS configuration from scratch. Download the GHRD from the
Terasic DE1-SoC system CD (or your TA's class-issued copy). It already has:

- HPS pin mux + DDR3 timings configured for the DE1-SoC.
- HPS-to-FPGA Lightweight bridge present in Qsys.
- Linux SD-card boot setup.

### 6.2 Patch the GHRD Qsys system to expose `nml_gpu`

In Platform Designer, open the GHRD `soc_system.qsys`:

1. Confirm the **Lightweight HPS-to-FPGA Bridge** is present and exported
   (master interface).
2. Add the `nml_gpu` Avalon slave as an external connection. Either:
   - Wrap `nml_gpu` as a Qsys component (write a `_hw.tcl`), or
   - Keep `nml_gpu` outside Qsys and connect the LW master conduit to its
     Avalon slave in the top-level wrapper. This is the simpler path; it's
     what `de1soc_top.sv` is structured for if you swap the standalone
     instantiation for a `soc_system` instance plus an `nml_gpu`
     instance with the LW master signals routed to its `avs_*` ports.
3. Set the LW bridge base address so `nml_gpu` lives at offset `0x0` of the
   bridge → HPS sees it at `0xFF200000` (the value `nml_gpu.h` already
   uses). If you place it elsewhere, update `NML_LWFPGA_BASE` in
   `sw/nml_gpu.h` accordingly.
4. **Generate** the Qsys system. This produces `soc_system.v` and a
   directory of submodule files.

### 6.3 Replace the standalone top with a Qsys-aware top

The current `de1soc_top.sv` is the standalone (no-HPS) wrapper used for
Phase 1. For Phase 2 you'll either:

- Modify `de1soc_top.sv` to instantiate `soc_system` (the Qsys output) plus
  `nml_gpu`, with the LW master signals from `soc_system` connected to the
  `avs_*` ports of `nml_gpu`. The HPS pins (DDR3, USB, UART, etc.) come
  from the GHRD top — copy them in.
- Or keep `de1soc_top.sv` for Phase 1 as a separate revision in
  `nml_gpu.qpf`, and create a second top file for Phase 2.

### 6.4 Update the .qsf with HPS pin assignments

The GHRD `.qsf` contains ~150 HPS pin location constraints. Append them to
`hw/quartus/nml_gpu.qsf` (or create a Phase-2-specific `.qsf`). Don't try
to derive these from the manual — just copy from the GHRD.

### 6.5 Recompile and reload

Rerun §1.4. You should now produce a `.rbf` that the Linux kernel on the
SD card can load via `fpga_manager`.

---

## 7. Quick reference

### One-liner Phase 1 build (after `gen_rom.py`)

```bash
( cd hw/quartus && quartus_sh --flow compile nml_gpu )                 \
  && quartus_pgm -m JTAG -o "p;hw/quartus/output_files/nml_gpu.sof@2"  \
  && echo "monitor should show the test scene now"
```

### Regenerating ROMs after editing palette or sprite layout

```bash
cd hw && python3 gen_rom.py
```

Then recompile in Quartus — `$readmemh` is read at synthesis time, not at
runtime.

### Building the C binary

```bash
cd sw
make             # cross-compile for the board
make terminal    # host-only debug build
make clean
```

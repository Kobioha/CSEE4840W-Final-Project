# No Man's Land -- Setup, Build, and Test Guide (DE1-SoC)

This guide takes you from a fresh `git pull` on the lab Linux machine to a
working VGA image on the monitor, and then on to running the C game on the
HPS. It is structured in three phases -- do them in order.

| Phase | Goal | Time | Hardware needed |
|------:|------|------|-----------------|
| 0 | Sanity-check on a host laptop | 10 min | none |
| 1 | **Smoke-test bitstream -- pixels on the monitor** | 60-90 min | DE1-SoC, VGA monitor, USB Blaster |
| 2 | HPS-driven C game on top of the FPGA | several hours | Phase 1 + DE1-SoC SD card with Linux |

> **Status of this checkpoint:**
> - **Phase 0:** complete.
> - **Phase 1 (standalone JTAG bitstream):** complete and verified on the
>   board -- sprites render correctly. The smoke-test build lives in `hw/`.
>   Three SystemVerilog bug fixes landed during bring-up: an `eval_done`
>   pulse from `sprite_eval` (was edge-detecting `active_mask`, which
>   silently failed when consecutive scanlines had the same sprite set), a
>   `WAIT_PIXEL` bubble in `sprite_fetch` to honour the sprite-ROM read
>   latency, and `vga_clk = ~pix_clk` in `nml_gpu.sv` so the ADV7123 DAC
>   samples R/G/B mid-cycle.
> - **Phase 2 (HPS-integrated build):** Platform Designer system is wired
>   and the kernel sees the peripheral. `nml_gpu` is integrated as an
>   Avalon slave on the HPS-to-FPGA Lightweight bridge via the `lab3-hw`
>   skeleton (sibling directory `nml_gpu_hw/`). `soc_system.rbf` and
>   `soc_system.dtb` are on the SD card; Linux boots and
>   `/proc/device-tree/sopc@0/bridge@0xc0000000/vga@0x100000000/compatible`
>   reads `csee4840,nml_gpu-1.0`. **Next step is running the C driver.**

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
`sprite_table.hex` -- all four are required for the bitstream to show pixels.

---

## 1. Phase 1 -- smoke-test bitstream on the DE1-SoC

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

Or open `nml_gpu.qpf` in the Quartus GUI and click **Processing -> Start
Compilation**. Compilation should finish with no errors. Common warnings to
ignore:

- "Width mismatch" notes inside the SystemVerilog: harmless if the file
  compiles.
- "No exact match for clock constraint": expected -- we don't ship an SDC
  file for the smoke-test bitstream. The 25 MHz divided clock is well below
  Cyclone V's slowest speed grade Fmax for the small logic in `nml_gpu`.

If compilation fails, the most likely causes are listed in Sec. 5
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
  - A dark-grey background made of 8x8 tiles.
  - A green 16x16 square (player) near the centre.
  - Two red 16x16 squares (enemies) in the upper half.
  - A small yellow 4x4 dot (bullet) below the player.

If any of those are missing, jump to Sec. 5.

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

## 2. Phase 2 -- HPS-integrated build, Linux boot, run the C game

**Goal:** integrate `nml_gpu` as an Avalon slave on the HPS-to-FPGA
Lightweight (LW) bridge, generate `soc_system.rbf` + `soc_system.dtb`, boot
Linux from the SD card, and run the C game binary against the peripheral.

The Phase 1 bitstream in `hw/` is **standalone** (JTAG-loaded, no HPS) and
keeps working as a fast iteration path for GPU bugs. It is **not** what
Linux runs against -- for Phase 2 we use a separate build directory,
`nml_gpu_hw/`, that wraps the same SystemVerilog sources in a Platform
Designer system containing the HPS and the LW bridge.

### 2.1 Set up `nml_gpu_hw/` from the Lab 3 hardware skeleton

The Lab 3 `lab3-hw.tar.gz` (class website) ships a turnkey Platform
Designer system: HPS configured for the DE1-SoC, DDR3 timings, LW bridge,
SDRAM pin assignments, and a Makefile that drives `qsys -> quartus -> rbf ->
dtb`. We reuse that as our Phase 2 build harness, swapping the lab's
`vga_ball` peripheral for `nml_gpu`.

On the lab Linux machine:

```bash
tar zxf lab3-hw.tar.gz
mv lab3-hw nml_gpu_hw
cd nml_gpu_hw
```

Copy the project's SystemVerilog and hex files into the working dir so
Platform Designer and Quartus can find them:

```bash
PROJ=~/path/to/CSEE4840W-Final-Project/hw
cp $PROJ/nml_gpu.sv             .
cp $PROJ/avalon_slave_iface.sv  .
cp $PROJ/vga_timing.sv          .
cp $PROJ/sprite_eval.sv         .
cp $PROJ/sprite_fetch.sv        .
cp $PROJ/compositor.sv          .
cp $PROJ/pll_25mhz.v            .
cp $PROJ/sprite_rom.hex $PROJ/tile_rom.hex \
   $PROJ/palette.hex $PROJ/sprite_table.hex .
```

Do **not** copy `de1soc_top.sv` -- `nml_gpu_hw/soc_system_top.sv` replaces
it. Keep both `hw/` and `nml_gpu_hw/` in the repo; they share the SV
sources (sync manually for now, or symlink).

### 2.2 Define `nml_gpu` as a Platform Designer component

```bash
qsys-edit soc_system.qsys
```

In Platform Designer:

1. **File -> New Component**.
2. **Component Type tab:** Name `nml_gpu`, Display Name "NML GPU".
3. **Files tab:** *Add File* every source -- `nml_gpu.sv`,
   `avalon_slave_iface.sv`, `vga_timing.sv`, `sprite_eval.sv`,
   `sprite_fetch.sv`, `compositor.sv`, `pll_25mhz.v` -- into the
   **Synthesis Files** list. Click **Analyze Synthesis Files**. Set
   **Top-Level Module** to `nml_gpu` (it will only appear after all the
   above are in the list and analyze succeeds).
4. **Signals & Interfaces tab:**
   - `clock` interface contains `clk` (Signal Type `clk`).
   - `reset` interface contains `reset_n` (Signal Type `reset_n`,
     **polarity LOW**, Synchronous Edges NONE, Associated Clock `clock`).
   - `avalon_slave_0` (Avalon Memory Mapped Slave): set Associated Clock
     `clock`, Associated Reset `reset`. Drag in the seven `avs_*` signals,
     map each to its standard Signal Type:
     - `avs_address` -> `address`
     - `avs_read` -> `read`
     - `avs_write` -> `write`
     - `avs_writedata` -> `writedata`
     - `avs_byteenable` -> `byteenable`
     - `avs_readdata` -> `readdata`
     - `avs_waitrequest` -> `waitrequest`
   - Create a new **Conduit** named `vga`. Drag the eight `vga_*` signals
     in. Signal Type = lowercase part after `vga_` (`vga_r` -> `r`,
     `vga_blank_n` -> `blank_n`, etc.).
5. Confirm Messages shows **0 errors, 0 warnings**. **Finish -> Yes,
   Save**. This writes `nml_gpu_hw.tcl`.
6. Edit `nml_gpu_hw.tcl` and append after the `module nml_gpu` block:

   ```tcl
   set_module_assignment embeddedsw.dts.vendor "csee4840"
   set_module_assignment embeddedsw.dts.name   "nml_gpu"
   set_module_assignment embeddedsw.dts.group  "vga"
   ```

   These three lines are what makes the device tree node carry
   `compatible = "csee4840,nml_gpu-1.0"`. Forget them and the kernel
   won't recognise the peripheral.

### 2.3 Connect `nml_gpu_0` into the Qsys system

Still in Platform Designer's main `soc_system` window:

1. If the lab3 skeleton instantiated a `vga_ball_0`, **right-click ->
   Remove**. Two components both exporting a `vga` conduit will refuse to
   generate.
2. Library -> Project -> **NML GPU** -> **+ Add...**. Default instance name
   `nml_gpu_0`.
3. Connections:
   - `nml_gpu_0.clock` -> `clk_0.clk`
   - `nml_gpu_0.reset` -> `clk_0.clk_reset`
   - `nml_gpu_0.avalon_slave_0` -> `hps_0.h2f_lw_axi_master`
4. Double-click `nml_gpu_0.vga` in the **Export** column and name the
   export `vga`.
5. **File -> Save**, then **Generate HDL...** (defaults), then close.

### 2.4 Wire the VGA conduit out to the DE1-SoC pins

Edit `nml_gpu_hw/soc_system_top.sv`. Inside the `soc_system soc_system0
( ... )` instantiation, add at the end of the port list (with a comma on
the previous-last line):

```systemverilog
.vga_r       (VGA_R),
.vga_g       (VGA_G),
.vga_b       (VGA_B),
.vga_clk     (VGA_CLK),
.vga_hs      (VGA_HS),
.vga_vs      (VGA_VS),
.vga_blank_n (VGA_BLANK_N),
.vga_sync_n  (VGA_SYNC_N)
```

Then near the bottom of the file, **delete** the two `assign` statements
that drive `VGA_*` from the lab3 skeleton -- leaving them in causes
multi-driver errors at compile.

Sanity check:

```bash
grep -n VGA_R soc_system_top.sv
```

Should show exactly two lines: the `output ... VGA_R` declaration, and
your `.vga_r (VGA_R),` inside the instance.

### 2.5 Build the bitstream and device tree

From `nml_gpu_hw/`:

```bash
make qsys-clean ; make qsys     # regenerate Platform Designer output
make quartus                    # full P&R, ~10-20 min
make rbf                        # output_files/soc_system.rbf
make dtb                        # soc_system.dtb (+ soc_system.dts for inspection)
```

Sanity-check the device tree contains `nml_gpu`:

```bash
grep -A4 nml_gpu soc_system.dts
```

Expect:

```
nml_gpu_0: vga@0x100000000 {
    compatible = "csee4840,nml_gpu-1.0";
    reg = <0x00000001 0x00000000 0x00010000>;
    clocks = <&clk_0>;
};
```

The node name is `vga@0x100000000` (from `embeddedsw.dts.group "vga"`) -- 
the kernel exposes it as `vga@0x100000000` under the bridge. The address
span is rounded up to `0x10000` even though `avs_address` is only 14 bits
(`0x4000`).

Quartus's `$readmemh` should find the four hex files in
`nml_gpu_hw/`. If you see "Can't find $readmemh file ...", add
`set_global_assignment -name SEARCH_PATH "."` to `soc_system.tcl` near
the other `set_global_assignment` lines and rebuild.

### 2.6 Boot the board from the new files

The DE1-SoC's U-Boot loads exactly `soc_system.rbf` and `soc_system.dtb`
from the FAT (boot) partition of the SD card. So both files must land
there with those exact names.

Pop the SD card out and put it in a USB SD-card reader on a workstation.
Then either via Finder/file manager (drag-and-drop overwrite) or
command-line:

```bash
sudo cp -f nml_gpu_hw/output_files/soc_system.rbf /Volumes/<boot>/
sudo cp -f nml_gpu_hw/soc_system.dtb              /Volumes/<boot>/
sync
```

Verify the dtb that actually landed has your peripheral (works without
`dtc` because the compatible string is plain text inside the binary):

```bash
strings /Volumes/<boot>/soc_system.dtb | grep -i nml
# -> csee4840,nml_gpu-1.0
```

Eject properly (Finder -> Eject; on Linux `sudo umount`), insert into
DE1-SoC, plug mini-USB UART to workstation, open serial console:

```bash
sudo screen /dev/ttyUSB0 115200    # or /dev/ttyUSB1, whichever shows up
```

Power on. Linux should boot to a `login:` prompt; `root`, no password.

Verify the kernel sees `nml_gpu`:

```bash
ls /proc/device-tree/sopc@0/bridge@0xc0000000/
# expect:  vga@0x100000000   among the entries
cat /proc/device-tree/sopc@0/bridge@0xc0000000/vga@0x100000000/compatible
# -> csee4840,nml_gpu-1.0
```

The VGA monitor should already show the sprite scene -- `ctrl_enable`
defaults to 1 at reset, so the bitstream draws pixels the moment the FPGA
configures, before any HPS code runs. **If you see the player + enemies +
bullet on the monitor at this point, the entire HW path including the LW
bridge is working.**

### 2.7 Build the C binary on the lab machine

```bash
cd sw
make             # cross-compiles for armhf using arm-linux-gnueabihf-gcc
ls -lh nml_game  # armhf ELF
```

If `arm-linux-gnueabihf-gcc` isn't installed:

```bash
sudo apt install gcc-arm-linux-gnueabihf
```

Or build natively on the board:

```bash
scp sw/*.c sw/*.h sw/Makefile root@<board>:/root/sw/
ssh root@<board>
cd sw && make native
```

### 2.8 Run the game

```bash
scp sw/nml_game root@<board>:/root/
ssh root@<board>
./nml_game        # must run as root for /dev/mem mmap
```

Expected behaviour:

- Monitor shows the same scene as Phase 1.
- The player sprite (green square) drifts right for ~1.3 s, then left for
  ~1.3 s, then back -- that's `input_fake.c` running its hardcoded test
  pattern.
- Yellow bullets fire upward periodically.
- Red enemies spawn at the top and chase the player.
- After the player is hit enough times, `GAME OVER. Final score: N`
  prints on the console and the binary exits.

The driver mmaps `/dev/mem` at `NML_LWFPGA_BASE = 0xFF200000` (set in
`sw/nml_gpu.h`). That's the LW-bridge base from the HPS side; because we
placed `nml_gpu_0` at offset 0 of the bridge in Platform Designer, no
header change is needed. If you ever move it, update `NML_LWFPGA_BASE`
to match.

### 2.9 Check frame timing

While the game is running, in another shell:

```bash
ssh root@<board> top -n 1 -b | head
```

CPU should be < 5 % -- the loop spends most of its time in `nanosleep` and
the `nml_commit_frame` SWAP poll. If CPU is pegged, the SWAP poll is timing
out (likely cause: the bitstream isn't actually loaded -- re-do Sec. 2.3).

---

## 3. File map

```
.
|-- DESIGN.md                 -- requirements doc (Section 5 = register map)
|-- SETUP.md                  -- this file
|-- README.md
|-- hw/                       -- Phase 1 standalone (JTAG, no HPS) build
|   |-- nml_gpu.sv            -- top peripheral (Avalon slave + RAMs + pipeline)
|   |-- avalon_slave_iface.sv -- register decode
|   |-- vga_timing.sv         -- 640x480@60 timing
|   |-- sprite_eval.sv        -- per-scanline sprite priority sort (eval_done pulse out)
|   |-- sprite_fetch.sv       -- HBLANK sprite fetch with WAIT_PIXEL ROM-read bubble
|   |-- compositor.sv         -- tile + sprite priority MUX + palette lookup
|   |-- pll_25mhz.v           -- 50->25 MHz divide-by-2 (smoke-test PLL)
|   |-- de1soc_top.sv         -- DE1-SoC pins + nml_gpu instantiation (no HPS)
|   |-- gen_rom.py            -- emits the four hex files below
|   |-- sprite_rom.hex        -- 16 KB, $readmemh into sprite_rom
|   |-- tile_rom.hex          --  4 KB, $readmemh into tile_rom
|   |-- palette.hex           -- 256 x 24b, $readmemh into palette_ram
|   |-- sprite_table.hex      -- 32 x 64b, $readmemh into sprite_table_active/shadow
|   `-- quartus/
|       |-- nml_gpu.qpf       -- Quartus project
|       `-- nml_gpu.qsf       -- pin assignments + file list + settings
|-- nml_gpu_hw/               -- Phase 2 HPS-integrated build (lab3-hw skeleton)
|   |-- soc_system.qsys       -- Platform Designer system (HPS + LW bridge + nml_gpu_0)
|   |-- soc_system.tcl        -- Quartus project setup (regenerates .qpf/.qsf)
|   |-- soc_system_top.sv     -- top wrapper, instantiates soc_system + DE1-SoC pins
|   |-- soc_system.srf        -- suppressed Quartus warnings
|   |-- soc_system_board_info.xml -- sopc2dts board metadata
|   |-- nml_gpu_hw.tcl        -- nml_gpu component descriptor (incl. dts assignments)
|   |-- Makefile              -- qsys / quartus / rbf / dtb targets
|   |-- nml_gpu.sv ...          -- copies of hw/*.sv + .v + .hex (sync manually)
|   |-- output_files/soc_system.rbf  -- generated bitstream -> SD card boot partition
|   `-- soc_system.dtb        -- generated device tree -> SD card boot partition
`-- sw/
    |-- main.c                -- 60 Hz game loop (FPGA or terminal build)
    |-- game.c, game.h        -- entity pool, AI, collision, score
    |-- input.h               -- input bitmask abstraction
    |-- input_fake.c          -- hardcoded test pattern (Phase 2 needs libusb)
    |-- render.h              -- render_frame(const game_t*)
    |-- render.c              -- entity -> nml_gpu sprite-table writer
    |-- render_terminal.c     -- alt: prints sprite slots to stdout
    |-- nml_gpu.h             -- driver: register offsets, sprite struct, prototypes
    |-- nml_gpu.c             -- driver: /dev/mem mmap + register writers
    `-- Makefile              -- targets: all (default), native, terminal, clean
```

---

## 4. What's done vs. what's outstanding

### Done

- All compositor SystemVerilog modules.
- 50->25 MHz pixel clock (`pll_25mhz.v`).
- `$readmemh` pre-init of palette + initial sprite table -- bitstream alone
  shows a complete frame.
- DE1-SoC top-level wrapper and Quartus project for Phase 1 (`hw/`).
- C driver (`nml_gpu.h` / `nml_gpu.c`) covering all register regions.
- Render path (`render.c`) translating game entities to sprite-table writes.
- Makefile with cross-compile, native, and terminal targets.
- `gen_rom.py` so future art / level changes don't require manual hex
  editing.
- Phase 1 SystemVerilog bug fixes verified on the board:
  - `eval_done` is now an explicit one-cycle pulse from `sprite_eval`
    (latched in `nml_gpu.sv` until the next `eval_strobe`). The previous
    edge-detector on `active_mask` silently failed for any scanline where
    the same set of sprites stayed active -- i.e., 15 of every 16 lines.
  - `sprite_fetch` has a new `WAIT_PIXEL` state inserted between
    `READ_PIXEL` and `WRITE_PIXEL` so the sprite-ROM read latency lines
    up; previously each sprite was shifted by 1 px with the right border
    missing.
  - `vga_clk = ~pix_clk` so the ADV7123 DAC samples R/G/B mid-cycle.
- Phase 2 hardware path: `nml_gpu` integrated as Avalon slave on the LW
  bridge via Platform Designer, lives at `0xFF200000` from the HPS side.
  Linux boots from the new `soc_system.rbf` + `soc_system.dtb`; kernel
  device tree exposes it at `vga@0x100000000` with
  `compatible = "csee4840,nml_gpu-1.0"`. Standalone bitstream produces
  the correct sprite scene the moment the FPGA configures.

### Outstanding (per `DESIGN.md`)

- **End-to-end HPS run.** Sec. 2.7-2.8: cross-compile `sw/`, scp to the
  board, run `./nml_game`, confirm the player drifts under
  `input_fake.c`'s test pattern. The hardware path is ready; this is the
  remaining mile.
- **HUD overlay** in `compositor.sv` (HP bar, wave, score). Reads from
  `player_pos`, `player_stats`, `score_reg`, `kill_count` already exposed
  by `avalon_slave_iface`.
- **Real input device.** Replace `input_fake.c` with a libusb thread that
  reads SNES (preferred) or USB mouse, per `DESIGN.md` Sec. 3.
- **Wave system / auto-attacks / state machine.** Needs the modules
  enumerated in `DESIGN.md` Sec. 6.2: `wave.c`, `autoatk.c`, `entity.c`, `ai.c`,
  `collision.c`, plus the `STATE_MENU/PLAYING/LEVELUP/DEAD` machine.
- **SDL simulator** (`sdl_sim.c`) so game logic can be developed without the
  board.
- **Real sprite art** replacing the placeholder solid squares in
  `gen_rom.py`. The format is documented in `DESIGN.md` Sec. 8.1-8.2.
- **`sprite_eval` semantics drift from spec.** The HW reads bit 47 of each
  64-bit sprite entry as an "active" bit (`sprite_eval.sv:32`); the design
  doc instead says sprites are hidden by writing sentinel coords. The C
  driver works around this by setting bit 7 of `flags` for visible sprites
  via `NML_FLAGS(...)`. Cleanup item: change `sprite_eval` to use the
  sentinel and drop the workaround, or formalise the active bit in the
  design doc.
- **Frame counter** in `STATUS[15:8]` is wired to a constant `8'd0` in
  `nml_gpu.sv` -- needs a counter incremented on each VSYNC.
- **Cleanup duplicated SV sources.** `hw/` and `nml_gpu_hw/` both hold
  copies of `nml_gpu.sv` etc. Add a sync rule (or symlinks) so editing one
  can't drift from the other.

---

## 5. Troubleshooting

### Phase 1 -- bitstream

| Symptom | Likely cause | Fix |
|---|---|---|
| `quartus_pgm` says "no hardware found" | USB Blaster not detected | `jtagconfig`; reconnect USB cable; check user is in `plugdev` |
| Compile error: "cannot find sprite_rom.hex" | hex files not generated, or `SEARCH_PATH` wrong | `cd hw && python3 gen_rom.py`; confirm `nml_gpu.qsf` has `set_global_assignment -name SEARCH_PATH ..` |
| Compile error: port name mismatch in `avalon_slave_iface` | older checkout missing the `score_reg` rename fix | git pull; the fix is in `nml_gpu.sv:167-178` |
| Monitor: "no signal" | wrong VGA pin assignments OR pixel clock not running | Verify pins in `nml_gpu.qsf` against the GHRD `.qsf` (DE1-SoC system CD); check LEDR[1] flickers; if not, the PLL register isn't being clocked -- see next row |
| Monitor: "out of range" or constant black-and-white bars | pixel clock is at 50 MHz instead of 25 MHz | `pll_25mhz` not instantiated, or AUTO_GLOBAL_CLOCK_BUFFER off; check Quartus warnings for "register inferred from clock" |
| Monitor shows scrolling / tearing | reset is asserted continuously (KEY[0] held), or PLL is glitching | Confirm KEY[0] is high (released); upgrade `pll_25mhz.v` to an `altera_pll` IP if the divide-by-2 is unstable |
| Monitor shows correct sync but all-black | palette[0x20] not initialised or `$readmemh` failed silently | Check Quartus messages for "$readmemh: file not found"; regenerate hex; recompile |
| Monitor shows correct sync, grey background, but no sprites | sprite_table active bit (bit 47) is zero | Check `sprite_table.hex` contents -- entries should start with `00008001...`; regenerate via `gen_rom.py` |

### Phase 2 -- HPS / Linux

| Symptom | Likely cause | Fix |
|---|---|---|
| `screen` exits immediately | Wrong device path / permission / port busy | `ls /dev/ttyUSB* /dev/ttyACM*`; `dmesg \| tail` to see what attached when you plugged in; `sudo screen /dev/ttyUSB0 115200` |
| U-Boot: "can't find valid device tree" | `soc_system.dtb` missing or under a non-default name on FAT partition | Pop SD card, drop a valid `soc_system.dtb` onto the boot partition, sync, eject |
| Linux boots from old `vga_ball` dtb instead of new one | Either copy didn't land or there are multiple dtbs and U-Boot picked the wrong one | `strings /Volumes/<boot>/soc_system.dtb \| grep nml` -- if it shows `vga_ball` you've still got the old one. Force overwrite with `cp -f`, `sync`, eject properly. |
| `make dtb` produces dts without `nml_gpu` node | `set_module_assignment` lines missing from `nml_gpu_hw.tcl`, or stale dts/dtb on disk | `grep set_module_assignment nml_gpu_hw.tcl` (expect 3 lines: vendor / name / group); `rm -f soc_system.dtb soc_system.dts ; make dtb` |
| `nml_open: /dev/mem: Permission denied` | Not root | `sudo ./nml_game` or `su -` first |
| `nml_open: mmap: Invalid argument` | LW bridge isn't routed through Qsys, or bridge base mismatched | Check `cat /proc/device-tree/sopc@0/bridge@0xc0000000/vga@0x100000000/reg`; reconcile with `NML_LWFPGA_BASE` in `sw/nml_gpu.h` (default `0xFF200000`) |
| Game runs, no visual change on monitor | Bitstream loaded but Qsys isn't routing LW bridge to nml_gpu | Verify `nml_gpu_0` is connected to `hps_0.h2f_lw_axi_master` in `soc_system.qsys`; check it's at offset 0 of the bridge; rebuild |
| Bitstream not loaded into FPGA at boot | U-Boot's `fpga_load` failed, often because rbf is missing or wrong size | Watch U-Boot console; expect `fatload mmc 0:1 ... soc_system.rbf` to print "~7 MB read"; if not, file is missing/corrupt |
| `nml_commit_frame: timed out waiting for SWAP commit` | VBLANK/SWAP_PENDING not making it back through STATUS register read path | Check that `vblank_in` and `swap_pending_in` are connected in `nml_gpu.sv` and that the slave iface read path returns them |

---

## 6. Notes on the Phase 2 build harness (`nml_gpu_hw/`)

The full Platform Designer integration procedure lives in Sec. 2.1-2.5
above; this section just records the design choices and gotchas worth
remembering when the system needs to be rebuilt or extended.

### 6.1 Why `lab3-hw`, not the GHRD

We considered starting from the Terasic DE1-SoC Golden Hardware Reference
Design and porting `nml_gpu` into it directly. The lab3 skeleton is a
simpler version of the same idea (HPS + LW bridge + Quartus build
harness) that's already known-good for the class image, so the integration
takes minutes instead of hours.

### 6.2 Where things live

- **Custom-component descriptor:** `nml_gpu_hw/nml_gpu_hw.tcl`. Lists
  every SystemVerilog source, the interface mappings (avalon_slave_0,
  conduit `vga`, clock, reset), and the three `embeddedsw.dts.*`
  assignments that produce the kernel's compatible string.
- **Platform Designer system:** `nml_gpu_hw/soc_system.qsys`. Contains
  `clk_0`, `hps_0`, and `nml_gpu_0`, with `nml_gpu_0`'s Avalon slave
  connected to `hps_0.h2f_lw_axi_master`.
- **Top-level wrapper:** `nml_gpu_hw/soc_system_top.sv`. Wires the `vga`
  conduit to the DE1-SoC `VGA_*` pins. The two `assign` statements that
  drive `VGA_*` from the lab3 skeleton are deleted (otherwise: multi-driver
  errors).
- **Build script:** `nml_gpu_hw/Makefile`. Targets `qsys`, `quartus`,
  `rbf`, `dtb`, plus `qsys-clean` to force regeneration.

### 6.3 Address layout

`nml_gpu` advertises a 14-bit byte address (`avs_address[13:0]` => 16 KB
span). Platform Designer rounded up to the next power of two >= requested,
so the LW-bridge mapping uses a `0x10000` (64 KB) window. From the HPS
side, this is `0xFF200000`-`0xFF20FFFF`. Only the low 16 KB are decoded
inside `nml_gpu`; reads/writes to the upper 48 KB hit nothing. The C
driver's `NML_LWFPGA_BASE = 0xFF200000` and per-region offsets in
`sw/nml_gpu.h` are unaffected by the rounding.

### 6.4 Things that bit us during bring-up

- **Component Editor failed to recognise `nml_gpu` as a top-level module
  unless every dependency was in the Synthesis Files list.** With only a
  subset, it silently fell back to `avalon_slave_iface` as the analyzed
  top, generating Avalon-spec errors on `bg_scroll`, `ctrl_enable`, etc.
  Fix: add all 7 source files (`nml_gpu.sv`, `avalon_slave_iface.sv`,
  `vga_timing.sv`, `sprite_eval.sv`, `sprite_fetch.sv`, `compositor.sv`,
  `pll_25mhz.v`) and re-Analyze.
- **Auto-detection placed `vga_r/g/b` (8-bit) into `avalon_slave_0` as
  `readdata`,** producing "readdata appears 4 times" and width-mismatch
  errors. Fix: drag the eight `vga_*` signals into the `vga` Conduit and
  set Signal Type to the lowercase part after `vga_`.
- **`make dtb` happily reused a stale `soc_system.dts`.** If the dts
  doesn't reflect a recent `nml_gpu_hw.tcl` change, force a regen with
  `rm -f soc_system.dtb soc_system.dts ; make dtb`.
- **U-Boot only loads files named exactly `soc_system.rbf` and
  `soc_system.dtb`** from the FAT partition. Any other name silently
  doesn't load (or, worse, U-Boot continues with no fpga config and Linux
  boots against an unconfigured fabric).
- **Drag-and-drop to the SD card on macOS doesn't always commit until
  Eject.** Verify the dtb actually landed with
  `strings /Volumes/<boot>/soc_system.dtb | grep nml` before
  re-inserting.

---

## 7. Quick reference

### One-liner Phase 1 build (standalone JTAG, after `gen_rom.py`)

```bash
( cd hw/quartus && quartus_sh --flow compile nml_gpu )                 \
  && quartus_pgm -m JTAG -o "p;hw/quartus/output_files/nml_gpu.sof@2"  \
  && echo "monitor should show the test scene now"
```

### Phase 2 -- rebuild rbf + dtb after editing `nml_gpu_hw/`

```bash
cd nml_gpu_hw
make qsys-clean ; make qsys     # only if .qsys / .tcl changed
make quartus                    # ~10-20 min
make rbf
make dtb
```

Then drop `output_files/soc_system.rbf` and `soc_system.dtb` onto the
SD card boot partition (overwrite, eject properly).

### Phase 2 -- dtb-only iteration (no Quartus rerun)

```bash
cd nml_gpu_hw
rm -f soc_system.dtb soc_system.dts
make dtb
strings soc_system.dtb | grep nml          # sanity-check
```

### Regenerating ROMs after editing palette or sprite layout

```bash
cd hw && python3 gen_rom.py
# then re-copy hex files into nml_gpu_hw/ if Phase 2 build is also relevant:
cp sprite_rom.hex tile_rom.hex palette.hex sprite_table.hex ../nml_gpu_hw/
```

`$readmemh` is read at synthesis time, not at runtime -- re-run the
appropriate Quartus build.

### Building the C binary

```bash
cd sw
make             # cross-compile for the board (armhf)
make terminal    # host-only debug build
make clean
```

### Verify peripheral on a booted board

```bash
ls /proc/device-tree/sopc@0/bridge@0xc0000000/
cat /proc/device-tree/sopc@0/bridge@0xc0000000/vga@0x100000000/compatible
# -> csee4840,nml_gpu-1.0
```

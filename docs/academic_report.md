# No Man's Land: A Custom SystemVerilog GPU and Game on the DE1-SoC

**CSEE 4840 -- Embedded System Design, Spring 2026**

Rohit Biswas (rb3908), Kambinachi Obioha (kno2117), Nicola Paparella (np2953)

Instructor: Prof. Stephen Edwards

---

## Abstract

No Man's Land is a top-down WWI horde-survival game implemented on the Terasic
DE1-SoC. The project partitions the system between a custom SystemVerilog GPU
peripheral (`nml_gpu`) running in the Cyclone V FPGA fabric and a C game on
the ARM Cortex-A9 hard processor. The GPU drives a 640x480 @ 60 Hz VGA output
with tile-mapped background, 32 hardware sprites with per-scanline priority
sorting, a 256-entry palette, and an on-chip HUD overlay. The HPS writes the
sprite table, palette, tile map, and player-state registers through a 16 KB
Avalon-MM lightweight slave and signals a tear-free buffer swap at VBLANK.
Two build flows exist: a fabric-only smoke test (`hw/quartus`) that
confirms the rendering pipeline by initialising the memories from `$readmemh`
hex files, and an HPS-integrated Platform Designer system (`nml_gpu_hw/`)
that mounts the peripheral on the lightweight bridge and exposes it to a
Linux userspace driver via `/dev/mem`. The smoke-test bitstream is verified
on hardware. The HPS-integrated build boots and runs the game loop, but at
the time of writing exhibits a banded-stripe rendering artefact and a broken
Game-Over screen that are still under debugging. This report covers the
architecture, register map, build flow, current state, and open issues.

\newpage
\tableofcontents
\newpage

## 1. Introduction

The course's final-project remit is to design and integrate a custom
SystemVerilog peripheral that drives a VGA display and to write a C
application that uses the peripheral to do something interesting. We chose a
WWI-themed horde-survival game on the principle that it stresses both
sides: the FPGA must composite many sprites per scanline at pixel rate, and
the CPU must run wave logic, enemy AI, projectile physics, collision
checks, an auto-attack system, and a HUD-driven UI within a 16.67 ms frame
budget.

The player controls a soldier in a trench at the bottom of the screen.
Waves of enemies spawn at the top and march down. The player fires bullets
in four directions, picks up ammo drops, and at the end of each wave
selects an auto-attack upgrade (mortar, mustard gas, or artillery). The
goal is to survive as many waves as possible.

Input is a SNES-style USB gamepad (DragonRise generic, VID 0079:0011) read
through Linux evdev. A keyboard fallback was explicitly disallowed by the
instructor; we kept a deterministic stub input (`input_fake.c`) for off-line
development and demos when the controller is unavailable.

The deliverables and supporting documents are:

* `DESIGN.md` -- the original design document with system block diagram,
  register map, and game design.
* `SETUP.md` -- bring-up instructions: host sanity check, JTAG smoke test,
  and HPS integration on the SD-card image.
* This report and the companion internal deep-dive
  (`docs/internal_deep_dive.md`).

The rest of the report walks through the system architecture (Sec. 2),
hardware design (Sec. 3), software design (Sec. 4), the hardware-software
interface (Sec. 5), build and toolchain (Sec. 6), verification (Sec. 7), results (Sec. 8),
challenges and what we did about them (Sec. 9), and the open work (Sec. 10).

## 2. System Architecture

The system is partitioned along the obvious line: anything that runs at
pixel rate is in hardware; everything else is in software. The block
diagram is shown in Figure 1.

```
+--------------------------+                       +-------------------+
| HPS (ARM Cortex-A9 dual) |                       | FPGA fabric       |
|                          |                       |                   |
|  user app: nml_game      |  Avalon-MM LW bridge  |  nml_gpu          |
|  game loop, AI, collision| <-----  50 MHz  -----> |  - Avalon slave  |
|  wave system, auto-atks  |   32-bit, 14-bit addr |  - sprite engine  |
|  /dev/mem mmap driver    |                       |  - compositor     |
|                          |                       |  - palette + ROMs |
+------------+-------------+                       +---------+---------+
             |                                               |
             |       USB SNES gamepad (evdev)                | VGA 24-bit RGB
             |       /dev/input/event0                       | + HS/VS/CLK
             v                                               v
        controller                                       VGA monitor
```

The HPS reads input from the USB gamepad, ticks the game state at 60 Hz,
writes the resulting sprite table, palette, and tile map into FPGA memory
through the Avalon-MM lightweight slave, and signals a frame swap by
writing the `CTRL.SWAP` bit. The FPGA latches the new state at the next
VSYNC and continues to drive the monitor without ever stalling. A 50 MHz
Avalon clock and a 25 MHz pixel clock are the only two clock domains in
the design; both derive from the board's 50 MHz oscillator.

Frame budget at 60 Hz is 16.67 ms. Per VGA frame at 640x480 there are
800 x 525 = 420,000 pixel cycles at 25 MHz (16.8 ms wall-clock), of which
640 x 480 = 307,200 are visible. The 96-pixel horizontal blanking and
multi-line vertical blanking give the sprite engine the time it needs to
prepare each scanline.

## 3. Hardware Design

The hardware lives in [`hw/`](../hw/) and consists of seven SystemVerilog
modules plus one Verilog clock divider. A separate Platform Designer
project under [`nml_gpu_hw/`](../nml_gpu_hw/) wraps the same RTL as a Qsys
component for HPS integration. The hierarchy is:

```
de1soc_top (or soc_system_top for Phase 2)
  nml_gpu
    pll_25mhz                   50 MHz -> 25 MHz pixel clock
    vga_timing                  pixel/line counters, HSYNC/VSYNC/blanking
    avalon_slave_iface          register decode + write fan-out
    sprite_eval                 per-scanline priority sort (top 8)
    sprite_fetch                sprite ROM read + line-buffer fill
    compositor                  4-stage tile + sprite + HUD pipeline
    (inferred RAMs/ROMs)        sprite table, palette, tile map,
                                sprite ROM, tile ROM, two line buffers
```

The remainder of this section walks through each block.

### 3.1 Pixel clock generation (`pll_25mhz.v`)

The VGA standard for 640x480 @ 60 Hz specifies a 25.175 MHz pixel clock.
The Cyclone V on-chip PLL cannot produce that exact frequency from the
board's 50 MHz reference, so we use 25 MHz, which monitors in the lab
tolerate without issue. For the Phase 1 smoke test, the divider is a
single D flip-flop that toggles on every 50 MHz rising edge
(`hw/pll_25mhz.v:21-26`). Quartus promotes the divided register output
onto a global clock network when it's used as a clock downstream, which is
exactly how it's consumed in [`hw/nml_gpu.sv:194`](../hw/nml_gpu.sv) and
elsewhere.

The Phase 2 upgrade path is to replace the divider with an `altera_pll`
IP instance. The port names (`refclk`, `rst`, `outclk_0`) already match
the `altera_pll` defaults so swapping in the IP is a drop-in.

The board's VGA DAC (ADV7123) samples R/G/B on its own rising edge. We
drive `vga_clk` as the inversion of the internal pixel clock
(`hw/nml_gpu.sv:38`) so the DAC samples in the middle of the FPGA's hold
window. This avoids hold-time violations into the DAC without adding a
register stage.

### 3.2 VGA timing (`vga_timing.sv`)

`vga_timing` counts a horizontal axis from 0 to 799 and a vertical axis
from 0 to 524, producing standard `640x480 @ 60 Hz` parameters
(`hw/vga_timing.sv:11-21`):

| Axis | Active | Front porch | Sync | Back porch | Total |
|------|--------|-------------|------|------------|-------|
| H    | 640    | 16          | 96   | 48         | 800   |
| V    | 480    | 10          | 2    | 33         | 525   |

`hsync` and `vsync` are active low, matching the VGA spec for this mode.
`hblank` is high when `h_count >= 640`; `vblank` is high when
`v_count >= 480`; `visible = !hblank && !vblank`.

### 3.3 Sprite engine

#### 3.3.1 `sprite_eval.sv`

`sprite_eval` is a small FSM (`IDLE -> FETCH -> EVAL -> DONE -> IDLE`) that
walks the 32-entry sprite table once per scanline and selects the top
eight sprites by priority that intersect the *next* scanline (`y + 1`).
The state machine starts when the top-level asserts `eval_strobe` at
`x == 642`, two pixels into HBLANK (`hw/nml_gpu.sv:248`). Each sprite is
fetched in `FETCH` and tested in `EVAL`; the test is

```systemverilog
intersects = spr_active &&
             (next_scanline >= spr_y) &&
             (next_scanline <  (spr_y + 16));
```

(`hw/sprite_eval.sv:39-41`). All sprites are 16x16. The signed Y compare
permits sprites to sit partly above the screen.

The insertion sort is fully unrolled (`hw/sprite_eval.sv:77-94`): each
cycle, the incoming sprite is compared against eight registered slots; it
lands in the first empty slot or the first slot with lower priority. Slots
below the insertion point shift down by one; the lowest-priority slot is
dropped if the array is full. Total walk takes 32 cycles for fetch+eval
plus one cycle for `DONE`, well under the ~160-cycle HBLANK budget at
25 MHz.

#### 3.3.2 `sprite_fetch.sv`

`sprite_fetch` is the data-mover. Its FSM
(`IDLE -> CLEAR_BUF -> PROCESS_SPRITE -> READ_PIXEL -> WAIT_PIXEL ->
WRITE_PIXEL`) clears the inactive line buffer to zero (the transparent
palette index), then walks the eight sorted sprites from lowest to
highest priority, reading 16 palette-index bytes from the sprite ROM per
sprite and writing each non-zero, on-screen pixel into the line buffer
(`hw/sprite_fetch.sv:67-131`). The `READ_PIXEL -> WAIT_PIXEL -> WRITE_PIXEL`
3-state inner loop matches the 1-cycle read latency of the sprite ROM
(`hw/nml_gpu.sv:194`).

A sprite is addressed by `sprrom_raddr = (spr_id << 8) + (pixel_v << 4) +
actual_u`, where `actual_u = spr_hflip ? (15 - pixel_u) : pixel_u`
(`hw/sprite_fetch.sv:107-109`). Vertical flip is in the register layout
but not wired in this revision -- only `hflip` is honoured.

Walking from lowest to highest priority is a deliberate choice: each
sprite's pixels overwrite whatever was written by a lower-priority
sprite, so the highest-priority sprite ends up on top. The buffer-clear
fills with zero (transparent) so the compositor can fall through to the
tile layer where no sprite wrote.

#### 3.3.3 Double line buffer

Two 640x8 line buffers (`linebuf_A`, `linebuf_B`,
`hw/nml_gpu.sv:198-221`) form a ping-pong pair. While `sprite_fetch`
fills buffer A during the HBLANK after scanline N, the compositor reads
buffer B for scanline N+1; on the next HBLANK, the selector toggles. The
toggle fires when `hblank && x == 640` (`hw/nml_gpu.sv:209`), exactly at
the transition from visible to blanking. This buys one full HBLANK worth
of slack on top of the eval window: the fetch unit has time to finish
writing the next line's sprites before the read side flips.

### 3.4 Compositor (`compositor.sv`)

The compositor is a 4-stage pipeline that delivers one RGB triplet per
pixel. The stages, with the latches between them, are
(`hw/compositor.sv:75-311`):

1. **S1 (combinational):** decode the HUD region predicates
   (`in_hp_bar`, `in_ammo_label`, `in_ammo_digits`, ...,
   `in_score_text`), compute the HUD label tile IDs from the local
   character index, and select the BCD digit nibble from
   `player_stats`, `hud_aux`, and `score_reg`. Compute the background
   tile map address from `(x[9:3], y[9:3])`.
2. **S1->S2 latch (`hw/compositor.sv:256-268`):** register the HUD
   tile ID, HUD tile X/Y, HP-bar fill flag, sprite pixel from the line
   buffer, and the background tile map data. This stage exists so that
   the tile ROM read in S2 has its address valid one cycle early.
3. **S2 (combinational):** mux the tile ROM read address -- either the
   background path (`tile_id, y[2:0], x[2:0]`) or the HUD glyph path
   (`hud_tile_id, hud_tile_y, hud_tile_x`).
4. **S2->S3 latch (`hw/compositor.sv:280-286`):** register the HP-bar
   and HUD-text flags so they line up with the tile ROM read data
   coming back.
5. **S3 (combinational):** select the palette index -- sprite pixel if
   non-zero, else the tile ROM byte (`hw/compositor.sv:289-292`).
6. **S3->S4 (registered output):** drive `rgb_out`. The HUD layer wins
   in the top 16 pixels; the HP bar fills 0-191 in green or dark red
   depending on the per-pixel fill flag; HUD glyph pixels use white on
   dark grey; everywhere else, the palette RAM output drives the line.

The 4-stage pipeline is necessary because the palette read is a
1-cycle-latency M10K port and the tile ROM read is also 1-cycle. Total
latency from `x, y` to `rgb_out` is three to four cycles (120-160 ns
at 25 MHz), well within `VGA_BLANK_N` slop.

### 3.5 Memories

All six FPGA-side memories are inferred. The relevant ports and depths
are summarised below.

| Memory          | Storage         | Words x Width    | Total bits | Used for                          |
|-----------------|------------------|------------------|------------|-----------------------------------|
| Sprite table A  | `[63:0] [0:31]`  | 32 x 64          | 2,048      | Active sprite list                |
| Sprite table B  | `[63:0] [0:31]`  | 32 x 64          | 2,048      | Shadow written by HPS             |
| Palette RAM     | `[23:0] [0:255]` | 256 x 24         | 6,144      | RGB888 colour table               |
| Tile map RAM    | `[3:0][7:0] [0:1199]` | 1200 x 32   | 38,400     | 80 x 60 tile IDs, packed bytes    |
| Sprite ROM      | `[7:0] [0:16383]`| 16,384 x 8       | 131,072    | 64 sprites x 256 bytes each       |
| Tile ROM        | `[7:0] [0:4095]` | 4,096 x 8        | 32,768     | 64 tiles x 64 bytes each          |
| Line buffer A   | `[7:0] [0:639]`  | 640 x 8          | 5,120      | Sprite scanline (ping)            |
| Line buffer B   | `[7:0] [0:639]`  | 640 x 8          | 5,120      | Sprite scanline (pong)            |

The sprite ROM, tile ROM, palette, and shadow sprite table are
initialised from `.hex` files via `$readmemh` at synthesis
(`hw/nml_gpu.sv:183-192`). The palette and sprite table are subsequently
overwritten by the HPS at runtime; the ROMs stay frozen.

Tile-map storage is unusual: the register declaration is
`logic [3:0][7:0] tilemap_ram [0:1199]` rather than the more obvious
`logic [7:0] tilemap_ram [0:4799]`. The reason is byte-enable handling
on the lightweight bridge. ARM emits `STRB` (single-byte stores) for
the C driver's `nml_write_tile()` call, and the bridge translates them
into a 32-bit write with `avs_byteenable` selecting one lane. With
byte-organised storage, the bridge's implicit read-modify-write would
see only one byte of the word and write back zeros for the other three,
silently clobbering 75 % of the tile map (`hw/nml_gpu.sv:138-149`).
Packed four-lane storage matches Quartus's M10K byte-enable inference
template; each `tilemap_ram[idx][i]` lane updates independently.

### 3.6 Avalon-MM slave interface (`avalon_slave_iface.sv`)

The peripheral is a 32-bit lightweight Avalon-MM slave with a 14-bit
byte-address window (16 KB). The full register map
(`hw/avalon_slave_iface.sv:1-22`) is reproduced in Sec. 5; here, only the
mechanics matter.

Reads of the scalar registers (CTRL, STATUS, BG_SCROLL, IRQ_MASK,
player state, HUD_AUX) return combinationally on the read cycle. Reads
of the sprite-table, palette, and tile-map regions go through an
`always_ff` register on `*_rdata_sw` lines and therefore arrive one
cycle late. To make this work without breaking the master, the slave
asserts `avs_waitrequest` for the first cycle of any RAM-region read
and releases it on the second (`hw/avalon_slave_iface.sv:133-141`).

Writes to the CTRL register specifically handle `SWAP` by widening the
single-cycle Avalon strobe into a 7-cycle counter (`swap_req_cnt`,
`hw/avalon_slave_iface.sv:195-209`). The widening matters because the
swap signal crosses from the 50 MHz Avalon clock into the 25 MHz pixel
clock, where it is captured by a three-flop synchroniser
(`hw/nml_gpu.sv:94-106`) and edge-detected to produce a one-pulse
event in the pixel-clock domain. A single 20 ns pulse on the Avalon
side is too narrow for the 40 ns pixel clock to catch reliably;
holding for 140 ns (seven 50 MHz cycles) makes the crossing
deterministic.

### 3.7 Top-level integration (`nml_gpu.sv`, `de1soc_top.sv`)

`nml_gpu.sv` is the integration shell. It instantiates the slave, the
timing generator, sprite eval, sprite fetch, the compositor, and the
PLL, and declares the inferred memories. The active and shadow sprite
tables are kept inside this file rather than inside the slave because
the swap logic lives here: when `swap_pending` is set and `vsync` is
high, the 32-entry active table is bulk-assigned from the shadow on a
single clock edge (`hw/nml_gpu.sv:109-118`).

`de1soc_top.sv` is the Phase 1 wrapper. It ties the Avalon inputs off
(no master in the standalone build), wires reset to `KEY[0]`, mirrors
`VGA_BLANK_N` to `LEDR[1]` as a heartbeat, and passes the VGA outputs
through. The Phase 2 wrapper is `nml_gpu_hw/soc_system_top.sv`, which
instantiates the Platform Designer system, exposes nml_gpu's slave on
the HPS-to-FPGA lightweight bridge, and connects the same VGA pins.

## 4. Software Design

The software lives in [`sw/`](../sw/) and is structured as nine
translation units plus three headers. Three build variants share the
same sources:

| Variant            | Target | Renderer       | Input          | Output binary    |
|--------------------|--------|----------------|----------------|------------------|
| `make` (default)   | armhf  | `render.c`     | `input_real.c` | `nml_game`       |
| `make fake-input`  | armhf  | `render.c`     | `input_fake.c` | `nml_game_fake`  |
| `make terminal`    | host   | `render_terminal.c` | `input_fake.c` | `nml_game_term` |
| `make native`      | board native | `render.c` | `input_real.c` | `nml_game`       |

(`sw/Makefile:42-72`). The terminal build also `#define`s
`NML_TERMINAL_BUILD`, which compiles out the FPGA paths in `main.c`.

### 4.1 Game state machine and entity pool (`game.c`, `game.h`)

State is held in a single struct (`sw/game.h:93-133`). The active
states are `STATE_PLAYING`, `STATE_LEVELUP`, and `STATE_GAMEOVER`. A
fixed-size pool of 64 entities (`MAX_ENTITIES`, `sw/game.h:6`) holds
the player, enemies, bullets, hazards, and ammo drops. Each entity
tracks kind, active flag, position, velocity, hp, an animation
phase, a TTL, a payload (used by auto-attack projectiles and hazards
to remember which weapon spawned them), and a fire cooldown
(`sw/game.h:79-90`).

`game_tick()` (`sw/game.c:428-485`) dispatches on state:

* In `STATE_PLAYING`, it updates the player, runs the wave spawner,
  moves enemies, advances bullets, ages hazards, runs auto-attack
  timers, and resolves collisions. On wave clear it transitions to
  `STATE_LEVELUP`. On player HP <= 0 it transitions to
  `STATE_GAMEOVER` and deactivates every entity including the player
  so the death screen has a clean tilemap to draw into.
* In `STATE_LEVELUP`, the player picks one of three offered weapons
  with the D-pad and confirms with the B button
  (`sw/game.c:416-426`). On confirm, the chosen weapon is upgraded
  and the next wave starts.
* In `STATE_GAMEOVER`, START restarts the game by re-calling
  `game_init()`.

Enemy motion (`sw/game.c:219-254`) is a per-enemy random walk in X
combined with vertical pursuit of the player. The walk decision
re-rolls every 12 frames (`ENEMY_DIR_FRAMES`) using a per-enemy seed
(`phase`) so the cohort does not collapse into a single column where
the player's bullet stream would mop them up trivially. Armed enemies
also fire bullets back on a per-enemy cooldown jittered at spawn
(`sw/game.c:21-23, 239-246`).

Collision resolution is AABB at 16 x 16 (`sw/game.c:298-312`). All
kills go through `apply_damage()` (`sw/game.c:318-333`) so that score,
kill counter, and ammo-drop probability are recorded uniformly across
weapon paths. The drop probability is 30 % per kill
(`sw/game.h:144`, `AMMO_DROP_PERCENT`).

### 4.2 Wave system (`wave.c`, `wave.h`)

The wave table is a hard-coded array of five entries
(`sw/wave.c:9-16`):

| Wave | Spawn period (frames) | Total enemies | Armed | Armed HP | Speed |
|------|----------------------:|--------------:|------:|---------:|------:|
| 1    | 60                    | 6             | 0     | 1        | 1     |
| 2    | 50                    | 8             | 1     | 2        | 1     |
| 3    | 45                    | 10            | 2     | 2        | 2     |
| 4    | 35                    | 12            | 4     | 2        | 2     |
| 5    | 25                    | 16            | 6     | 3        | 3     |

After wave 5 the table clamps to its last entry, so the game can keep
running indefinitely at maximum difficulty. `wave_tick()` is called
once per `STATE_PLAYING` tick; it cools down a spawn timer and
allocates a fresh entity each time the counter reaches zero, with
armed enemies emitted first deterministically. `wave_advance()`
top-ups ammo (+40, capped at 99), artillery (+2, capped at 5), and
gas (+2, capped at 5) on every transition out of `STATE_LEVELUP`.

### 4.3 Auto-attack system (`autoatk.c`, `autoatk.h`)

Three auto-attack kinds exist (`sw/game.h:60-65`):

* **Mortar** is timer-driven. At level 1 it fires every 180 frames,
  decreasing by 30 frames per upgrade, floored at 60 frames
  (`sw/autoatk.c:10-20, 39-45`). Each shell launches upward from the
  player's column with random horizontal jitter and a TTL of 200
  frames.
* **Artillery** is player-triggered with the L shoulder button. On
  fire it kills every enemy in a 16-pixel column above the player
  (`sw/autoatk.c:200-219`) and paints a vertical column of
  `TILE_BEAM` glyphs into the tile map for six frames as the visual
  effect.
* **Mustard gas** is player-triggered with the R shoulder. It
  spawns a single `ENT_HAZARD` at the player's position with a TTL of
  90 frames. The cloud grows from 16 x 16 px to 48 x 32 px over the
  first 30 frames, holds, and shrinks over the last 15
  (`sw/autoatk.c:160-178`).

Each weapon has a per-button input cooldown of 20 frames
(`ABILITY_INPUT_COOLDOWN`, `sw/game.h:145`) to debounce noisy SNES
shoulder buttons.

The level-up menu offers three random distinct weapon kinds; with
`AA_COUNT = 3` it ends up offering all three every time
(`sw/autoatk.c:244-264`). Upgrading a maxed weapon (level 5) is a
no-op.

### 4.4 Input subsystem (`input.h`, `input_real.c`, `input_fake.c`)

The input API is a single function: `uint16_t input_read(int frame)`
(`sw/input.h:6`). Either `input_real.c` (the DragonRise reader) or
`input_fake.c` (the deterministic stub) is linked.

The real reader opens `/dev/input/event0` (the only evdev node that
shows up for the gamepad on the DE1-SoC kernel -- `joydev` is not built
in, so there is no `/dev/input/js0`). It drains pending events on every
call, updating an internal button bitmask and axis values
(`sw/input_real.c:127-147`). The D-pad is reported as `ABS_X`/`ABS_Y`;
axes are converted to four directional bits with a deadzone equal to a
quarter of the reported range, queried via `EVIOCGABS` so the same code
works whether the pad reports -1..1, 0..255, or signed 16-bit values
(`sw/input_real.c:76-84, 149-153`).

The face buttons map as follows (`sw/input_real.c:11-22`):

| SNES button | evdev code      | Action          |
|-------------|------------------|-----------------|
| B           | `BTN_THUMB2`     | Fire down       |
| Y           | `BTN_TOP`        | Fire left       |
| X           | `BTN_TRIGGER`    | Fire up         |
| A           | `BTN_THUMB`      | Fire right      |
| L shoulder  | `BTN_TOP2`       | Artillery       |
| R shoulder  | `BTN_PINKIE`     | Mustard gas     |
| Start       | `BTN_BASE4`      | Menu confirm    |
| Select      | `BTN_BASE3`      | Pause/quit      |

`input_fake.c` exercises every code path on a fixed cycle of 480
frames: strafe right, strafe left, move up, move down, idle. A bullet
fires every 20 frames cycling through DOWN/LEFT/UP/RIGHT; L pulses
every 200 frames; R every 240; START every 600.

### 4.5 Rendering pipeline (`render.c`, `render_terminal.c`)

`render_frame()` (`sw/render.c:342-421`) is called once per tick. It
writes the entity pool into the FPGA sprite table:

* Slot 0 is always the player.
* Slots 1-31 hold active non-player entities in pool order.
* Gas hazards expand into up to four sprite slots (centre, left,
  right, top) as the cloud grows (`sw/render.c:98-111`).
* Bullets get priority 2 (low), enemies and hazards priority 1, the
  player priority 0 (high).

Leftover slots are explicitly hidden by `nml_hide_sprite()` so prior
frames do not ghost (`sw/render.c:407-409`). The HUD mailbox registers
are updated every frame (`sw/render.c:411-420`).

Two special states bypass the entity-to-sprite mapping. In
`STATE_LEVELUP`, the renderer draws the player, three weapon-sprite
options at fixed positions, and a cursor above the active option
(`sw/render.c:230-283`). In `STATE_GAMEOVER`, the entire battlefield
tile map is overwritten with centred text using
`draw_text_centered()` (`sw/render.c:302-340`). On the
`STATE_GAMEOVER -> STATE_PLAYING` restart, `render_init_tilemap()` is
called to restore the battlefield ground tiles (`sw/render.c:347-354`).

The terminal renderer (`sw/render_terminal.c`) mirrors the slot
allocation logic exactly but prints each sprite slot to stdout as an
ASCII glyph. This lets the game logic be exercised in CI and on a
laptop without hardware.

### 4.6 Main loop (`main.c`)

`main()` (`sw/main.c:171-274`) is a fixed-timestep 60 Hz loop:

1. Read input (`input_read`).
2. Tick the game (`game_tick`).
3. Render (`render_frame` writes the sprite table).
4. Commit (`nml_commit_frame()` sets `CTRL.SWAP`, then polls
   `STATUS.SWAP_PENDING` until it clears or a 200 000-iteration
   timeout trips).
5. Sleep the remainder of the 16 666 us frame budget.

On the host (terminal build), `nml_commit_frame()` is `#ifdef`-ed out
and `nanosleep` is the sole pacing source. On the board, the SWAP
poll synchronises the loop with VBLANK.

Out of order with these steps, `main.c` also prints a console line for
each one-shot game-event transition (wave start, wave clear, level-up
cursor move, level-up confirmation, game over). The terminal build
relies on these for the demo; the FPGA build prints them on the
serial console so the SSH operator can follow along.

Palette initialisation happens once at startup
(`init_palette_runtime()`, `sw/main.c:139-168`). The HPS writes
sixteen entries covering player, enemies, projectiles, dirt, grass,
and the white sprite border. This step shadows the `$readmemh`
initialisation; once it runs, the software is the authoritative
source of palette data.

## 5. Hardware-Software Interface

The peripheral occupies 16 KB starting at 0xFF200000 in HPS physical
address space (the lightweight bridge base on the DE1-SoC). The
userspace driver `mmap()`s the whole window via `/dev/mem`
(`sw/nml_gpu.c:33-59`) and exposes typed writers. The register layout
is reproduced below.

### 5.1 Register map

| Offset       | Name           | Width | Access | Bit layout                                                              |
|--------------|----------------|------:|--------|--------------------------------------------------------------------------|
| 0x0000       | `CTRL`         | 32    | R/W    | `[0]=ENABLE`, `[1]=SWAP` (auto-clear), `[2]=HUD_ON`                      |
| 0x0004       | `STATUS`       | 32    | R      | `[0]=VBLANK`, `[1]=SWAP_PENDING`, `[15:8]=frame_ctr` (wired 0 at present)|
| 0x0008       | `BG_SCROLL`    | 32    | R/W    | `[15:0]=scroll_x` (signed), `[31:16]=scroll_y` (signed)                  |
| 0x000C       | `IRQ_MASK`     | 32    | R/W    | `[0]=VBLANK IRQ enable` (Phase 2; wired 0 here)                          |
| 0x0010       | `PLAYER_POS`   | 32    | R/W    | `[15:0]=px`, `[31:16]=py`                                                |
| 0x0014       | `PLAYER_STATS` | 32    | R/W    | `[7:0]=hp`, `[15:8]=wave` BCD (2 digits), `[31:16]=level`                |
| 0x0018       | `SCORE`        | 32    | R/W    | `[23:0]=score` BCD (6 digits)                                            |
| 0x001C       | `KILL_COUNT`   | 32    | R/W    | 32-bit kill counter (HUD-only, optional)                                 |
| 0x0020       | `HUD_AUX`      | 32    | R/W    | `[7:0]=ammo` BCD (2 digits), `[11:8]=art` BCD, `[15:12]=gas` BCD         |
| 0x0100-0x01FF| Sprite table   | 256 B | R/W    | 32 x 8 B entries; per-entry layout below                                 |
| 0x0400-0x07FF| Palette        | 1 KB  | R/W    | 256 x 4 B; entry = `0x00, R, G, B` packed `[23:0]=RGB888`                |
| 0x1000-0x22BF| Tile map       | 4.7 KB| R/W    | 80 x 60 bytes; byte = tile ID                                            |

Each sprite-table entry is two 32-bit words:

| Word | Bits  | Field        |
|------|-------|--------------|
| W0   | 15:0  | `x` (signed) |
| W0   | 31:16 | `y` (signed) |
| W1   | 7:0   | `sprite_id`  |
| W1   | 15:8  | `flags`      |
| W1   | 23:16 | `palette_off`|
| W1   | 31:24 | reserved     |

The `flags` byte packs (`sw/nml_gpu.h:58-69`): bit 2 hflip, bit 3 vflip,
bits 6:4 priority (0 = highest), bit 7 ACTIVE. The `ACTIVE` bit is the
flag that `sprite_eval` actually checks at bit 47 of the 64-bit packed
word; the original design used sentinel coordinates (x = y = -256)
exclusively, but the hardware now also honours the explicit active bit
(`sw/nml_gpu.h:12-16`, `hw/sprite_eval.sv:32-34`).

### 5.2 Frame-commit handshake

The frame-commit sequence is:

1. CPU writes the sprite table, palette, tile map, player state, and
   HUD aux fields. All writes land in shadow registers/RAMs
   immediately.
2. CPU writes `CTRL.SWAP = 1`. The slave widens the strobe to seven
   50 MHz cycles (`hw/avalon_slave_iface.sv:199-209`), so the pixel
   clock can synchronise the request through three flip-flops and
   capture an edge (`hw/nml_gpu.sv:94-106`). `SWAP` auto-clears in
   the same cycle it was written.
3. At the next VSYNC, the 32-entry sprite table is bulk-assigned
   from shadow to active in one clock edge
   (`hw/nml_gpu.sv:113-118`). `swap_pending` clears.
4. CPU polls `STATUS.SWAP_PENDING` until it falls or the
   200 000-iteration timeout trips (`sw/nml_gpu.c:179-190`). The
   timeout exists so a stuck pipeline cannot freeze the game loop.

The palette and tile map don't go through the swap handshake. Both
have one logical bank that is written in the Avalon clock domain and
read in the pixel clock domain. Visible artefacts from mid-frame writes
are possible in theory; in practice palette and tile updates are rare
(palette init at startup, tile updates only for artillery beam and
game-over text), so we left it that way.

### 5.3 Byte-store semantics for the tile map

`nml_write_tile()` issues a single-byte `STRB` to the tile-map region
(`sw/nml_gpu.c:92-99`). The lightweight bridge translates this into a
32-bit Avalon write with `avs_byteenable` set to one nibble. On the
slave side, the packed-byte tile-map storage `logic [3:0][7:0]
tilemap_ram` exposes four byte-enable lanes per M10K word and writes
only the addressed lane (`hw/nml_gpu.sv:158-162`). Reads return the
full word; the pixel side selects one of the four bytes per pixel
based on `tilemap_raddr[1:0]` (`hw/nml_gpu.sv:169-173`).

This packed layout is non-obvious but is the difference between a
correct tile map and one where only every fourth column receives the
write.

## 6. Build and Toolchain

### 6.1 Hardware build

Quartus Prime Lite 21.1 is the synthesis and place-and-route tool.
Two flows exist.

**Phase 1 (`hw/quartus/`).** A standalone bitstream that places `nml_gpu`
inside `de1soc_top` with the Avalon slave tied off. The four `.hex`
files generated by `hw/gen_rom.py` are picked up via `$readmemh` at
synthesis time, courtesy of `SEARCH_PATH ".."` in the QSF
(`hw/quartus/nml_gpu.qsf`). Compilation runs in 63 seconds on a lab
machine and produces `output_files/nml_gpu.sof` (for JTAG) and
`output_files/nml_gpu.rbf` (raw binary for SD-card boot).

**Phase 2 (`nml_gpu_hw/`).** A Platform Designer system with an HPS
instance, a clock bridge, an AXI-to-Avalon adapter on the
HPS-to-FPGA lightweight bridge, and the nml_gpu component as an
Avalon slave. The component descriptor lives in `nml_gpu_hw.tcl`
and sets the device-tree metadata
(`compatible = "csee4840,nml_gpu-1.0"`) so the kernel exposes the
peripheral at `/proc/device-tree/sopc@0/bridge@0xc0000000/vga@0x100000000/`.
The Makefile targets are `qsys`, `quartus`, `rbf`, and `dtb`.

### 6.2 ROM generation (`hw/gen_rom.py`)

`gen_rom.py` is a 777-line Python script that emits four hex files:

* `sprite_rom.hex` -- 16 384 bytes covering 64 x 16 x 16 sprites.
  Sprite IDs in use today are 1 (player, green-bordered square),
  2 (armed enemy, red with white cross), 3 (player bullet,
  4 x 4 yellow centred), 4 (unarmed enemy, solid pink), 5 (mortar
  shell, orange diamond), 7 (gas, green circle), 8 (artillery flash,
  white plus), 9 (ammo crate), 10 (enemy bullet). Sprite 0 is
  reserved as the fully-transparent slot.
* `tile_rom.hex` -- 4 096 bytes covering 64 x 8 x 8 tiles. Slots in
  use: 0 (blank background), 4-5 (dirt), 6-7 (grass), 8 (dirt/grass
  transition), 9 (mud puddle), 10 (dense grass), 11 (artillery beam
  column), 16-41 (A-Z glyphs), 42 (colon), 43 (space), 48-57 (digits
  0-9).
* `palette.hex` -- 256 RGB888 entries. The active entries are
  documented inline in `sw/main.c:139-168` and `hw/gen_rom.py`.
* `sprite_table.hex` -- 32 initial sprite slots that draw the smoke-test
  scene before the HPS takes over.

### 6.3 Software build

The userspace binary cross-compiles with `arm-linux-gnueabihf-gcc`
(`sw/Makefile:23`). It links statically (`-static`) so it runs on
the board without a matching glibc, and uses standard POSIX APIs
(`/dev/mem`, `mmap`, `evdev`, `nanosleep`, `clock_gettime`). No
threading; everything is in one process loop. The terminal build
swaps in `render_terminal.c` and `input_fake.c` and links against
the host's gcc.

### 6.4 Boot and SD-card image

The Phase 2 image follows the standard DE1-SoC Linux boot flow:
preloader -> U-Boot -> kernel (Linux 4.19 from `altera-fpga/linux-socfpga`,
tag v4.19) -> root filesystem on the ext4 partition. The FPGA RBF
and DTB sit on the FAT boot partition. U-Boot loads the bitstream
into the FPGA fabric before launching the kernel, so the smoke-test
scene is already on the VGA monitor by the time userspace comes up.
After login, the operator copies `nml_game` onto the board and runs
it as root (`/dev/mem` requires root).

## 7. Testing and Verification

Three test rungs:

* **Phase 0 (host).** `cd sw && make terminal && ./nml_game_term` runs
  the entire game loop without hardware, printing every frame's sprite
  table to stdout. The deterministic input stub exercises every
  weapon path on a fixed cycle, so a regression in `game.c`,
  `wave.c`, `autoatk.c`, or `render.c` shows up immediately. This is
  the loop we use during local development.
* **Phase 1 (FPGA only).** With the `.sof` loaded over JTAG, the
  monitor shows the smoke-test scene (dark battlefield, green
  player square, two red enemies, a yellow bullet) within one
  second. LEDR[0] is on, LEDR[1] flickers at 60 Hz with
  `VGA_BLANK_N`. This proves the entire pixel pipeline: 25 MHz
  clock, VGA timing, M10K `$readmemh` init, sprite eval priority
  sort, sprite fetch line-buffer fill, compositor priority MUX,
  palette lookup, and ADV7123 pin assignments.
* **Phase 2 (HPS-integrated).** With the RBF and DTB on the SD card,
  the kernel exposes the peripheral, and `./nml_game` mmaps it. The
  game loop runs at 60 Hz, the controller drives the player, and
  the terminal logs wave starts and clears. At the time of writing,
  this configuration boots and the game runs, but the VGA output
  has a vertical-stripe rendering bug and the Game-Over screen is
  broken -- see Sec. 9 and the deep-dive companion for details.

## 8. Results

### 8.1 Resource utilisation

From `hw/quartus/output_files/nml_gpu.fit.summary` (Phase 1 build,
Cyclone V SE 5CSEMA5F31C6):

| Resource         | Used   | Available | Fraction |
|------------------|-------:|----------:|---------:|
| ALMs             | 205    | 32,070    | < 1 %    |
| Registers        | 365    | -- | -- |
| Pins             | 54     | 457       | 12 %     |
| Block memory bits| 86,144 | 4,065,280 | 2 %      |
| M10K blocks      | 14     | 397       | 4 %      |
| DSP blocks       | 0      | 87        | 0 %      |
| PLLs             | 0      | 6         | 0 %      |

The design uses less than 1 % of the part. There is room for an order
of magnitude more sprite slots, deeper tile/sprite ROMs, multiple
parallax layers, or a hardware audio engine -- all of which were
considered and dropped for scope.

Total compile time: 63 seconds (Analysis & Synthesis 13 s, Fitter 30 s,
Assembler 12 s, Timing Analyzer 8 s).

### 8.2 Timing analysis

`hw/quartus/output_files/nml_gpu.sta.summary` reports negative slack on
the slow-1100mV-85 degC corner:

| Corner          | Clock      | Setup slack | TNS         |
|-----------------|------------|------------:|------------:|
| Slow 1100mV 85C | clk_div    | -5.111 ns   | -1409.97 ns |
| Slow 1100mV 85C | CLOCK_50   | -3.506 ns   | -3.506 ns   |
| Fast 1100mV 85C | clk_div    | -2.654 ns   | -725.39 ns  |

All hold checks pass. The dominant violation is a minimum-pulse-width
report on the divided clock register. The pixel-clock domain runs at
25 MHz (40 ns period), but the timing analyser treats the divider
register as if it were producing a much higher-frequency clock; the
combination of `posedge` and `negedge` from the alternating toggle
makes the analyser see a 20 ns half-period. On real silicon the
register is promoted to the global clock network and the duty cycle
is exact, so the divider works as intended -- but the analyser cannot
see that, and reports failure.

The fix is to replace the divide-by-2 register with an `altera_pll`
IP instance configured for 25 MHz, which the analyser models
correctly. We have not done this yet because the Phase 1 board
passes timing visually at lab temperature; the slack numbers will
matter only when integration timing problems start mounting.

### 8.3 Frame timing

In the deployed (Phase 2) configuration, `top -n 1 -b` while
`nml_game` is running shows CPU usage under 5 %. The loop spends
most of its time in `nanosleep` or polling `STATUS.SWAP_PENDING`
inside `nml_commit_frame()`. We do not currently log per-frame
durations; an explicit instrument is on the open-issues list.

## 9. Challenges and Resolutions

### 9.1 Tile-map byte-enable RMW

The original tile-map storage was `logic [7:0] tilemap_ram [0:4799]`
 -- one byte per word. The Linux driver's `nml_write_tile()` issues an
ARM `STRB` for each tile update. The HPS-to-FPGA lightweight bridge
sets `avs_byteenable` to one nibble and the Avalon slave's effective
behaviour was a read-modify-write: read the 32-bit word, mask in the
new byte, write back. With byte-organised storage, the slave only
returned one byte from each read, so the bridge masked three zero
bytes into the other lanes and silently clobbered them. The visible
symptom was that only every fourth tile column showed new content;
the rest stayed at reset value 0.

The fix is the packed `logic [3:0][7:0] tilemap_ram [0:1199]` storage
plus a per-lane write in `hw/nml_gpu.sv:158-162`. This is the
Quartus-recommended template for M10K with `byteena_a`
auto-inference. Each lane updates independently, and reads return
all four bytes, so the bridge's read-modify-write preserves the
untouched bytes.

### 9.2 Single-cycle `eval_done` against multi-cycle `CLEAR_BUF`

`sprite_eval` finishes its 32-sprite walk in roughly 33 cycles and
asserts `eval_done` for one cycle. `sprite_fetch` is in `CLEAR_BUF`
for 640 cycles (one write per pixel) and only checks `eval_done`
once, at the end of the buffer clear. If the eval pulse fires while
fetch is still mid-clear, the pulse is gone by the time fetch
samples it.

The first fix attempted was to edge-detect a transition on
`active_mask`. That worked for the smoke-test scene because the
active set changes between non-sprite and sprite rows. It silently
failed for the game's much busier scenes: 15 of every 16 scanlines
inside a 16-pixel sprite see the *same* active set, so no transition
fires.

The current fix is a sustain latch: `eval_done_r` is set when the
pulse arrives and cleared at the next `eval_strobe`. `sprite_fetch`
samples `eval_done_r` and is guaranteed to see it set after the
first eval of a line, regardless of when fetch happens to check
(`hw/nml_gpu.sv:264-270`).

### 9.3 CDC on `ctrl_swap_req`

`CTRL.SWAP` is written from the 50 MHz Avalon clock and consumed in
the 25 MHz pixel clock. The straight single-cycle pulse from the
slave was 20 ns wide; a two-flop synchroniser at 25 MHz samples
every 40 ns, so it could easily miss the pulse entirely.

The slave now widens the request: a 3-bit counter loaded with 7 on
each SWAP write holds `ctrl_swap_req` high for seven 50 MHz cycles
(140 ns), which is enough that a three-flop chain on the pixel side
captures the rising edge deterministically. The third flop's
falling edge re-arms the detector for the next swap.

### 9.4 Pixel-clock duty cycle

The divide-by-2 register works on silicon but fails the analyser's
minimum-pulse-width check. We documented the failure mode and
deferred the `altera_pll` swap until Phase 2 closes timing on its
own merits. The divider's port names already match the IP's
defaults, so the swap is a drop-in.

### 9.5 Active rendering bugs

In the current Phase 2 build, the VGA monitor shows the game across
the full screen but with vertical stripes: the rendered pixels are
correct in some columns and stale in others. The Game-Over screen
also draws incorrectly -- the centred text appears partially or not
at all.

The stripe pattern is consistent with a partial mismatch between
the byte-enable behaviour on writes and the byte-select behaviour
on reads. The packed-byte tile-map fix in Sec. 9.1 addressed the gross
clobbering, but a residual issue in the read-side byte select
(`hw/nml_gpu.sv:169-173`) or in the way the slave decodes
`avs_address` for the upper half of the tile-map region
(`hw/avalon_slave_iface.sv:169-178`) is the most likely culprit.
The decode there packs `{avs_address[13], avs_address[11:0]}` to
handle the 0x1000-0x22BF range crossing bit 13; if a byte falls
into the upper half (rows 25 and above), the lane the slave writes
may not match the lane the compositor reads, producing the visible
stripe.

The Game-Over visual is downstream of the same problem: the
centred-text routine writes one byte at a time into the tile map,
and the visible state depends on lane alignment.

Diagnosis of both is on the open-issues list. Both are intermittent
on the smoke-test bitstream where the HPS does not write the tile
map; this isolates the regression to the byte-write path under
real load.

## 10. Conclusion and Open Work

The Phase 1 smoke-test bitstream works as designed on hardware:
640 x 480 @ 60 Hz, tile + sprite + palette + HUD pipeline, full
VGA pin set. The Phase 2 HPS-integrated build boots Linux, exposes
the peripheral on the lightweight bridge, runs the C game at
60 Hz, and reads SNES controller input via evdev. Player movement,
firing, enemy spawning, wave progression, level-up, mortar timers,
artillery, gas clouds, ammo drops, and the HUD numerics all
function in the deployed configuration.

The two open items at the time of writing are:

1. The vertical-stripe rendering artefact on the HPS-integrated
   build (Sec. 9.5). Tracing the byte-lane alignment between the
   slave's tile-map write decode and the compositor's read mux is
   the next step.
2. The Game-Over screen visual (Sec. 9.5), likely a downstream of (1).

After those, the remaining work, in rough priority order:

* Swap `pll_25mhz` for `altera_pll` and close timing on the
  analyser.
* Wire `STATUS[15:8]` to an actual VSYNC counter (currently
  constant zero, `hw/nml_gpu.sv:231`).
* Sound: the WM8731 audio codec is on the board but not used.
* Real sprite art: the placeholder squares are functional but
  unattractive.
* Reduce the duplication between `hw/` and `nml_gpu_hw/` (the
  SystemVerilog sources are currently copied manually; symlinks or
  a Makefile rule would prevent drift).

## References

1. `DESIGN.md` -- Project design document, April 2026. System block
   diagram, register map, game design.
2. `SETUP.md` -- Bring-up instructions for Phase 0, Phase 1, and
   Phase 2.
3. Terasic Inc., *DE1-SoC User Manual*, Revision 1.2.4.
4. Intel/Altera, *Cyclone V Device Handbook* (Volume 1: Device
   Interfaces and Integration).
5. Intel/Altera, *Avalon Interface Specifications*, MNL-AVABUSREF.
6. Analog Devices, *ADV7123 Triple High-Speed Video DAC Datasheet*.
7. VESA, *VGA 640x480 @ 60 Hz timing specification*.
8. Linux kernel evdev documentation: `Documentation/input/event-codes.rst`.
9. `lab3-reference/CSEE4840W-Lab3` -- VGA ball lab providing the
   timing-generator skeleton.

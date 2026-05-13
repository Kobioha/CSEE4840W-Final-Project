#!/usr/bin/env python3
"""
gen_rom.py -- generate sprite_rom.hex, tile_rom.hex, palette.hex, sprite_table.hex

Format: $readmemh-compatible. One byte per line, lowercase hex, no addresses.

Sprite ROM: 64 sprites x 256 bytes (16x16, palette indices). Sprite 0 reserved
            transparent. Sprites 1-10 are the live game art:
              1 = player           (green bordered)
              2 = armed enemy      (red bordered + white cross)
              3 = player bullet    (small yellow block)
              4 = unarmed enemy    (solid pink, no border)
              5 = mortar shell     (orange diamond)
              6 = (retired -- was barbed wire; AA_WIRE removed in this batch)
              7 = mustard gas      (large green-yellow circle, SW tiles 3+ to form cloud)
              8 = artillery flash  (white plus; for the stacked-sprite beam)
              9 = ammo drop        (green crate / box outline, white border)
             10 = enemy bullet     (red dot, small)

Tile ROM:   64 tiles x 64 bytes (8x8, palette indices).
              0      = solid background fill
              1      = background with corner accents
              2      = trench horizontal stripes
              3      = dirt scatter
              4..10  = battlefield mosaic (crater / sandbag / trench-edge /
                       barb-wire / mud-streak / shell-hole / reserved)
              11     = artillery beam (vertical white line) -- used by Batch C
              16..41 = letter glyphs A..Z   (tile_id = 16 + (letter - 'A'))
              42     = colon ':'
              43     = space ' '
              44..47 = reserved
              48..57 = digits 0..9
              58..63 = reserved

Palette indices used here are mirrored in sw/main.c init_palette_runtime():
    0x00 = transparent (sprite ROM only)
    0x10 = player color (green)
    0x11 = armed enemy red
    0x12 = bullet yellow
    0x13 = unarmed enemy pink
    0x14 = mortar orange
    0x15 = (retired -- barbed wire gray)
    0x16 = mustard gas yellow-green
    0x17 = artillery bright white-yellow
    0x18 = ammo drop green (new this batch)
    0x19 = enemy bullet red (new this batch)
    0x20 = tile background
    0x21 = tile accent
    0xFF = white (sprite borders)

Run: ``python3 gen_rom.py`` from the hw/ directory. Tested against the lab
Python (3.6) so the type hints below stick to syntax that predates PEP 585.
"""

from pathlib import Path

HERE = Path(__file__).resolve().parent

SPRITE_ROM_BYTES = 64 * 256          # 16384
TILE_ROM_BYTES   = 64 * 64           #  4096

PAL_TRANSPARENT  = 0x00
PAL_PLAYER       = 0x10
PAL_ENEMY_ARMED  = 0x11
PAL_BULLET       = 0x12
PAL_ENEMY_UNARMD = 0x13
PAL_MORTAR       = 0x14
# 0x15 retired (was PAL_WIRE)
PAL_GAS          = 0x16
PAL_ARTILLERY    = 0x17
PAL_AMMO_DROP    = 0x18
PAL_ENEMY_BULLET = 0x19
# Battlefield ground palette (top-down view, pixel-art style):
PAL_BG           = 0x20   # PAL_DIRT_MID    -- primary dirt color
PAL_BG_ACCENT    = 0x21   # PAL_DIRT_LIGHT  -- dirt highlight
PAL_DIRT_MID     = 0x20
PAL_DIRT_LIGHT   = 0x21
PAL_DIRT_DARK    = 0x22
PAL_GRASS_MID    = 0x23
PAL_GRASS_DARK   = 0x24
PAL_GRASS_LIGHT  = 0x25
PAL_MUD_DEEP     = 0x26
PAL_BORDER       = 0xFF


def make_sprite_rom() -> bytearray:
    rom = bytearray(SPRITE_ROM_BYTES)  # zero-init = all transparent

    def put(slot: int, u: int, v: int, pal: int) -> None:
        rom[slot * 256 + v * 16 + u] = pal

    def fill_solid(slot: int, pal: int) -> None:
        for v in range(16):
            for u in range(16):
                put(slot, u, v, pal)

    def fill_bordered(slot: int, fill: int, border: int) -> None:
        for v in range(16):
            for u in range(16):
                edge = (u == 0 or u == 15 or v == 0 or v == 15)
                put(slot, u, v, border if edge else fill)

    def fill_centered(slot: int, pal: int, half: int) -> None:
        # half=2 -> 4x4 centered block, etc.
        for v in range(16):
            for u in range(16):
                if abs(u - 8) < half and abs(v - 8) < half:
                    put(slot, u, v, pal)

    def fill_diamond(slot: int, pal: int, radius: int) -> None:
        # Manhattan-distance diamond centered at (8, 8). radius=7 fills 15px wide.
        for v in range(16):
            for u in range(16):
                if abs(u - 8) + abs(v - 8) <= radius:
                    put(slot, u, v, pal)

    def fill_circle(slot: int, pal: int, radius: int) -> None:
        # Euclidean disk centered at (8, 8).
        r2 = radius * radius
        for v in range(16):
            for u in range(16):
                dx = u - 8
                dy = v - 8
                if dx * dx + dy * dy <= r2:
                    put(slot, u, v, pal)

    def fill_x(slot: int, pal: int, thickness: int) -> None:
        # Two diagonals from corner to corner; thickness in pixels (1, 2, ...).
        for v in range(16):
            for u in range(16):
                if abs(u - v) < thickness or abs(u + v - 15) < thickness:
                    put(slot, u, v, pal)

    def fill_plus(slot: int, pal: int, arm_half: int, thickness: int) -> None:
        # A '+' centered at (8, 8). arm_half = arm length each side from center,
        # thickness = bar width.
        cx, cy = 7, 7   # so the bars span an even pair around the visual center
        for v in range(16):
            for u in range(16):
                in_h = (abs(v - cy) < thickness and abs(u - cx) <= arm_half)
                in_v = (abs(u - cx) < thickness and abs(v - cy) <= arm_half)
                if in_h or in_v:
                    put(slot, u, v, pal)

    def overlay_cross(slot: int, pal: int) -> None:
        # Small '+' marker centered, used to badge armed enemies.
        for d in range(-2, 3):
            put(slot, 7 + d, 7, pal)
            put(slot, 7, 7 + d, pal)
            put(slot, 8 + d, 8, pal)
            put(slot, 8, 8 + d, pal)

    def fill_ammo_crate(slot: int, fill: int, border: int) -> None:
        # Rounded "ammo box" outline with hollow interior, plus a faint cross
        # marker so it reads as a pickup. 12x12 inside a 16x16 sprite.
        for v in range(2, 14):
            for u in range(2, 14):
                on_edge = (u == 2 or u == 13 or v == 2 or v == 13)
                put(slot, u, v, border if on_edge else fill)
        # interior cross marker
        for d in range(-3, 4):
            put(slot, 7 + d, 7, border)
            put(slot, 7, 7 + d, border)

    def fill_dot(slot: int, pal: int, radius: int) -> None:
        # Small filled disk for the enemy bullet -- slightly bigger than the
        # player's 4x4 bullet so it reads as "incoming".
        r2 = radius * radius
        for v in range(16):
            for u in range(16):
                dx = u - 8
                dy = v - 8
                if dx * dx + dy * dy <= r2:
                    put(slot, u, v, pal)

    # slot 0: transparent (already)
    fill_bordered(1, PAL_PLAYER,       PAL_BORDER)   # player
    fill_bordered(2, PAL_ENEMY_ARMED,  PAL_BORDER)   # armed enemy
    overlay_cross(2, PAL_BORDER)                     # cross badge identifies "armed"
    fill_centered(3, PAL_BULLET, 2)                  # bullet: 4x4 centered
    fill_solid   (4, PAL_ENEMY_UNARMD)               # unarmed: solid pink
    fill_diamond (5, PAL_MORTAR, 7)                  # mortar shell: diamond
    # slot 6 retired (was barbed wire); leave transparent so SW won't render anything if reused
    fill_circle  (7, PAL_GAS, 7)                     # mustard gas: large disk
    fill_plus    (8, PAL_ARTILLERY, 7, 2)            # artillery flash: thick '+'
    fill_ammo_crate(9, PAL_AMMO_DROP, PAL_BORDER)    # ammo drop: green crate
    fill_dot     (10, PAL_ENEMY_BULLET, 3)           # enemy bullet: small red dot

    return rom


# 8x8 letter glyphs. Each cell is a tuple of 8 strings of 8 chars where '#'
# marks a lit pixel (PAL_BORDER white) and any other char leaves the pixel as
# the tile background (palette index 0 -> compositor draws RGB_HUD_BG).
# Designed in a 5-wide x 7-tall window with 1-pixel margins so adjacent
# glyphs don't run together when stitched at 8-px boundaries.
LETTER_GLYPHS = {
    'A': ("........",
          "..####..",
          ".##..##.",
          ".##..##.",
          ".######.",
          ".##..##.",
          ".##..##.",
          "........"),
    'B': ("........",
          ".#####..",
          ".##..##.",
          ".#####..",
          ".##..##.",
          ".##..##.",
          ".#####..",
          "........"),
    'C': ("........",
          "..####..",
          ".##..##.",
          ".##.....",
          ".##.....",
          ".##..##.",
          "..####..",
          "........"),
    'D': ("........",
          ".#####..",
          ".##..##.",
          ".##..##.",
          ".##..##.",
          ".##..##.",
          ".#####..",
          "........"),
    'E': ("........",
          ".######.",
          ".##.....",
          ".#####..",
          ".##.....",
          ".##.....",
          ".######.",
          "........"),
    'F': ("........",
          ".######.",
          ".##.....",
          ".#####..",
          ".##.....",
          ".##.....",
          ".##.....",
          "........"),
    'G': ("........",
          "..####..",
          ".##..##.",
          ".##.....",
          ".##.###.",
          ".##..##.",
          "..####..",
          "........"),
    'H': ("........",
          ".##..##.",
          ".##..##.",
          ".######.",
          ".##..##.",
          ".##..##.",
          ".##..##.",
          "........"),
    'I': ("........",
          ".######.",
          "...##...",
          "...##...",
          "...##...",
          "...##...",
          ".######.",
          "........"),
    'J': ("........",
          ".######.",
          ".....##.",
          ".....##.",
          ".....##.",
          ".##..##.",
          "..####..",
          "........"),
    'K': ("........",
          ".##..##.",
          ".##.##..",
          ".####...",
          ".####...",
          ".##.##..",
          ".##..##.",
          "........"),
    'L': ("........",
          ".##.....",
          ".##.....",
          ".##.....",
          ".##.....",
          ".##.....",
          ".######.",
          "........"),
    'M': ("........",
          ".##..##.",
          ".######.",
          ".######.",
          ".##..##.",
          ".##..##.",
          ".##..##.",
          "........"),
    'N': ("........",
          ".##..##.",
          ".###.##.",
          ".######.",
          ".##.###.",
          ".##..##.",
          ".##..##.",
          "........"),
    'O': ("........",
          "..####..",
          ".##..##.",
          ".##..##.",
          ".##..##.",
          ".##..##.",
          "..####..",
          "........"),
    'P': ("........",
          ".#####..",
          ".##..##.",
          ".#####..",
          ".##.....",
          ".##.....",
          ".##.....",
          "........"),
    'Q': ("........",
          "..####..",
          ".##..##.",
          ".##..##.",
          ".##.###.",
          ".##..##.",
          "..#####.",
          "........"),
    'R': ("........",
          ".#####..",
          ".##..##.",
          ".#####..",
          ".####...",
          ".##.##..",
          ".##..##.",
          "........"),
    'S': ("........",
          "..####..",
          ".##..##.",
          "..##....",
          "....##..",
          ".##..##.",
          "..####..",
          "........"),
    'T': ("........",
          ".######.",
          "...##...",
          "...##...",
          "...##...",
          "...##...",
          "...##...",
          "........"),
    'U': ("........",
          ".##..##.",
          ".##..##.",
          ".##..##.",
          ".##..##.",
          ".##..##.",
          "..####..",
          "........"),
    'V': ("........",
          ".##..##.",
          ".##..##.",
          ".##..##.",
          ".##..##.",
          "..####..",
          "...##...",
          "........"),
    'W': ("........",
          ".##..##.",
          ".##..##.",
          ".##..##.",
          ".######.",
          ".######.",
          ".##..##.",
          "........"),
    'X': ("........",
          ".##..##.",
          ".##..##.",
          "..####..",
          "..####..",
          ".##..##.",
          ".##..##.",
          "........"),
    'Y': ("........",
          ".##..##.",
          ".##..##.",
          "..####..",
          "...##...",
          "...##...",
          "...##...",
          "........"),
    'Z': ("........",
          ".######.",
          ".....##.",
          "....##..",
          "...##...",
          "..##....",
          ".######.",
          "........"),
    ':': ("........",
          "........",
          "...##...",
          "...##...",
          "........",
          "...##...",
          "...##...",
          "........"),
    ' ': ("........",
          "........",
          "........",
          "........",
          "........",
          "........",
          "........",
          "........"),
}
LETTER_TILE_BASE   = 16   # 'A' -> tile 16, 'B' -> 17, ..., 'Z' -> 41
COLON_TILE_ID      = 42
SPACE_TILE_ID      = 43
BEAM_TILE_ID       = 11   # artillery beam glyph (vertical white line)

# Battlefield ground tiles (top-down view, pixel-art style inspired by the
# brown-dirt + green-grass mosaic look of period RPG tilesets). Each cell is
# 8x8 and uses up to 6 palette colors. The legend maps each glyph char to a
# palette index:
#   'M' = PAL_DIRT_MID    (mid brown, primary dirt)
#   'D' = PAL_DIRT_DARK   (darker mud splotches)
#   'L' = PAL_DIRT_LIGHT  (sandy-tan dirt highlight)
#   'G' = PAL_GRASS_MID   (primary grass green)
#   'g' = PAL_GRASS_DARK  (shadowed grass / dense clump)
#   'h' = PAL_GRASS_LIGHT (highlight / new growth)
#   'm' = PAL_MUD_DEEP    (very dark mud puddle interior)
GROUND_LEGEND = {
    'M': PAL_DIRT_MID,
    'D': PAL_DIRT_DARK,
    'L': PAL_DIRT_LIGHT,
    'G': PAL_GRASS_MID,
    'g': PAL_GRASS_DARK,
    'h': PAL_GRASS_LIGHT,
    'm': PAL_MUD_DEEP,
}

BATTLEFIELD_GLYPHS = {
    4:  ("MMMDMMLM",   # dirt_a: mostly mid-brown with sparse dark/light specks
         "MDMMMMMM",
         "MMMMDMMM",
         "MLMMMMDM",
         "MMMDMMMM",
         "MMMMMLMM",
         "DMMMMMMM",
         "MMMMDMML"),
    5:  ("MMLMMMDM",   # dirt_b: same palette, different scatter to break tiling
         "DMMMDMML",
         "MMMMMMMM",
         "MMMMMLDM",
         "MLMMMMMM",
         "MMMDMMMM",
         "MMMMMMDM",
         "MDMMLMMM"),
    6:  ("GGgGGGGh",   # grass_a: mostly mid-green with shadow + highlight specks
         "GGGGgGGG",
         "GhGGGGGG",
         "GGGGGgGh",
         "GGgGGGGG",
         "GGGhGGGG",
         "gGGGGGGG",
         "GGGGgGhG"),
    7:  ("GGGGhGGG",   # grass_b: different scatter
         "GgGGGGGG",
         "GGGGGGgG",
         "hGGGGGGh",
         "GGGGgGGG",
         "GGgGGGhG",
         "GGGGGGGG",
         "GhGGgGGG"),
    8:  ("MMMMGGGG",   # transition: dirt on left, grass on right
         "MMMMGGGG",
         "MDMMGgGG",
         "MMMMGGGG",
         "MMLMGGgG",
         "MMMMhGGG",
         "MDMMGGGG",
         "MMMMGGGG"),
    9:  ("MMDDDDMM",   # mud puddle: dark wet crater
         "MDmmmDDM",
         "DmmmmmDM",
         "DmmDmmDD",
         "DmmmmmDD",
         "MDmmmDDM",
         "MDDDDDMM",
         "MMMMMMMM"),
   10:  ("ghGGgGhG",   # grass_dense: lush patch
         "GGghGGgG",
         "gGGGhGGG",
         "GgGGGgGh",
         "GhGgGGGG",
         "GGGGGhgG",
         "gGhGGGGg",
         "GGGgGhGG"),
}

# Vertical beam tile: 2-px-wide bright line spanning the full 8x8 cell, with
# faint edges so adjacent beam tiles read as continuous.
BEAM_GLYPH = ("...##...",
              "...##...",
              "...##...",
              "...##...",
              "...##...",
              "...##...",
              "...##...",
              "...##...")


DIGIT_GLYPHS = {
    0: ("........",
        ".######.",
        ".##..##.",
        ".##..##.",
        ".##..##.",
        ".##..##.",
        ".######.",
        "........"),
    1: ("........",
        "...##...",
        "..###...",
        "...##...",
        "...##...",
        "...##...",
        ".######.",
        "........"),
    2: ("........",
        ".######.",
        ".....##.",
        ".....##.",
        ".######.",
        ".##.....",
        ".######.",
        "........"),
    3: ("........",
        ".######.",
        ".....##.",
        ".....##.",
        ".######.",
        ".....##.",
        ".######.",
        "........"),
    4: ("........",
        "..#..#..",
        "..#..#..",
        "..####..",
        ".....#..",
        ".....#..",
        ".....#..",
        "........"),
    5: ("........",
        ".######.",
        ".##.....",
        ".######.",
        ".....##.",
        ".....##.",
        ".######.",
        "........"),
    6: ("........",
        ".######.",
        ".##.....",
        ".######.",
        ".##..##.",
        ".##..##.",
        ".######.",
        "........"),
    7: ("........",
        ".######.",
        ".....##.",
        ".....##.",
        "....##..",
        "...##...",
        "...##...",
        "........"),
    8: ("........",
        ".######.",
        ".##..##.",
        ".######.",
        ".##..##.",
        ".##..##.",
        ".######.",
        "........"),
    9: ("........",
        ".######.",
        ".##..##.",
        ".######.",
        ".....##.",
        ".....##.",
        ".######.",
        "........"),
}
DIGIT_TILE_BASE = 48   # tile slots 48..57


def make_tile_rom() -> bytearray:
    rom = bytearray(TILE_ROM_BYTES)

    def put(slot: int, u: int, v: int, pal: int) -> None:
        rom[slot * 64 + v * 8 + u] = pal

    def fill_solid(slot: int, pal: int) -> None:
        for v in range(8):
            for u in range(8):
                put(slot, u, v, pal)

    def fill_glyph(slot: int, glyph: tuple, fg: int) -> None:
        for v in range(8):
            for u in range(8):
                if glyph[v][u] == '#':
                    put(slot, u, v, fg)
                # else leave 0 (transparent / HUD background fill)

    def fill_bg_glyph(slot: int, glyph: tuple, fg: int, bg: int) -> None:
        # Like fill_glyph but writes `bg` to non-lit cells too, so the result is
        # a full opaque background tile (no transparent pixels).
        for v in range(8):
            for u in range(8):
                put(slot, u, v, fg if glyph[v][u] == '#' else bg)

    def fill_multicolor(slot: int, glyph: tuple, legend: dict) -> None:
        # Multi-color tile: each glyph char maps to a palette index via `legend`.
        # Any char missing from the legend is treated as PAL_BG so tiles can
        # safely include "fallback" pixels.
        for v in range(8):
            for u in range(8):
                ch = glyph[v][u]
                put(slot, u, v, legend.get(ch, PAL_BG))

    # tile 0: solid background
    fill_solid(0, PAL_BG)

    # tile 1: background with accent dots in the corners
    fill_solid(1, PAL_BG)
    for v, u in ((0, 0), (0, 7), (7, 0), (7, 7)):
        put(1, u, v, PAL_BG_ACCENT)

    # tile 2: trench horizontal stripes (alternating dark/light rows)
    for v in range(8):
        for u in range(8):
            put(2, u, v, PAL_BG_ACCENT if (v & 1) else PAL_BG)

    # tile 3: dirt scatter (a few accent dots inside the cell)
    fill_solid(3, PAL_BG)
    for v, u in ((1, 3), (3, 1), (3, 5), (5, 2), (6, 6)):
        put(3, u, v, PAL_BG_ACCENT)

    # tiles 4..10 = battlefield ground mosaic (grass + dirt with mud accents).
    # Multi-color tiles using GROUND_LEGEND for palette mapping.
    for slot, glyph in BATTLEFIELD_GLYPHS.items():
        fill_multicolor(slot, glyph, GROUND_LEGEND)

    # tile 11 = artillery beam (vertical white line). HUD-style: leave non-lit
    # pixels transparent so the beam can be overlaid on existing background
    # tiles when SW writes it into the tile map (Batch C usage).
    fill_glyph(BEAM_TILE_ID, BEAM_GLYPH, PAL_BORDER)

    # tiles 16..41 = letter glyphs A..Z. tile_id = LETTER_TILE_BASE + ord(L) - 'A'.
    # FG pixel = PAL_BORDER (0xFF white); compositor treats any non-zero pixel
    # as lit and emits RGB_HUD_FG.
    for letter, glyph in LETTER_GLYPHS.items():
        if letter == ':':
            slot = COLON_TILE_ID
        elif letter == ' ':
            slot = SPACE_TILE_ID
        else:
            slot = LETTER_TILE_BASE + (ord(letter) - ord('A'))
        fill_glyph(slot, glyph, PAL_BORDER)

    # tiles 48..57 = digit glyphs for HUD font. FG pixel = PAL_BORDER (0xFF
    # white). compositor.sv treats any non-zero pixel as "lit" and emits
    # RGB_HUD_FG, so any non-zero palette index works.
    for d, glyph in DIGIT_GLYPHS.items():
        fill_glyph(DIGIT_TILE_BASE + d, glyph, PAL_BORDER)

    return rom


def write_hex(path: Path, data: bytes) -> None:
    with path.open("w") as f:
        for byte in data:
            f.write(f"{byte:02x}\n")
    print(f"wrote {path} ({len(data)} bytes)")


PALETTE_DEPTH = 256
SPRITE_TABLE_DEPTH = 32


def make_palette():
    """24-bit RGB888 entries; default black, key colors set explicitly. Must
       stay in sync with sw/main.c init_palette_runtime() since that overrides
       this table once the C driver opens the device."""
    pal = [0] * PALETTE_DEPTH
    pal[PAL_PLAYER]       = 0x00FF00   # green
    pal[PAL_ENEMY_ARMED]  = 0xFF0000   # red
    pal[PAL_BULLET]       = 0xFFFF00   # yellow
    pal[PAL_ENEMY_UNARMD] = 0xFF80A0   # pink
    pal[PAL_MORTAR]       = 0xFF8000   # orange
    # 0x15 (wire gray) retired
    pal[PAL_GAS]          = 0xC0E000   # yellow-green
    pal[PAL_ARTILLERY]    = 0xFFFFE0   # bright white
    pal[PAL_AMMO_DROP]    = 0x00C040   # ammo crate green
    pal[PAL_ENEMY_BULLET] = 0xC02020   # enemy bullet red
    # Battlefield ground palette: brown dirt (3 shades) + green grass (3 shades)
    # + deep mud. Inspired by retro pixel-art tilesets where saturated earth
    # tones read clearly against the player/enemy sprites.
    pal[PAL_DIRT_MID]     = 0x6B4423   # mid brown (primary dirt)
    pal[PAL_DIRT_LIGHT]   = 0x9B7142   # sandy tan highlight
    pal[PAL_DIRT_DARK]    = 0x3A2515   # dark wet earth
    pal[PAL_GRASS_MID]    = 0x5A8B2E   # mid grass green (primary grass)
    pal[PAL_GRASS_DARK]   = 0x3D5A1F   # shadowed grass
    pal[PAL_GRASS_LIGHT]  = 0x8FBC3E   # highlight / new growth
    pal[PAL_MUD_DEEP]     = 0x1A0F08   # mud puddle interior
    pal[PAL_BORDER]       = 0xFFFFFF   # white (sprite borders)
    return pal


def make_sprite_table():
    """
    32 entries, 64 bits each, packed as the HW expects.
      W0 [31:0]  = y [31:16] | x [15:0]
      W1 [31:0]  = reserved | palette_off | flags | sprite_id
        flags layout: bit15=ACTIVE (workaround for sprite_eval), bits14:12=prio,
                      bit11=vflip, bit10=hflip, bits9:8=reserved
    Packed as int64: (W1 << 32) | W0.
    """
    def pack(x: int, y: int, sprite_id: int, prio: int = 0) -> int:
        x &= 0xFFFF
        y &= 0xFFFF
        w0 = (y << 16) | x
        active = 1 << 15                       # bit 47 of 64-bit word
        prio_field = (prio & 0x7) << 12        # bits 14:12
        w1 = (sprite_id & 0xFF) | active | prio_field
        return (w1 << 32) | w0

    table = [0] * SPRITE_TABLE_DEPTH

    # slot 0 = player, centered-ish, sprite_id 1
    table[0] = pack(x=312, y=232, sprite_id=1, prio=0)
    # slot 1 = enemy, top-left quadrant, sprite_id 2
    table[1] = pack(x=120, y=80,  sprite_id=2, prio=1)
    # slot 2 = enemy, top-right quadrant, sprite_id 2
    table[2] = pack(x=480, y=80,  sprite_id=2, prio=1)
    # slot 3 = bullet, sprite_id 3
    table[3] = pack(x=320, y=300, sprite_id=3, prio=2)
    # slots 4..31 stay 0 (active=0, invisible)

    return table


def write_hex_words(path: Path, values, width_bits: int) -> None:
    nibbles = width_bits // 4
    with path.open("w") as f:
        for v in values:
            f.write(f"{v:0{nibbles}x}\n")
    print(f"wrote {path} ({len(values)} entries, {width_bits}b each)")


def main() -> None:
    write_hex(HERE / "sprite_rom.hex", make_sprite_rom())
    write_hex(HERE / "tile_rom.hex",   make_tile_rom())
    write_hex_words(HERE / "palette.hex",       make_palette(),       24)
    write_hex_words(HERE / "sprite_table.hex",  make_sprite_table(),  64)


if __name__ == "__main__":
    main()

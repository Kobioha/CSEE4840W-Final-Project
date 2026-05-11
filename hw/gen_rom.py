#!/usr/bin/env python3
"""
gen_rom.py -- generate sprite_rom.hex, tile_rom.hex, palette.hex, sprite_table.hex

Format: $readmemh-compatible. One byte per line, lowercase hex, no addresses.

Sprite ROM: 64 sprites x 256 bytes (16x16, palette indices). Sprite 0 reserved
            transparent. Sprites 1-8 are the live game art:
              1 = player           (green bordered)
              2 = armed enemy      (red bordered + white cross)
              3 = player bullet    (small yellow block)
              4 = unarmed enemy    (solid pink, no border)
              5 = mortar shell     (orange diamond)
              6 = barbed wire      (gray X)
              7 = mustard gas      (large green-yellow circle)
              8 = artillery flash  (white plus)
Tile ROM:   64 tiles x 64 bytes (8x8, palette indices). Tile 0 = background,
            Tile 1 = background with accent dots.

Palette indices used here are mirrored in sw/main.c init_palette_runtime():
    0x00 = transparent (sprite ROM only)
    0x10 = player color (green)
    0x11 = armed enemy red
    0x12 = bullet yellow
    0x13 = unarmed enemy pink
    0x14 = mortar orange
    0x15 = barbed wire gray
    0x16 = mustard gas yellow-green
    0x17 = artillery bright white-yellow
    0x20 = tile background
    0x21 = tile accent
    0xFF = white (sprite borders)

Run: ``python3 gen_rom.py`` from the hw/ directory.
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
PAL_WIRE         = 0x15
PAL_GAS          = 0x16
PAL_ARTILLERY    = 0x17
PAL_BG           = 0x20
PAL_BG_ACCENT    = 0x21
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

    # slot 0: transparent (already)
    fill_bordered(1, PAL_PLAYER,       PAL_BORDER)   # player
    fill_bordered(2, PAL_ENEMY_ARMED,  PAL_BORDER)   # armed enemy
    overlay_cross(2, PAL_BORDER)                     # cross badge identifies "armed"
    fill_centered(3, PAL_BULLET, 2)                  # bullet: 4x4 centered
    fill_solid   (4, PAL_ENEMY_UNARMD)               # unarmed: solid pink
    fill_diamond (5, PAL_MORTAR, 7)                  # mortar shell: diamond
    fill_x       (6, PAL_WIRE, 2)                    # barbed wire: thick X
    fill_circle  (7, PAL_GAS, 7)                     # mustard gas: large disk
    fill_plus    (8, PAL_ARTILLERY, 7, 2)            # artillery flash: thick '+'

    return rom


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


def make_palette() -> list[int]:
    """24-bit RGB888 entries; default black, key colors set explicitly. Must
       stay in sync with sw/main.c init_palette_runtime() since that overrides
       this table once the C driver opens the device."""
    pal = [0] * PALETTE_DEPTH
    pal[PAL_PLAYER]       = 0x00FF00   # green
    pal[PAL_ENEMY_ARMED]  = 0xFF0000   # red
    pal[PAL_BULLET]       = 0xFFFF00   # yellow
    pal[PAL_ENEMY_UNARMD] = 0xFF80A0   # pink
    pal[PAL_MORTAR]       = 0xFF8000   # orange
    pal[PAL_WIRE]         = 0x808080   # gray
    pal[PAL_GAS]          = 0xC0E000   # yellow-green
    pal[PAL_ARTILLERY]    = 0xFFFFE0   # bright white
    pal[PAL_BG]           = 0x404040   # dark gray
    pal[PAL_BG_ACCENT]    = 0x606060   # lighter gray
    pal[PAL_BORDER]       = 0xFFFFFF   # white (sprite borders)
    return pal


def make_sprite_table() -> list[int]:
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


def write_hex_words(path: Path, values: list[int], width_bits: int) -> None:
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

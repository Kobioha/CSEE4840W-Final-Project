#!/usr/bin/env python3
"""
gen_rom.py -- generate placeholder sprite_rom.hex and tile_rom.hex

Format: $readmemh-compatible. One byte per line, lowercase hex, no addresses.

Sprite ROM: 64 sprites x 256 bytes (16x16, palette indices). Sprite 0 reserved
            transparent. Sprites 1-3 are the placeholder player/enemy/bullet.
Tile ROM:   64 tiles x 64 bytes (8x8, palette indices). Tile 0 = background,
            Tile 1 = background with accent dots.

Palette indices used here are referenced from the SW driver:
    0x00 = transparent (sprite ROM only)
    0x10 = player color  (e.g. green)
    0x11 = enemy color   (e.g. red)
    0x12 = bullet color  (e.g. yellow)
    0x20 = tile background
    0x21 = tile accent

Run: ``python3 gen_rom.py`` from the hw/ directory.
"""

from pathlib import Path

HERE = Path(__file__).resolve().parent

SPRITE_ROM_BYTES = 64 * 256          # 16384
TILE_ROM_BYTES   = 64 * 64           #  4096

PAL_TRANSPARENT = 0x00
PAL_PLAYER      = 0x10
PAL_ENEMY       = 0x11
PAL_BULLET      = 0x12
PAL_BG          = 0x20
PAL_BG_ACCENT   = 0x21


def make_sprite_rom() -> bytearray:
    rom = bytearray(SPRITE_ROM_BYTES)  # zero-init = all transparent

    def fill_solid(slot: int, pal: int) -> None:
        for v in range(16):
            for u in range(16):
                rom[slot * 256 + v * 16 + u] = pal

    def fill_bordered(slot: int, fill: int, border: int) -> None:
        for v in range(16):
            for u in range(16):
                edge = (u == 0 or u == 15 or v == 0 or v == 15)
                rom[slot * 256 + v * 16 + u] = border if edge else fill

    def fill_centered(slot: int, pal: int, half: int) -> None:
        # half=2 -> 4x4 centered block, etc.
        for v in range(16):
            for u in range(16):
                in_center = abs(u - 8) < half and abs(v - 8) < half
                rom[slot * 256 + v * 16 + u] = pal if in_center else 0

    # slot 0: transparent (already)
    fill_bordered(1, PAL_PLAYER, 0xFF)   # player: green with magenta border (0xFF=white-ish)
    fill_bordered(2, PAL_ENEMY,  0xFF)   # enemy:  red with border
    fill_centered(3, PAL_BULLET, 2)      # bullet: 4x4 centered block
    return rom


def make_tile_rom() -> bytearray:
    rom = bytearray(TILE_ROM_BYTES)

    def fill_solid(slot: int, pal: int) -> None:
        for v in range(8):
            for u in range(8):
                rom[slot * 64 + v * 8 + u] = pal

    # tile 0: solid background
    fill_solid(0, PAL_BG)

    # tile 1: background with accent dots in the corners
    fill_solid(1, PAL_BG)
    for v, u in ((0, 0), (0, 7), (7, 0), (7, 7)):
        rom[1 * 64 + v * 8 + u] = PAL_BG_ACCENT

    return rom


def write_hex(path: Path, data: bytes) -> None:
    with path.open("w") as f:
        for byte in data:
            f.write(f"{byte:02x}\n")
    print(f"wrote {path} ({len(data)} bytes)")


PALETTE_DEPTH = 256
SPRITE_TABLE_DEPTH = 32


def make_palette() -> list[int]:
    """24-bit RGB888 entries; default black, key colors set explicitly."""
    pal = [0] * PALETTE_DEPTH
    pal[PAL_PLAYER]    = 0x00FF00      # green
    pal[PAL_ENEMY]     = 0xFF0000      # red
    pal[PAL_BULLET]    = 0xFFFF00      # yellow
    pal[PAL_BG]        = 0x404040      # dark gray
    pal[PAL_BG_ACCENT] = 0x606060      # lighter gray
    pal[0xFF]          = 0xFFFFFF      # white (sprite borders)
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

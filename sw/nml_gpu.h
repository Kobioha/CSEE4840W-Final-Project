#ifndef NML_GPU_H
#define NML_GPU_H

/*
 * nml_gpu.h -- userspace driver header for the No Man's Land FPGA peripheral.
 *
 * Register offsets, sprite layout, and writer prototypes mirror DESIGN.md
 * sections 5 and 6.3. The driver maps the HPS-to-FPGA Lightweight bridge
 * (16 KB at 0xFF200000 in HPS physical address space) into the process via
 * /dev/mem and exposes typed writers for each register region.
 *
 * Caveat: the SystemVerilog sprite_eval module reads bit 47 of the 64-bit
 * sprite-table entry as an "active" flag (not bit-for-bit faithful to the
 * design doc, which uses sentinel coordinates). The convenience writers
 * below set bit 7 of `flags` for visible sprites and clear it for hidden.
 */

#include <stdint.h>

/* HPS physical address of the Lightweight bridge base + window size. */
#define NML_LWFPGA_BASE       0xFF200000UL
#define NML_LWFPGA_SPAN       0x00004000UL    /* 16 KB */

/* Register offsets (bytes from peripheral base). */
#define NML_REG_CTRL          0x0000
#define NML_REG_STATUS        0x0004
#define NML_REG_BG_SCROLL     0x0008
#define NML_REG_IRQ_MASK      0x000C
#define NML_REG_PLAYER_POS    0x0010
#define NML_REG_PLAYER_STATS  0x0014
#define NML_REG_SCORE         0x0018
#define NML_REG_KILL_COUNT    0x001C
#define NML_SPRITE_TABLE_BASE 0x0100   /* 32 entries x 8B   = 256B  */
#define NML_PALETTE_BASE      0x0400   /* 256 entries x 4B  = 1KB   */
#define NML_TILEMAP_BASE      0x1000   /* 80 cols x 60 rows = 4800B */

/* CTRL bits */
#define NML_CTRL_ENABLE       (1u << 0)
#define NML_CTRL_SWAP         (1u << 1)
#define NML_CTRL_HUD_ON       (1u << 2)

/* STATUS bits */
#define NML_STATUS_VBLANK       (1u << 0)
#define NML_STATUS_SWAP_PENDING (1u << 1)

/* Off-screen sentinel (kept for future use; the current sprite_eval relies on
 * the flags ACTIVE bit instead, so nml_hide_sprite() also clears that bit). */
#define NML_SPRITE_HIDE       ((int16_t)-256)

/* Tile map dimensions */
#define NML_TILEMAP_COLS      80
#define NML_TILEMAP_ROWS      60

/* Number of sprite slots */
#define NML_MAX_SPRITES       32

/* Flags bit layout in the sprite struct (matches HW packing of W1[15:8]):
 *   bit 0..1  reserved (must be 0)
 *   bit 2     hflip
 *   bit 3     vflip
 *   bit 4..6  priority (0 = highest)
 *   bit 7     ACTIVE -- set for visible sprites (sprite_eval workaround)
 */
#define NML_FLAG_HFLIP        (1u << 2)
#define NML_FLAG_VFLIP        (1u << 3)
#define NML_FLAG_PRIO_SHIFT   4
#define NML_FLAG_PRIO_MASK    (0x7u << NML_FLAG_PRIO_SHIFT)
#define NML_FLAG_ACTIVE       (1u << 7)

#define NML_FLAGS(prio, hflip, vflip)                       \
    (((((prio) & 0x7u) << NML_FLAG_PRIO_SHIFT)) |           \
     ((hflip) ? NML_FLAG_HFLIP : 0u) |                      \
     ((vflip) ? NML_FLAG_VFLIP : 0u) |                      \
     NML_FLAG_ACTIVE)

/* Sprite descriptor matching the HW 8-byte slot layout exactly. */
typedef struct {
    int16_t  x;
    int16_t  y;
    uint8_t  sprite_id;
    uint8_t  flags;            /* use NML_FLAGS(...) */
    uint8_t  palette_off;
    uint8_t  reserved;
} nml_sprite_t;

/* Public API. */
int  nml_open(void);
void nml_close(void);
void nml_set_enable(int on);
void nml_set_hud_on(int on);
void nml_write_palette(int idx, uint8_t r, uint8_t g, uint8_t b);
void nml_write_tile(int col, int row, uint8_t tile_id);
void nml_write_sprite(int slot, const nml_sprite_t *s);
void nml_hide_sprite(int slot);
void nml_clear_sprites(void);
void nml_set_player_state(int16_t px, int16_t py,
                          uint8_t hp, uint8_t wave, uint16_t level);
void nml_set_score(uint32_t score, uint32_t kills);
void nml_commit_frame(void);              /* sets SWAP, returns when committed */
int  nml_in_vblank(void);

#endif /* NML_GPU_H */

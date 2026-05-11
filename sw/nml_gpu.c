/*
 * nml_gpu.c -- userspace driver for the No Man's Land FPGA peripheral.
 * Maps the HPS-to-FPGA Lightweight bridge via /dev/mem and provides typed
 * writers for the register map described in DESIGN.md sections 5 and 6.3.
 */

#define _GNU_SOURCE
#include "nml_gpu.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

static int             g_devmem_fd = -1;
static volatile void  *g_base      = NULL;

static volatile uint32_t *reg_ptr(unsigned offset) {
    return (volatile uint32_t *)((volatile uint8_t *)g_base + offset);
}

static void reg_write(unsigned offset, uint32_t value) {
    *reg_ptr(offset) = value;
}

static uint32_t reg_read(unsigned offset) {
    return *reg_ptr(offset);
}

int nml_open(void) {
    if (g_base != NULL) return 0;

    g_devmem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (g_devmem_fd < 0) {
        perror("nml_open: /dev/mem");
        return -1;
    }

    g_base = mmap(NULL,
                  NML_LWFPGA_SPAN,
                  PROT_READ | PROT_WRITE,
                  MAP_SHARED,
                  g_devmem_fd,
                  (off_t)NML_LWFPGA_BASE);
    if (g_base == MAP_FAILED) {
        perror("nml_open: mmap");
        close(g_devmem_fd);
        g_devmem_fd = -1;
        g_base = NULL;
        return -1;
    }

    /* Hidden sentinel for unused slots. */
    nml_clear_sprites();
    return 0;
}

void nml_close(void) {
    if (g_base != NULL) {
        munmap((void *)g_base, NML_LWFPGA_SPAN);
        g_base = NULL;
    }
    if (g_devmem_fd >= 0) {
        close(g_devmem_fd);
        g_devmem_fd = -1;
    }
}

void nml_set_enable(int on) {
    uint32_t ctrl = reg_read(NML_REG_CTRL);
    if (on) ctrl |=  NML_CTRL_ENABLE;
    else    ctrl &= ~NML_CTRL_ENABLE;
    reg_write(NML_REG_CTRL, ctrl);
}

void nml_set_hud_on(int on) {
    uint32_t ctrl = reg_read(NML_REG_CTRL);
    if (on) ctrl |=  NML_CTRL_HUD_ON;
    else    ctrl &= ~NML_CTRL_HUD_ON;
    reg_write(NML_REG_CTRL, ctrl);
}

void nml_write_palette(int idx, uint8_t r, uint8_t g, uint8_t b) {
    if (idx < 0 || idx >= 256) return;
    reg_write(NML_PALETTE_BASE + ((unsigned)idx * 4u),
              ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}

void nml_write_tile(int col, int row, uint8_t tile_id) {
    if (col < 0 || col >= NML_TILEMAP_COLS) return;
    if (row < 0 || row >= NML_TILEMAP_ROWS) return;
    unsigned offset = NML_TILEMAP_BASE + (unsigned)(row * NML_TILEMAP_COLS + col);
    /* Byte write: ARM emits STRB, the LW bridge sets avs_byteenable for the
     * targeted lane, and the HW selects that lane via avs_address[1:0]. */
    *((volatile uint8_t *)g_base + offset) = tile_id;
}

void nml_write_sprite(int slot, const nml_sprite_t *s) {
    if (slot < 0 || slot >= NML_MAX_SPRITES || s == NULL) return;
    unsigned base = NML_SPRITE_TABLE_BASE + (unsigned)slot * 8u;

    uint32_t w0 = ((uint32_t)(uint16_t)s->y << 16) | (uint32_t)(uint16_t)s->x;
    uint32_t w1 = (uint32_t)s->sprite_id        |
                  ((uint32_t)s->flags       <<  8) |
                  ((uint32_t)s->palette_off << 16) |
                  ((uint32_t)s->reserved    << 24);

    reg_write(base + 0, w0);
    reg_write(base + 4, w1);
}

void nml_hide_sprite(int slot) {
    nml_sprite_t hidden = {
        .x           = NML_SPRITE_HIDE,
        .y           = NML_SPRITE_HIDE,
        .sprite_id   = 0,
        .flags       = 0,                 /* clears ACTIVE */
        .palette_off = 0,
        .reserved    = 0,
    };
    nml_write_sprite(slot, &hidden);
}

void nml_clear_sprites(void) {
    for (int i = 0; i < NML_MAX_SPRITES; ++i) nml_hide_sprite(i);
}

/* BCD-pack `n` into nibbles for the HW HUD digit lookup. nibble 0 = ones,
 * nibble 1 = tens, etc. Up to `nibbles` digits; overflow silently wraps. */
static uint32_t bcd_pack(uint32_t n, int nibbles) {
    uint32_t out = 0;
    for (int i = 0; i < nibbles; ++i) {
        out |= (n % 10u) << (i * 4);
        n /= 10u;
    }
    return out;
}

void nml_set_player_state(int16_t px, int16_t py,
                          uint8_t hp, uint8_t wave, uint16_t level) {
    reg_write(NML_REG_PLAYER_POS,
              ((uint32_t)(uint16_t)py << 16) | (uint32_t)(uint16_t)px);

    /* Wave is BCD-packed into bits [15:8] so the HW HUD can read 2 digit
     * nibbles without a binary divider. hp stays binary 0..100 because the
     * HP bar is a fill-fraction, not a digit string. */
    uint32_t wave_bcd = bcd_pack((uint32_t)wave, 2);
    reg_write(NML_REG_PLAYER_STATS,
              (uint32_t)hp                  |
              ((wave_bcd & 0xFFu)    <<  8) |
              ((uint32_t)level       << 16));
}

void nml_set_score(uint32_t score, uint32_t kills) {
    /* Score is BCD-packed into bits [23:0] (6 digits). Above 999,999 the
     * top digits silently wrap -- bullet stream caps score well below that. */
    uint32_t score_bcd = bcd_pack(score, 6);
    reg_write(NML_REG_SCORE,      score_bcd);
    reg_write(NML_REG_KILL_COUNT, kills);
}

void nml_set_hud_aux(uint8_t ammo, uint8_t art_charges, uint8_t gas_charges) {
    /* Ammo BCD-packed into bits [7:0]; charges occupy single 4-bit nibbles
     * at [11:8] (art) and [15:12] (gas). Clamp inputs so a runaway value
     * can't corrupt other bits. */
    uint32_t ammo_bcd = bcd_pack((uint32_t)ammo, 2) & 0xFFu;
    uint32_t art      = (uint32_t)(art_charges > 9 ? 9 : art_charges) & 0xFu;
    uint32_t gas      = (uint32_t)(gas_charges > 9 ? 9 : gas_charges) & 0xFu;
    reg_write(NML_REG_HUD_AUX, ammo_bcd | (art << 8) | (gas << 12));
}

int nml_in_vblank(void) {
    return (reg_read(NML_REG_STATUS) & NML_STATUS_VBLANK) ? 1 : 0;
}

uint32_t nml_probe_tile_word(unsigned byte_offset) {
    unsigned word_off = byte_offset & ~0x3u;
    return reg_read(NML_TILEMAP_BASE + word_off);
}

/* Settled read: do the access twice and return the second. Avalon-MM slaves
 * that drive avs_readdata one cycle after avs_read (registered output) need
 * an extra dummy access for the master to capture the right cycle. If reads
 * are merely stale-by-one, the second access settles to the right value. */
static uint32_t reg_read_settled(unsigned offset) {
    (void)reg_read(offset);
    __sync_synchronize();
    return reg_read(offset);
}

void nml_debug_full_probe(void) {
    if (g_base == NULL) {
        printf("nml_debug_full_probe: g_base is NULL, call nml_open first\n");
        return;
    }

    printf("=== Comprehensive HW probe ===\n");

    /* [Z0] Definitive register round-trip on CTRL. If we cannot write a
     * known value and read it back, no other test result is meaningful. */
    uint32_t ctrl_orig = reg_read(NML_REG_CTRL);
    printf("[Z0] CTRL initial (first read)  = 0x%08x\n", ctrl_orig);
    uint32_t ctrl_orig_settled = reg_read_settled(NML_REG_CTRL);
    printf("[Z0] CTRL initial (settled)     = 0x%08x  (expect 0x01 at reset)\n",
           ctrl_orig_settled);

    reg_write(NML_REG_CTRL, 0x00000005u);   /* ENABLE | HUD_ON */
    __sync_synchronize();
    printf("[Z0] After WR 0x05, single read = 0x%08x\n", reg_read(NML_REG_CTRL));
    printf("[Z0] After WR 0x05, settled rd  = 0x%08x  (expect 0x05)\n",
           reg_read_settled(NML_REG_CTRL));

    reg_write(NML_REG_CTRL, 0x00000001u);   /* back to ENABLE only */
    __sync_synchronize();
    printf("[Z0] After WR 0x01, settled rd  = 0x%08x  (expect 0x01)\n",
           reg_read_settled(NML_REG_CTRL));

    /* [A] Original CTRL + STATUS reads, both single and settled. */
    printf("[A] CTRL single=0x%08x  CTRL settled=0x%08x\n",
           reg_read(NML_REG_CTRL),
           reg_read_settled(NML_REG_CTRL));
    printf("[A] STATUS single=0x%08x  STATUS settled=0x%08x\n",
           reg_read(NML_REG_STATUS),
           reg_read_settled(NML_REG_STATUS));

    /* [B] Sanity: palette readback. init_palette_runtime() should have set
     * 0x20 to 0x6B4423 (mid-brown dirt) and 0x25 to 0x8FBC3E (grass-light). */
    printf("[B] palette[0x20] single=0x%08x  settled=0x%08x (expect 0x006B4423)\n",
           reg_read(NML_PALETTE_BASE + 0x20u * 4u),
           reg_read_settled(NML_PALETTE_BASE + 0x20u * 4u));
    printf("[B] palette[0x25] single=0x%08x  settled=0x%08x (expect 0x008FBC3E)\n",
           reg_read(NML_PALETTE_BASE + 0x25u * 4u),
           reg_read_settled(NML_PALETTE_BASE + 0x25u * 4u));

    /* [C] Tile-map cold reads (BEFORE any tile writes). Use settled reads. */
    printf("[C] Tile-map cold reads (no writes yet, settled):\n");
    for (unsigned o = 0; o < 32; o += 4) {
        printf("    off=0x%04x word=0x%08x\n", o,
               reg_read_settled(NML_TILEMAP_BASE + o));
    }

    /* [D] Word-write test at offset 0x100. */
    reg_write(NML_TILEMAP_BASE + 0x100u, 0x12345678u);
    __sync_synchronize();
    printf("[D] After STR 0x12345678 at off=0x100: single=0x%08x settled=0x%08x\n",
           reg_read(NML_TILEMAP_BASE + 0x100u),
           reg_read_settled(NML_TILEMAP_BASE + 0x100u));

    /* [E] Single STRB test at offset 0x110, byte_lane=0. */
    *((volatile uint8_t *)g_base + NML_TILEMAP_BASE + 0x110u) = 0xA5u;
    __sync_synchronize();
    printf("[E] After STRB 0xA5 at off=0x110 (lane 0): single=0x%08x settled=0x%08x\n",
           reg_read(NML_TILEMAP_BASE + 0x110u),
           reg_read_settled(NML_TILEMAP_BASE + 0x110u));

    /* [F] Four consecutive STRBs at offsets 0x120..0x123 (all 4 byte lanes). */
    *((volatile uint8_t *)g_base + NML_TILEMAP_BASE + 0x120u) = 0x60u;
    *((volatile uint8_t *)g_base + NML_TILEMAP_BASE + 0x121u) = 0x61u;
    *((volatile uint8_t *)g_base + NML_TILEMAP_BASE + 0x122u) = 0x62u;
    *((volatile uint8_t *)g_base + NML_TILEMAP_BASE + 0x123u) = 0x63u;
    __sync_synchronize();
    printf("[F] After 4 STRBs 0x60..0x63 at offsets 0x120..0x123:\n");
    printf("    word at 0x120 settled=0x%08x  (low byte should be 0x60)\n",
           reg_read_settled(NML_TILEMAP_BASE + 0x120u));
    printf("    word at 0x124 settled=0x%08x  (slot unwritten)\n",
           reg_read_settled(NML_TILEMAP_BASE + 0x124u));

    /* [H] Spread reads to detect whether mem_waddr propagates. */
    printf("[H] Spread settled reads:\n");
    unsigned offs[] = { 0u, 4u, 0x40u, 0x80u, 0x100u, 0x110u, 0x120u, 0x300u, 0x1000u };
    for (unsigned i = 0; i < sizeof(offs)/sizeof(offs[0]); i++) {
        printf("    off=0x%04x: 0x%08x\n", offs[i],
               reg_read_settled(NML_TILEMAP_BASE + offs[i]));
    }

    fflush(stdout);
}

void nml_commit_frame(void) {
    /* Set SWAP; HW auto-clears it. Wait until SWAP_PENDING falls (commit
     * happened on the next vsync). Cap the wait so a stuck pipeline doesn't
     * hang the game loop. */
    uint32_t ctrl = reg_read(NML_REG_CTRL);
    reg_write(NML_REG_CTRL, ctrl | NML_CTRL_SWAP);

    for (int i = 0; i < 200000; ++i) {
        if (!(reg_read(NML_REG_STATUS) & NML_STATUS_SWAP_PENDING)) return;
    }
    fprintf(stderr, "nml_commit_frame: timed out waiting for SWAP commit\n");
}

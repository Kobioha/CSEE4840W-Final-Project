/*
 * render.c -- translates the entity pool into nml_gpu sprite-table writes.
 *
 * Conventions (matching render_terminal.c so behavior is identical between
 * builds): slot 0 is always the player, sprite_id 1. Subsequent slots are
 * filled in entity-array order with enemies, bullets, hazards, etc.; gas
 * clouds expand to up to 4 sprite slots each. Unused slots are hidden via
 * nml_hide_sprite() so leftover state from prior frames is gone.
 *
 * Sprite-id assignments must match the layout in hw/gen_rom.py.
 */

#include "render.h"
#include "autoatk.h"
#include "nml_gpu.h"

#define SPRITE_ID_PLAYER         1
#define SPRITE_ID_ENEMY_ARMED    2
#define SPRITE_ID_BULLET         3
#define SPRITE_ID_ENEMY_UNARMED  4
#define SPRITE_ID_MORTAR         5
/* Sprite ID 6 (barbed wire) retired this batch -- no game-side dispatch
   targets it any longer. Batch B may reuse the ROM slot. */
#define SPRITE_ID_GAS            7
#define SPRITE_ID_ARTILLERY      8

/* Placeholder palette offsets until Batch B gives ammo / enemy bullet their
   own sprite art. Offset is added to the sprite-pixel index to recolor a
   stock sprite. Hardware ignores the offset when the base index is 0
   (transparent). Values chosen so the existing yellow bullet sprite tints
   green for ammo drops and red for enemy bullets. */
#define PALETTE_OFF_AMMO_DROP    0x06  /* 0x12 yellow + 6 -> green-ish */
#define PALETTE_OFF_ENEMY_BULLET 0xFE  /* (-2 mod 256) yellow -> red   */

static uint8_t autoatk_sprite_id(int payload) {
    switch (payload) {
        case AA_MORTAR:    return SPRITE_ID_MORTAR;
        case AA_GAS:       return SPRITE_ID_GAS;
        case AA_ARTILLERY: return SPRITE_ID_ARTILLERY;
        default:           return SPRITE_ID_BULLET;
    }
}

static uint8_t entity_to_sprite_id(const entity_t *e) {
    switch (e->kind) {
        case ENT_PLAYER:        return SPRITE_ID_PLAYER;
        case ENT_ENEMY_ARMED:   return SPRITE_ID_ENEMY_ARMED;
        case ENT_ENEMY_UNARMED: return SPRITE_ID_ENEMY_UNARMED;
        case ENT_BULLET:        return SPRITE_ID_BULLET;
        case ENT_ENEMY_BULLET:  return SPRITE_ID_BULLET;
        case ENT_AMMO_DROP:     return SPRITE_ID_BULLET;
        case ENT_AUTO_PROJ:
        case ENT_HAZARD:        return autoatk_sprite_id(e->payload);
        default:                return 0;
    }
}

static uint8_t entity_palette_off(const entity_t *e) {
    if (e->kind == ENT_AMMO_DROP)    return PALETTE_OFF_AMMO_DROP;
    if (e->kind == ENT_ENEMY_BULLET) return PALETTE_OFF_ENEMY_BULLET;
    return 0;
}

/* Emit a 16x16 sprite into slot `slot_inout` (advanced in-place). Returns 1
   if a slot was consumed, 0 if the table is already full (silent drop). */
static int emit_sprite(int *slot_inout, int x, int y, uint8_t sid,
                       uint8_t pal_off, int prio) {
    int slot = *slot_inout;
    if (slot >= NML_MAX_SPRITES) return 0;
    nml_sprite_t s = {
        .x = (int16_t)x, .y = (int16_t)y,
        .sprite_id   = sid,
        .flags       = NML_FLAGS(prio, /*hflip=*/0, /*vflip=*/0),
        .palette_off = pal_off,
        .reserved    = 0,
    };
    nml_write_sprite(slot, &s);
    *slot_inout = slot + 1;
    return 1;
}

/* Gas cloud rendering: emit a 16x16 sprite at the center plus up to 3 more
   forming a + pattern as the cloud expands toward its full 48x32 size.
   The visible-size threshold for emitting the side/top sprites grows with
   the cloud's current width/height. */
static void emit_gas_cluster(const entity_t *e, int *slot_inout) {
    int w, h;
    autoatk_gas_size(e, &w, &h);
    int cx = e->x;        /* origin is top-left of central 16x16 */
    int cy = e->y;
    emit_sprite(slot_inout, cx, cy, SPRITE_ID_GAS, 0, 1);   /* center */
    if (w >= 24) {
        emit_sprite(slot_inout, cx - 12, cy, SPRITE_ID_GAS, 0, 1);   /* left */
        emit_sprite(slot_inout, cx + 12, cy, SPRITE_ID_GAS, 0, 1);   /* right */
    }
    if (h >= 24) {
        emit_sprite(slot_inout, cx, cy - 10, SPRITE_ID_GAS, 0, 1);   /* top */
    }
}

/*
 * Level-up scene: player frozen + three weapon-sprite tiles along the top
 * (each showing the actual sprite of the weapon on offer), with a cursor
 * sprite above the active option. The SSH terminal still carries the menu
 * text and level annotations; this gives the player on-screen "what am I
 * picking" feedback without leaving the VGA monitor.
 */
static void render_levelup(const game_t *g) {
    const entity_t *p = &g->ents[g->player_i];

    nml_sprite_t player = {
        .x = (int16_t)p->x, .y = (int16_t)p->y,
        .sprite_id = SPRITE_ID_PLAYER,
        .flags = NML_FLAGS(/*prio=*/0, /*hflip=*/0, /*vflip=*/0),
        .palette_off = 0, .reserved = 0,
    };
    nml_write_sprite(0, &player);

    const int xs[3]   = { 160, 320, 480 };
    const int y_opt   = 100;
    const int y_curs  = 80;

    for (int i = 0; i < LEVELUP_OPTIONS; ++i) {
        int kind = g->levelup_options[i];
        uint8_t sid = (kind >= 0 && kind < AA_COUNT)
            ? autoatk_sprite_id(kind)
            : SPRITE_ID_BULLET;
        nml_sprite_t s = {
            .x = (int16_t)xs[i], .y = (int16_t)y_opt,
            .sprite_id = sid,
            .flags = NML_FLAGS(/*prio=*/1, 0, 0),
            .palette_off = 0, .reserved = 0,
        };
        nml_write_sprite(1 + i, &s);
    }

    int cursor_x = xs[g->levelup_cursor < 0 ? 0 :
                      (g->levelup_cursor >= LEVELUP_OPTIONS
                       ? LEVELUP_OPTIONS - 1
                       : g->levelup_cursor)];
    nml_sprite_t cur = {
        .x = (int16_t)cursor_x, .y = (int16_t)y_curs,
        .sprite_id = SPRITE_ID_PLAYER,
        .flags = NML_FLAGS(/*prio=*/0, 0, 0),
        .palette_off = 0, .reserved = 0,
    };
    nml_write_sprite(4, &cur);

    for (int slot = 5; slot < NML_MAX_SPRITES; ++slot) {
        nml_hide_sprite(slot);
    }

    nml_set_player_state((int16_t)p->x, (int16_t)p->y,
                         (uint8_t)(g->player_hp > 0 ? g->player_hp : 0),
                         (uint8_t)(g->wave_index + 1),
                         /*level=*/0);
    nml_set_score((uint32_t)g->score, 0u);
}

/*
 * Game-over screen: lay out a 5-sprite X centered on the screen using bullet
 * sprites. Batch C replaces this with on-screen text rendered via the tile
 * map once letter glyphs land in tile ROM.
 */
static void render_game_over(const game_t *g) {
    const int cx = SCREEN_W / 2 - 8;   /* sprites are 16x16; offset to center */
    const int cy = SCREEN_H / 2 - 8;
    const int step = 20;

    const int xs[5] = { cx,           cx - step, cx + step, cx - step, cx + step };
    const int ys[5] = { cy,           cy - step, cy - step, cy + step, cy + step };

    /* Slot 0 was always the player; replace with first X-arm. */
    for (int i = 0; i < 5; ++i) {
        nml_sprite_t s = {
            .x           = (int16_t)xs[i],
            .y           = (int16_t)ys[i],
            .sprite_id   = SPRITE_ID_BULLET,
            .flags       = NML_FLAGS(/*prio=*/0, /*hflip=*/0, /*vflip=*/0),
            .palette_off = 0,
            .reserved    = 0,
        };
        nml_write_sprite(i, &s);
    }

    /* Hide the rest. */
    for (int slot = 5; slot < NML_MAX_SPRITES; ++slot) {
        nml_hide_sprite(slot);
    }

    /* Still publish the final score for the (future) HUD. */
    nml_set_player_state(0, 0, 0, /*wave=*/0, /*level=*/0);
    nml_set_score((uint32_t)g->score, /*kills=*/0u);
}

void render_frame(const game_t *g) {
    if (g->state == STATE_GAMEOVER) {
        render_game_over(g);
        return;
    }
    if (g->state == STATE_LEVELUP) {
        render_levelup(g);
        return;
    }

    /* Slot 0 = player, always written. */
    const entity_t *p = &g->ents[g->player_i];

    nml_sprite_t player = {
        .x           = (int16_t)p->x,
        .y           = (int16_t)p->y,
        .sprite_id   = SPRITE_ID_PLAYER,
        .flags       = NML_FLAGS(/*prio=*/0, /*hflip=*/0, /*vflip=*/0),
        .palette_off = 0,
        .reserved    = 0,
    };
    nml_write_sprite(0, &player);

    /* Slots 1..31: active non-player entities. Gas hazards expand into
       multiple slots; other entities consume one slot each. */
    int slot = 1;
    for (int i = 0; i < MAX_ENTITIES && slot < NML_MAX_SPRITES; ++i) {
        const entity_t *e = &g->ents[i];
        if (!e->active || e->kind == ENT_PLAYER) continue;

        if (e->kind == ENT_HAZARD && e->payload == (int)AA_GAS) {
            emit_gas_cluster(e, &slot);
            continue;
        }

        int prio = (e->kind == ENT_BULLET || e->kind == ENT_ENEMY_BULLET) ? 2 : 1;
        emit_sprite(&slot, e->x, e->y,
                    entity_to_sprite_id(e),
                    entity_palette_off(e),
                    prio);
    }

    /* Hide leftover slots so old state doesn't ghost. */
    for (; slot < NML_MAX_SPRITES; ++slot) {
        nml_hide_sprite(slot);
    }

    /* Mailbox the player state for the HUD overlay. */
    nml_set_player_state((int16_t)p->x,
                         (int16_t)p->y,
                         (uint8_t)(g->player_hp > 0 ? g->player_hp : 0),
                         (uint8_t)(g->wave_index + 1),
                         /*level=*/0);
    nml_set_score((uint32_t)g->score, /*kills=*/0u);
}

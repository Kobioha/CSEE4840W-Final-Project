/*
 * render.c -- translates the entity pool into nml_gpu sprite-table writes.
 *
 * Conventions (matching render_terminal.c so behavior is identical between
 * builds): slot 0 is always the player, sprite_id 1. Subsequent slots are
 * filled in entity-array order with enemies and bullets. Unused slots are
 * hidden via nml_hide_sprite() so leftover state from prior frames is gone.
 *
 * Sprite-id assignments must match the layout in hw/gen_rom.py.
 */

#include "render.h"
#include "nml_gpu.h"

#define SPRITE_ID_PLAYER         1
#define SPRITE_ID_ENEMY_ARMED    2
#define SPRITE_ID_BULLET         3
#define SPRITE_ID_ENEMY_UNARMED  4
#define SPRITE_ID_MORTAR         5
#define SPRITE_ID_WIRE           6
#define SPRITE_ID_GAS            7
#define SPRITE_ID_ARTILLERY      8

static uint8_t autoatk_sprite_id(int payload) {
    switch (payload) {
        case AA_MORTAR:    return SPRITE_ID_MORTAR;
        case AA_WIRE:      return SPRITE_ID_WIRE;
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
        case ENT_AUTO_PROJ:
        case ENT_HAZARD:        return autoatk_sprite_id(e->payload);
        default:                return 0;
    }
}

/*
 * Level-up scene: player frozen + three weapon-sprite tiles along the top
 * (each showing the actual sprite of the weapon on offer: mortar diamond /
 * wire X / gas circle / artillery plus), with a green cursor sprite above
 * the active option. The SSH terminal still carries the menu text and
 * level annotations; this gives the player on-screen "what am I picking"
 * feedback without leaving the VGA monitor.
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
 * sprites. No font yet, so this is a placeholder distinct enough from gameplay
 * (static, centered, X-shaped) that the player can tell they died.
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

    /* Slots 1..31: first N active non-player entities. */
    int slot = 1;
    for (int i = 0; i < MAX_ENTITIES && slot < NML_MAX_SPRITES; ++i) {
        const entity_t *e = &g->ents[i];
        if (!e->active || e->kind == ENT_PLAYER) continue;

        nml_sprite_t s = {
            .x           = (int16_t)e->x,
            .y           = (int16_t)e->y,
            .sprite_id   = entity_to_sprite_id(e),
            .flags       = NML_FLAGS(/*prio=*/(e->kind == ENT_BULLET ? 2 : 1),
                                     /*hflip=*/0, /*vflip=*/0),
            .palette_off = 0,
            .reserved    = 0,
        };
        nml_write_sprite(slot++, &s);
    }

    /* Hide leftover slots so old state doesn't ghost. */
    for (; slot < NML_MAX_SPRITES; ++slot) {
        nml_hide_sprite(slot);
    }

    /* Mailbox the player state for the eventual HUD overlay. */
    nml_set_player_state((int16_t)p->x,
                         (int16_t)p->y,
                         (uint8_t)(g->player_hp > 0 ? g->player_hp : 0),
                         /*wave=*/0,
                         /*level=*/0);
    nml_set_score((uint32_t)g->score, /*kills=*/0u);
}

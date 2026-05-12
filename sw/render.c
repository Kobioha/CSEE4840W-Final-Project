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

#include <stdio.h>
#include <string.h>

/* Tile slots from hw/gen_rom.py BATTLEFIELD_GLYPHS + letter/digit tables. */
#define TILE_BLANK_BG       0
#define TILE_DIRT_A         4
#define TILE_DIRT_B         5
#define TILE_GRASS_A        6
#define TILE_GRASS_B        7
#define TILE_TRANSITION     8
#define TILE_MUD_PUDDLE     9
#define TILE_GRASS_DENSE    10
#define TILE_BEAM           11
#define TILE_LETTER_BASE    16   /* 'A' -> 16, 'Z' -> 41 */
#define TILE_COLON          42
#define TILE_SPACE          43
#define TILE_DIGIT_BASE     48   /* '0' -> 48, '9' -> 57 */

#define SPRITE_ID_PLAYER         1
#define SPRITE_ID_ENEMY_ARMED    2
#define SPRITE_ID_BULLET         3
#define SPRITE_ID_ENEMY_UNARMED  4
#define SPRITE_ID_MORTAR         5
/* Sprite ID 6 (barbed wire) retired -- AA_WIRE removed in Batch A. */
#define SPRITE_ID_GAS            7
#define SPRITE_ID_ARTILLERY      8
#define SPRITE_ID_AMMO           9   /* green ammo crate (Batch B sprite art) */
#define SPRITE_ID_ENEMY_BULLET   10  /* red dot for incoming enemy fire       */

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
        case ENT_ENEMY_BULLET:  return SPRITE_ID_ENEMY_BULLET;
        case ENT_AMMO_DROP:     return SPRITE_ID_AMMO;
        case ENT_AUTO_PROJ:
        case ENT_HAZARD:        return autoatk_sprite_id(e->payload);
        default:                return 0;
    }
}

static uint8_t entity_palette_off(const entity_t *e) {
    (void)e;
    /* All entities now have dedicated sprite art with native palette colors;
       palette-offset shifting from Batch A is retired. */
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

/* -----------------------------------------------------------------------
 * Tile-map helpers: ground init, artillery beam, on-screen text.
 * Owned by render.c so the FPGA-only nml_write_tile() / nml_gpu.h surface
 * stays out of game.c and main.c.
 * --------------------------------------------------------------------- */

/* Same coarse-noise function used to populate the battlefield at startup.
   Exposed via tile_for(row, col) so the beam save/restore and game-over
   restore can recompute any cell without keeping a shadow copy. */
static uint8_t tile_for(int row, int col) {
    unsigned zone = ((unsigned)(row / 8) * 7u +
                     (unsigned)(col / 10) * 11u) % 5u;
    int favor_grass = (zone == 1 || zone == 3);

    unsigned r = ((unsigned)(row * 17 + col * 31) >> 1) & 31u;

    if (favor_grass) {
        if      (r < 16) return TILE_GRASS_A;
        else if (r < 22) return TILE_GRASS_B;
        else if (r < 26) return TILE_GRASS_DENSE;
        else if (r < 28) return TILE_DIRT_A;
        else if (r < 30) return TILE_TRANSITION;
        else             return TILE_MUD_PUDDLE;
    } else {
        if      (r < 16) return TILE_DIRT_A;
        else if (r < 22) return TILE_DIRT_B;
        else if (r < 26) return TILE_TRANSITION;
        else if (r < 28) return TILE_GRASS_A;
        else if (r < 30) return TILE_MUD_PUDDLE;
        else             return TILE_GRASS_DENSE;
    }
}

void render_init_tilemap(void) {
    for (int row = 0; row < NML_TILEMAP_ROWS; ++row) {
        for (int col = 0; col < NML_TILEMAP_COLS; ++col) {
            nml_write_tile(col, row, tile_for(row, col));
        }
    }
}

/* Artillery-beam tile-map state. The visual is a vertical column of
   TILE_BEAM glyphs written into the tile map for ~6 frames; the underlying
   ground tiles are recomputed (not stored) when the beam expires, so no
   per-cell shadow buffer is needed. */
static int s_beam_drawn      = 0;   /* 1 = beam column currently overwritten */
static int s_beam_drawn_col  = -1;  /* the column we overwrote               */

static void beam_paint_column(int col) {
    if (col < 0 || col >= NML_TILEMAP_COLS) return;
    /* Paint from just below the HUD strip (row 2 = y=16+) down to the row
       above the player's current tile. Painting all the way to the bottom
       reads better -- gives the player visual confirmation the beam reached
       deep into the field. */
    for (int row = 2; row < NML_TILEMAP_ROWS; ++row) {
        nml_write_tile(col, row, TILE_BEAM);
    }
}

static void beam_restore_column(int col) {
    if (col < 0 || col >= NML_TILEMAP_COLS) return;
    for (int row = 2; row < NML_TILEMAP_ROWS; ++row) {
        nml_write_tile(col, row, tile_for(row, col));
    }
}

/* Maintain beam_drawn state machine from the game's beam_ttl signal. */
static void beam_update(const game_t *g) {
    if (g->beam_ttl > 0 && !s_beam_drawn) {
        s_beam_drawn_col = g->beam_col;
        beam_paint_column(s_beam_drawn_col);
        s_beam_drawn = 1;
    } else if (g->beam_ttl == 0 && s_beam_drawn) {
        beam_restore_column(s_beam_drawn_col);
        s_beam_drawn = 0;
        s_beam_drawn_col = -1;
    }
}

/* Map an ASCII char to a tile slot. Unknown chars fall through to blank bg
   (tile 0) so the caller can include punctuation/whitespace freely. */
static uint8_t ascii_to_tile(char c) {
    if (c >= 'A' && c <= 'Z') return (uint8_t)(TILE_LETTER_BASE + (c - 'A'));
    if (c >= 'a' && c <= 'z') return (uint8_t)(TILE_LETTER_BASE + (c - 'a'));
    if (c >= '0' && c <= '9') return (uint8_t)(TILE_DIGIT_BASE  + (c - '0'));
    if (c == ':')             return TILE_COLON;
    if (c == ' ')             return TILE_SPACE;
    return TILE_BLANK_BG;
}

/* Write a string into the tile map, left-aligned at (row, col). Out-of-range
   cells are silently skipped. */
static void draw_text_at(int row, int col, const char *s) {
    int len = (int)strlen(s);
    for (int i = 0; i < len; ++i) {
        int c = col + i;
        if (c < 0) continue;
        if (c >= NML_TILEMAP_COLS) break;
        nml_write_tile(c, row, ascii_to_tile(s[i]));
    }
}

/* Center the string on `row` so it sits at the horizontal midpoint of the
   80-column tile map. */
static void draw_text_centered(int row, const char *s) {
    int len = (int)strlen(s);
    int col = (NML_TILEMAP_COLS - len) / 2;
    draw_text_at(row, col, s);
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
    nml_set_hud_aux((uint8_t)(g->ammo > 99 ? 99 : g->ammo),
                    (uint8_t)g->artillery_charges,
                    (uint8_t)g->gas_charges);
}

/*
 * Game-over screen: draw a centered block of text directly into the tile
 * map. Letter glyphs live at tile slots 16..41 (A..Z), digits at 48..57,
 * colon at 42, blank at 43. The block lays out as:
 *
 *     GAME OVER
 *
 *     SCORE: NNNNNN
 *     WAVE:  NN
 *     KILLS A: NN
 *     KILLS U: NN
 *
 *     PRESS START
 *
 * Vertical center of the playfield is row ~30. We anchor the block around
 * row 24 so it sits comfortably above center.
 */
static void render_game_over(const game_t *g) {
    /* Cap displayable score at 999999 to fit the 6-digit slot consistently
       with the HUD score readout. */
    int score = g->score;
    if (score < 0)       score = 0;
    if (score > 999999)  score = 999999;
    int wave_n  = g->wave_index + 1;
    if (wave_n  > 99) wave_n = 99;
    int ka      = g->kills_armed;   if (ka > 99) ka = 99;
    int ku      = g->kills_unarmed; if (ku > 99) ku = 99;

    char buf[40];

    draw_text_centered(22, "GAME OVER");

    snprintf(buf, sizeof(buf), "SCORE: %06d", score);
    draw_text_centered(24, buf);

    snprintf(buf, sizeof(buf), "WAVE:  %02d", wave_n);
    draw_text_centered(25, buf);

    snprintf(buf, sizeof(buf), "KILLS A: %02d", ka);
    draw_text_centered(26, buf);

    snprintf(buf, sizeof(buf), "KILLS U: %02d", ku);
    draw_text_centered(27, buf);

    draw_text_centered(29, "PRESS START");

    /* Hide every sprite slot so nothing draws over the text. */
    for (int slot = 0; slot < NML_MAX_SPRITES; ++slot) {
        nml_hide_sprite(slot);
    }

    /* HUD still shows the final values (HP=0, ammo=0, etc.). */
    nml_set_player_state(0, 0, 0, /*wave=*/0, /*level=*/0);
    nml_set_score((uint32_t)g->score, /*kills=*/0u);
    nml_set_hud_aux(0, 0, 0);
}

void render_frame(const game_t *g) {
    /* Track state to drive one-shot tile-map restores. The game-over screen
       overwrites the battlefield with stat text; on the GAMEOVER -> PLAYING
       restart we re-init the whole tile map. The artillery beam follows the
       same save/restore pattern but is column-scoped. */
    static game_state_t s_prev_state = STATE_PLAYING;
    if (s_prev_state == STATE_GAMEOVER && g->state == STATE_PLAYING) {
        /* If the beam was mid-flash when the game ended, drop our shadow
           state so the next fire doesn't double-restore. */
        s_beam_drawn     = 0;
        s_beam_drawn_col = -1;
        render_init_tilemap();
    }
    s_prev_state = g->state;

    if (g->state == STATE_GAMEOVER) {
        render_game_over(g);
        return;
    }
    if (g->state == STATE_LEVELUP) {
        /* Pause the beam visual during LEVELUP so it doesn't ghost across
           the menu. STATE_LEVELUP can only fire after a wave clears, and
           beam_ttl is short enough that it usually expires before then. */
        beam_update(g);
        render_levelup(g);
        return;
    }

    /* Update the artillery-beam tile column. Must run BEFORE we draw sprites
       so the new tile state takes effect on the same frame. */
    beam_update(g);

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
    nml_set_hud_aux((uint8_t)(g->ammo > 99 ? 99 : g->ammo),
                    (uint8_t)g->artillery_charges,
                    (uint8_t)g->gas_charges);
}

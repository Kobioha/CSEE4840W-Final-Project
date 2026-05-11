#include "render.h"
#include "autoatk.h"
#include <stdio.h>

/* Subdivides the game sprites into 32 slots in order to accommodate the FPGA.
   This mirrors the production render.c so behavior is identical between
   builds; the only difference is that this file prints sprite-table writes
   to stdout instead of memory-mapping them. */

#define MAX_SPRITES 32

#define SPRITE_PLAYER         1
#define SPRITE_ENEMY_ARMED    2
#define SPRITE_BULLET         3
#define SPRITE_ENEMY_UNARMED  4
#define SPRITE_MORTAR         5
/* Sprite 6 (wire) retired this batch. */
#define SPRITE_GAS            7
#define SPRITE_ARTILLERY      8

typedef struct {
    int active;
    int x, y;
    int sprite_id;
    char glyph;    /* ASCII representation, for the terminal-build only */
} mock_sprite_t;

static int autoatk_sprite_id(int payload) {
    switch (payload) {
        case AA_MORTAR:    return SPRITE_MORTAR;
        case AA_GAS:       return SPRITE_GAS;
        case AA_ARTILLERY: return SPRITE_ARTILLERY;
        default:           return SPRITE_BULLET;
    }
}

static int entity_to_sprite_id(const entity_t *e) {
    switch (e->kind) {
        case ENT_PLAYER:        return SPRITE_PLAYER;
        case ENT_ENEMY_ARMED:   return SPRITE_ENEMY_ARMED;
        case ENT_ENEMY_UNARMED: return SPRITE_ENEMY_UNARMED;
        case ENT_BULLET:        return SPRITE_BULLET;
        case ENT_ENEMY_BULLET:  return SPRITE_BULLET;
        case ENT_AMMO_DROP:     return SPRITE_BULLET;
        case ENT_AUTO_PROJ:
        case ENT_HAZARD:        return autoatk_sprite_id(e->payload);
        default:                return 0;
    }
}

static char entity_glyph(const entity_t *e) {
    switch (e->kind) {
        case ENT_PLAYER:        return 'P';
        case ENT_ENEMY_ARMED:   return 'A';
        case ENT_ENEMY_UNARMED: return 'u';
        case ENT_BULLET:        return '*';
        case ENT_ENEMY_BULLET:  return '^';
        case ENT_AMMO_DROP:     return '+';
        case ENT_AUTO_PROJ: {
            switch (e->payload) {
                case AA_MORTAR:    return 'M';
                case AA_ARTILLERY: return '|';
                default:           return '?';
            }
        }
        case ENT_HAZARD: {
            if (e->payload == (int)AA_GAS) return '~';
            return '#';
        }
        default: return '.';
    }
}

static int alloc_slot(mock_sprite_t *sprites, int *slot_inout,
                      const entity_t *e) {
    if (*slot_inout >= MAX_SPRITES) return 0;
    int s = *slot_inout;
    sprites[s].active    = 1;
    sprites[s].x         = e->x;
    sprites[s].y         = e->y;
    sprites[s].sprite_id = entity_to_sprite_id(e);
    sprites[s].glyph     = entity_glyph(e);
    *slot_inout = s + 1;
    return 1;
}

static void emit_gas_cluster(mock_sprite_t *sprites, int *slot_inout,
                             const entity_t *e) {
    int w, h;
    autoatk_gas_size(e, &w, &h);
    /* Up to 4 sprite slots for the cluster -- center always; +sides when
       width crosses 24, +top when height crosses 24. */
    int cx = e->x, cy = e->y;
    entity_t synth = *e;

    synth.x = cx; synth.y = cy;
    alloc_slot(sprites, slot_inout, &synth);

    if (w >= 24) {
        synth.x = cx - 12; synth.y = cy;
        alloc_slot(sprites, slot_inout, &synth);
        synth.x = cx + 12; synth.y = cy;
        alloc_slot(sprites, slot_inout, &synth);
    }
    if (h >= 24) {
        synth.x = cx; synth.y = cy - 10;
        alloc_slot(sprites, slot_inout, &synth);
    }
}

void render_frame(const game_t *g) {
    mock_sprite_t sprites[MAX_SPRITES];

    for (int i = 0; i < MAX_SPRITES; i++) {
        sprites[i].active = 0;
        sprites[i].x = 0;
        sprites[i].y = 0;
        sprites[i].sprite_id = 0;
        sprites[i].glyph = ' ';
    }

    int slot = 0;

    /* Slot 0 is always the player. */
    const entity_t *p = &g->ents[g->player_i];
    sprites[slot].active = 1;
    sprites[slot].x = p->x;
    sprites[slot].y = p->y;
    sprites[slot].sprite_id = SPRITE_PLAYER;
    sprites[slot].glyph = 'P';
    slot++;

    /* Slots 1..31: active non-player entities. Gas hazards expand into up
       to 4 slots. */
    for (int i = 0; i < MAX_ENTITIES && slot < MAX_SPRITES; i++) {
        const entity_t *e = &g->ents[i];
        if (!e->active) continue;
        if (e->kind == ENT_PLAYER) continue;
        if (e->kind == ENT_HAZARD && e->payload == (int)AA_GAS) {
            emit_gas_cluster(sprites, &slot, e);
            continue;
        }
        sprites[slot].active = 1;
        sprites[slot].x = e->x;
        sprites[slot].y = e->y;
        sprites[slot].sprite_id = entity_to_sprite_id(e);
        sprites[slot].glyph = entity_glyph(e);
        slot++;
    }

    printf("frame=%d hp=%d ammo=%d art=%d gas=%d score=%d wave=%d "
           "kills(A/U)=%d/%d drops=%d sprite_count=%d state=%d\n",
           g->frame, g->player_hp, g->ammo,
           g->artillery_charges, g->gas_charges, g->score,
           g->wave_index + 1, g->kills_armed, g->kills_unarmed,
           g->drops_collected, slot, (int)g->state);

    for (int i = 0; i < MAX_SPRITES; i++) {
        if (!sprites[i].active) {
            printf("  slot %02d: inactive\n", i);
        } else {
            printf("  slot %02d: sprite_id=%d glyph=%c x=%d y=%d\n",
                   i,
                   sprites[i].sprite_id,
                   sprites[i].glyph,
                   sprites[i].x,
                   sprites[i].y);
        }
    }

    printf("\n");
}

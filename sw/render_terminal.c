#include "render.h"
#include <stdio.h>

/*Subdivides the game sprites into 32 slots in order to accomodate the FPGA.
 This follows hardware logic*/

#define MAX_SPRITES 32

#define SPRITE_PLAYER 1
#define SPRITE_ENEMY  2
#define SPRITE_BULLET 3

typedef struct {
    int active;
    int x, y;
    int sprite_id;
} mock_sprite_t;

static int entity_to_sprite_id(ent_kind_t kind) {
    switch (kind) {
        case ENT_PLAYER:        return SPRITE_PLAYER;
        case ENT_ENEMY_ARMED:
        case ENT_ENEMY_UNARMED: return SPRITE_ENEMY;
        case ENT_BULLET:
        case ENT_AUTO_PROJ:
        case ENT_HAZARD:        return SPRITE_BULLET;
        default:                return 0;
    }
}

void render_frame(const game_t *g) {
    mock_sprite_t sprites[MAX_SPRITES];

    for (int i = 0; i < MAX_SPRITES; i++) {
        sprites[i].active = 0;
        sprites[i].x = 0;
        sprites[i].y = 0;
        sprites[i].sprite_id = 0;
    }

    int slot = 0;

    /*
      Slot 0 is always the player.
      This matches how you will probably think about FPGA sprite slots later.
    */
    const entity_t *p = &g->ents[g->player_i];

    sprites[slot].active = 1;
    sprites[slot].x = p->x;
    sprites[slot].y = p->y;
    sprites[slot].sprite_id = SPRITE_PLAYER;
    slot++;

    /*
      Remaining slots are enemies and bullets.
      Hardware supports 32 sprites, so this mock also caps at 32.
    */
    for (int i = 0; i < MAX_ENTITIES && slot < MAX_SPRITES; i++) {
        const entity_t *e = &g->ents[i];

        if (!e->active) continue;
        if (e->kind == ENT_PLAYER) continue;

        sprites[slot].active = 1;
        sprites[slot].x = e->x;
        sprites[slot].y = e->y;
        sprites[slot].sprite_id = entity_to_sprite_id(e->kind);
        slot++;
    }

    printf("frame=%d hp=%d score=%d wave=%d kills(A/U)=%d/%d sprite_count=%d\n",
           g->frame, g->player_hp, g->score,
           g->wave_index + 1, g->kills_armed, g->kills_unarmed, slot);

    for (int i = 0; i < MAX_SPRITES; i++) {
        if (!sprites[i].active) {
            printf("  slot %02d: inactive\n", i);
        } else {
            printf("  slot %02d: sprite_id=%d x=%d y=%d\n",
                   i,
                   sprites[i].sprite_id,
                   sprites[i].x,
                   sprites[i].y);
        }
    }

    printf("\n");
}

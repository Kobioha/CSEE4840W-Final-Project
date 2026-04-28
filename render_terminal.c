#include "render.h"
#include <stdio.h>

void render_frame(const game_t *g) {
    const entity_t *p = &g->ents[g->player_i];

    printf("frame=%d player=(%d,%d) hp=%d score=%d\n",
           g->frame, p->x, p->y, g->player_hp, g->score);

    for (int i = 0; i < MAX_ENTITIES; i++) {
        const entity_t *e = &g->ents[i];
        if (!e->active) continue;

        if (e->kind == ENT_ENEMY) {
            printf("  enemy  %02d at (%d,%d)\n", i, e->x, e->y);
        }

        if (e->kind == ENT_BULLET) {
            printf("  bullet %02d at (%d,%d)\n", i, e->x, e->y);
        }
    }

    printf("\n");
}

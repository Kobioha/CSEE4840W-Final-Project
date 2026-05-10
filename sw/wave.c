#include "wave.h"
#include <stdlib.h>

/*
 * Hardcoded wave table. To rebalance, edit this array and rebuild -- no
 * runtime file I/O. The narrative-arc shift toward more enemies and a higher
 * armed-fraction over time lives entirely in this table.
 */
const wave_def_t WAVES[] = {
    /* spawn_period, total, armed, armed_hp, enemy_speed */
    {  60,  6, 0, 1, 1 },   /* wave 1 -- warmup, all unarmed, slow */
    {  50,  8, 1, 2, 1 },   /* wave 2 -- first armed appears */
    {  45, 10, 2, 2, 2 },   /* wave 3 -- enemies move 2x faster */
    {  35, 12, 4, 2, 2 },   /* wave 4 */
    {  25, 16, 6, 3, 3 },   /* wave 5 -- loops; fastest + tankiest */
};
const int WAVE_COUNT = (int)(sizeof(WAVES) / sizeof(WAVES[0]));

/* spawn_entity is static inside game.c; re-implement the bits we need here.
   game.c retains ownership of the entity pool layout. */
static int alloc_entity(game_t *g) {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        if (!g->ents[i].active) return i;
    }
    return -1;
}

static int active_enemy_count(const game_t *g) {
    int n = 0;
    for (int i = 0; i < MAX_ENTITIES; i++) {
        const entity_t *e = &g->ents[i];
        if (!e->active) continue;
        if (e->kind == ENT_ENEMY_ARMED || e->kind == ENT_ENEMY_UNARMED) n++;
    }
    return n;
}

const wave_def_t *wave_current(const game_t *g) {
    int idx = g->wave_index;
    if (idx < 0)             idx = 0;
    if (idx >= WAVE_COUNT)   idx = WAVE_COUNT - 1;
    return &WAVES[idx];
}

void wave_system_init(game_t *g) {
    g->wave_index           = 0;
    g->wave_enemies_spawned = 0;
    g->wave_armed_remaining = WAVES[0].armed_count;
    g->wave_spawn_cooldown  = 0;   /* first enemy spawns immediately */
    g->kills_armed          = 0;
    g->kills_unarmed        = 0;
}

void wave_tick(game_t *g) {
    const wave_def_t *w = wave_current(g);

    /* Already finished spawning this wave? wave_complete() will trip once
       the on-screen enemies are cleared and game_tick will advance us. */
    if (g->wave_enemies_spawned >= w->total_enemies) return;

    if (g->wave_spawn_cooldown > 0) {
        g->wave_spawn_cooldown--;
        return;
    }

    int slot = alloc_entity(g);
    if (slot < 0) {
        /* Pool full; try again next frame. Don't burn the cooldown. */
        return;
    }

    int spawn_armed = (g->wave_armed_remaining > 0);

    entity_t *e = &g->ents[slot];
    e->active = 1;
    e->kind   = spawn_armed ? ENT_ENEMY_ARMED : ENT_ENEMY_UNARMED;
    e->x      = rand() % (SCREEN_W - 16);
    e->y      = HUD_H;
    e->vx     = 0;
    e->vy     = 0;
    e->hp     = spawn_armed ? w->armed_hp : 1;
    e->phase  = rand() & 0xff;       /* desync horizontal random-walk across enemies */

    g->wave_enemies_spawned++;
    if (spawn_armed) g->wave_armed_remaining--;
    g->wave_spawn_cooldown = w->spawn_period_frames;
}

int wave_complete(const game_t *g) {
    const wave_def_t *w = wave_current(g);
    return (g->wave_enemies_spawned >= w->total_enemies) &&
           (active_enemy_count(g) == 0);
}

void wave_advance(game_t *g) {
    if (g->wave_index < WAVE_COUNT - 1) {
        g->wave_index++;
    }
    const wave_def_t *w = wave_current(g);
    g->wave_enemies_spawned = 0;
    g->wave_armed_remaining = w->armed_count;
    g->wave_spawn_cooldown  = w->spawn_period_frames;  /* brief pause between waves */
}

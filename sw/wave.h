#ifndef WAVE_H
#define WAVE_H

#include "game.h"

typedef struct {
    int spawn_period_frames;   /* frames between consecutive spawns */
    int total_enemies;         /* enemies emitted this wave */
    int armed_count;           /* of total, how many are ENT_ENEMY_ARMED */
    int armed_hp;              /* hp for armed enemies in this wave */
    int enemy_speed;           /* pixels/frame both axes; >=1 */
} wave_def_t;

extern const int        WAVE_COUNT;
extern const wave_def_t WAVES[];

/* Sets wave_index = 0 and seeds the per-wave counters. Call from game_init(). */
void                wave_system_init(game_t *g);

/* Returns the active wave def. After the final wave it clamps to the last
   entry rather than returning NULL, so the game can keep running. */
const wave_def_t *  wave_current(const game_t *g);

/* Runs once per game_tick during STATE_PLAYING. Spawns enemies according
   to the active wave's schedule. Armed enemies emit first (deterministic). */
void                wave_tick(game_t *g);

/* True when every enemy this wave was going to spawn has spawned, and no
   enemy entity (armed or unarmed) is still active. */
int                 wave_complete(const game_t *g);

/* Bumps wave_index (clamps at last wave) and resets per-wave counters. */
void                wave_advance(game_t *g);

#endif

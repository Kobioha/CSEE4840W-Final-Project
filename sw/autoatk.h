#ifndef AUTOATK_H
#define AUTOATK_H

#include "game.h"

/* Resets all four weapons to level 0 / cooldown 0. Call from game_init. */
void          autoatk_init(game_t *g);

/* Decrements cooldowns and fires any weapon whose cooldown reaches 0.
   Call once per PLAYING tick. */
void          autoatk_tick(game_t *g);

/* Moves auto-projectiles, ages hazards (decrements ttl, deactivates at 0).
   Call once per PLAYING tick after autoatk_tick. */
void          autoatk_update_proj(game_t *g);

/* Bumps the given weapon's level (capped at AA_LEVEL_CAP) and refreshes its
   period. Picking a maxed weapon is a no-op. */
void          autoatk_upgrade(game_t *g, autoatk_kind_t k);

/* Damage a single hit from this weapon does. Used by handle_collisions. */
int           autoatk_damage(autoatk_kind_t k);

/* True iff the auto-projectile/hazard should despawn the moment it lands a hit.
   Mortar shells despawn; persistent hazards (wire/gas) do not; artillery rides
   its TTL flash. */
int           autoatk_despawn_on_hit(autoatk_kind_t k);

/* Hitbox-padding helper. Gas has a wider hitbox than the standard 16x16. */
int           autoatk_hitbox_pad(autoatk_kind_t k);

/* Fills g->levelup_options[] with LEVELUP_OPTIONS distinct random weapon
   kinds. Resets g->levelup_cursor to 0. */
void          autoatk_pick_levelup_options(game_t *g);

const char *  autoatk_name(autoatk_kind_t k);

#endif

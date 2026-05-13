#ifndef AUTOATK_H
#define AUTOATK_H

#include "game.h"

/* Resets all weapons to level 0 / cooldown 0. Call from game_init. */
void          autoatk_init(game_t *g);

/* Decrements the mortar cooldown and fires it when ready. Gas and Artillery
   are now player-triggered (see autoatk_try_fire_gas / artillery below) so
   they are NOT serviced here. Call once per PLAYING tick. */
void          autoatk_tick(game_t *g);

/* Moves auto-projectiles, ages hazards (decrements ttl, deactivates at 0).
   Call once per PLAYING tick after autoatk_tick. */
void          autoatk_update_proj(game_t *g);

/* Bumps the given weapon's level (capped at AA_LEVEL_CAP) and refreshes its
   period (mortar only; Gas/Artillery upgrades currently affect nothing
   game-side besides scoring "owned" semantics, but we still track levels so
   the level-up menu can show them). Picking a maxed weapon is a no-op. */
void          autoatk_upgrade(game_t *g, autoatk_kind_t k);

/* Damage a single hit from this weapon does. Used by handle_collisions. */
int           autoatk_damage(autoatk_kind_t k);

/* True iff the auto-projectile/hazard should despawn the moment it lands a hit.
   Mortar shells despawn; persistent hazards (gas) do not. */
int           autoatk_despawn_on_hit(autoatk_kind_t k);

/* Padded AABB hitbox helper for hazards. For AA_GAS the live hitbox grows
   with the cloud, so callers should pass the entity to get the right size. */
int           autoatk_hitbox_pad(autoatk_kind_t k);

/* Returns 1 if `e` is a gas cloud entity (ENT_HAZARD with payload=AA_GAS)
   and the given target rect overlaps the gas cloud's current (size-aware)
   hitbox. (target rect is the standard 16x16 sprite at tx,ty.) */
int           autoatk_gas_overlaps(const entity_t *e, int tx, int ty);

/* Returns the current size of a gas cloud entity, expanding from a small
   puff (16x16) to full size (48x32) over the first ~30 frames of life,
   then holding, then shrinking. (out_w, out_h are full size in pixels.) */
void          autoatk_gas_size(const entity_t *e, int *out_w, int *out_h);

/* Player-triggered abilities. Each returns 1 if it actually fired (caller
   should then decrement the corresponding charge counter & set the input
   cooldown), 0 if blocked (no enemies in target column, slot pool full, etc.). */
int           autoatk_fire_artillery(game_t *g);
int           autoatk_fire_gas(game_t *g);

/* Fills g->levelup_options[] with up to LEVELUP_OPTIONS distinct random
   weapon kinds. Resets g->levelup_cursor to 0. */
void          autoatk_pick_levelup_options(game_t *g);

const char *  autoatk_name(autoatk_kind_t k);

#endif

#include "game.h"
#include "wave.h"
#include "autoatk.h"
#include <stdlib.h>

/* Score weights -- tune here. */
#define SCORE_KILL_ARMED    200
#define SCORE_KILL_UNARMED  50

/* Player fire cooldown in frames. 60 fps -> 5 frames == 12 shots/sec. Shared
   across all 4 face buttons so the four-direction setup can't fire 4 bullets
   on the same tick (first-direction-found-in-the-priority-order wins). */
#define FIRE_COOLDOWN_FRAMES 5

/* Player bullet speed (always axis-aligned). */
#define BULLET_SPEED 8

/* Armed-enemy fire cadence + bullet speed. Generous gap so the player has
   time to dodge; cadence is randomized at spawn so the cohort doesn't
   coordinate. */
#define ENEMY_FIRE_BASE_COOLDOWN  120
#define ENEMY_FIRE_JITTER         60
#define ENEMY_BULLET_SPEED        3

/* Ammo drop entity lifetime in frames. */
#define AMMO_DROP_TTL  600   /* 10 seconds */

/*Function that spawns the entities on the map at coordinates x and y according to their kind*/

static int spawn_entity(game_t *g, ent_kind_t kind, int x, int y){
  for (int i = 0; i < MAX_ENTITIES; i++){
    if(!g->ents[i].active){
      g->ents[i].active = 1;
      g->ents[i].kind = kind;
      g->ents[i].x = x;
      g->ents[i].y = y;
      g->ents[i].vx = 0;
      g->ents[i].vy = 0;
      g->ents[i].hp = 1;
      g->ents[i].phase = 0;     /* player/bullet don't use phase; only enemies do */
      g->ents[i].ttl = 0;
      g->ents[i].ttl_max = 0;
      g->ents[i].payload = 0;
      g->ents[i].fire_cd = 0;
      return i;
    }
  }
  return -1;
}

static int is_enemy(const entity_t *e) {
    return e->kind == ENT_ENEMY_ARMED || e->kind == ENT_ENEMY_UNARMED;
}

static void maybe_spawn_ammo_drop(game_t *g, int x, int y) {
    if ((rand() % 100) >= AMMO_DROP_PERCENT) return;
    int slot = spawn_entity(g, ENT_AMMO_DROP, x, y);
    if (slot < 0) return;
    g->ents[slot].ttl     = AMMO_DROP_TTL;
    g->ents[slot].ttl_max = AMMO_DROP_TTL;
}

/*Initializes the game at frame 0 and with the player having 100 hp*/
void game_init(game_t *g) {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        g->ents[i].active = 0;
    }

    g->frame = 0;
    g->score = 0;
    g->player_hp = 100;
    g->state = STATE_PLAYING;
    g->prev_state = STATE_PLAYING;
    g->prev_input = 0;
    g->fire_cooldown = 0;

    g->ammo              = AMMO_INITIAL;
    g->artillery_charges = ART_CHARGES_INITIAL;
    g->gas_charges       = GAS_CHARGES_INITIAL;
    g->art_input_cd      = 0;
    g->gas_input_cd      = 0;
    g->drops_collected   = 0;

    g->player_i = spawn_entity(g, ENT_PLAYER, SCREEN_W / 2, SCREEN_H - 60);

    wave_system_init(g);
    autoatk_init(g);
}

/* Edge-detect: was the bit unset last tick and set this tick? Static here
   because both update_player and tick_levelup want it; tick_levelup needs
   access via a local copy below. */
static int input_pressed(const game_t *g, uint16_t input, uint16_t bit) {
    return (input & bit) && !(g->prev_input & bit);
}

/* Try to fire one bullet in the requested direction. The cooldown is shared
   across all four directions: if any face button is held the player gets
   12 shots/sec, and if multiple are pressed the priority order (DOWN, LEFT,
   UP, RIGHT) decides which direction wins. Returns 1 if a bullet was fired. */
static int try_fire_bullet(game_t *g, int vx, int vy, int origin_dx, int origin_dy) {
    const entity_t *p = &g->ents[g->player_i];
    if (g->fire_cooldown != 0) return 0;
    if (g->ammo <= 0) return 0;
    int b = spawn_entity(g, ENT_BULLET, p->x + origin_dx, p->y + origin_dy);
    if (b < 0) return 0;
    g->ents[b].vx = vx;
    g->ents[b].vy = vy;
    g->fire_cooldown = FIRE_COOLDOWN_FRAMES;
    g->ammo--;
    return 1;
}

/* L-button: artillery beam. R-button: gas cloud. Both use edge-detect plus
   a 20-frame post-fire lockout to defend against bouncy SNES buttons. */
static void try_fire_abilities(game_t *g, uint16_t input) {
    if (g->art_input_cd == 0 &&
        input_pressed(g, input, INPUT_ABIL_ART) &&
        g->artillery_charges > 0) {
        if (autoatk_fire_artillery(g)) {
            g->artillery_charges--;
            g->art_input_cd = ABILITY_INPUT_COOLDOWN;
        }
    }
    if (g->gas_input_cd == 0 &&
        input_pressed(g, input, INPUT_ABIL_GAS) &&
        g->gas_charges > 0) {
        if (autoatk_fire_gas(g)) {
            g->gas_charges--;
            g->gas_input_cd = ABILITY_INPUT_COOLDOWN;
        }
    }
}

/*Updates the player's position and actions according to the received input*/
static void update_player(game_t *g, uint16_t input) {
    entity_t *p = &g->ents[g->player_i];
    int speed = 4;

    if (input & INPUT_LEFT)  p->x -= speed;
    if (input & INPUT_RIGHT) p->x += speed;
    if (input & INPUT_UP)    p->y -= speed;
    if (input & INPUT_DOWN)  p->y += speed;

    if (p->x < 0) p->x = 0;
    if (p->x > SCREEN_W - 16) p->x = SCREEN_W - 16;
    if (p->y < HUD_H) p->y = HUD_H;
    if (p->y > SCREEN_H - 16) p->y = SCREEN_H - 16;

    if (g->fire_cooldown > 0) g->fire_cooldown--;
    if (g->art_input_cd  > 0) g->art_input_cd--;
    if (g->gas_input_cd  > 0) g->gas_input_cd--;

    /* Four-direction face-button fire. Bullets spawn just outside the player
       sprite on the firing side so they don't immediately self-collide. */
    if (input & INPUT_FIRE_DOWN)  try_fire_bullet(g,  0,  BULLET_SPEED,  8,  16);
    else if (input & INPUT_FIRE_LEFT)  try_fire_bullet(g, -BULLET_SPEED, 0, -8,   8);
    else if (input & INPUT_FIRE_UP)    try_fire_bullet(g,  0, -BULLET_SPEED, 8, -8);
    else if (input & INPUT_FIRE_RIGHT) try_fire_bullet(g,  BULLET_SPEED, 0, 16,   8);

    try_fire_abilities(g, input);
}

/*Step a coord toward a target by at most `speed` pixels, clamping the final
  step so we don't overshoot and oscillate.*/
static int step_toward(int from, int to, int speed) {
    int delta = to - from;
    if (delta >  speed) return from + speed;
    if (delta < -speed) return from - speed;
    return to;
}

/*Updates enemy motion. Vertical: enemies always close on the player's row at
  the wave's speed, so they reliably threaten the trench. Horizontal: a
  per-enemy random walk that re-decides direction every ENEMY_DIR_FRAMES.
  Phase desync prevents the whole cohort from synchronizing into a column,
  so the player's bullet stream no longer mops them up trivially.

  Decision distribution per re-roll (256 buckets):
    [   0,  64) -> strafe left   (25%)
    [  64, 128) -> strafe right  (25%)
    [ 128, 256) -> chase player  (50%) -- keeps a weak pull so enemies still
                                          gravitate toward the player overall.

  Armed enemies also fire bullets at the player on a per-enemy cooldown
  staggered at spawn. Bullet velocity locks on at fire time (no homing).*/
#define ENEMY_DIR_FRAMES 12u

static unsigned enemy_rng(int frame, int phase) {
    unsigned window = (unsigned)(frame + phase) / ENEMY_DIR_FRAMES;
    return (window * 1103515245u + (unsigned)phase * 12345u) >> 24;
}

/* Spawn an enemy bullet from `e` aimed at the player's current position.
   Manhattan-norm velocity so the bullet stays on a clean integer trajectory
   without floats. The bullet doesn't track the player after spawn. */
static void enemy_fire_bullet(game_t *g, const entity_t *e) {
    const entity_t *p = &g->ents[g->player_i];
    int dx = (p->x + 8) - (e->x + 8);
    int dy = (p->y + 8) - (e->y + 8);
    int absdx = dx < 0 ? -dx : dx;
    int absdy = dy < 0 ? -dy : dy;
    int norm = absdx + absdy;
    if (norm == 0) norm = 1;
    int vx = (dx * ENEMY_BULLET_SPEED) / norm;
    int vy = (dy * ENEMY_BULLET_SPEED) / norm;
    /* Avoid zero-velocity bullets if the player is exactly on top of the
       enemy: bias downward so it at least leaves the muzzle. */
    if (vx == 0 && vy == 0) vy = ENEMY_BULLET_SPEED;
    int b = spawn_entity(g, ENT_ENEMY_BULLET, e->x + 4, e->y + 4);
    if (b < 0) return;
    g->ents[b].vx = vx;
    g->ents[b].vy = vy;
}

static void update_enemies(game_t *g) {
    entity_t *p = &g->ents[g->player_i];
    int speed = wave_current(g)->enemy_speed;
    if (speed < 1) speed = 1;

    for (int i = 0; i < MAX_ENTITIES; i++) {
        entity_t *e = &g->ents[i];
        if (!e->active || !is_enemy(e)) continue;

        e->y = step_toward(e->y, p->y, speed);

        unsigned r = enemy_rng(g->frame, e->phase);
        if      (r <  64) e->x -= speed;
        else if (r < 128) e->x += speed;
        else              e->x = step_toward(e->x, p->x, speed);

        if (e->x < 0)              e->x = 0;
        if (e->x > SCREEN_W - 16)  e->x = SCREEN_W - 16;

        /* Armed enemies shoot back. */
        if (e->kind == ENT_ENEMY_ARMED) {
            if (e->fire_cd > 0) {
                e->fire_cd--;
            } else {
                enemy_fire_bullet(g, e);
                e->fire_cd = ENEMY_FIRE_BASE_COOLDOWN + (rand() % ENEMY_FIRE_JITTER);
            }
        }

	/*Makes the player lose hp if an enemy steps over the trench!*/
	if(e->y >= SCREEN_H - 20){
	  e->active = 0;
	  g->player_hp -= 10;
	}
    }
}

/*Updates player bullet position; bullets despawn when they leave the screen.*/
static void update_bullets(game_t *g) {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entity_t *e = &g->ents[i];
        if (!e->active || e->kind != ENT_BULLET) continue;

        e->x += e->vx;
        e->y += e->vy;

        if (e->y < 0 || e->y > SCREEN_H ||
            e->x < 0 || e->x > SCREEN_W) e->active = 0;
    }
}

/* Move enemy bullets each frame. Despawn off-screen. Collision with player
   is handled in handle_collisions(). */
static void update_enemy_bullets(game_t *g) {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entity_t *e = &g->ents[i];
        if (!e->active || e->kind != ENT_ENEMY_BULLET) continue;

        e->x += e->vx;
        e->y += e->vy;

        if (e->y < 0 || e->y > SCREEN_H ||
            e->x < 0 || e->x > SCREEN_W) e->active = 0;
    }
}

/* Tick ammo-drop ttl and despawn expired drops. Pickup collision is in
   handle_collisions(). Drops don't move. */
static void update_ammo_drops(game_t *g) {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entity_t *e = &g->ents[i];
        if (!e->active || e->kind != ENT_AMMO_DROP) continue;
        if (e->ttl > 0) {
            e->ttl--;
            if (e->ttl <= 0) e->active = 0;
        }
    }
}

/*Boolean function that returns 1 if entities a and b are making contact with each other*/
static int touching(entity_t *a, entity_t *b) {
    return a->x < b->x + 16 &&
           a->x + 16 > b->x &&
           a->y < b->y + 16 &&
           a->y + 16 > b->y;
}

/* Padded AABB overlap for hazards with wider effective hitboxes (e.g. gas). */
static int touching_pad(entity_t *a, entity_t *b, int pad) {
    return a->x - pad < b->x + 16 &&
           a->x + 16 + pad > b->x &&
           a->y - pad < b->y + 16 &&
           a->y + 16 + pad > b->y;
}

/* Apply `damage` to enemy `e`; if it kills, score + kills counters update,
   and we roll for an ammo drop at the kill site. Exposed via game.h so
   autoatk_fire_artillery() can route its instakill through here and get
   the drop probability uniformly. */
void apply_damage(game_t *g, entity_t *e, int damage) {
    if (damage <= 0) return;
    if (e->hp <= damage) {
        if (e->kind == ENT_ENEMY_ARMED) {
            g->score += SCORE_KILL_ARMED;
            g->kills_armed++;
        } else {
            g->score += SCORE_KILL_UNARMED;
            g->kills_unarmed++;
        }
        maybe_spawn_ammo_drop(g, e->x, e->y);
        e->active = 0;
    } else {
        e->hp -= damage;
    }
}

/*Handles all collisions: enemy<->player touch, player bullets<->enemy,
  auto-projectile/hazard<->enemy, enemy-bullet<->player, ammo-drop<->player.
  Damage and score are funneled through apply_damage() so kill bookkeeping
  stays consistent across damage sources.*/
static void handle_collisions(game_t *g) {
    entity_t *p = &g->ents[g->player_i];

    for (int i = 0; i < MAX_ENTITIES; i++) {
        entity_t *e = &g->ents[i];
        if (!e->active) continue;

        /* Player vs ammo drop: pick it up. */
        if (e->kind == ENT_AMMO_DROP) {
            if (touching(p, e)) {
                g->ammo += AMMO_PICKUP_AMOUNT;
                if (g->ammo > AMMO_MAX) g->ammo = AMMO_MAX;
                g->drops_collected++;
                e->active = 0;
            }
            continue;
        }

        /* Player vs enemy bullet: HP damage, bullet despawns. */
        if (e->kind == ENT_ENEMY_BULLET) {
            if (touching(p, e)) {
                g->player_hp -= 10;
                e->active = 0;
            }
            continue;
        }

        if (!is_enemy(e)) continue;

        /* Enemy touches player: 10 HP off, enemy dies. No score for suicide
           kills since neither bullet nor auto-attack made the play. */
        if (touching(p, e)) {
            e->active = 0;
            g->player_hp -= 10;
            continue;
        }

        /* Enemy vs player bullets: 1 damage per bullet, bullet despawns. */
        for (int j = 0; j < MAX_ENTITIES; j++) {
            entity_t *b = &g->ents[j];
            if (!b->active || b->kind != ENT_BULLET) continue;
            if (!touching(b, e)) continue;
            b->active = 0;
            apply_damage(g, e, 1);
            if (!e->active) break;
        }
        if (!e->active) continue;

        /* Enemy vs auto-projectiles / hazards. Damage and despawn-on-hit
           policy come from autoatk.c per the payload weapon kind. Gas uses
           its own size-aware overlap so the expanding cloud's hitbox tracks
           the visible sprite. */
        for (int j = 0; j < MAX_ENTITIES; j++) {
            entity_t *q = &g->ents[j];
            if (!q->active) continue;
            if (q->kind != ENT_AUTO_PROJ && q->kind != ENT_HAZARD) continue;

            int hit;
            if (q->kind == ENT_HAZARD && q->payload == (int)AA_GAS) {
                hit = autoatk_gas_overlaps(q, e->x, e->y);
            } else {
                int pad = autoatk_hitbox_pad((autoatk_kind_t)q->payload);
                hit = touching_pad(q, e, pad);
            }
            if (!hit) continue;

            apply_damage(g, e, autoatk_damage((autoatk_kind_t)q->payload));
            if (autoatk_despawn_on_hit((autoatk_kind_t)q->payload)) {
                q->active = 0;
            }
            if (!e->active) break;
        }
    }
}

/* LEVELUP state input handling. Returns 1 if the player confirmed a choice
   this tick; the caller should then apply the upgrade and advance the wave. */
static int tick_levelup(game_t *g, uint16_t input) {
    g->levelup_prev_cursor = g->levelup_cursor;

    if (input_pressed(g, input, INPUT_LEFT)) {
        g->levelup_cursor = (g->levelup_cursor + LEVELUP_OPTIONS - 1) % LEVELUP_OPTIONS;
    }
    if (input_pressed(g, input, INPUT_RIGHT)) {
        g->levelup_cursor = (g->levelup_cursor + 1) % LEVELUP_OPTIONS;
    }
    return input_pressed(g, input, INPUT_FIRE);
}

void game_tick(game_t *g, uint16_t input) {
    g->prev_state = g->state;
    g->frame++;

    if (g->state == STATE_GAMEOVER) {
        if (input_pressed(g, input, INPUT_START)) {
            game_init(g);
            g->prev_input = input;
            return;
        }
        g->prev_input = input;
        return;
    }

    if (g->state == STATE_LEVELUP) {
        if (tick_levelup(g, input)) {
            int pick_idx = g->levelup_cursor;
            int kind = g->levelup_options[pick_idx];
            if (kind >= 0 && kind < AA_COUNT) {
                autoatk_upgrade(g, (autoatk_kind_t)kind);
            }
            wave_advance(g);
            g->state = STATE_PLAYING;
        }
        g->prev_input = input;
        return;
    }

    /* STATE_PLAYING */
    update_player(g, input);
    wave_tick(g);
    update_enemies(g);
    update_bullets(g);
    update_enemy_bullets(g);
    update_ammo_drops(g);
    autoatk_tick(g);
    autoatk_update_proj(g);
    handle_collisions(g);

    /* On wave clear, pause for level-up. The wave doesn't advance here --
       it advances when the player confirms an upgrade in STATE_LEVELUP. */
    if (wave_complete(g)) {
        g->state = STATE_LEVELUP;
        autoatk_pick_levelup_options(g);
    }

    if (g->player_hp <= 0) {
        g->state = STATE_GAMEOVER;
        /* Freeze the death frame: deactivate everything except the player so the
           game-over screen has clean slots to draw into. */
        for (int i = 0; i < MAX_ENTITIES; i++) {
            if (i != g->player_i) g->ents[i].active = 0;
        }
        g->ents[g->player_i].active = 0;
    }

    g->prev_input = input;
}

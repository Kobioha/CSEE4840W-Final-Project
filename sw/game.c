#include "game.h"
#include "wave.h"
#include "autoatk.h"
#include <stdlib.h>

/* Score weights -- tune here. */
#define SCORE_KILL_ARMED    200
#define SCORE_KILL_UNARMED  50

/* Player fire cooldown in frames. 60 fps -> 5 frames == 12 shots/sec. */
#define FIRE_COOLDOWN_FRAMES 5

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
      g->ents[i].payload = 0;
      return i;
    }
  }
  return -1;
}

static int is_enemy(const entity_t *e) {
    return e->kind == ENT_ENEMY_ARMED || e->kind == ENT_ENEMY_UNARMED;
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

    g->player_i = spawn_entity(g, ENT_PLAYER, SCREEN_W / 2, SCREEN_H - 60);

    wave_system_init(g);
    autoatk_init(g);
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

    if ((input & INPUT_FIRE) && g->fire_cooldown == 0) {
        int b = spawn_entity(g, ENT_BULLET, p->x + 8, p->y - 8);
        if (b >= 0) {
            g->ents[b].vy = -8;
            g->fire_cooldown = FIRE_COOLDOWN_FRAMES;
        }
    }
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
                                          gravitate toward the player overall.*/
#define ENEMY_DIR_FRAMES 12u

static unsigned enemy_rng(int frame, int phase) {
    unsigned window = (unsigned)(frame + phase) / ENEMY_DIR_FRAMES;
    return (window * 1103515245u + (unsigned)phase * 12345u) >> 24;
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

	/*Makes the player lose hp if an enemy steps over the trench!*/
	if(e->y >= SCREEN_H - 20){
	  e->active = 0;
	  g->player_hp -= 10;
	}
    }
}
/*Updates bullet position*/

static void update_bullets(game_t *g) {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entity_t *e = &g->ents[i];
        if (!e->active || e->kind != ENT_BULLET) continue;

        e->x += e->vx;
        e->y += e->vy;

        if (e->y < 0) e->active = 0;
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

/* Apply `damage` to enemy `e`; if it kills, score + kills counters update. */
static void apply_damage(game_t *g, entity_t *e, int damage) {
    if (damage <= 0) return;
    if (e->hp <= damage) {
        if (e->kind == ENT_ENEMY_ARMED) {
            g->score += SCORE_KILL_ARMED;
            g->kills_armed++;
        } else {
            g->score += SCORE_KILL_UNARMED;
            g->kills_unarmed++;
        }
        e->active = 0;
    } else {
        e->hp -= damage;
    }
}

/*Handles all enemy collisions: player touch, player bullets, auto-projectiles,
  and persistent hazards. Damage and score are funneled through apply_damage()
  so the kill bookkeeping stays consistent across damage sources.*/
static void handle_collisions(game_t *g) {
    entity_t *p = &g->ents[g->player_i];

    for (int i = 0; i < MAX_ENTITIES; i++) {
        entity_t *e = &g->ents[i];
        if (!e->active || !is_enemy(e)) continue;

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
           policy come from autoatk.c per the payload weapon kind. */
        for (int j = 0; j < MAX_ENTITIES; j++) {
            entity_t *q = &g->ents[j];
            if (!q->active) continue;
            if (q->kind != ENT_AUTO_PROJ && q->kind != ENT_HAZARD) continue;

            int pad = autoatk_hitbox_pad((autoatk_kind_t)q->payload);
            if (!touching_pad(q, e, pad)) continue;

            apply_damage(g, e, autoatk_damage((autoatk_kind_t)q->payload));
            if (autoatk_despawn_on_hit((autoatk_kind_t)q->payload)) {
                q->active = 0;
            }
            if (!e->active) break;
        }
    }
}

/* Edge-detect: was the bit unset last tick and set this tick? */
static int input_pressed(const game_t *g, uint16_t input, uint16_t bit) {
    return (input & bit) && !(g->prev_input & bit);
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

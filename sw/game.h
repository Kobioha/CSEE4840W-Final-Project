#ifndef GAME_H
#define GAME_H

#include <stdint.h>

#define MAX_ENTITIES 64

#define SCREEN_W 640
#define SCREEN_H 480
#define HUD_H 24

#define INPUT_LEFT   (1 << 0)
#define INPUT_RIGHT  (1 << 1)
#define INPUT_UP     (1 << 2)
#define INPUT_DOWN   (1 << 3)
#define INPUT_FIRE   (1 << 4)
#define INPUT_START  (1 << 5)
#define INPUT_SELECT (1 << 6)
/*Entity kinds. ENT_ENEMY_ARMED and ENT_ENEMY_UNARMED replaced the old
 generic ENT_ENEMY in batch 2; armed enemies have higher HP and are worth
 more score, unarmed are single-shot fodder. ENT_AUTO_PROJ and ENT_HAZARD
 added in batch 3 for the auto-attack system (mortar shells / artillery
 flashes vs persistent barbed-wire / gas clouds).*/
typedef enum {
  ENT_NONE = 0,
  ENT_PLAYER,
  ENT_ENEMY_ARMED,
  ENT_ENEMY_UNARMED,
  ENT_BULLET,
  ENT_AUTO_PROJ,
  ENT_HAZARD
} ent_kind_t;

typedef enum {
  STATE_PLAYING = 0,
  STATE_LEVELUP,
  STATE_GAMEOVER
} game_state_t;

/* Auto-attack catalogue. Kept here (not in autoatk.h) so game_t can embed
   the active state directly without an extra include cycle. */
typedef enum {
  AA_MORTAR = 0,
  AA_WIRE,
  AA_GAS,
  AA_ARTILLERY,
  AA_COUNT
} autoatk_kind_t;

typedef struct {
  int level;       /* 0 = not owned, 1..AA_LEVEL_CAP = owned & upgraded */
  int cooldown;    /* frames until next fire */
  int period;      /* current period in frames (derived from level) */
} autoatk_t;

#define AA_LEVEL_CAP 5
#define LEVELUP_OPTIONS 3
/*Struct which contains the various attributes of the
 kinds of entities that exist in the game:
their kind, whether or not they are active, their position, velocity
and hp*/
typedef struct{
  ent_kind_t kind;
  int active;
  int x,y;
  int vx, vy;
  int hp;
  int phase;   /* per-spawn random seed; used by enemy AI for de-synced motion */
  int ttl;     /* >0 = frames until despawn; 0 = no ttl (player/bullet/enemy) */
  int payload; /* aux: for ENT_AUTO_PROJ/ENT_HAZARD, the autoatk_kind_t that spawned us */
} entity_t;
/*Struct which contains the various statistics and information that
 are crucial to the updating process of the game*/
typedef struct{
  entity_t ents[MAX_ENTITIES];
  int player_i;
  int frame;
  int score;
  int player_hp;
  game_state_t state;
  uint16_t prev_input;

  /* Wave system (batch 2) */
  int wave_index;                 /* 0-based; clamped to last wave */
  int wave_enemies_spawned;       /* enemies emitted this wave so far */
  int wave_armed_remaining;       /* armed enemies still to spawn this wave */
  int wave_spawn_cooldown;        /* frames until next spawn */
  int kills_armed;
  int kills_unarmed;
  int fire_cooldown;              /* frames until player can fire again */
  game_state_t prev_state;        /* for one-shot game-over transition events */

  /* Auto-attack system (batch 3) */
  autoatk_t autoatks[AA_COUNT];
  int levelup_options[LEVELUP_OPTIONS];   /* autoatk_kind_t values; -1 = empty */
  int levelup_cursor;                     /* 0..LEVELUP_OPTIONS-1 */
  int levelup_prev_cursor;                /* for cursor-move edge events in main.c */
} game_t;

void game_init(game_t *g);
void game_tick(game_t *g, uint16_t input);

#endif

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
 more score, unarmed are single-shot fodder.*/
typedef enum {
  ENT_NONE = 0,
  ENT_PLAYER,
  ENT_ENEMY_ARMED,
  ENT_ENEMY_UNARMED,
  ENT_BULLET
} ent_kind_t;

typedef enum {
  STATE_PLAYING = 0,
  STATE_GAMEOVER
} game_state_t;
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
} game_t;

void game_init(game_t *g);
void game_tick(game_t *g, uint16_t input);

#endif

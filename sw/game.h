#ifndef GAME_H
#define GAME_H

#include <stdint.h>

#define MAX_ENTITIES 64

#define SCREEN_W 640
#define SCREEN_H 480
#define HUD_H 24

#define INPUT_LEFT  (1 << 0)
#define INPUT_RIGHT (1 << 1)
#define INPUT_UP    (1 << 2)
#define INPUT_DOWN  (1 << 3)
#define INPUT_FIRE  (1 << 4)
/*Struct which defines the types of entities that can be
 found in the game: player, enemy, and bullet*/
typedef enum {
  ENT_NONE = 0,
  ENT_PLAYER,
  ENT_ENEMY,
  ENT_BULLET
} ent_kind_t;
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
} game_t;

void game_init(game_t *g);
void game_tick(game_t *g, uint16_t input);

#endif

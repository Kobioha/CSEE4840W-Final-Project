#ifndef GAME_H
#define GAME_H

#include <stdint.h>

#define MAX_ENTITIES 64

#define SCREEN_W 640
#define SCREEN_H 480
#define HUD_H 24

/* D-pad (movement) */
#define INPUT_LEFT        (1u << 0)
#define INPUT_RIGHT       (1u << 1)
#define INPUT_UP          (1u << 2)
#define INPUT_DOWN        (1u << 3)
/* Face buttons -- each fires a bullet in the direction the button "points".
   B = DOWN, Y = LEFT, X = UP, A = RIGHT. */
#define INPUT_FIRE_DOWN   (1u << 4)
#define INPUT_FIRE_LEFT   (1u << 5)
#define INPUT_FIRE_UP     (1u << 6)
#define INPUT_FIRE_RIGHT  (1u << 7)
/* Shoulder buttons -- player-triggered abilities. */
#define INPUT_ABIL_ART    (1u << 8)   /* L: artillery beam   */
#define INPUT_ABIL_GAS    (1u << 9)   /* R: gas cloud        */
/* Menu / utility. */
#define INPUT_START       (1u << 10)
#define INPUT_SELECT      (1u << 11)
/* Legacy alias: code that still says INPUT_FIRE (e.g. level-up "confirm")
   means "the B button" -- which now fires DOWN. */
#define INPUT_FIRE        INPUT_FIRE_DOWN

/*Entity kinds. ENT_ENEMY_ARMED and ENT_ENEMY_UNARMED replaced the old
 generic ENT_ENEMY in batch 2; armed enemies have higher HP and are worth
 more score, unarmed are single-shot fodder. ENT_AUTO_PROJ and ENT_HAZARD
 added in batch 3 for the auto-attack system (mortar shells / gas clouds).
 ENT_ENEMY_BULLET and ENT_AMMO_DROP added in this batch: armed enemies
 shoot back, and kills can drop ammo pickups the player walks over.*/
typedef enum {
  ENT_NONE = 0,
  ENT_PLAYER,
  ENT_ENEMY_ARMED,
  ENT_ENEMY_UNARMED,
  ENT_BULLET,
  ENT_AUTO_PROJ,
  ENT_HAZARD,
  ENT_ENEMY_BULLET,
  ENT_AMMO_DROP
} ent_kind_t;

typedef enum {
  STATE_PLAYING = 0,
  STATE_LEVELUP,
  STATE_GAMEOVER
} game_state_t;

/* Auto-attack catalogue. Kept here (not in autoatk.h) so game_t can embed
   the active state directly without an extra include cycle.
   AA_WIRE was retired this batch (barbed wire removed from the game). */
typedef enum {
  AA_MORTAR = 0,
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
  int ttl_max; /* original ttl at spawn; used for animation phase (e.g. gas growth) */
  int payload; /* aux: for ENT_AUTO_PROJ/ENT_HAZARD, the autoatk_kind_t that spawned us */
  int fire_cd; /* armed-enemy fire cooldown; counts down each frame */
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

  /* Ammo + ability charges (this batch). Fully additive with caps:
     on wave_advance() each is bumped by its refill amount, capped at the max.
     Initial values come from game_init(). */
  int ammo;
  int artillery_charges;
  int gas_charges;
  int art_input_cd;       /* L-button post-fire lockout, frames */
  int gas_input_cd;       /* R-button post-fire lockout, frames */
  int drops_collected;    /* lifetime stat for the game-over log */
} game_t;

/* Ammo / charge tuning. */
#define AMMO_MAX             99
#define CHARGE_MAX           5
#define AMMO_INITIAL         40
#define ART_CHARGES_INITIAL  2
#define GAS_CHARGES_INITIAL  2
#define AMMO_REFILL_PER_WAVE 40
#define CHARGES_REFILL_PER_WAVE 2
#define AMMO_PICKUP_AMOUNT   10
#define AMMO_DROP_PERCENT    30
#define ABILITY_INPUT_COOLDOWN 20

void game_init(game_t *g);
void game_tick(game_t *g, uint16_t input);

/* Damage an enemy entity. On kill it scores, increments the kill counter,
   rolls for an ammo drop, and deactivates the entity. Exposed so autoatk.c
   can route its artillery instakill through the same bookkeeping path. */
void apply_damage(game_t *g, entity_t *e, int damage);

#endif

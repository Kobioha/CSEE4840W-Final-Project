#include "game.h"
#include <stdlib.h>

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
      return i;
    }
  }
  return -1;
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
    g->prev_input = 0;

    g->player_i = spawn_entity(g, ENT_PLAYER, SCREEN_W / 2, SCREEN_H - 60);
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

    if (input & INPUT_FIRE) {
        int b = spawn_entity(g, ENT_BULLET, p->x + 8, p->y - 8);
        if (b >= 0) {
            g->ents[b].vy = -8;
        }
    }
}

/*Spawns enemies on the screen*/
static void spawn_enemies(game_t *g) {
    if (g->frame % 60 == 0) {
        int x = rand() % (SCREEN_W - 16);
        spawn_entity(g, ENT_ENEMY, x, HUD_H);
    }
}
/*Updates position of the enemies*/
static void update_enemies(game_t *g) {
    entity_t *p = &g->ents[g->player_i];

    for (int i = 0; i < MAX_ENTITIES; i++) {
        entity_t *e = &g->ents[i];
        if (!e->active || e->kind != ENT_ENEMY) continue;

        if (e->x < p->x) e->x++;
        if (e->x > p->x) e->x--;
        if (e->y < p->y) e->y++;
        if (e->y > p->y) e->y--;

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

/*Handles collision amongst entities e.g.: player's hp are reduced if they are hit!*/
static void handle_collisions(game_t *g) {
    entity_t *p = &g->ents[g->player_i];

    for (int i = 0; i < MAX_ENTITIES; i++) {
        entity_t *e = &g->ents[i];
        if (!e->active || e->kind != ENT_ENEMY) continue;

        if (touching(p, e)) {
            e->active = 0;
            g->player_hp -= 10;
        }

        for (int j = 0; j < MAX_ENTITIES; j++) {
            entity_t *b = &g->ents[j];
            if (!b->active || b->kind != ENT_BULLET) continue;

            if (touching(b, e)) {
                b->active = 0;
                e->active = 0;
                g->score += 100;
                break;
            }
        }
    }
}

/* Edge-detect: was the bit unset last tick and set this tick? */
static int input_pressed(const game_t *g, uint16_t input, uint16_t bit) {
    return (input & bit) && !(g->prev_input & bit);
}

void game_tick(game_t *g, uint16_t input) {
    g->frame++;

    if (g->state == STATE_GAMEOVER) {
        if (input_pressed(g, input, INPUT_START)) {
            game_init(g);
            /* game_init() reset prev_input to 0; preserve the current input so
               the still-held Start button doesn't re-trigger next tick. */
            g->prev_input = input;
            return;
        }
        g->prev_input = input;
        return;
    }

    update_player(g, input);
    spawn_enemies(g);
    update_enemies(g);
    update_bullets(g);
    handle_collisions(g);

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

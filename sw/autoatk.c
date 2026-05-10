#include "autoatk.h"
#include <stdlib.h>

/* Period at level 1, in frames. Higher level shortens by PERIOD_STEP per
   level, floored at PERIOD_MIN so a maxed weapon still has breathing room. */
static const int BASE_PERIOD[AA_COUNT] = {
    [AA_MORTAR]    = 180,
    [AA_WIRE]      = 600,
    [AA_GAS]       = 360,
    [AA_ARTILLERY] = 240,
};
static const int PERIOD_STEP[AA_COUNT] = {
    [AA_MORTAR]    = 30,
    [AA_WIRE]      = 60,
    [AA_GAS]       = 45,
    [AA_ARTILLERY] = 30,
};
#define PERIOD_MIN 60   /* 1 second */

static int period_for(autoatk_kind_t k, int level) {
    if (level <= 0) return 0;
    int p = BASE_PERIOD[k] - PERIOD_STEP[k] * (level - 1);
    if (p < PERIOD_MIN) p = PERIOD_MIN;
    return p;
}

static int alloc_entity(game_t *g) {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        if (!g->ents[i].active) return i;
    }
    return -1;
}

static int find_nearest_enemy(const game_t *g, int *out_x, int *out_y) {
    const entity_t *p = &g->ents[g->player_i];
    int best = -1;
    long best_dist = -1;
    for (int i = 0; i < MAX_ENTITIES; i++) {
        const entity_t *e = &g->ents[i];
        if (!e->active) continue;
        if (e->kind != ENT_ENEMY_ARMED && e->kind != ENT_ENEMY_UNARMED) continue;
        long dx = e->x - p->x;
        long dy = e->y - p->y;
        long d  = dx * dx + dy * dy;
        if (best < 0 || d < best_dist) { best = i; best_dist = d; }
    }
    if (best < 0) return 0;
    *out_x = g->ents[best].x;
    *out_y = g->ents[best].y;
    return 1;
}

static void emit_proj(game_t *g, ent_kind_t kind, int x, int y, int vx, int vy,
                      int ttl, autoatk_kind_t payload) {
    int slot = alloc_entity(g);
    if (slot < 0) return;
    entity_t *e = &g->ents[slot];
    e->active  = 1;
    e->kind    = kind;
    e->x       = x;
    e->y       = y;
    e->vx      = vx;
    e->vy      = vy;
    e->hp      = 1;
    e->phase   = 0;
    e->ttl     = ttl;
    e->payload = (int)payload;
}

static void fire_mortar(game_t *g) {
    int x = rand() % (SCREEN_W - 16);
    emit_proj(g, ENT_AUTO_PROJ, x, HUD_H + 4, 0, 4, /*ttl=*/200, AA_MORTAR);
}

static void fire_wire(game_t *g) {
    int x = (rand() % (SCREEN_W - 32)) + 8;
    int y = HUD_H + 80 + (rand() % (SCREEN_H - HUD_H - 160));
    emit_proj(g, ENT_HAZARD, x, y, 0, 0, /*ttl=*/600, AA_WIRE);
}

static void fire_gas(game_t *g) {
    const entity_t *p = &g->ents[g->player_i];
    int x = p->x;
    int y = p->y - 60;
    if (y < HUD_H) y = HUD_H;
    emit_proj(g, ENT_HAZARD, x, y, 0, 0, /*ttl=*/300, AA_GAS);
}

static void fire_artillery(game_t *g) {
    int tx, ty;
    if (!find_nearest_enemy(g, &tx, &ty)) return;
    emit_proj(g, ENT_AUTO_PROJ, tx, ty, 0, 0, /*ttl=*/20, AA_ARTILLERY);
}

void autoatk_init(game_t *g) {
    for (int i = 0; i < AA_COUNT; i++) {
        g->autoatks[i].level    = 0;
        g->autoatks[i].cooldown = 0;
        g->autoatks[i].period   = 0;
    }
    for (int i = 0; i < LEVELUP_OPTIONS; i++) {
        g->levelup_options[i] = -1;
    }
    g->levelup_cursor      = 0;
    g->levelup_prev_cursor = 0;
}

void autoatk_tick(game_t *g) {
    for (int i = 0; i < AA_COUNT; i++) {
        autoatk_t *a = &g->autoatks[i];
        if (a->level <= 0) continue;
        if (a->cooldown > 0) { a->cooldown--; continue; }

        switch ((autoatk_kind_t)i) {
            case AA_MORTAR:    fire_mortar(g);    break;
            case AA_WIRE:      fire_wire(g);      break;
            case AA_GAS:       fire_gas(g);       break;
            case AA_ARTILLERY: fire_artillery(g); break;
            default: break;
        }
        a->cooldown = a->period;
    }
}

void autoatk_update_proj(game_t *g) {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entity_t *e = &g->ents[i];
        if (!e->active) continue;
        if (e->kind != ENT_AUTO_PROJ && e->kind != ENT_HAZARD) continue;

        e->x += e->vx;
        e->y += e->vy;

        if (e->y > SCREEN_H || e->y < -16 || e->x < -16 || e->x > SCREEN_W) {
            e->active = 0;
            continue;
        }
        if (e->ttl > 0) {
            e->ttl--;
            if (e->ttl <= 0) e->active = 0;
        }
    }
}

void autoatk_upgrade(game_t *g, autoatk_kind_t k) {
    if ((int)k < 0 || (int)k >= AA_COUNT) return;
    autoatk_t *a = &g->autoatks[k];
    if (a->level >= AA_LEVEL_CAP) return;
    a->level++;
    a->period = period_for(k, a->level);
    /* Don't let a freshly-upgraded weapon fire on the same tick it was picked. */
    if (a->cooldown == 0) a->cooldown = a->period;
}

int autoatk_damage(autoatk_kind_t k) {
    switch (k) {
        case AA_MORTAR:    return 2;
        case AA_ARTILLERY: return 99;
        case AA_WIRE:      return 1;
        case AA_GAS:       return 1;
        default:           return 1;
    }
}

int autoatk_despawn_on_hit(autoatk_kind_t k) {
    /* Mortar shells expend on first hit. Artillery flash, wire, and gas all
       remain visible until their TTL expires. */
    return k == AA_MORTAR;
}

int autoatk_hitbox_pad(autoatk_kind_t k) {
    return (k == AA_GAS) ? 6 : 0;
}

void autoatk_pick_levelup_options(game_t *g) {
    /* Rejection-sample 3 distinct kinds. Trivially fast since AA_COUNT == 4. */
    int picked[LEVELUP_OPTIONS];
    int n = 0;
    while (n < LEVELUP_OPTIONS) {
        int k = rand() % AA_COUNT;
        int dup = 0;
        for (int j = 0; j < n; j++) if (picked[j] == k) { dup = 1; break; }
        if (dup) continue;
        picked[n++] = k;
    }
    for (int i = 0; i < LEVELUP_OPTIONS; i++) {
        g->levelup_options[i] = picked[i];
    }
    g->levelup_cursor      = 0;
    g->levelup_prev_cursor = 0;
}

const char *autoatk_name(autoatk_kind_t k) {
    switch (k) {
        case AA_MORTAR:    return "Mortar";
        case AA_WIRE:      return "Barbed Wire";
        case AA_GAS:       return "Mustard Gas";
        case AA_ARTILLERY: return "Artillery";
        default:           return "?";
    }
}

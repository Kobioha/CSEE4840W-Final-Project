#include "autoatk.h"
#include <stdlib.h>

/* Period at level 1, in frames. Higher level shortens by PERIOD_STEP per
   level, floored at PERIOD_MIN so a maxed weapon still has breathing room.
   Only AA_MORTAR is timer-driven now; AA_GAS and AA_ARTILLERY are
   player-triggered abilities with charges (see autoatk_fire_*). Their
   period entries are unused but kept so AA_COUNT-sized arrays stay aligned
   with the enum. */
static const int BASE_PERIOD[AA_COUNT] = {
    [AA_MORTAR]    = 180,
    [AA_GAS]       = 0,
    [AA_ARTILLERY] = 0,
};
static const int PERIOD_STEP[AA_COUNT] = {
    [AA_MORTAR]    = 30,
    [AA_GAS]       = 0,
    [AA_ARTILLERY] = 0,
};
#define PERIOD_MIN 60   /* 1 second */

/* Gas cloud animation phases. ttl_max == GAS_TTL_MAX; we map remaining ttl
   to one of three windows: grow, hold, shrink. Sizes are full-cloud pixels. */
#define GAS_TTL_MAX     90
#define GAS_GROW_FRAMES 30
#define GAS_SHRINK_FRAMES 15
#define GAS_W_MIN       16
#define GAS_W_MAX       48
#define GAS_H_MIN       16
#define GAS_H_MAX       32
#define GAS_BASE_HITPAD 4    /* small additive pad on top of size-derived box */

/* Artillery beam: how many frames the tile-map beam stays visible. The
   visual itself is a column of tile-11 (vertical white line) glyphs written
   into the tile map by render.c when g->beam_ttl > 0. */
#define BEAM_VISUAL_TTL     6
#define ARTILLERY_KILL_DMG  999

static int period_for(autoatk_kind_t k, int level) {
    if (level <= 0) return 0;
    if (BASE_PERIOD[k] == 0) return 0;   /* non-timer weapons */
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
    e->ttl_max = ttl;
    e->payload = (int)payload;
    e->fire_cd = 0;
}

/* Mortar (the only remaining auto-fire weapon) -- spawns near the player and
   launches upward so it visually reads as the player's lobbed attack rather
   than an enemy mortar dropping in. */
static void fire_mortar(game_t *g) {
    const entity_t *p = &g->ents[g->player_i];
    int jitter = (rand() % 65) - 32;        /* -32 .. +32 px */
    int x = p->x + jitter;
    if (x < 0) x = 0;
    if (x > SCREEN_W - 16) x = SCREEN_W - 16;
    int y = p->y - 8;
    if (y < HUD_H) y = HUD_H;
    emit_proj(g, ENT_AUTO_PROJ, x, y, 0, -6, /*ttl=*/200, AA_MORTAR);
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
    /* Only mortar runs on a timer now. */
    autoatk_t *a = &g->autoatks[AA_MORTAR];
    if (a->level <= 0) return;
    if (a->cooldown > 0) { a->cooldown--; return; }
    fire_mortar(g);
    a->cooldown = a->period;
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
    if (a->period > 0 && a->cooldown == 0) a->cooldown = a->period;
}

int autoatk_damage(autoatk_kind_t k) {
    switch (k) {
        case AA_MORTAR:    return 2;
        case AA_ARTILLERY: return ARTILLERY_KILL_DMG;
        case AA_GAS:       return 1;
        default:           return 1;
    }
}

int autoatk_despawn_on_hit(autoatk_kind_t k) {
    /* Mortar shells expend on first hit. Gas persists, artillery flash
       sprites self-despawn via ttl regardless. */
    return k == AA_MORTAR;
}

int autoatk_hitbox_pad(autoatk_kind_t k) {
    /* For gas the precise box is computed in autoatk_gas_overlaps; this
       fallback is used by the generic touching_pad path. Keep it small. */
    return (k == AA_GAS) ? GAS_BASE_HITPAD : 0;
}

void autoatk_gas_size(const entity_t *e, int *out_w, int *out_h) {
    int w = GAS_W_MAX, h = GAS_H_MAX;
    int elapsed = e->ttl_max - e->ttl;
    int hold_until = GAS_TTL_MAX - GAS_SHRINK_FRAMES;
    if (elapsed < GAS_GROW_FRAMES) {
        /* Linearly interpolate from MIN to MAX. */
        w = GAS_W_MIN + (GAS_W_MAX - GAS_W_MIN) * elapsed / GAS_GROW_FRAMES;
        h = GAS_H_MIN + (GAS_H_MAX - GAS_H_MIN) * elapsed / GAS_GROW_FRAMES;
    } else if (elapsed > hold_until) {
        int t = GAS_TTL_MAX - elapsed;  /* frames left in shrink window */
        if (t < 0) t = 0;
        w = GAS_W_MIN + (GAS_W_MAX - GAS_W_MIN) * t / GAS_SHRINK_FRAMES;
        h = GAS_H_MIN + (GAS_H_MAX - GAS_H_MIN) * t / GAS_SHRINK_FRAMES;
    }
    if (w < GAS_W_MIN) w = GAS_W_MIN;
    if (h < GAS_H_MIN) h = GAS_H_MIN;
    *out_w = w;
    *out_h = h;
}

int autoatk_gas_overlaps(const entity_t *e, int tx, int ty) {
    int w, h;
    autoatk_gas_size(e, &w, &h);
    /* Cloud is centered on the entity's stored origin (which is the player's
       position at fire time). Cloud rect: [cx - w/2, cy - h/2] .. [+w, +h]. */
    int cx = e->x + 8;
    int cy = e->y + 8;
    int gx0 = cx - w / 2 - GAS_BASE_HITPAD;
    int gy0 = cy - h / 2 - GAS_BASE_HITPAD;
    int gx1 = cx + w / 2 + GAS_BASE_HITPAD;
    int gy1 = cy + h / 2 + GAS_BASE_HITPAD;
    /* Target is a 16x16 sprite at (tx, ty). */
    return gx0 < tx + 16 && gx1 > tx && gy0 < ty + 16 && gy1 > ty;
}

/* Player-triggered artillery: instant column kill from the player upward,
   plus a tile-map beam visual that render.c paints into the tilemap for
   BEAM_VISUAL_TTL frames. Routes the kill through apply_damage() so
   ammo-drop probability + score bookkeeping stay consistent with bullet
   and mortar kills. */
int autoatk_fire_artillery(game_t *g) {
    const entity_t *p = &g->ents[g->player_i];
    int beam_x = p->x;   /* left edge of 16-px-wide beam column */

    for (int i = 0; i < MAX_ENTITIES; i++) {
        entity_t *e = &g->ents[i];
        if (!e->active) continue;
        if (e->kind != ENT_ENEMY_ARMED && e->kind != ENT_ENEMY_UNARMED) continue;
        if (e->x + 16 <= beam_x || e->x >= beam_x + 16) continue;
        if (e->y > p->y) continue;   /* beam shoots upward only */
        apply_damage(g, e, ARTILLERY_KILL_DMG);
    }

    /* Mark the beam visual. render.c owns the actual tile-map writes (and
       restores the underlying ground tiles when beam_ttl drops to 0). */
    g->beam_col = (p->x + 8) / 8;          /* tile column at player's center */
    g->beam_ttl = BEAM_VISUAL_TTL + 1;     /* +1 because game_tick decrements
                                              before render reads it          */
    return 1;
}

/* Player-triggered gas: spawns ONE ENT_HAZARD at player position. The hazard
   is the source-of-truth; render.c emits up to 4 sprite slots around it
   according to autoatk_gas_size(). */
int autoatk_fire_gas(game_t *g) {
    const entity_t *p = &g->ents[g->player_i];
    int slot = alloc_entity(g);
    if (slot < 0) return 0;
    entity_t *e = &g->ents[slot];
    e->active  = 1;
    e->kind    = ENT_HAZARD;
    e->x       = p->x;
    e->y       = p->y;
    e->vx      = 0;
    e->vy      = 0;
    e->hp      = 1;
    e->phase   = 0;
    e->ttl     = GAS_TTL_MAX;
    e->ttl_max = GAS_TTL_MAX;
    e->payload = (int)AA_GAS;
    e->fire_cd = 0;
    return 1;
}

void autoatk_pick_levelup_options(game_t *g) {
    /* AA_COUNT is small (3). With LEVELUP_OPTIONS == 3 we end up offering all
       three weapons -- the rejection-sampling loop still terminates quickly. */
    int picked[LEVELUP_OPTIONS];
    int n = 0;
    int safety = 0;
    while (n < LEVELUP_OPTIONS && safety < 256) {
        safety++;
        int k = rand() % AA_COUNT;
        int dup = 0;
        for (int j = 0; j < n; j++) if (picked[j] == k) { dup = 1; break; }
        if (dup) continue;
        picked[n++] = k;
    }
    /* Fill any tail with -1 if AA_COUNT < LEVELUP_OPTIONS. */
    for (int i = 0; i < LEVELUP_OPTIONS; i++) {
        g->levelup_options[i] = (i < n) ? picked[i] : -1;
    }
    g->levelup_cursor      = 0;
    g->levelup_prev_cursor = 0;
}

const char *autoatk_name(autoatk_kind_t k) {
    switch (k) {
        case AA_MORTAR:    return "Mortar";
        case AA_GAS:       return "Mustard Gas";
        case AA_ARTILLERY: return "Artillery";
        default:           return "?";
    }
}

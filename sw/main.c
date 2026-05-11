/*
 * main.c -- top-level loop.
 *
 * Two build modes are supported via the Makefile:
 *   - Default (FPGA target): mmaps nml_gpu via /dev/mem and renders to VGA.
 *   - make terminal (host build): falls back to render_terminal.c so the
 *     game logic can be exercised on a laptop without hardware. The selection
 *     is purely a link-time choice between render.c and render_terminal.c;
 *     this file is unchanged either way.
 */

#define _POSIX_C_SOURCE 200809L

#include "game.h"
#include "input.h"
#include "render.h"
#include "wave.h"
#include "autoatk.h"

#include <signal.h>
#include <stdio.h>
#include <time.h>

#ifndef NML_TERMINAL_BUILD
#  include "nml_gpu.h"
#endif

static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

/* Wave-progression tracker. -1 means "not initialized yet"; on the first
   iteration (or after a restart) we print only the START banner without a
   spurious CLEAR line. */
typedef struct {
    int last_wave_index;
    int last_frame;
    int score_at_wave_start;
    int kills_armed_at_wave_start;
    int kills_unarmed_at_wave_start;
} wave_tracker_t;

static void wave_tracker_init(wave_tracker_t *t) {
    t->last_wave_index             = -1;
    t->last_frame                  =  0;
    t->score_at_wave_start         =  0;
    t->kills_armed_at_wave_start   =  0;
    t->kills_unarmed_at_wave_start =  0;
}

static void print_wave_start(const game_t *g) {
    const wave_def_t *w = wave_current(g);
    printf("=== Wave %d START | %d enemies (%d armed @ %d HP, %d unarmed) "
           "| speed %d px/frame | spawn every %d frames ===\n",
           g->wave_index + 1, w->total_enemies, w->armed_count,
           w->armed_hp, w->total_enemies - w->armed_count,
           w->enemy_speed, w->spawn_period_frames);
    printf("    starting bullets=%d  artillery=%d  gas=%d  hp=%d\n",
           g->ammo, g->artillery_charges, g->gas_charges, g->player_hp);
    fflush(stdout);
}

static void print_levelup_menu(const game_t *g) {
    printf("\n=== LEVEL UP === (wave %d cleared, pick one)\n", g->wave_index + 1);
    for (int i = 0; i < LEVELUP_OPTIONS; i++) {
        int kind = g->levelup_options[i];
        if (kind < 0 || kind >= AA_COUNT) continue;
        int lvl = g->autoatks[kind].level;
        char marker = (i == g->levelup_cursor) ? '>' : ' ';
        if (lvl >= AA_LEVEL_CAP) {
            printf("  %c [%d] %-12s (LV %d MAX)\n",
                   marker, i + 1, autoatk_name((autoatk_kind_t)kind), lvl);
        } else {
            printf("  %c [%d] %-12s (LV %d -> %d)\n",
                   marker, i + 1, autoatk_name((autoatk_kind_t)kind), lvl, lvl + 1);
        }
    }
    printf("  Use LEFT/RIGHT to move cursor, B to confirm.\n");
    fflush(stdout);
}

static void print_levelup_cursor(const game_t *g) {
    int kind = g->levelup_options[g->levelup_cursor];
    if (kind < 0 || kind >= AA_COUNT) return;
    printf("  cursor -> [%d] %s\n",
           g->levelup_cursor + 1, autoatk_name((autoatk_kind_t)kind));
    fflush(stdout);
}

static void print_levelup_selection(const game_t *g, autoatk_kind_t k) {
    printf("=== SELECTED: %s (now LV %d) ===\n\n",
           autoatk_name(k), g->autoatks[k].level);
    fflush(stdout);
}

/* Prints the CLEAR delta vs the last wave-start snapshot. Caller is
   responsible for refreshing the tracker after start of the next wave. */
static void print_wave_clear(const game_t *g, const wave_tracker_t *t) {
    if (t->last_wave_index < 0) return;
    int dscore = g->score         - t->score_at_wave_start;
    int dka    = g->kills_armed   - t->kills_armed_at_wave_start;
    int dku    = g->kills_unarmed - t->kills_unarmed_at_wave_start;
    printf("=== Wave %d CLEAR | +%d score | +%dA / +%dU kills | "
           "total score %d ===\n",
           t->last_wave_index + 1, dscore, dka, dku, g->score);
    fflush(stdout);
}

static void snapshot_wave_start(const game_t *g, wave_tracker_t *t) {
    t->last_wave_index             = g->wave_index;
    t->score_at_wave_start         = g->score;
    t->kills_armed_at_wave_start   = g->kills_armed;
    t->kills_unarmed_at_wave_start = g->kills_unarmed;
}

/* Detects fresh wave starts (initial or after upgrade). Used every tick.
   No longer handles CLEAR -- that's printed explicitly on PLAYING->LEVELUP. */
static void track_wave_transitions(const game_t *g, wave_tracker_t *t) {
    /* Restart detected: game.frame jumped backward. Forget previous state. */
    if (g->frame < t->last_frame) {
        wave_tracker_init(t);
    }
    t->last_frame = g->frame;

    if (g->wave_index == t->last_wave_index) return;

    print_wave_start(g);
    snapshot_wave_start(g, t);
}

#ifndef NML_TERMINAL_BUILD
/* The battlefield-ground init lives in render.c so the same tile_for()
   helper drives both startup paint and the post-game-over restore. Call
   render_init_tilemap() instead of duplicating the layout logic here. */

/* DEBUG: read back tilemap_ram at a curated set of (row,col) and report
 * whether the stored byte matches what render_init_tilemap() wrote. Each
 * sample uses a byte_lane=0 offset (col % 4 == 0) so the current SV's
 * lane-0 readback path is sufficient to surface the stored value. */
static void debug_probe_tilemap(void) {
    static const struct { int row; int col; } pts[] = {
        {0, 0}, {0, 4}, {0, 8}, {0, 76},
        {1, 0}, {1, 4},
        {2, 0}, {2, 4},
        {10, 0}, {20, 0}, {30, 0}, {40, 0},
        {47, 0}, {50, 0},
        {51, 0}, {51, 4}, {51, 16},
        {52, 0}, {52, 4},
        {59, 0}, {59, 4}, {59, 76},
    };
    int n = (int)(sizeof(pts) / sizeof(pts[0]));
    int matches = 0, zeros = 0, diffs = 0;
    printf("=== Tile-map probe (after render_init_tilemap) ===\n");
    for (int i = 0; i < n; i++) {
        int r = pts[i].row, c = pts[i].col;
        unsigned byte_off = (unsigned)(r * NML_TILEMAP_COLS + c);
        uint32_t w = nml_probe_tile_word(byte_off);
        uint8_t got = (uint8_t)(w & 0xFFu);
        uint8_t exp = render_debug_tile_for(r, c);
        const char *tag = (got == exp) ? "OK" : (got == 0 ? "ZERO" : "DIFF");
        if      (got == exp) matches++;
        else if (got == 0)   zeros++;
        else                 diffs++;
        printf("  off=0x%04x (r=%2d,c=%2d): word=0x%08x got=0x%02x expect=0x%02x [%s]\n",
               byte_off, r, c, w, got, exp, tag);
    }
    printf("  Summary: %d match, %d zero, %d differ\n", matches, zeros, diffs);

    /* Clobber test: prove (or refute) whether a byte_lane=1 write corrupts
     * the adjacent byte_lane=0 slot. If the bridge presents word-aligned
     * avs_address with the byte in the correct lane, our HW's
     * `case(mem_waddr[1:0])` will mis-route the write and clobber lane 0. */
    nml_write_tile(0, 0, 0xAA);
    uint32_t before = nml_probe_tile_word(0);
    nml_write_tile(1, 0, 0xBB);  /* byte_lane=1 */
    uint32_t after  = nml_probe_tile_word(0);
    printf("Clobber test: wrote 0xAA at (0,0) then 0xBB at (1,0) [byte_lane=1]\n");
    printf("  before=0x%08x after=0x%08x\n", before, after);
    if ((after & 0xFFu) == 0xAA) {
        printf("  -> lane-0 slot survived. byte_lane=1 write went somewhere else.\n");
    } else if ((after & 0xFFu) == 0x00) {
        printf("  -> lane-0 slot was CLOBBERED to 0. Bridge likely word-aligns avs_address.\n");
    } else if ((after & 0xFFu) == 0xBB) {
        printf("  -> lane-0 slot got 0xBB. byte_lane=1 write aliased to slot 0.\n");
    } else {
        printf("  -> unexpected value 0x%02x. Investigate manually.\n", (unsigned)(after & 0xFFu));
    }
    /* Repair so the real init isn't left with garbage at (0,0)/(1,0). */
    nml_write_tile(0, 0, render_debug_tile_for(0, 0));
    nml_write_tile(1, 0, render_debug_tile_for(0, 1));
    fflush(stdout);
}

static void init_palette_runtime(void) {
    /* Mirrors hw/gen_rom.py palette indices. We rewrite them here so the SW
     * remains the authoritative source of palette data once the C driver is
     * in charge -- the FPGA $readmemh init only matters before this runs.
     * Keep this table in sync with make_palette() in gen_rom.py. */
    nml_write_palette(0x10, 0x00, 0xFF, 0x00); /* player: green                */
    nml_write_palette(0x11, 0xFF, 0x00, 0x00); /* armed enemy: red             */
    nml_write_palette(0x12, 0xFF, 0xFF, 0x00); /* bullet: yellow               */
    nml_write_palette(0x13, 0xFF, 0x80, 0xA0); /* unarmed enemy: pink          */
    nml_write_palette(0x14, 0xFF, 0x80, 0x00); /* mortar: orange               */
    /* 0x15 (barbed wire gray) retired -- AA_WIRE removed from game. Leaving
       the palette slot blank since gen_rom.py still ships an X glyph there
       and Batch B may reclaim it. */
    nml_write_palette(0x16, 0xC0, 0xE0, 0x00); /* mustard gas: yellow-green    */
    nml_write_palette(0x17, 0xFF, 0xFF, 0xE0); /* artillery flash: bright white*/
    nml_write_palette(0x18, 0x00, 0xC0, 0x40); /* ammo drop placeholder: green */
    nml_write_palette(0x19, 0xC0, 0x20, 0x20); /* enemy bullet placeholder: red*/
    /* Battlefield ground palette: brown dirt (3 shades) + green grass (3
       shades) + deep mud. Inspired by retro pixel-art tilesets — saturated
       earth tones read clearly behind the player/enemy sprites. Keep in
       sync with the same palette block in hw/gen_rom.py make_palette(). */
    nml_write_palette(0x20, 0x6B, 0x44, 0x23); /* PAL_DIRT_MID:   mid brown    */
    nml_write_palette(0x21, 0x9B, 0x71, 0x42); /* PAL_DIRT_LIGHT: sandy tan    */
    nml_write_palette(0x22, 0x3A, 0x25, 0x15); /* PAL_DIRT_DARK:  dark earth   */
    nml_write_palette(0x23, 0x5A, 0x8B, 0x2E); /* PAL_GRASS_MID:  primary grass*/
    nml_write_palette(0x24, 0x3D, 0x5A, 0x1F); /* PAL_GRASS_DARK: shadowed grass*/
    nml_write_palette(0x25, 0x8F, 0xBC, 0x3E); /* PAL_GRASS_LIGHT:new growth   */
    nml_write_palette(0x26, 0x1A, 0x0F, 0x08); /* PAL_MUD_DEEP:   puddle dark  */
    nml_write_palette(0xFF, 0xFF, 0xFF, 0xFF); /* sprite border                */
}
#endif

int main(void) {
#ifndef NML_TERMINAL_BUILD
    if (nml_open() != 0) {
        fprintf(stderr,
                "nml_open() failed -- make sure the FPGA is loaded and "
                "you're running as root (mmap /dev/mem).\n");
        return 1;
    }
    init_palette_runtime();
    render_init_tilemap();
    debug_probe_tilemap();
    nml_set_enable(1);
    nml_set_hud_on(1);
#endif

    /* Catch Ctrl-C so we can shut down cleanly and turn the video off. */
    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    game_t game;
    game_init(&game);

    wave_tracker_t tracker;
    wave_tracker_init(&tracker);

    while (g_running) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        uint16_t input = input_read(game.frame);
        game_tick(&game, input);

        /* Natural game-flow ordering:
           PLAYING -> LEVELUP: "Wave N CLEAR" then the menu.
           In LEVELUP        : cursor moves.
           LEVELUP -> PLAYING: "SELECTED: X" then "Wave N+1 START" (via tracker).
           PLAYING -> GAMEOVER: the final breakdown block. */

        if (game.prev_state == STATE_PLAYING && game.state == STATE_LEVELUP) {
            print_wave_clear(&game, &tracker);
            print_levelup_menu(&game);
        }

        if (game.state == STATE_LEVELUP &&
            game.levelup_cursor != game.levelup_prev_cursor) {
            print_levelup_cursor(&game);
        }

        if (game.prev_state == STATE_LEVELUP && game.state == STATE_PLAYING) {
            int kind = game.levelup_options[game.levelup_cursor];
            if (kind >= 0 && kind < AA_COUNT) {
                print_levelup_selection(&game, (autoatk_kind_t)kind);
            }
        }

        /* Wave-START printer: handles initial wave on game start, every
           wave_index change (LEVELUP->PLAYING), and game restart. */
        track_wave_transitions(&game, &tracker);

        if (game.prev_state == STATE_PLAYING && game.state == STATE_GAMEOVER) {
            printf("\n=== GAME OVER ===\n");
            printf("  score        : %d\n", game.score);
            printf("  wave reached : %d\n", game.wave_index + 1);
            printf("  kills armed  : %d\n", game.kills_armed);
            printf("  kills unarmd : %d\n", game.kills_unarmed);
            printf("  bullets left : %d\n", game.ammo);
            printf("  art charges  : %d\n", game.artillery_charges);
            printf("  gas charges  : %d\n", game.gas_charges);
            printf("  bullet drops : %d\n", game.drops_collected);
            printf("  press START to restart\n");
            printf("=================\n\n");
            fflush(stdout);
        }

        render_frame(&game);

#ifndef NML_TERMINAL_BUILD
        nml_commit_frame();
#endif

        /* Frame pacing: 60 Hz fixed timestep. nanosleep is a coarse cap; the
         * SWAP wait above is the real timing edge once the FPGA is driving
         * us. The terminal build needs nanosleep alone. */
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long elapsed_us = (t1.tv_sec - t0.tv_sec) * 1000000L
                        + (t1.tv_nsec - t0.tv_nsec) / 1000L;
        long target_us = 16666;
        if (elapsed_us < target_us) {
            struct timespec rem = {
                .tv_sec  = 0,
                .tv_nsec = (target_us - elapsed_us) * 1000L,
            };
            nanosleep(&rem, NULL);
        }
    }

    printf("\nShutting down. Final score: %d\n", game.score);

#ifndef NML_TERMINAL_BUILD
    nml_set_enable(0);
    nml_close();
#endif
    return 0;
}

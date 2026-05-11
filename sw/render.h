#ifndef RENDER_H
#define RENDER_H

#include "game.h"

void render_frame(const game_t *g);

/* FPGA-only: paint the battlefield ground into the tile map. Called from
   main.c at startup and re-invoked after game-over → restart to restore
   the playfield. No-op in terminal builds. */
#ifndef NML_TERMINAL_BUILD
void render_init_tilemap(void);

/* DEBUG: exposes the static tile_for() noise function so the startup probe
   in main.c can compare expected vs actual tile bytes for any (row, col). */
#include <stdint.h>
uint8_t render_debug_tile_for(int row, int col);
#endif

#endif

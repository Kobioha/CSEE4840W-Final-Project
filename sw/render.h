#ifndef RENDER_H
#define RENDER_H

#include "game.h"

void render_frame(const game_t *g);

/* FPGA-only: paint the battlefield ground into the tile map. Called from
   main.c at startup and re-invoked after game-over -> restart to restore
   the playfield. No-op in terminal builds. */
#ifndef NML_TERMINAL_BUILD
void render_init_tilemap(void);
#endif

#endif

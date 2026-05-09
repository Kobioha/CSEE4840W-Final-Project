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

#include <stdio.h>
#include <time.h>

#ifndef NML_TERMINAL_BUILD
#  include "nml_gpu.h"
#endif

#ifndef NML_TERMINAL_BUILD
static void init_palette_runtime(void) {
    /* Mirrors hw/gen_rom.py palette indices. We rewrite them here so the SW
     * remains the authoritative source of palette data once the C driver is
     * in charge -- the FPGA $readmemh init only matters before this runs. */
    nml_write_palette(0x10, 0x00, 0xFF, 0x00); /* player: green        */
    nml_write_palette(0x11, 0xFF, 0x00, 0x00); /* enemy:  red          */
    nml_write_palette(0x12, 0xFF, 0xFF, 0x00); /* bullet: yellow       */
    nml_write_palette(0x20, 0x40, 0x40, 0x40); /* tile bg:  dark gray  */
    nml_write_palette(0x21, 0x60, 0x60, 0x60); /* tile bg accent       */
    nml_write_palette(0xFF, 0xFF, 0xFF, 0xFF); /* sprite border        */
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
    nml_set_enable(1);
#endif

    game_t game;
    game_init(&game);

    while (game.player_hp > 0) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        uint16_t input = input_read(game.frame);
        game_tick(&game, input);
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

    printf("GAME OVER. Final score: %d\n", game.score);

#ifndef NML_TERMINAL_BUILD
    nml_set_enable(0);
    nml_close();
#endif
    return 0;
}

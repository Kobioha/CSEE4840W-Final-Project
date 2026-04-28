#define _POSIX_C_SOURCE 200809L

#include <time.h>
#include "game.h"
#include "input.h"
#include "render.h"

int main(void) {
    game_t game;

    game_init(&game);

    while (game.player_hp > 0) {
        uint16_t input = input_read(game.frame);

        game_tick(&game, input);
        render_frame(&game);

	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 16666 * 1000;
	nanosleep(&ts, NULL);
    }

    return 0;
}

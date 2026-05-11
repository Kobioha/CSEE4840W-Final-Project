#include "input.h"
#include "game.h"

/* Fakes a SNES controller. Used by the terminal build and the fake-input
 * cross-build. The pattern exercises all four directional fires plus both
 * shoulder abilities so terminal traces can show the new code paths working.
 *
 * Cycle (mod 480 frames, ~8 sec at 60 fps):
 *   [0..80)   strafe right
 *   [80..160) strafe left
 *   [160..240) move up
 *   [240..320) move down
 *   [320..480) idle so the wave/levelup loop can play out
 *
 * Fire pattern (regardless of movement window):
 *   every 20 frames cycle through DOWN, LEFT, UP, RIGHT in turn.
 *   frame % 200 == 0 -> press L (artillery)
 *   frame % 240 == 0 -> press R (gas)
 *   frame % 600 == 0 -> press START (menu confirm / restart)
 */
uint16_t input_read(int frame) {
    uint16_t input = 0;

    int phase = frame % 480;
    if      (phase < 80)  input |= INPUT_RIGHT;
    else if (phase < 160) input |= INPUT_LEFT;
    else if (phase < 240) input |= INPUT_UP;
    else if (phase < 320) input |= INPUT_DOWN;

    if (frame % 20 == 0) {
        switch ((frame / 20) % 4) {
            case 0: input |= INPUT_FIRE_DOWN;  break;
            case 1: input |= INPUT_FIRE_LEFT;  break;
            case 2: input |= INPUT_FIRE_UP;    break;
            case 3: input |= INPUT_FIRE_RIGHT; break;
        }
    }

    if (frame % 200 == 0) input |= INPUT_ABIL_ART;
    if (frame % 240 == 0) input |= INPUT_ABIL_GAS;
    if (frame > 0 && frame % 600 == 0) input |= INPUT_START;

    return input;
}

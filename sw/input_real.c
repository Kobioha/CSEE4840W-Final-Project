/*
 * input_real.c -- Linux joydev (/dev/input/js0) reader for the SNES-style
 * USB gamepad (KIWITATA Classic SNES). Implements the same input_read(frame)
 * signature as input_fake.c so main.c can swap between them at link time.
 *
 * On first call we open /dev/input/js0 non-blocking; later calls drain
 * pending events into a sticky bitmask. Failures (no controller, no
 * permission) are warned once and then silently produce zero -- the game
 * still runs, the player just doesn't move.
 *
 * Button index assumptions (KIWITATA SNES via hid-generic on Linux 4.19):
 *   0  -> B        -> INPUT_FIRE
 *   8  -> Select   -> INPUT_SELECT
 *   9  -> Start    -> INPUT_START
 *
 * Axes (D-pad usually appears as an axis pair on SNES adapters):
 *   0  -> X        -> INPUT_LEFT / INPUT_RIGHT (deadzone DEADZONE)
 *   1  -> Y        -> INPUT_UP   / INPUT_DOWN
 *
 * If the real mapping differs, fix the constants below. Use the diagnostic
 * `od -tx1 -w8 /dev/input/js0` to confirm button indices before changing.
 */

#include "input.h"
#include "game.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/joystick.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define JS_DEVICE      "/dev/input/js0"
#define DEADZONE       8000     /* ~24% of INT16_MAX */

#define JS_FD_UNINIT   (-1)
#define JS_FD_DISABLED (-2)     /* sentinel: open failed, don't retry */

static int      js_fd        = JS_FD_UNINIT;
static uint16_t button_state = 0;
static int      axis_x       = 0;
static int      axis_y       = 0;

static void open_device(void) {
    js_fd = open(JS_DEVICE, O_RDONLY | O_NONBLOCK);
    if (js_fd < 0) {
        fprintf(stderr,
                "input_real: open(%s) failed: %s -- "
                "no gamepad input until fixed.\n",
                JS_DEVICE, strerror(errno));
        js_fd = JS_FD_DISABLED;
    }
}

static uint16_t button_bit_for(uint8_t number) {
    switch (number) {
        case 0:  return INPUT_FIRE;
        case 8:  return INPUT_SELECT;
        case 9:  return INPUT_START;
        default: return 0;          /* ignored */
    }
}

static void drain_events(void) {
    struct js_event ev;
    while (read(js_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        /* Strip the JS_EVENT_INIT bit: synthetic events at open emit the
           initial state of every button and axis, which we want to apply. */
        uint8_t type = ev.type & ~JS_EVENT_INIT;

        if (type == JS_EVENT_BUTTON) {
            uint16_t bit = button_bit_for(ev.number);
            if (!bit) continue;
            if (ev.value) button_state |=  bit;
            else          button_state &= ~bit;
        } else if (type == JS_EVENT_AXIS) {
            if      (ev.number == 0) axis_x = ev.value;
            else if (ev.number == 1) axis_y = ev.value;
        }
    }
    /* read() returning -1 with EAGAIN/EWOULDBLOCK means "drained"; not an
       error. Any other failure we silently swallow -- the game continues. */
}

uint16_t input_read(int frame) {
    (void)frame;   /* unused; kept for ABI parity with input_fake.c */

    if (js_fd == JS_FD_UNINIT) open_device();
    if (js_fd == JS_FD_DISABLED) return 0;

    drain_events();

    uint16_t dpad = 0;
    if (axis_x < -DEADZONE) dpad |= INPUT_LEFT;
    if (axis_x >  DEADZONE) dpad |= INPUT_RIGHT;
    if (axis_y < -DEADZONE) dpad |= INPUT_UP;
    if (axis_y >  DEADZONE) dpad |= INPUT_DOWN;

    return button_state | dpad;
}

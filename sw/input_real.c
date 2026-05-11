/*
 * input_real.c -- Linux evdev (/dev/input/event0) reader for the DragonRise
 * generic SNES-style USB gamepad (VID 0079:0011). Implements the same
 * input_read(frame) signature as input_fake.c so main.c can swap between
 * them at link time.
 *
 * Why evdev and not joydev: the DE1-SoC class kernel (4.19) is built
 * without joydev (modules.dep.bin missing, no js0 device), but evdev is
 * built-in and event0 enumerates the moment the gamepad is plugged in.
 *
 * DragonRise mapping verified against this controller (KIWITATA SNES USB):
 *   B button       -> BTN_THUMB2  (0x122)  -> INPUT_FIRE_DOWN  (shoot down)
 *   Y button       -> BTN_TOP     (0x123)  -> INPUT_FIRE_LEFT  (shoot left)
 *   X button       -> BTN_TRIGGER (0x120)  -> INPUT_FIRE_UP    (shoot up)
 *   A button       -> BTN_THUMB   (0x121)  -> INPUT_FIRE_RIGHT (shoot right)
 *   L shoulder     -> BTN_TOP2    (0x124)  -> INPUT_ABIL_ART   (artillery)
 *   R shoulder     -> BTN_PINKIE  (0x125)  -> INPUT_ABIL_GAS   (gas cloud)
 *   Start          -> BTN_BASE4   (0x129)  -> INPUT_START
 *   Select         -> BTN_BASE3   (0x128)  -> INPUT_SELECT
 *   D-pad X        -> ABS_X       (0x00)   -> INPUT_LEFT / INPUT_RIGHT
 *   D-pad Y        -> ABS_Y       (0x01)   -> INPUT_UP   / INPUT_DOWN
 *
 * If the mapping differs on this controller, set NML_INPUT_DEBUG=1 in the
 * environment and the first few events get dumped to stderr -- adjust the
 * BTN_CODE_* / ABS_CODE_* constants below and rebuild.
 *
 * Axis ranges are queried via EVIOCGABS at open time, so the deadzone math
 * works regardless of whether DragonRise reports 0..255, -1..1, or signed
 * 16-bit values.
 */

#include "input.h"
#include "game.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#define EV_DEVICE        "/dev/input/event0"

#define FD_UNINIT        (-1)
#define FD_DISABLED      (-2)

/* Button codes -- verified against the DragonRise dump from this controller. */
#define BTN_CODE_FIRE_DOWN   BTN_THUMB2    /* 0x122 -- B */
#define BTN_CODE_FIRE_LEFT   BTN_TOP       /* 0x123 -- Y */
#define BTN_CODE_FIRE_UP     BTN_TRIGGER   /* 0x120 -- X */
#define BTN_CODE_FIRE_RIGHT  BTN_THUMB     /* 0x121 -- A */
#define BTN_CODE_ABIL_ART    BTN_TOP2      /* 0x124 -- L shoulder */
#define BTN_CODE_ABIL_GAS    BTN_PINKIE    /* 0x125 -- R shoulder */
#define BTN_CODE_START       BTN_BASE4     /* 0x129 -- Start */
#define BTN_CODE_SELECT      BTN_BASE3     /* 0x128 -- Select */

/* D-pad axis codes -- edit if it's reported on ABS_HAT0X/HAT0Y instead. */
#define ABS_CODE_X       ABS_X
#define ABS_CODE_Y       ABS_Y

static int      ev_fd          = FD_UNINIT;
static uint16_t button_state   = 0;
static int      axis_x_value   = 0;
static int      axis_x_min     = -1;
static int      axis_x_max     =  1;
static int      axis_x_center  =  0;
static int      axis_y_value   = 0;
static int      axis_y_min     = -1;
static int      axis_y_max     =  1;
static int      axis_y_center  =  0;
static int      debug_enabled  = 0;

static void query_axis(int code, int *min, int *max, int *center, int *initial) {
    struct input_absinfo info;
    if (ioctl(ev_fd, EVIOCGABS(code), &info) == 0) {
        *min     = info.minimum;
        *max     = info.maximum;
        *center  = info.minimum + (info.maximum - info.minimum) / 2;
        *initial = info.value;
    }
}

static void open_device(void) {
    debug_enabled = (getenv("NML_INPUT_DEBUG") != NULL);

    ev_fd = open(EV_DEVICE, O_RDONLY | O_NONBLOCK);
    if (ev_fd < 0) {
        fprintf(stderr,
                "input_real: open(%s) failed: %s -- "
                "no gamepad input until fixed.\n",
                EV_DEVICE, strerror(errno));
        ev_fd = FD_DISABLED;
        return;
    }

    query_axis(ABS_CODE_X, &axis_x_min, &axis_x_max, &axis_x_center, &axis_x_value);
    query_axis(ABS_CODE_Y, &axis_y_min, &axis_y_max, &axis_y_center, &axis_y_value);

    if (debug_enabled) {
        fprintf(stderr,
                "input_real: opened %s; "
                "X[min=%d max=%d center=%d initial=%d] "
                "Y[min=%d max=%d center=%d initial=%d]\n",
                EV_DEVICE,
                axis_x_min, axis_x_max, axis_x_center, axis_x_value,
                axis_y_min, axis_y_max, axis_y_center, axis_y_value);
    }
}

static uint16_t button_bit_for(uint16_t code) {
    switch (code) {
        case BTN_CODE_FIRE_DOWN:  return INPUT_FIRE_DOWN;
        case BTN_CODE_FIRE_LEFT:  return INPUT_FIRE_LEFT;
        case BTN_CODE_FIRE_UP:    return INPUT_FIRE_UP;
        case BTN_CODE_FIRE_RIGHT: return INPUT_FIRE_RIGHT;
        case BTN_CODE_ABIL_ART:   return INPUT_ABIL_ART;
        case BTN_CODE_ABIL_GAS:   return INPUT_ABIL_GAS;
        case BTN_CODE_START:      return INPUT_START;
        case BTN_CODE_SELECT:     return INPUT_SELECT;
        default:                  return 0;
    }
}

static void drain_events(void) {
    struct input_event ev;
    while (read(ev_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (debug_enabled && ev.type != EV_SYN) {
            fprintf(stderr, "ev: type=0x%02x code=0x%03x value=%d\n",
                    ev.type, ev.code, ev.value);
        }

        if (ev.type == EV_KEY) {
            uint16_t bit = button_bit_for(ev.code);
            if (!bit) continue;
            if (ev.value) button_state |=  bit;
            else          button_state &= ~bit;
        } else if (ev.type == EV_ABS) {
            if      (ev.code == ABS_CODE_X) axis_x_value = ev.value;
            else if (ev.code == ABS_CODE_Y) axis_y_value = ev.value;
        }
    }
    /* read() returning -1 with EAGAIN/EWOULDBLOCK means "drained"; not an
       error. Any other failure we silently swallow -- the game continues. */
}

static int deadzone_threshold(int min, int max) {
    int range = max - min;
    if (range <= 2) return 0;          /* digital pad on a -1/0/+1 axis */
    return range / 4;                  /* analog: 25% deadzone */
}

uint16_t input_read(int frame) {
    (void)frame;   /* unused; kept for ABI parity with input_fake.c */

    if (ev_fd == FD_UNINIT)   open_device();
    if (ev_fd == FD_DISABLED) return 0;

    drain_events();

    uint16_t dpad = 0;
    int dx = axis_x_value - axis_x_center;
    int dy = axis_y_value - axis_y_center;
    int tx = deadzone_threshold(axis_x_min, axis_x_max);
    int ty = deadzone_threshold(axis_y_min, axis_y_max);

    if (dx < -tx) dpad |= INPUT_LEFT;
    if (dx >  tx) dpad |= INPUT_RIGHT;
    if (dy < -ty) dpad |= INPUT_UP;
    if (dy >  ty) dpad |= INPUT_DOWN;

    return button_state | dpad;
}

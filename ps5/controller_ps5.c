/* controller_ps5 - reads the DualSense through ScePad and presents it as an
 * N64 controller, replacing the SDL backend.
 *
 * The sample layout and the call sequence are the ones already proven on
 * hardware by PS5SDK/tools/ps5link/pad_test.c, which in turn took the offsets
 * from SharpProspero's GamePad.FromSample. The button values come from its
 * ScePadButton enumeration rather than from guesswork.
 */

#include <stdint.h>
#include <stdbool.h>

#include <ultra64.h>
#include "controller/controller_api.h"

extern int sceUserServiceInitialize(void *initParams);
extern int sceUserServiceGetInitialUser(int *userId);
extern int scePadInit(void);
extern int scePadOpen(int userId, int type, int index, void *param);
extern int scePadReadState(int handle, void *data);

/* DualSense buttons, from SharpProspero's ScePadButton. */
#define PAD_L3          0x00000002u
#define PAD_R3          0x00000004u
#define PAD_OPTIONS     0x00000008u
#define PAD_UP          0x00000010u
#define PAD_RIGHT       0x00000020u
#define PAD_DOWN        0x00000040u
#define PAD_LEFT        0x00000080u
#define PAD_L2          0x00000100u
#define PAD_R2          0x00000200u
#define PAD_L1          0x00000400u
#define PAD_R1          0x00000800u
#define PAD_TRIANGLE    0x00001000u
#define PAD_CIRCLE      0x00002000u
#define PAD_CROSS       0x00004000u
#define PAD_SQUARE      0x00008000u
#define PAD_TOUCH_PAD   0x00100000u    /* opens the options menu (menu_ps5.c) */
/* The system sets this while it has taken the pad for itself - a system
 * dialogue, say. The rest of the sample then describes what the system is
 * doing rather than what the player is pressing, so it is discarded whole. */
#define PAD_INTERCEPTED 0x80000000u

/* Where each field sits in the 1024-byte sample. */
#define OFF_BUTTONS   0
#define OFF_LSTICK_X  4
#define OFF_LSTICK_Y  5
#define OFF_RSTICK_X  6
#define OFF_RSTICK_Y  7
#define OFF_L2_ANALOG 8
#define OFF_R2_ANALOG 9

#define STICK_CENTRE  128
#define STICK_DEADZONE 12          /* in raw units either side of centre */
#define TRIGGER_THRESHOLD 30       /* analogue travel that counts as pressed */
#define C_STICK_THRESHOLD 64       /* right stick deflection that fires a C button */

static int s_handle = -1;
static unsigned char s_sample[1024];

extern bool menu_ps5_blocks_input(void);

static unsigned int sample_buttons(const unsigned char *sample) {
    return (unsigned int)sample[OFF_BUTTONS]
         | ((unsigned int)sample[OFF_BUTTONS + 1] << 8)
         | ((unsigned int)sample[OFF_BUTTONS + 2] << 16)
         | ((unsigned int)sample[OFF_BUTTONS + 3] << 24);
}

/* The pad's buttons as they are now, for the options menu, which runs whether
 * or not the game is reading the pad. 0 while the system holds the pad. */
unsigned int controller_ps5_poll_buttons(void) {
    static unsigned char sample[1024];
    if (s_handle < 0 || scePadReadState(s_handle, sample) < 0) return 0;
    unsigned int buttons = sample_buttons(sample);
    return (buttons & PAD_INTERCEPTED) ? 0 : buttons;
}

static void controller_ps5_init(void) {
    sceUserServiceInitialize(0);

    int user_id = 0;
    if (sceUserServiceGetInitialUser(&user_id) < 0) return;
    if (scePadInit() < 0) return;

    /* The pad has to be opened for the signed-in user, not the system one: a
     * handle opened for the system user is accepted and then never delivers
     * anything, so every button would read as released forever. */
    int handle = scePadOpen(user_id, 0 /* standard port */, 0, 0);
    if (handle < 0) return;
    s_handle = handle;
}

/* The open pad, for the rumble (ps5/glue/rumble_ps5.c). */
int controller_ps5_pad_handle(void) {
    return s_handle;
}

/* Maps one stick axis from the pad's 0..255 with 128 at rest onto the N64's
 * -80..80, with a small dead zone so a resting stick reads as still. */
static int8_t map_stick(unsigned char raw) {
    int v = (int)raw - STICK_CENTRE;
    if (v > -STICK_DEADZONE && v < STICK_DEADZONE) return 0;
    v = v * 80 / 127;
    if (v > 80) v = 80;
    if (v < -80) v = -80;
    return (int8_t)v;
}

static void controller_ps5_read(OSContPad *pad) {
    /* The pad is reset here because nothing else does it: upstream's
     * osContGetReadData clears it before asking each backend, but the PS5
     * entry point linked into the engine only forwards the call. The buttons
     * below are OR'd in, so without this a button once pressed stayed down for
     * good, and the game, which acts on the change from released to pressed,
     * never saw another press. A failed or intercepted read leaves it at rest. */
    pad->button = 0;
    pad->stick_x = 0;
    pad->stick_y = 0;
    pad->errnum = 0;

    if (s_handle < 0) return;
    if (menu_ps5_blocks_input()) return;       /* the menu has the pad */
    if (scePadReadState(s_handle, s_sample) < 0) return;

    unsigned int buttons = sample_buttons(s_sample);
    if (buttons & PAD_INTERCEPTED) return;

    /* Cross jumps and Square is the action button, which is how the SDL
     * backend maps a gamepad too (its A and X). */
    if (buttons & PAD_CROSS)    pad->button |= A_BUTTON;
    if (buttons & PAD_SQUARE)   pad->button |= B_BUTTON;
    if (buttons & PAD_CIRCLE)   pad->button |= B_BUTTON;
    if (buttons & PAD_OPTIONS)  pad->button |= START_BUTTON;
    if (buttons & PAD_L1)       pad->button |= Z_TRIG;
    if (buttons & PAD_R1)       pad->button |= R_TRIG;

    if (buttons & PAD_UP)       pad->button |= U_JPAD;
    if (buttons & PAD_DOWN)     pad->button |= D_JPAD;
    if (buttons & PAD_LEFT)     pad->button |= L_JPAD;
    if (buttons & PAD_RIGHT)    pad->button |= R_JPAD;

    /* The triggers, analogue or digital, work as the shoulders: L2 as Z, R2 as R. */
    if ((buttons & PAD_L2) || s_sample[OFF_L2_ANALOG] > TRIGGER_THRESHOLD) pad->button |= Z_TRIG;
    if ((buttons & PAD_R2) || s_sample[OFF_R2_ANALOG] > TRIGGER_THRESHOLD) pad->button |= R_TRIG;

    /* The right stick stands in for the C buttons, which is what moves the
     * camera. */
    int rx = (int)s_sample[OFF_RSTICK_X] - STICK_CENTRE;
    int ry = (int)s_sample[OFF_RSTICK_Y] - STICK_CENTRE;
    if (rx < -C_STICK_THRESHOLD) pad->button |= L_CBUTTONS;
    if (rx >  C_STICK_THRESHOLD) pad->button |= R_CBUTTONS;
    if (ry < -C_STICK_THRESHOLD) pad->button |= U_CBUTTONS;
    if (ry >  C_STICK_THRESHOLD) pad->button |= D_CBUTTONS;

    pad->stick_x = map_stick(s_sample[OFF_LSTICK_X]);
    /* The pad counts Y downwards and the N64 counts it upwards. */
    pad->stick_y = (int8_t)-map_stick(s_sample[OFF_LSTICK_Y]);
}

struct ControllerAPI controller_ps5 = {
    controller_ps5_init,
    controller_ps5_read,
};

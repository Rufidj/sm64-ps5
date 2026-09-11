/* The rumble, on the DualSense's own motors.
 *
 * The game already knows when to rumble: Shindou's rumble code is in
 * src/game/rumble_init.c and the hundred or so places that call
 * queue_rumble_data(), all of which the build turns on with ENABLE_RUMBLE.
 * What it expects underneath is an N64 Rumble Pak, driven through three
 * functions of the console's library - start, stop, and asking whether one is
 * there. They are those three, over scePadSetVibration.
 *
 * The Rumble Pak had one speed, so the game varies the strength by pulsing it.
 * The DualSense does not have to: the strength the game is asking for is in
 * gCurrRumbleSettings, and it is handed to the motors directly, which makes a
 * hard landing feel different from a soft one.
 */

#include <ultra64.h>
#include "macros.h"
#include "game/main.h"
#include "game/rumble_init.h"
#include "ps5_settings.h"

#if ENABLE_RUMBLE

/* 0 to 255 each: the big motor in the left grip, the small one in the right. */
typedef struct {
    unsigned char largeMotor;
    unsigned char smallMotor;
} ScePadVibrationParam;

extern int scePadSetVibration(int handle, const ScePadVibrationParam *param);
extern int controller_ps5_pad_handle(void);

static int set_motors(unsigned char large, unsigned char small) {
    ScePadVibrationParam param;
    int handle = controller_ps5_pad_handle();

    if (handle < 0) {
        return -1;
    }
    param.largeMotor = large;
    param.smallMotor = small;
    return scePadSetVibration(handle, &param) < 0 ? -1 : 0;
}

/* The strength the game is asking for, as the motors take it. Its own scale
 * runs to about 100; below a quarter of that the big motor alone is enough. */
static void strength(unsigned char *large, unsigned char *small) {
    s32 level = gCurrRumbleSettings.unk02;

    if (level <= 0) {
        level = 40;                       /* a pulse with no level left: a tap */
    }
    if (level > 100) {
        level = 100;
    }
    *large = (unsigned char) (60 + level * 195 / 100);
    *small = (unsigned char) (level > 25 ? level * 180 / 100 : 0);
}

/* Whether there is a rumble pak: there always is, and the game reads anything
 * under 1 as success. */
u32 osMotorInit(UNUSED OSMesgQueue *queue, UNUSED void *pfs, UNUSED s32 port) {
    return 0;
}

s32 osMotorStart(UNUSED void *pfs) {
    unsigned char large, small;

    if (!g_ps5_settings.rumble) {
        return set_motors(0, 0);
    }
    strength(&large, &small);
    return set_motors(large, small);
}

s32 osMotorStop(UNUSED void *pfs) {
    return set_motors(0, 0);
}

#endif

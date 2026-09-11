/* The rumble, on the DualSense's actuators.
 *
 * The game already knows when to rumble: Shindou's rumble code is in
 * src/game/rumble_init.c and the hundred or so places that call
 * queue_rumble_data(), all of which the build turns on with ENABLE_RUMBLE.
 * What it expects underneath is an N64 Rumble Pak, driven through three
 * functions of the console's library - start, stop, and asking whether one is
 * there - and those three are here.
 *
 * The DualSense has no rumble motors. Where a DualShock had two weights on
 * eccentric shafts, it has two voice coils, and they are driven as sound: an
 * audio port of the vibration type (10) carries the waveform they play. The
 * older scePadSetVibration call is accepted and returns success, but nothing
 * moves, which is what this was written the wrong way round the first time.
 *
 * So a thread writes to that port for as long as the title runs: silence while
 * the game is not asking for anything, and a low tone in each coil while it
 * is - 60 Hz in the left one, which stands in for the big motor, and 120 Hz in
 * the right for the small one. The Rumble Pak had one speed and pulsed it to
 * suggest strength; here the strength the game is asking for becomes the
 * amplitude, so a light bump and a hard landing feel different.
 */

#include <stdint.h>

#include <ultra64.h>
#include "macros.h"
#include "game/main.h"
#include "game/rumble_init.h"
#include "ps5_settings.h"

#if ENABLE_RUMBLE

extern int sceAudioOutInit(void);
extern int sceAudioOutOpen(int userId, int type, int index, uint32_t length, uint32_t freq, uint32_t param);
extern int sceAudioOutOutput(int handle, const void *ptr);
extern int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *arg, const char *name);
extern int controller_ps5_user_id(void);

#define PORT_VIBRATION     10      /* the controller's vibration channel */
#define FORMAT_S16_STEREO  1
#define RATE               48000
#define GRAIN              256     /* frames an output call takes: 5.3 ms */

#define LEFT_HZ            60      /* the big motor's stand-in */
#define RIGHT_HZ           120     /* the small one's */

/* What the game is asking for, 0 to 255 each. Written by the game's thread,
 * read by the one below; a stale value for one grain does not matter. */
static volatile int32_t sLarge, sSmall;

static int sPort = -1;

/* A triangle, which the coils take as well as a sine and needs no maths.
 * phase runs over a whole turn of 65536. */
static int32_t triangle(uint32_t phase) {
    int32_t x = (int32_t) (phase & 0xFFFFu);
    if (x < 16384) return x * 2;                 /* 0 -> 32767   */
    if (x < 49152) return 65536 - x * 2;         /* 32767 -> -32767 */
    return x * 2 - 131072;                       /* -32767 -> 0  */
}

static void *vibration_thread(UNUSED void *unused) {
    int16_t grain[GRAIN * 2];
    uint32_t left_phase = 0, right_phase = 0;
    const uint32_t left_step = (uint32_t) (((uint64_t) LEFT_HZ << 16) / RATE);
    const uint32_t right_step = (uint32_t) (((uint64_t) RIGHT_HZ << 16) / RATE);

    for (;;) {
        int32_t large = sLarge, small = sSmall;
        int32_t i;

        for (i = 0; i < GRAIN; i++) {
            grain[i * 2] = (int16_t) ((triangle(left_phase) * large) / 255);
            grain[i * 2 + 1] = (int16_t) ((triangle(right_phase) * small) / 255);
            left_phase += left_step;
            right_phase += right_step;
        }
        sceAudioOutOutput(sPort, grain);      /* returns once the grain has played */
    }
    return NULL;
}

static void open_port(void) {
    void *thread;
    int user = controller_ps5_user_id();

    if (sPort >= 0 || user < 0) {
        return;
    }
    sceAudioOutInit();
    sPort = sceAudioOutOpen(user, PORT_VIBRATION, 0, GRAIN, RATE, FORMAT_S16_STEREO);
    if (sPort < 0) {
        return;
    }
    scePthreadCreate(&thread, NULL, vibration_thread, NULL, "sm64_rumble");
}

/* The strength the game is asking for, as the coils take it. Its own scale
 * runs to about 100. */
static void set_levels(int32_t large, int32_t small) {
    open_port();
    sLarge = large;
    sSmall = small;
}

/* Whether there is a rumble pak: there always is, and the game reads anything
 * under 1 as success. */
u32 osMotorInit(UNUSED OSMesgQueue *queue, UNUSED void *pfs, UNUSED s32 port) {
    open_port();
    return 0;
}

s32 osMotorStart(UNUSED void *pfs) {
    s32 level = gCurrRumbleSettings.unk02;

    if (!g_ps5_settings.rumble) {
        set_levels(0, 0);
        return 0;
    }
    if (level <= 0) {
        level = 40;                      /* a pulse with no level left: a tap */
    }
    if (level > 100) {
        level = 100;
    }
    set_levels(60 + level * 195 / 100, level > 25 ? level * 180 / 100 : 0);
    return 0;
}

s32 osMotorStop(UNUSED void *pfs) {
    set_levels(0, 0);
    return 0;
}

#endif

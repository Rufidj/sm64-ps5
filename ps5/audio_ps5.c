/* audio_ps5 - the AudioAPI half of the PS5 backend: plays the game's sound
 * through the system's main audio output, replacing the SDL backend.
 *
 * Two mismatches are bridged here. The game mixes at 32000 Hz and the main
 * output takes 48000 or 192000 and nothing else, so play() resamples by 3:2 on
 * the way in. And sceAudioOutOutput blocks until its grain has been played, so
 * it runs on a thread of its own, fed through a ring buffer: the game thread
 * writes, the audio thread reads, and each moves only its own index, so the
 * two never need a lock.
 *
 * The constants are SharpProspero's, from its AudioOut interop and
 * AudioOutDevice.OpenStereo, which opens the same port the same way.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "audio/audio_api.h"

extern int sceAudioOutInit(void);
extern int sceAudioOutOpen(int userId, int type, int index, uint32_t length, uint32_t freq, uint32_t param);
extern int sceAudioOutOutput(int handle, const void *ptr);
extern int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *arg, const char *name);

#define SYSTEM_USER        0xFF
#define PORT_MAIN          0
#define FORMAT_S16_STEREO  1
#define GAME_RATE          32000
#define OUTPUT_RATE        48000
#define GRAIN              256     /* frames per output call: the smallest, for the least latency */

/* What the game loop aims to keep queued, and the most it may, in frames at
 * the game's rate - the SDL backend's figures, which the loop is tuned to. */
#define DESIRED_BUFFERED   1100
#define MAX_BUFFERED       6000

/* Output frames, stereo. A power of two so the indices wrap with a mask; about
 * two thirds of a second, well above what the game loop ever keeps queued. */
#define RING_FRAMES        32768u
#define RING_MASK          (RING_FRAMES - 1u)

static int16_t s_ring[RING_FRAMES * 2];
static volatile uint32_t s_write;          /* moved only by the game thread  */
static volatile uint32_t s_read;           /* moved only by the audio thread */

static int s_handle = -1;

/* The resampler's position between the previous input frame and the next, in
 * steps of 1/OUTPUT_RATE of a frame, and that previous frame. */
static uint32_t s_phase;
static int16_t s_previous[2];

static void *audio_thread(void *arg) {
    (void)arg;
    int16_t grain[GRAIN * 2];
    for (;;) {
        uint32_t available = s_write - s_read;
        uint32_t n = available < GRAIN ? available : GRAIN;
        uint32_t read = s_read;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t at = (read + i) & RING_MASK;
            grain[i * 2] = s_ring[at * 2];
            grain[i * 2 + 1] = s_ring[at * 2 + 1];
        }
        /* An underrun plays silence rather than stalling the port. */
        for (uint32_t i = n; i < GRAIN; i++) {
            grain[i * 2] = 0;
            grain[i * 2 + 1] = 0;
        }
        s_read = read + n;
        sceAudioOutOutput(s_handle, grain);     /* returns once the grain has played */
    }
    return NULL;
}

static bool audio_ps5_init(void) {
    sceAudioOutInit();          /* fails harmlessly if something already initialised it */
    int handle = sceAudioOutOpen(SYSTEM_USER, PORT_MAIN, 0, GRAIN, OUTPUT_RATE, FORMAT_S16_STEREO);
    if (handle < 0) return false;
    s_handle = handle;

    void *thread = NULL;
    if (scePthreadCreate(&thread, NULL, audio_thread, NULL, "sm64_audio") < 0) return false;
    return true;
}

/* Queued sound, in frames at the game's rate, as the game loop expects. */
static int audio_ps5_buffered(void) {
    uint32_t queued = s_write - s_read;
    return (int)((uint64_t)queued * GAME_RATE / OUTPUT_RATE);
}

static int audio_ps5_get_desired_buffered(void) {
    return DESIRED_BUFFERED;
}

static void emit_frame(int16_t left, int16_t right) {
    uint32_t write = s_write;
    if (write - s_read >= RING_FRAMES) return;      /* full: drop rather than overwrite */
    uint32_t at = write & RING_MASK;
    s_ring[at * 2] = left;
    s_ring[at * 2 + 1] = right;
    s_write = write + 1;
}

/* Takes interleaved 16-bit stereo at the game's rate and queues it at the
 * output's, interpolating linearly: each input frame yields output frames for
 * every position that falls between it and the one before. */
static void audio_ps5_play(const uint8_t *buf, size_t len) {
    if (s_handle < 0) return;
    if (audio_ps5_buffered() >= MAX_BUFFERED) return;   /* as the SDL backend does */

    size_t frames = len / 4;
    const int16_t *in = (const int16_t *)(const void *)buf;
    for (size_t f = 0; f < frames; f++) {
        int16_t left = in[f * 2], right = in[f * 2 + 1];
        while (s_phase < OUTPUT_RATE) {
            int32_t l = s_previous[0] + (int32_t)(((int64_t)(left - s_previous[0]) * s_phase) / OUTPUT_RATE);
            int32_t r = s_previous[1] + (int32_t)(((int64_t)(right - s_previous[1]) * s_phase) / OUTPUT_RATE);
            emit_frame((int16_t)l, (int16_t)r);
            s_phase += GAME_RATE;
        }
        s_phase -= OUTPUT_RATE;
        s_previous[0] = left;
        s_previous[1] = right;
    }
}

struct AudioAPI audio_ps5 = {
    audio_ps5_init,
    audio_ps5_buffered,
    audio_ps5_get_desired_buffered,
    audio_ps5_play,
};

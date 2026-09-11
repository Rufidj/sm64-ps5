/* main_ps5 - entry point for the PS5 build, replacing pc_main.c.
 *
 * This is a plain C application launched as a real title by ps5link, so it
 * owns its own main() and its own loop. That is the whole difference from the
 * earlier attempt in sm64engine/pc_main_ps5.c, which exposed sm64_init() and
 * sm64_produce_one_frame() to be driven from a C# host - an arrangement that
 * never worked and is not reused here.
 *
 * Everything below mirrors pc_main.c's main_func(), with the four platform
 * backends swapped for the native ones: gfx_agc (drawing), gfx_ps5 (display
 * and frame loop), controller_ps5 (the pad) and audio_ps5 (sound), which
 * falls back to audio_null if the output will not open.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sm64.h"
#include "game/memory.h"
#include "game/game_init.h"
#include "audio/external.h"
#include "gfx/gfx_pc.h"
#include "gfx/gfx_window_manager_api.h"
#include "gfx/gfx_rendering_api.h"
#include "audio/audio_api.h"
#include "controller/controller_api.h"

#include "ps5gpu.h"
#include "asset_loader.h"
#include "menu_ps5.h"
#include "ps5_settings.h"
#include "hd_textures.h"

extern struct GfxRenderingAPI gfx_agc_api;
extern struct GfxWindowManagerAPI gfx_ps5_api;
extern struct AudioAPI audio_ps5;
extern struct AudioAPI audio_null;

/* Globals the engine expects the platform layer to define. */
OSMesg gMainReceivedMesg;
OSMesgQueue gSIEventMesgQueue;
s8 gResetTimer;
s8 gNmiResetBarsTimer;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

static struct AudioAPI *audio_api;
static uint8_t inited;

extern void thread5_game_loop(void *arg);
extern void create_next_audio_buffer(s16 *samples, u32 num_samples);
void game_loop_one_iteration(void);

void dispatch_audio_sptask(UNUSED struct SPTask *spTask) { }
void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler,
                        UNUSED OSMesgQueue *queue, UNUSED OSMesg *msg) { }

/* The last frame's display list. The game builds each frame's in one of its
 * pools and only overwrites it when it builds the next, so while the game is
 * frozen under the menu the same list can simply be run again. */
static Gfx *s_last_display_list;

extern struct SPTask *gGfxSPTask;

/* 60 frames a second (enhancements/60fps.patch): the game builds each frame's
 * display lists as the frame half way between the last one and this one, and
 * notes what differs. The list is drawn like that first; then these put this
 * frame's values in and it is drawn again. */
static void patch_interpolations(void) {
    extern void mtx_patch_interpolated(void);
    extern void patch_screen_transition_interpolated(void);
    extern void patch_title_screen_scales(void);
    extern void patch_interpolated_dialog(void);
    extern void patch_interpolated_hud(void);
    extern void patch_interpolated_paintings(void);
    extern void patch_interpolated_bubble_particles(void);
    extern void patch_interpolated_snow_particles(void);
    mtx_patch_interpolated();
    patch_screen_transition_interpolated();
    patch_title_screen_scales();
    patch_interpolated_dialog();
    patch_interpolated_hud();
    patch_interpolated_paintings();
    patch_interpolated_bubble_particles();
    patch_interpolated_snow_particles();
}

void exec_display_list(struct SPTask *spTask) {
    if (!inited) return;
    s_last_display_list = (Gfx *)spTask->task.t.data_ptr;
    /* At 30 frames a second only the game's own frame is shown, so the
     * in-between one the list starts as is patched away before drawing. */
    if (!g_ps5_settings.fps60) patch_interpolations();
    gfx_run(s_last_display_list);
}

#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528

static void produce_one_frame(void) {
    gfx_start_frame();
    /* With the options menu open the game stands still: its last frame is
     * drawn again, with the menu over it. Sound carries on. */
    int frozen = menu_ps5_update() && s_last_display_list != NULL;
    /* The game advances 30 times a second. At 60 each step is shown twice -
     * the in-between frame, then its own - each held one vertical blank;
     * otherwise its one frame is held for two. A frozen game is drawn at 30,
     * so the sound below is still made once per 30th of a second. */
    int fps60 = g_ps5_settings.fps60 && !frozen;
    ps5gpu_set_frame_interval(fps60 ? 1 : 2);
    if (frozen)
        gfx_run(s_last_display_list);
    else
        game_loop_one_iteration();

    int samples_left = audio_api->buffered();
    u32 num_audio_samples = samples_left < audio_api->get_desired_buffered() ? SAMPLES_HIGH : SAMPLES_LOW;
    s16 audio_buffer[SAMPLES_HIGH * 2 * 2];
    for (int i = 0; i < 2; i++)
        create_next_audio_buffer(audio_buffer + i * (num_audio_samples * 2), num_audio_samples);
    audio_api->play((u8 *)audio_buffer, 2 * num_audio_samples * 4);

    gfx_end_frame();

    if (fps60 && gGfxSPTask != NULL) {
        gfx_start_frame();
        patch_interpolations();
        exec_display_list(gGfxSPTask);
        gfx_end_frame();
    }
}

/* The save file lives in the game's own folder, where the earlier port kept
 * it: data/sm64_ps5/saves/ under the title, which the title sees as /app0. A
 * title runs sandboxed and cannot reach /data directly - a first attempt there
 * never found the file. The engine's EEPROM emulation is compiled with the
 * same path (SM64_SAVE_FILE_PATH, src/pc/ultra_reimplementation.c). */
#define SAVE_DIR  "/app0/data/sm64_ps5/saves"
#define SAVE_FILE SAVE_DIR "/sm64_save_file.bin"
#define SAVE_PROBE SAVE_DIR "/write_test.tmp"

/* Makes sure the save folder exists, and says whether a save was found and
 * whether the folder takes writes: the title's folder may be mounted
 * read-only, in which case a save loads but progress cannot be kept. */
static void check_save_folder(void) {
    mkdir("/app0/data", 0777);             /* each fails harmlessly if already there */
    mkdir("/app0/data/sm64_ps5", 0777);
    mkdir(SAVE_DIR, 0777);

    FILE *save = fopen(SAVE_FILE, "rb");
    int found = save != NULL;
    if (save) fclose(save);

    FILE *probe = fopen(SAVE_PROBE, "wb");
    int writable = probe != NULL && fwrite("", 1, 1, probe) == 1;
    if (probe) {
        fclose(probe);
        unlink(SAVE_PROBE);
    }

    if (found)
        ps5gpu_notify(writable ? "sm64: partida encontrada (se puede guardar)"
                               : "sm64: partida encontrada (sin permiso para guardar)");
    else
        ps5gpu_notify(writable ? "sm64: sin partida guardada (se puede guardar)"
                               : "sm64: sin partida guardada ni permiso para guardar");
}

int main(void) {
    ps5gpu_notify("sm64: arrancando");

    /* A shareable build carries no ROM data; it is copied back here, from the
     * player's own ROM, before anything reads it. */
    if (asset_loader_restore() < 0) {
        ps5gpu_notify(asset_loader_error());
        return 1;
    }

    check_save_folder();
    hd_textures_init();

    main_pool_init();
    gGfxAllocOnlyPool = alloc_only_pool_init();
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);

    gfx_init(&gfx_ps5_api, &gfx_agc_api, "Super Mario 64", true);
    menu_ps5_init();

    /* The system's main output if it opens; silence otherwise, so the game
     * still runs without it. */
    audio_api = &audio_ps5;
    if (!audio_api->init()) {
        ps5gpu_notify("sm64: sin audio (no se pudo abrir la salida)");
        audio_api = &audio_null;
    }
    audio_init();
    sound_init();

    thread5_game_loop(NULL);
    inited = 1;

    ps5gpu_notify("sm64: entrando al bucle");
    gfx_ps5_api.main_loop(produce_one_frame);
    return 0;
}

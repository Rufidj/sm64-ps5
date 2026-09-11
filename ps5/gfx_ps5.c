/* gfx_ps5 - the GfxWindowManagerAPI half of the PS5 backend. It owns the frame
 * loop and the clock; gfx_agc.c owns the drawing.
 *
 * There is no window to manage: the display is opened once by ps5gpu_init and
 * the title owns the whole screen, so the fullscreen and keyboard entry points
 * are deliberately empty rather than missing - the caller calls them
 * unconditionally.
 */

#include <stdint.h>
#include <stdbool.h>

#include "gfx/gfx_window_manager_api.h"
#include "ps5gpu.h"
#include "ps5_settings.h"

extern uint64_t sceKernelGetProcessTimeCounter(void);
extern uint64_t sceKernelGetProcessTimeCounterFrequency(void);

static uint64_t s_counter_frequency;

static void gfx_ps5_init(const char *game_name, bool start_in_fullscreen) {
    (void)game_name; (void)start_in_fullscreen;
    if (ps5gpu_init() < 0) {
        char msg[128];
        const char *why = ps5gpu_last_error();
        int n = 0;
        const char *p = "ps5gpu: fallo al iniciar: ";
        for (int i = 0; p[i] && n < 100; i++) msg[n++] = p[i];
        for (int i = 0; why[i] && n < 120; i++) msg[n++] = why[i];
        msg[n] = 0;
        ps5gpu_notify(msg);
        return;
    }
    s_counter_frequency = sceKernelGetProcessTimeCounterFrequency();
    if (s_counter_frequency == 0) s_counter_frequency = 1;
    /* The game advances its logic once per frame and was built for 30 of them
     * a second, as the SDL backend enforces. At the display's 60 it would run
     * at double speed, so every frame is held for two vertical blanks. */
    ps5gpu_set_frame_interval(2);
    ps5gpu_notify("ps5gpu: listo");
}

static void gfx_ps5_set_keyboard_callbacks(bool (*on_key_down)(int), bool (*on_key_up)(int), void (*on_all_keys_up)(void)) {
    (void)on_key_down; (void)on_key_up; (void)on_all_keys_up;
}

static void gfx_ps5_set_fullscreen_changed_callback(void (*on_fullscreen_changed)(bool)) {
    (void)on_fullscreen_changed;
}

static void gfx_ps5_set_fullscreen(bool enable) { (void)enable; }

static void gfx_ps5_main_loop(void (*run_one_game_iter)(void)) {
    /* Pacing comes from the flip waiting on the vertical blank inside
     * ps5gpu_end_frame, so there is nothing to sleep for here. */
    for (;;) run_one_game_iter();
}

static void gfx_ps5_get_dimensions(uint32_t *width, uint32_t *height) {
    ps5gpu_get_dimensions(width, height);
    /* In 4:3 the game is told the narrower picture, which the engine - built
     * for any aspect - lays its HUD and sky out for; ps5gpu centres it. */
    if (g_ps5_settings.aspect == PS5_ASPECT_4_3 && width && height) *width = *height * 4 / 3;
}

static void gfx_ps5_handle_events(void) {
    /* The controller backend reads the pad directly; there is no event queue. */
}

static bool gfx_ps5_start_frame(void) { return true; }

static void gfx_ps5_swap_buffers_begin(void) {
    /* This is where the recorded frame is handed to the processor and the flip
     * queued - the counterpart to the rendering backend's start_frame. */
    ps5gpu_end_frame();
}

static void gfx_ps5_swap_buffers_end(void) { }

static double gfx_ps5_get_time(void) {
    return (double)sceKernelGetProcessTimeCounter() / (double)s_counter_frequency;
}

struct GfxWindowManagerAPI gfx_ps5_api = {
    gfx_ps5_init,
    gfx_ps5_set_keyboard_callbacks,
    gfx_ps5_set_fullscreen_changed_callback,
    gfx_ps5_set_fullscreen,
    gfx_ps5_main_loop,
    gfx_ps5_get_dimensions,
    gfx_ps5_handle_events,
    gfx_ps5_start_frame,
    gfx_ps5_swap_buffers_begin,
    gfx_ps5_swap_buffers_end,
    gfx_ps5_get_time,
};

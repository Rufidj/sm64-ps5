/* menu_ps5 - the options menu, opened and closed with the touchpad.
 *
 * While it is open the game is frozen - main_ps5.c stops advancing it and draws
 * its last frame again - and the menu is drawn over that, at the display's own
 * resolution, by ps5gpu after the scene is resolved. The text comes from a
 * font atlas made on the build machine (menu_tools/make_font.py).
 *
 * Settings take effect as they change and are saved when the menu closes, to
 * a small text file of key=value lines beside the save.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "ps5gpu.h"
#include "ps5_settings.h"
#include "menu_ps5.h"
#include "menu_build/menu_font.h"
#include "hd_textures.h"

extern unsigned int controller_ps5_poll_buttons(void);
extern void gfx_texture_cache_invalidate(void);

#define PAD_OPTIONS   0x00000008u
#define PAD_UP        0x00000010u
#define PAD_RIGHT     0x00000020u
#define PAD_DOWN      0x00000040u
#define PAD_LEFT      0x00000080u
#define PAD_CIRCLE    0x00002000u
#define PAD_CROSS     0x00004000u
#define PAD_TOUCH_PAD 0x00100000u
#define CLOSE_BUTTONS (PAD_TOUCH_PAD | PAD_CIRCLE | PAD_OPTIONS)

#define SETTINGS_FILE "/app0/data/sm64_ps5/ajustes.txt"

#define DISPLAY_W 1920.0f
#define DISPLAY_H 1080.0f

/* ---- settings ---- */

Ps5Settings g_ps5_settings = { PS5_ASPECT_16_9, 1, 2, 1, 1, 2, 0, 1, 1, 0, PS5_LANG_ENGLISH, 1 };

/* Everything the menu says, in the language chosen: English first, Spanish
 * second. The game's own text is switched by ps5/lang/ps5_lang.c. */
#define LANG (g_ps5_settings.language)
extern void ps5_lang_set(int language);
/* The game's own rumble queue (src/game/rumble_init.c), used here to buzz the
 * pad once when the player switches the rumble on. */
extern void queue_rumble_data(short duration, short strength);

static const float kSkyStrength[] = { 0.0f, 0.15f, 0.35f, 0.6f, 1.0f };

float ps5_settings_sky_strength(void) {
    return kSkyStrength[g_ps5_settings.sky_level];
}

static const char *const kAspectNames[] = { "16:9", "4:3" };
static const char *const kOnOffEn[] = { "No", "Yes" };
static const char *const kOnOffEs[] = { "No", "Sí" };
static const char *const kSkyEn[] = { "Off", "15%", "35%", "60%", "100%" };
static const char *const kSkyEs[] = { "Apagado", "15%", "35%", "60%", "100%" };
static const char *const kShadowEn[] = { "Off", "Objects", "Everything" };
static const char *const kShadowEs[] = { "No", "Objetos", "Todo" };
static const char *const kDistanceEn[] = { "Normal", "Far", "Very far" };
static const char *const kDistanceEs[] = { "Normal", "Lejana", "Muy lejana" };
static const char *const kResolutionNames[] = { "1080p", "4K" };
static const char *const kLanguageNames[] = { "English", "Español" };

/* Supersampling is for 1080p: at 4K the scene is already drawn at 3840x2160. */
static bool antialiasing_available(void) { return g_ps5_settings.resolution == 0; }
static bool uhd_available(void) { return ps5gpu_4k_available() != 0; }
static const float kDrawDistanceScale[] = { 1.0f, 3.0f, 6.0f };
extern float gPs5DrawDistanceScale;

/* A row with no value is an option still to come. One whose availability
 * check fails is shown greyed, with the reason, and cannot be changed. */
typedef struct {
    const char *label[2];
    int *value;
    int count;
    const char *const *names[2];
    bool (*available)(void);
    const char *unavailable[2];
} Row;

static const Row kRows[] = {
    { { "Language", "Idioma" },            &g_ps5_settings.language,      2, { kLanguageNames, kLanguageNames } },
    { { "Display", "Pantalla" },           &g_ps5_settings.aspect,        2, { kAspectNames, kAspectNames } },
    { { "Water reflections", "Reflejos en el agua" },
                                           &g_ps5_settings.reflections,   2, { kOnOffEn, kOnOffEs } },
    { { "Sky reflection", "Reflejo del cielo" },
                                           &g_ps5_settings.sky_level,     5, { kSkyEn, kSkyEs } },
    { { "Antialiasing", "Antialiasing" },  &g_ps5_settings.antialiasing,  2, { kOnOffEn, kOnOffEs },
                                           antialiasing_available, { "1080p only", "solo en 1080p" } },
    { { "Resolution", "Resolución" },      &g_ps5_settings.resolution,    2, { kResolutionNames, kResolutionNames },
                                           uhd_available, { "not available", "no disponible" } },
    { { "60 FPS", "60 FPS" },              &g_ps5_settings.fps60,         2, { kOnOffEn, kOnOffEs } },
    { { "Show FPS", "Mostrar FPS" },       &g_ps5_settings.show_fps,      2, { kOnOffEn, kOnOffEs } },
    { { "HD textures", "Texturas HD" },    &g_ps5_settings.hd_textures,   2, { kOnOffEn, kOnOffEs },
                                           hd_textures_available, { "not installed", "no instaladas" } },
    { { "Real shadows", "Sombras reales" },&g_ps5_settings.shadows,       3, { kShadowEn, kShadowEs } },
    { { "Rumble", "Vibración" },           &g_ps5_settings.rumble,        2, { kOnOffEn, kOnOffEs } },
    { { "Draw distance", "Distancia de objetos" },
                                           &g_ps5_settings.draw_distance, 3, { kDistanceEn, kDistanceEs } },
};
#define ROW_COUNT (int)(sizeof kRows / sizeof kRows[0])

static void clamp_settings(void) {
    for (int i = 0; i < ROW_COUNT; i++) {
        const Row *r = &kRows[i];
        if (r->value && (*r->value < 0 || *r->value >= r->count)) *r->value = 0;
    }
}

static void apply_settings(void) {
    /* 4:3 is the middle of the screen; the game is told the narrower width by
     * gfx_ps5, and ps5gpu moves its drawing across to the centre. */
    ps5gpu_set_view_offset_x(g_ps5_settings.aspect == PS5_ASPECT_4_3 ? (1920 - 1440) / 2 : 0);
    gPs5DrawDistanceScale = kDrawDistanceScale[g_ps5_settings.draw_distance];
    ps5gpu_set_quality(g_ps5_settings.resolution == 1 && ps5gpu_4k_available(), g_ps5_settings.antialiasing);
    ps5_lang_set(g_ps5_settings.language);

    /* The textures imported so far came from the other source; all of them
     * are imported again as they are next drawn. */
    static int s_applied_hd = -1;
    int hd = g_ps5_settings.hd_textures && hd_textures_available();
    if (s_applied_hd != -1 && hd != s_applied_hd) gfx_texture_cache_invalidate();
    s_applied_hd = hd;
}

static bool row_usable(const Row *r) {
    return r->value && (!r->available || r->available());
}

static const struct { const char *key; int *value; } kKeys[] = {
    { "pantalla", &g_ps5_settings.aspect },
    { "reflejos", &g_ps5_settings.reflections },
    { "cielo",    &g_ps5_settings.sky_level },
    { "fps60",    &g_ps5_settings.fps60 },
    { "hd",       &g_ps5_settings.hd_textures },
    { "sombras",  &g_ps5_settings.shadows },
    { "fps",      &g_ps5_settings.show_fps },
    { "distancia", &g_ps5_settings.draw_distance },
    { "aa",       &g_ps5_settings.antialiasing },
    { "resolucion", &g_ps5_settings.resolution },
    { "idioma",     &g_ps5_settings.language },
    { "vibracion",  &g_ps5_settings.rumble },
};
#define KEY_COUNT (int)(sizeof kKeys / sizeof kKeys[0])

/* The console's own language, for the first run: Spanish if it is set to
 * Spanish, English otherwise. Its parameter 1 is the system language, and 3
 * and 27 are the two Spanishes. */
extern int sceSystemServiceParamGetInt(int paramId, int *value);

static int system_language(void) {
    int value = 1;
    if (sceSystemServiceParamGetInt(1, &value) < 0) return PS5_LANG_ENGLISH;
    return (value == 3 || value == 27) ? PS5_LANG_SPANISH : PS5_LANG_ENGLISH;
}

static void settings_load(void) {
    FILE *f = fopen(SETTINGS_FILE, "rb");
    if (!f) { g_ps5_settings.language = system_language(); return; }
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;

    int chose_language = 0;
    char *line = buf;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') end++;
        for (int k = 0; k < KEY_COUNT; k++) {
            const char *key = kKeys[k].key;
            int i = 0;
            while (key[i] && line + i < end && line[i] == key[i]) i++;
            if (key[i] || line + i >= end || line[i] != '=') continue;
            int v = 0, digits = 0;
            for (char *p = line + i + 1; p < end && *p >= '0' && *p <= '9'; p++, digits++) v = v * 10 + (*p - '0');
            if (digits) {
                *kKeys[k].value = v;
                if (kKeys[k].value == &g_ps5_settings.language) chose_language = 1;
            }
        }
        line = *end ? end + 1 : end;
    }
    if (!chose_language) g_ps5_settings.language = system_language();
    clamp_settings();
}

static void settings_save(void) {
    char buf[256];
    int n = 0;
    for (int k = 0; k < KEY_COUNT; k++) {
        for (const char *p = kKeys[k].key; *p; p++) buf[n++] = *p;
        buf[n++] = '=';
        int v = *kKeys[k].value;
        char digits[12];
        int d = 0;
        do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v > 0 && d < 11);
        while (d > 0) buf[n++] = digits[--d];
        buf[n++] = '\n';
    }
    FILE *f = fopen(SETTINGS_FILE, "wb");
    if (!f) { ps5gpu_notify(LANG ? "sm64: no se pudieron guardar los ajustes"
                                 : "sm64: the settings could not be saved"); return; }
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
}

/* ---- drawing ---- */

#define MAX_OVERLAY_VERTS 6 * 1024
static Ps5GpuVertex s_verts[MAX_OVERLAY_VERTS];
static int s_vert_count;
static uint32_t s_font_texture;
static uint8_t s_font_rgba[MENU_FONT_TEX_W * MENU_FONT_TEX_H * 4];

/* A rectangle in display pixels, textured from the atlas, in one colour. */
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t argb) {
    if (s_vert_count + 6 > MAX_OVERLAY_VERTS) return;
    float X0 = x0 / (DISPLAY_W * 0.5f) - 1.0f, X1 = x1 / (DISPLAY_W * 0.5f) - 1.0f;
    float Y0 = 1.0f - y0 / (DISPLAY_H * 0.5f), Y1 = 1.0f - y1 / (DISPLAY_H * 0.5f);
    const float corners[6][4] = {
        { X0, Y0, u0, v0 }, { X1, Y0, u1, v0 }, { X0, Y1, u0, v1 },
        { X1, Y0, u1, v0 }, { X1, Y1, u1, v1 }, { X0, Y1, u0, v1 },
    };
    for (int i = 0; i < 6; i++) {
        Ps5GpuVertex *v = &s_verts[s_vert_count++];
        v->x = corners[i][0]; v->y = corners[i][1]; v->z = 0.0f;
        v->u = corners[i][2]; v->v = corners[i][3]; v->n_pad = 0.0f;
        v->uv_unused[0] = 0.0f; v->uv_unused[1] = 0.0f;
        v->color = argb;
    }
}

static int glyph_of(unsigned codepoint) {
    for (int i = 0; i < MENU_FONT_GLYPHS; i++)
        if (menu_font_codepoints[i] == codepoint) return i;
    return '?' - 32;
}

static void cell_uv(int glyph, float inset, float *u0, float *v0, float *u1, float *v1) {
    float col = (float)(glyph % MENU_FONT_COLS), row = (float)(glyph / MENU_FONT_COLS);
    *u0 = (col + inset) * MENU_FONT_CELL_W / MENU_FONT_TEX_W;
    *u1 = (col + 1.0f - inset) * MENU_FONT_CELL_W / MENU_FONT_TEX_W;
    *v0 = (row + inset) * MENU_FONT_CELL_H / MENU_FONT_TEX_H;
    *v1 = (row + 1.0f - inset) * MENU_FONT_CELL_H / MENU_FONT_TEX_H;
}

static void panel(float x0, float y0, float x1, float y1, uint32_t argb) {
    float u0, v0, u1, v1;
    cell_uv(glyph_of(MENU_FONT_SOLID), 0.25f, &u0, &v0, &u1, &v1);
    quad(x0, y0, x1, y1, u0, v0, u1, v1, argb);
}

/* The strings are UTF-8; everything the font holds is one to three bytes. */
static unsigned next_codepoint(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    if (p[0] < 0x80) { *s += 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0 && p[1]) { *s += 2; return ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); }
    if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        *s += 3;
        return ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
    }
    *s += 1;
    return '?';
}

static float text_width(const char *s, float scale) {
    float w = 0.0f;
    while (*s) w += menu_font_advance[glyph_of(next_codepoint(&s))] * scale;
    return w;
}

/* Draws a string with its top left at (x, y); returns where it ends. */
static float text(float x, float y, const char *s, float scale, uint32_t argb) {
    while (*s) {
        int g = glyph_of(next_codepoint(&s));
        float u0, v0, u1, v1;
        cell_uv(g, 0.0f, &u0, &v0, &u1, &v1);
        float left = x - MENU_FONT_PAD * scale, top = y - MENU_FONT_PAD * scale;
        quad(left, top, left + MENU_FONT_CELL_W * scale, top + MENU_FONT_CELL_H * scale, u0, v0, u1, v1, argb);
        x += menu_font_advance[g] * scale;
    }
    return x;
}

static int s_fps_at_open;

#define COLOUR_PANEL    0xD0101828u
#define COLOUR_BORDER   0xFFE8B830u
#define COLOUR_TITLE    0xFFFFD040u
#define COLOUR_TEXT     0xFFFFFFFFu
#define COLOUR_SOON     0xFF7C7C8Cu
#define COLOUR_SELECTED 0x503070F0u
#define COLOUR_HINT     0xFFB8C0D0u

static void build_overlay(int selected) {
    s_vert_count = 0;
    const float px0 = 430.0f, px1 = 1490.0f, py0 = 130.0f, py1 = 950.0f;
    const float row_h = (float)MENU_FONT_CELL_H - 2 * MENU_FONT_PAD;
    panel(px0 - 4.0f, py0 - 4.0f, px1 + 4.0f, py1 + 4.0f, COLOUR_BORDER);
    panel(px0, py0, px1, py1, COLOUR_PANEL);

    const char *title = LANG ? "Opciones" : "Options";
    text((DISPLAY_W - text_width(title, 1.4f)) * 0.5f, py0 + 28.0f, title, 1.4f, COLOUR_TITLE);

    /* How fast the game was being shown just before the menu opened - the menu
     * itself runs at 30. */
    {
        char fps[32];
        int n = 0, v = s_fps_at_open, d = 0;
        for (const char *p = LANG ? "juego: " : "game: "; *p; p++) fps[n++] = *p;
        char digits[8];
        do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v > 0 && d < 7);
        while (d > 0) fps[n++] = digits[--d];
        for (const char *p = " fps"; *p; p++) fps[n++] = *p;
        fps[n] = 0;
        text(px1 - 40.0f - text_width(fps, 0.7f), py0 + 44.0f, fps, 0.7f, COLOUR_HINT);
    }

    float y = py0 + 120.0f;
    for (int i = 0; i < ROW_COUNT; i++, y += 58.0f) {
        const Row *r = &kRows[i];
        if (i == selected) panel(px0 + 24.0f, y - 10.0f, px1 - 24.0f, y + row_h + 10.0f, COLOUR_SELECTED);
        bool usable = row_usable(r);
        text(px0 + 60.0f, y, r->label[LANG], 1.0f, usable ? COLOUR_TEXT : COLOUR_SOON);

        char value[64];
        int n = 0;
        const char *name = usable ? r->names[LANG][*r->value]
                                  : (r->value ? r->unavailable[LANG] : (LANG ? "próximamente" : "to come"));
        if (usable && i == selected) { value[n++] = '<'; value[n++] = ' '; value[n++] = ' '; }
        for (const char *p = name; *p && n < 56; p++) value[n++] = *p;
        if (usable && i == selected) { value[n++] = ' '; value[n++] = ' '; value[n++] = '>'; }
        value[n] = 0;
        float scale = usable ? 1.0f : 0.8f;
        text(px1 - 60.0f - text_width(value, scale), y + (usable ? 0.0f : 4.0f), value, scale,
             usable ? (i == selected ? COLOUR_TITLE : COLOUR_TEXT) : COLOUR_SOON);
    }

    const char *hint = LANG ? "Cruceta: elegir y cambiar     Círculo o panel táctil: salir"
                            : "D-pad: choose and change     Circle or touchpad: close";
    text((DISPLAY_W - text_width(hint, 0.7f)) * 0.5f, py1 - 62.0f, hint, 0.7f, COLOUR_HINT);
}

/* While the menu is closed: the frames-a-second counter in the top left
 * corner if it is wanted, or nothing. The overlay stays until replaced, so at
 * 60 frames a second the frame drawn without a pass through here keeps it. */
static void idle_overlay(void) {
    if (!g_ps5_settings.show_fps) {
        ps5gpu_set_overlay(NULL, 0, 0);
        return;
    }
    char label[16];
    int n = 0, v = ps5gpu_measured_fps(), d = 0;
    char digits[8];
    do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v > 0 && d < 7);
    while (d > 0) label[n++] = digits[--d];
    for (const char *p = " FPS"; *p; p++) label[n++] = *p;
    label[n] = 0;
    s_vert_count = 0;
#ifdef SM64_PS5_PERF
    {
        /* Diagnostics: CPU milliseconds of each run of the display list, time
         * spent waiting for the graphics processor and the vertical blank,
         * the whole frame, and what each run drew. */
        extern double gfx_perf_ms[3], ps5gpu_perf_wait_ms, ps5gpu_perf_frame_ms;
        extern unsigned gfx_perf_tris[3], ps5gpu_perf_draws;
        extern int gPs5RumbleCalls, gPs5RumbleLastResult, gPs5RumbleMode;
        extern int controller_ps5_pad_handle(void);
        char lines[3][128];
        snprintf(lines[0], sizeof lines[0], "ms  sombras %.1f  reflejo %.1f  normal %.1f  espera %.1f  frame %.1f",
                 gfx_perf_ms[0], gfx_perf_ms[1], gfx_perf_ms[2], ps5gpu_perf_wait_ms, ps5gpu_perf_frame_ms);
        snprintf(lines[1], sizeof lines[1], "tri  sombras %u  reflejo %u  normal %u   draws %u",
                 gfx_perf_tris[0], gfx_perf_tris[1], gfx_perf_tris[2], ps5gpu_perf_draws);
        snprintf(lines[2], sizeof lines[2], "vibra  mando %d  modo %d  llamadas %d  ultimo %d",
                 controller_ps5_pad_handle(), gPs5RumbleMode, gPs5RumbleCalls, gPs5RumbleLastResult);
        float w = text_width(label, 0.8f);
        for (int i = 0; i < 3; i++) {
            if (text_width(lines[i], 0.7f) > w) w = text_width(lines[i], 0.7f);
        }
        float line = MENU_FONT_CELL_H * 0.7f;
        panel(24.0f, 20.0f, 24.0f + w + 32.0f, 20.0f + MENU_FONT_CELL_H * 0.8f + 3.0f * line + 16.0f, 0xA0000000u);
        text(40.0f, 26.0f, label, 0.8f, COLOUR_TITLE);
        for (int i = 0; i < 3; i++) {
            text(40.0f, 26.0f + MENU_FONT_CELL_H * 0.8f + 4.0f + i * line, lines[i], 0.7f, COLOUR_TEXT);
        }
        ps5gpu_set_overlay(s_verts, s_vert_count, s_font_texture);
        return;
    }
#endif
    panel(24.0f, 20.0f, 24.0f + text_width(label, 0.8f) + 32.0f, 20.0f + MENU_FONT_CELL_H * 0.8f + 8.0f, 0xA0000000u);
    text(40.0f, 26.0f, label, 0.8f, COLOUR_TITLE);
    ps5gpu_set_overlay(s_verts, s_vert_count, s_font_texture);
}

/* ---- the menu ---- */

static bool s_open, s_wait_release, s_changed;
static int s_row;
static unsigned int s_previous_buttons;

void menu_ps5_init(void) {
    settings_load();
    for (int i = 0; i < MENU_FONT_TEX_W * MENU_FONT_TEX_H; i++) {
        s_font_rgba[i * 4 + 0] = 255;
        s_font_rgba[i * 4 + 1] = 255;
        s_font_rgba[i * 4 + 2] = 255;
        s_font_rgba[i * 4 + 3] = menu_font_alpha[i];
    }
    s_font_texture = ps5gpu_texture_new();
    ps5gpu_texture_upload(s_font_texture, s_font_rgba, MENU_FONT_TEX_W, MENU_FONT_TEX_H);
    apply_settings();
}

bool menu_ps5_blocks_input(void) {
    return s_open || s_wait_release;
}

bool menu_ps5_update(void) {
    unsigned int buttons = controller_ps5_poll_buttons();
    unsigned int pressed = buttons & ~s_previous_buttons;
    s_previous_buttons = buttons;
    if (s_wait_release && !(buttons & (CLOSE_BUTTONS | PAD_CROSS))) s_wait_release = false;

    if (!s_open) {
        if (!(pressed & PAD_TOUCH_PAD)) { idle_overlay(); return false; }
        s_open = true;
        s_changed = false;
        s_fps_at_open = ps5gpu_measured_fps();
    } else if (pressed & CLOSE_BUTTONS) {
        s_open = false;
        s_wait_release = true;
        if (s_changed) settings_save();
        idle_overlay();
        return false;
    } else {
        if (pressed & PAD_UP)   s_row = (s_row + ROW_COUNT - 1) % ROW_COUNT;
        if (pressed & PAD_DOWN) s_row = (s_row + 1) % ROW_COUNT;
        const Row *r = &kRows[s_row];
        if (row_usable(r) && (pressed & (PAD_LEFT | PAD_RIGHT | PAD_CROSS))) {
            int step = (pressed & PAD_LEFT) ? r->count - 1 : 1;
            *r->value = (*r->value + step) % r->count;
            s_changed = true;
            apply_settings();
            if (r->value == &g_ps5_settings.rumble && g_ps5_settings.rumble) {
                queue_rumble_data(30, 80);
            }
        }
    }

    build_overlay(s_row);
    ps5gpu_set_overlay(s_verts, s_vert_count, s_font_texture);
    return true;
}

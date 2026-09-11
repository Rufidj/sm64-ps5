#include "types.h"
#include "sm64.h"
#include "game/ingame_menu.h"
#include "game/segment2.h"
#include "game/segment7.h"
#include "game/memory.h"
#include "ps5_lang.h"
#include "glyphs_es.h"

/* The Spanish tables, from ps5/lang/translation_es.c. */
extern const struct DialogEntry *const ps5_dialog_table_es[];
extern const u8 *const ps5_course_name_table_es[];
extern const u8 *const ps5_act_name_table_es[];
/* The menus' strings, from the generated strings_es.c. */
extern const struct Ps5LangString ps5_lang_strings[];
extern const s32 ps5_lang_string_count;

/* Defined in src/game/ingame_menu.c, with room for every code. */
extern u8 gDialogCharWidths[256];

s32 gPs5Language = PS5_LANG_ENGLISH;

/* The codes Spanish adds, which no US text uses. */
#define ES_FIRST 0xA4
enum {
    ES_a_ACUTE = 0xA4, ES_e_ACUTE, ES_i_ACUTE, ES_o_ACUTE, ES_u_ACUTE,
    ES_u_DIAERESIS, ES_n_TILDE,
    ES_A_ACUTE, ES_E_ACUTE, ES_I_ACUTE, ES_O_ACUTE, ES_U_ACUTE,
    ES_U_DIAERESIS, ES_N_TILDE,
    ES_INVERTED_QUESTION, ES_INVERTED_EXCLAMATION,
    ES_LAST
};

/* Which letter of the game's own each one is drawn from, and which mark goes
 * over it. A letter of 0 means the glyph is made here instead. */
enum { MARK_NONE, MARK_ACUTE_LOW, MARK_ACUTE_HIGH, MARK_TILDE_LOW, MARK_TILDE_HIGH,
       MARK_DIAERESIS_LOW, MARK_DIAERESIS_HIGH };

struct SpanishLetter {
    u8 letter;      /* the game's own character to draw underneath */
    u8 mark;        /* MARK_* */
    u8 width;       /* what the letter advances by, filled in on the first call */
};

#define DOTLESS_I 0x00   /* not a character of the game's: made here */

static struct SpanishLetter sLetters[ES_LAST - ES_FIRST] = {
    { ASCII_TO_DIALOG('a'), MARK_ACUTE_LOW, 0 },
    { ASCII_TO_DIALOG('e'), MARK_ACUTE_LOW, 0 },
    { DOTLESS_I,            MARK_ACUTE_LOW, 0 },
    { ASCII_TO_DIALOG('o'), MARK_ACUTE_LOW, 0 },
    { ASCII_TO_DIALOG('u'), MARK_ACUTE_LOW, 0 },
    { ASCII_TO_DIALOG('u'), MARK_DIAERESIS_LOW, 0 },
    { ASCII_TO_DIALOG('n'), MARK_TILDE_LOW, 0 },
    { ASCII_TO_DIALOG('A'), MARK_ACUTE_HIGH, 0 },
    { ASCII_TO_DIALOG('E'), MARK_ACUTE_HIGH, 0 },
    { ASCII_TO_DIALOG('I'), MARK_ACUTE_HIGH, 0 },
    { ASCII_TO_DIALOG('O'), MARK_ACUTE_HIGH, 0 },
    { ASCII_TO_DIALOG('U'), MARK_ACUTE_HIGH, 0 },
    { ASCII_TO_DIALOG('U'), MARK_DIAERESIS_HIGH, 0 },
    { ASCII_TO_DIALOG('N'), MARK_TILDE_HIGH, 0 },
    { 0xF4 /* ? */,         MARK_NONE, 0 },
    { 0xF2 /* ! */,         MARK_NONE, 0 },
};

/* ---- the letters made here ---- */

/* The dialogue font's cells are 16 by 8 IA4 textures on their side: the pixel
 * at column x, row y of the upright cell is the texel (15 - y, 7 - x), four
 * bits of it. The menus' small font is 8 by 8, a byte a pixel, the right way up. */
static u8 main_pixel(const u8 *tex, s32 x, s32 y) {
    s32 s = 15 - y, t = 7 - x;
    u8 byte = tex[t * 8 + s / 2];
    return (s % 2 == 0) ? (u8)(byte >> 4) : (u8)(byte & 0xF);
}

static void main_set_pixel(u8 *tex, s32 x, s32 y, u8 value) {
    s32 s = 15 - y, t = 7 - x, i = t * 8 + s / 2;
    if (s % 2 == 0) {
        tex[i] = (u8)((tex[i] & 0x0F) | (value << 4));
    } else {
        tex[i] = (u8)((tex[i] & 0xF0) | value);
    }
}

/* The same glyph turned half a turn about the pixels it covers: an inverted
 * question or exclamation mark from the player's own ROM. */
static void main_turn_over(const u8 *in, u8 *out) {
    s32 x, y, x0 = 8, x1 = -1, y0 = 16, y1 = -1;
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 8; x++) {
            if (main_pixel(in, x, y) != 0) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    for (x = 0; x < 64; x++) out[x] = 0;
    if (x1 < x0) return;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            main_set_pixel(out, x, y, main_pixel(in, x0 + x1 - x, y0 + y1 - y));
}

/* The game's i without its dot, for the accented one to sit over. Lowercase
 * letters start at row 7; the dot is the ink above that. */
static void main_drop_dot(const u8 *in, u8 *out) {
    s32 x, y;
    for (x = 0; x < 64; x++) out[x] = 0;
    for (y = 7; y < 16; y++)
        for (x = 0; x < 8; x++)
            main_set_pixel(out, x, y, main_pixel(in, x, y));
}

static void menu_turn_over(const u8 *in, u8 *out) {
    s32 x, y, x0 = 8, x1 = -1, y0 = 8, y1 = -1;
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            if (in[y * 8 + x] != 0) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    for (x = 0; x < 64; x++) out[x] = 0;
    if (x1 < x0) return;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            out[y * 8 + x] = in[(y0 + y1 - y) * 8 + (x0 + x1 - x)];
}

/* Made once, the first time one of them is drawn: by then the assets are in
 * place, restored from the player's ROM. */
static u8 sMainDotlessI[64], sMainQuestion[64], sMainExclamation[64];
static u8 sMenuQuestion[64], sMenuExclamation[64];
static s32 sMainMade, sMenuMade;

static void make_main_glyphs(void) {
    void **fontLUT = segmented_to_virtual(main_font_lut);
    const u8 *i = segmented_to_virtual(fontLUT[ASCII_TO_DIALOG('i')]);
    const u8 *q = segmented_to_virtual(fontLUT[0xF4]);
    const u8 *e = segmented_to_virtual(fontLUT[0xF2]);
    if (i != NULL) main_drop_dot(i, sMainDotlessI);
    if (q != NULL) main_turn_over(q, sMainQuestion);
    if (e != NULL) main_turn_over(e, sMainExclamation);
    sMainMade = TRUE;
}

static void make_menu_glyphs(void) {
    void **fontLUT = (void **) segmented_to_virtual(menu_font_lut);
    const u8 *q = segmented_to_virtual(fontLUT[0xF4]);
    const u8 *e = segmented_to_virtual(fontLUT[0xF2]);
    if (q != NULL) menu_turn_over(q, sMenuQuestion);
    if (e != NULL) menu_turn_over(e, sMenuExclamation);
    sMenuMade = TRUE;
}

/* What each character advances by. The game's table has room for every code;
 * the ones Spanish adds take the width of the letter underneath. */
static void set_widths(void) {
    s32 i;
    for (i = 0; i < ES_LAST - ES_FIRST; i++) {
        struct SpanishLetter *l = &sLetters[i];
        if (l->width != 0) continue;
        l->width = (u8)(l->letter == DOTLESS_I ? gDialogCharWidths[ASCII_TO_DIALOG('i')]
                                               : gDialogCharWidths[l->letter]);
        gDialogCharWidths[ES_FIRST + i] = l->width;
    }
}

void ps5_lang_set(s32 language) {
    gPs5Language = language == PS5_LANG_SPANISH ? PS5_LANG_SPANISH : PS5_LANG_ENGLISH;
    set_widths();
}

/* ---- what the game asks for ---- */

static const u8 *mark_main(u8 mark) {
    switch (mark) {
        case MARK_ACUTE_LOW:      return ps5_mark_main_acute_low;
        case MARK_ACUTE_HIGH:     return ps5_mark_main_acute_high;
        case MARK_TILDE_LOW:      return ps5_mark_main_tilde_low;
        case MARK_TILDE_HIGH:     return ps5_mark_main_tilde_high;
        case MARK_DIAERESIS_LOW:  return ps5_mark_main_diaeresis_low;
        case MARK_DIAERESIS_HIGH: return ps5_mark_main_diaeresis_high;
        default:                  return NULL;
    }
}

static const u8 *mark_menu(u8 mark) {
    switch (mark) {
        case MARK_ACUTE_LOW: case MARK_ACUTE_HIGH:         return ps5_mark_menu_acute;
        case MARK_TILDE_LOW: case MARK_TILDE_HIGH:         return ps5_mark_menu_tilde;
        case MARK_DIAERESIS_LOW: case MARK_DIAERESIS_HIGH: return ps5_mark_menu_diaeresis;
        default:                                           return NULL;
    }
}

s32 ps5_lang_main_glyph(u8 c, const void **glyph, const void **mark) {
    struct SpanishLetter *l;
    void **fontLUT;

    if (c < ES_FIRST || c >= ES_LAST) return FALSE;
    if (!sMainMade) make_main_glyphs();
    set_widths();

    l = &sLetters[c - ES_FIRST];
    fontLUT = segmented_to_virtual(main_font_lut);
    *mark = mark_main(l->mark);
    switch (c) {
        case ES_i_ACUTE:             *glyph = sMainDotlessI; break;
        case ES_INVERTED_QUESTION:   *glyph = sMainQuestion; break;
        case ES_INVERTED_EXCLAMATION:*glyph = sMainExclamation; break;
        default:                     *glyph = segmented_to_virtual(fontLUT[l->letter]); break;
    }
    return TRUE;
}

s32 ps5_lang_menu_glyph(u8 c, const void **glyph, const void **mark) {
    struct SpanishLetter *l;
    void **fontLUT;

    if (c < ES_FIRST || c >= ES_LAST) return FALSE;
    if (!sMenuMade) make_menu_glyphs();
    set_widths();

    l = &sLetters[c - ES_FIRST];
    fontLUT = (void **) segmented_to_virtual(menu_font_lut);
    *mark = mark_menu(l->mark);
    switch (c) {
        case ES_INVERTED_QUESTION:    *glyph = sMenuQuestion; break;
        case ES_INVERTED_EXCLAMATION: *glyph = sMenuExclamation; break;
        default:
            /* The small font has capitals only, so a lowercase letter with a
             * mark is drawn as its capital. */
            *glyph = segmented_to_virtual(fontLUT[l->letter >= ASCII_TO_DIALOG('a')
                                                 ? (u8)(l->letter - (ASCII_TO_DIALOG('a') - ASCII_TO_DIALOG('A')))
                                                 : l->letter]);
            break;
    }
    return TRUE;
}

void *ps5_lang_dialog_table(void) {
    if (gPs5Language == PS5_LANG_SPANISH) return (void *) ps5_dialog_table_es;
    return segmented_to_virtual(seg2_dialog_table);
}

void *ps5_lang_course_name_table(void) {
    if (gPs5Language == PS5_LANG_SPANISH) return (void *) ps5_course_name_table_es;
    return segmented_to_virtual(seg2_course_name_table);
}

void *ps5_lang_act_name_table(void) {
    if (gPs5Language == PS5_LANG_SPANISH) return (void *) ps5_act_name_table_es;
    return segmented_to_virtual(seg2_act_name_table);
}

const u8 *ps5_lang_string(const u8 *str) {
    s32 i, j;

    if (gPs5Language != PS5_LANG_SPANISH || str == NULL) return str;
    for (i = 0; i < ps5_lang_string_count; i++) {
        const u8 *en = ps5_lang_strings[i].en;
        for (j = 0; en[j] != DIALOG_CHAR_TERMINATOR; j++) {
            if (str[j] != en[j]) break;
        }
        if (en[j] == DIALOG_CHAR_TERMINATOR && str[j] == DIALOG_CHAR_TERMINATOR)
            return ps5_lang_strings[i].es;
    }
    return str;
}

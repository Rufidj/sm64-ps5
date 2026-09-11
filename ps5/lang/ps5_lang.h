/* ps5_lang - the game's text in the language the player chose.
 *
 * The game is compiled with its own English text and, next to it, a Spanish
 * translation of everything it prints: the dialogues, the names of the courses
 * and their stars, and the strings in the menus. Which one is used is decided
 * while playing, from the options menu.
 *
 * The US ROM has no accented letters. Where Spanish needs one, the game's own
 * letter is drawn and a mark drawn over it, as the European versions do; the
 * marks are in ps5/lang/make_glyphs.py. The inverted question and exclamation
 * marks, and the dotless i, are made on the console by turning or trimming the
 * player's own ROM glyphs.
 */
#ifndef PS5_LANG_H
#define PS5_LANG_H

#include "types.h"

/* The same numbers the options menu saves (ps5/ps5_settings.h). */
#ifndef PS5_LANG_ENGLISH
#define PS5_LANG_ENGLISH 0
#define PS5_LANG_SPANISH 1
#endif

/* What the game is printing in. Read by the hooks in ingame_menu.c. */
extern s32 gPs5Language;

void ps5_lang_set(s32 language);

/* The menus' strings: the one the game asked for, or its translation. */
const u8 *ps5_lang_string(const u8 *str);

/* The tables the dialogues, the course names and the star names come from. */
void *ps5_lang_dialog_table(void);
void *ps5_lang_course_name_table(void);
void *ps5_lang_act_name_table(void);

/* A letter Spanish adds: the glyph to draw and the mark to draw over it, for
 * the dialogue font and for the menus' small font. The mark can be NULL.
 * Returns FALSE for every character the game already has. */
s32 ps5_lang_main_glyph(u8 c, const void **glyph, const void **mark);
s32 ps5_lang_menu_glyph(u8 c, const void **glyph, const void **mark);

/* Each translated string, paired with the English one it replaces. Generated
 * from ps5/lang/strings_es.c.in. */
struct Ps5LangString {
    const u8 *en;
    const u8 *es;
};

#endif

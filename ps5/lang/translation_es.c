/* The Spanish text, in the same shape as the game's own: the dialogue table,
 * the course names and the star names. Built like the European versions build
 * theirs (bin/eu/translation_*.c) - the same generator over ps5/lang/text_es/,
 * with the symbols renamed so both languages are in the program at once. */

#include "macros.h"
#include "game/ingame_menu.h"
#include "make_const_nonconst.h"

#define seg2_course_name_table ps5_course_name_table_es
#define seg2_act_name_table    ps5_act_name_table_es
#define seg2_dialog_table      ps5_dialog_table_es
#define seg2_debug_text_table  ps5_debug_text_table_es

#include "text_es_define.inc.c"

/* menu_ps5 - the options menu, opened and closed with the touchpad. */
#ifndef MENU_PS5_H
#define MENU_PS5_H

#include <stdbool.h>

/* Loads the saved settings, applies them and prepares the font. Needs ps5gpu
 * up, so it comes after gfx_init. */
void menu_ps5_init(void);

/* Reads the pad, opens, drives or closes the menu, and hands its drawing to
 * ps5gpu for this frame. Returns whether the menu is open, in which case the
 * game should stay frozen. */
bool menu_ps5_update(void);

/* Whether the game must not see the pad: while the menu is open, and after it
 * closes until the button that closed it is let go. */
bool menu_ps5_blocks_input(void);

#endif
